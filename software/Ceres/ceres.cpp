#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include "clock.h"
#include "fx.h"

//
// Ceres - MIDI-synced dual-track bar looper for the Funbox v3.
// Design: docs/looper-design.md
//
// Phase 1b: internal clock + loop FX test. There are no loops yet, so the FX run on the
// live input (In L -> Out L, In R -> Out R, one instance per channel).
//   Knob 2 sets the tempo (60-200 BPM, whole BPM).
//   Knob 5 is the FX amount (0 = off), 3-way 2 picks the FX: Tape / Delay / Repeat.
//   LED 2 flashes on every beat, LED 1 + LED 2 flash on bar 1.
//   Tempo, FX and CPU load are printed over USB serial once per bar.
//   MIDI is still received and counted for diagnostics, but doesn't steer the clock yet.
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter  tempo_knob, fx_knob;

Led led1, led2;

Clock         clock;
CpuLoadMeter  cpu_meter;

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

// Loop FX, one per output channel. Buffers live in SDRAM.
enum FxType { FX_TAPE = 0, FX_DELAY, FX_REPEAT };
static const char* kFxNames[] = {"tape", "delay", "repeat"};

static const uint32_t kTapeBufSize   = 4096;    // ~85 ms, plenty for wow/flutter
static const uint32_t kDelayBufSize  = 120000;  // 2.5 s: a 1/4 note at 24 BPM, dotted 1/8 at 60 has lots of room
static const uint32_t kRepeatBufSize = 50000;   // 1.04 s: one beat at 60 BPM
static const uint32_t kDelayTicks    = 18;      // Dotted 1/8 note (24 ticks per beat)

float DSY_SDRAM_BSS tape_buf[2][kTapeBufSize];
float DSY_SDRAM_BSS delay_buf[2][kDelayBufSize];
float DSY_SDRAM_BSS repeat_buf[2][kRepeatBufSize];

ceres::TapeFx     tape[2];
ceres::SyncDelay  delay_fx[2];
ceres::BeatRepeat repeat[2];

// Set in the audio callback, printed in the main loop (no USB from the interrupt)
volatile bool     bar_print_pending;
volatile uint32_t bar_to_print;
volatile int      current_fx;
volatile float    current_amount;

// Dev commands over USB serial (letter + number + newline, except B/K/S):
//   B         reboot into the DFU bootloader (used by flash.sh)
//   T<bpm>    pin the tempo, ignoring knob 2 (e.g. T120)
//   K         hand tempo, FX type and amount back to the controls
//   F<0-2>    pin the FX type: 0 tape, 1 delay, 2 repeat
//   A<0-100>  pin the FX amount in percent
//   S         stress test on/off: all three FX in series on both channels
volatile bool  bootloader_requested;
volatile float tempo_override;    // 0 = use knob 2
volatile int   fx_override = -1;  // -1 = use 3-way 2
volatile float amount_override = -1.0f;  // < 0 = use knob 5
volatile bool  stress;
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


// 3-way 2: left = Tape, center = Delay, right = Repeat
int ReadFxSwitch()
{
    if (hw.switches[Funbox::SWITCH_2_LEFT].Pressed())
        return FX_TAPE;
    if (hw.switches[Funbox::SWITCH_2_RIGHT].Pressed())
        return FX_REPEAT;
    return FX_DELAY;
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


void RunUsbCommand(char cmd, int value)
{
    switch (cmd) {
        case 'T':
            if (value >= kMinBpm && value <= kMaxBpm)
                tempo_override = (float)value;
            break;
        case 'F':
            if (value >= FX_TAPE && value <= FX_REPEAT)
                fx_override = value;
            break;
        case 'A':
            if (value >= 0 && value <= 100)
                amount_override = value / 100.0f;
            break;
    }
}


void UsbReceive(uint8_t* buf, uint32_t* len)
{
    for (uint32_t i = 0; i < *len; i++) {
        char c = buf[i];
        if (c == 'B') {
            bootloader_requested = true;
        } else if (c == 'K') {
            tempo_override  = 0.0f;
            fx_override     = -1;
            amount_override = -1.0f;
        } else if (c == 'S') {
            stress = !stress;
        } else if (c == 'T' || c == 'F' || c == 'A') {
            usb_cmd_len = 0;
            usb_cmd[usb_cmd_len++] = c;
        } else if (usb_cmd_len > 0 && c >= '0' && c <= '9' && usb_cmd_len < sizeof(usb_cmd) - 1) {
            usb_cmd[usb_cmd_len++] = c;
        } else if (usb_cmd_len > 0 && (c == '\n' || c == '\r')) {
            usb_cmd[usb_cmd_len] = 0;
            RunUsbCommand(usb_cmd[0], atoi(&usb_cmd[1]));
            usb_cmd_len = 0;
        }
    }
}


static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    cpu_meter.OnBlockStart();

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    float bpm = UpdateTempoFromKnob(tempo_knob.Process(), clock.Tempo());
    if (tempo_override > 0.0f)
        bpm = tempo_override;
    if (bpm != clock.Tempo())
        clock.SetTempo(bpm);

    int   fx     = fx_override >= 0 ? (int)fx_override : ReadFxSwitch();
    float amount = amount_override >= 0.0f ? (float)amount_override : fx_knob.Process();
    current_fx     = fx;
    current_amount = amount;

    float delay_time = (float)(kDelayTicks * clock.SamplesPerTick());
    delay_fx[0].SetTime(delay_time);
    delay_fx[1].SetTime(delay_time);

    for (size_t i = 0; i < size; i++) {
        uint8_t events = clock.Process();
        if (events & Clock::BAR) {
            bar_to_print      = clock.Bar() + 1;
            bar_print_pending = true;
        }
        uint32_t tick = clock.Tick();

        for (int ch = 0; ch < 2; ch++) {
            float x = in[ch][i];
            if (stress) {
                // Worst case: everything at once
                x = tape[ch].Process(x, amount);
                x = delay_fx[ch].Process(x, amount);
                x = repeat[ch].Process(x, amount, events, tick);
            } else if (fx == FX_TAPE) {
                x = tape[ch].Process(x, amount);
            } else if (fx == FX_DELAY) {
                x = delay_fx[ch].Process(x, amount);
            } else {
                x = repeat[ch].Process(x, amount, events, tick);
            }
            out[ch][i] = x;
        }
    }

    // LEDs: LED 2 on every beat, LED 1 joins on bar 1 (and stays a bit longer)
    bool bar_one  = clock.BeatInBar() == 0;
    uint32_t tick = clock.TickInBeat();
    led1.Set(bar_one && tick < kBarFlashTicks ? 1.0f : 0.0f);
    led2.Set(tick < (bar_one ? kBarFlashTicks : kBeatFlashTicks) ? 1.0f : 0.0f);
    led1.Update();
    led2.Update();

    cpu_meter.OnBlockEnd();
}


int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);

    // USB serial log, doesn't wait for a host to connect
    hw.seed.StartLog(false);
    hw.seed.usb_handle.SetReceiveCallback(UsbReceive, UsbHandle::FS_INTERNAL);

    tempo_knob.Init(hw.knob[Funbox::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
    fx_knob.Init(hw.knob[Funbox::KNOB_5], 0.0f, 1.0f, Parameter::LINEAR);

    led1.Init(hw.seed.GetPin(Funbox::LED_1), false);
    led2.Init(hw.seed.GetPin(Funbox::LED_2), false);

    float sample_rate = hw.AudioSampleRate();
    if (sample_rate == 48000.0f)
        sample_rate = kTrueSampleRate48k;
    clock.Init(sample_rate, 120.0f);

    for (int ch = 0; ch < 2; ch++) {
        tape[ch].Init(sample_rate, tape_buf[ch], kTapeBufSize);
        delay_fx[ch].Init(sample_rate, delay_buf[ch], kDelayBufSize);
        repeat[ch].Init(sample_rate, repeat_buf[ch], kRepeatBufSize);
    }

    cpu_meter.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    hw.InitMidi();
    hw.midi.StartReceive();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    hw.seed.PrintLine("Ceres phase 1b: internal clock + loop FX test");

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
            hw.seed.PrintLine("bar %lu  tempo %d BPM  fx %s%s %d%%  cpu avg %d%% max %d%%  | midi ticks %lu start %lu stop %lu other %lu uart_restarts %lu",
                              bar_to_print, (int)clock.Tempo(),
                              stress ? "STRESS " : "", kFxNames[current_fx], (int)(current_amount * 100.0f),
                              (int)(cpu_meter.GetAvgCpuLoad() * 100.0f), (int)(cpu_meter.GetMaxCpuLoad() * 100.0f),
                              midi_ticks, midi_starts, midi_stops, midi_other, uart_restarts);
            cpu_meter.Reset();  // Max per bar
        }
    }
}
