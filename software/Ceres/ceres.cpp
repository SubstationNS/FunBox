#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include "clock.h"

//
// Ceres - MIDI-synced dual-track bar looper for the Funbox v3.
// Design: docs/looper-design.md
//
// Phase 1a: internal clock. Audio passes straight through.
//   Knob 2 sets the tempo (60-200 BPM, whole BPM).
//   LED 2 flashes on every beat, LED 1 + LED 2 flash on bar 1.
//   Tempo and bar are printed over USB serial once per bar.
//   MIDI is still received and counted for diagnostics, but doesn't steer the clock yet.
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter  tempo_knob;

Led led1, led2;

Clock clock;

// Tempo knob
static const float kMinBpm        = 60.0f;
static const float kMaxBpm        = 200.0f;
static const float kTempoHysteresis = 0.7f;  // BPM the knob must move past a whole BPM before it changes

// The codec doesn't run at exactly 48 kHz: libDaisy clocks the SAI from PLL3
// (16 MHz HSE / 6 * 295 / 16 = 49.1667 MHz) divided by 1024 = 48014.3 Hz, but reports 48000.
// Using the nominal rate would make the clock run 298 ppm fast (a beat every ~56 min at 120 BPM).
static const float kTrueSampleRate48k = 16000000.0f / 6.0f * 295.0f / 16.0f / 1024.0f;

// LED flash lengths, in clock ticks (24 per beat), so they scale with tempo
static const uint32_t kBeatFlashTicks = 3;
static const uint32_t kBarFlashTicks  = 6;

// Set in the audio callback, printed in the main loop (no USB from the interrupt)
volatile bool     bar_print_pending;
volatile uint32_t bar_to_print;

// Dev commands over USB serial:
//   B       reboot into the DFU bootloader (used by flash.sh)
//   T<bpm>  pin the tempo, ignoring knob 2 (e.g. T120, newline-terminated)
//   K       hand the tempo back to knob 2
volatile bool  bootloader_requested;
volatile float tempo_override;  // 0 = use knob 2
char           usb_cmd[8];
uint32_t       usb_cmd_len;

// MIDI diagnostics
uint32_t midi_ticks, midi_starts, midi_stops, midi_other, uart_restarts;


// Whole BPM with hysteresis, so pot noise can't wobble the tempo
float UpdateTempoFromKnob(float knob, float current_bpm)
{
    float target = kMinBpm + knob * (kMaxBpm - kMinBpm);
    if (target > current_bpm + kTempoHysteresis || target < current_bpm - kTempoHysteresis)
        return roundf(target);
    return current_bpm;
}


void HandleMidiMessage(MidiEvent m)
{
    if (m.type == SystemRealTime) {
        switch (m.srt_type) {
            case TimingClock: midi_ticks++; break;
            case Start: midi_starts++; break;
            case Stop: midi_stops++; break;
            default: break;
        }
    } else {
        midi_other++;
    }
}


void UsbReceive(uint8_t* buf, uint32_t* len)
{
    for (uint32_t i = 0; i < *len; i++) {
        char c = buf[i];
        if (c == 'B') {
            bootloader_requested = true;
        } else if (c == 'K') {
            tempo_override = 0.0f;
        } else if (c == 'T') {
            usb_cmd_len = 0;
            usb_cmd[usb_cmd_len++] = c;
        } else if (usb_cmd_len > 0 && c >= '0' && c <= '9' && usb_cmd_len < sizeof(usb_cmd) - 1) {
            usb_cmd[usb_cmd_len++] = c;
        } else if (usb_cmd_len > 0 && (c == '\n' || c == '\r')) {
            usb_cmd[usb_cmd_len] = 0;
            int bpm = atoi(&usb_cmd[1]);
            if (bpm >= kMinBpm && bpm <= kMaxBpm)
                tempo_override = (float)bpm;
            usb_cmd_len = 0;
        }
    }
}


static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    hw.ProcessAnalogControls();

    float bpm = UpdateTempoFromKnob(tempo_knob.Process(), clock.Tempo());
    if (tempo_override > 0.0f)
        bpm = tempo_override;
    if (bpm != clock.Tempo())
        clock.SetTempo(bpm);

    for (size_t i = 0; i < size; i++) {
        uint8_t events = clock.Process();
        if (events & Clock::BAR) {
            bar_to_print      = clock.Bar() + 1;
            bar_print_pending = true;
        }

        // Phase 1a: straight passthrough
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }

    // LEDs: LED 2 on every beat, LED 1 joins on bar 1 (and stays a bit longer)
    bool bar_one  = clock.BeatInBar() == 0;
    uint32_t tick = clock.TickInBeat();
    led1.Set(bar_one && tick < kBarFlashTicks ? 1.0f : 0.0f);
    led2.Set(tick < (bar_one ? kBarFlashTicks : kBeatFlashTicks) ? 1.0f : 0.0f);
    led1.Update();
    led2.Update();
}


int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);

    // USB serial log, doesn't wait for a host to connect
    hw.seed.StartLog(false);
    hw.seed.usb_handle.SetReceiveCallback(UsbReceive, UsbHandle::FS_INTERNAL);

    tempo_knob.Init(hw.knob[Funbox::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);

    led1.Init(hw.seed.GetPin(Funbox::LED_1), false);
    led2.Init(hw.seed.GetPin(Funbox::LED_2), false);

    float sample_rate = hw.AudioSampleRate();
    if (sample_rate == 48000.0f)
        sample_rate = kTrueSampleRate48k;
    clock.Init(sample_rate, 120.0f);

    hw.InitMidi();
    hw.midi.StartReceive();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    hw.seed.PrintLine("Ceres phase 1a: internal clock");

    while (1)
    {
        if (bootloader_requested)
            System::ResetToBootloader();  // STM32 DFU, no BOOT button needed

        // Listen() restarts the UART after an error (e.g. overrun), count those
        if (!hw.midi.RxActive())
            uart_restarts++;
        hw.midi.Listen();
        while (hw.midi.HasEvents())
            HandleMidiMessage(hw.midi.PopEvent());

        if (bar_print_pending) {
            bar_print_pending = false;
            hw.seed.PrintLine("bar %lu  tempo %d BPM  | midi ticks %lu start %lu stop %lu other %lu uart_restarts %lu",
                              bar_to_print, (int)clock.Tempo(),
                              midi_ticks, midi_starts, midi_stops, midi_other, uart_restarts);
        }
    }
}
