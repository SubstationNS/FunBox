#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include "clock.h"
#include "fx.h"
#include "midi_in.h"
#include "midi_sync.h"

//
// Ceres - MIDI-synced dual-track bar looper for the Funbox v3.
// Design: docs/looper-design.md
//
// Phase 1c: internal clock synced to MIDI clock, + loop FX test. There are no loops yet, so the
// FX run on the live input (In L -> Out L, In R -> Out R, one instance per channel).
//   MIDI clock steers the internal clock (midi_sync.h); Start / Continue / Stop / Song Position.
//   Without MIDI clock, knob 2 sets the tempo (60-200 BPM, whole BPM). When the MIDI clock goes
//   away the last tempo stays until knob 2 is moved.
//   Knob 5 is the FX amount (0 = off), 3-way 2 picks the FX: Tape / Delay / Repeat.
//   LED 2 flashes on every beat, LED 1 + LED 2 flash on bar 1.
//   Sync state, tempo, FX and CPU load are printed over USB serial once per bar.
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

// Knob 2 has to move this far (in BPM) to take the tempo back after a MIDI clock went away
static const float kTempoPickupBpm = 3.0f;

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
//   T<bpm>    pin the tempo, ignoring knob 2 (e.g. T120); MIDI clock still wins
//   K         hand tempo, FX type and amount back to the controls
//   F<0-2>    pin the FX type: 0 tape, 1 delay, 2 repeat
//   A<0-100>  pin the FX amount in percent
//   S         stress test on/off: all three FX in series on both channels
//   M<hex>    inject MIDI bytes as if they came in on the MIDI jack (e.g. Mf20010 = SPP bar 2)
//   raw bytes 0xF8-0xFF are injected directly (clock, start, continue, stop), timestamped on arrival
volatile bool  bootloader_requested;
volatile float tempo_override;    // 0 = use knob 2
volatile bool  tempo_override_new;
volatile int   fx_override = -1;  // -1 = use 3-way 2
volatile float amount_override = -1.0f;  // < 0 = use knob 5
volatile bool  stress;
char           usb_cmd[32];
uint32_t       usb_cmd_len;

// MIDI in. The UART interrupt timestamps every byte (timer ticks) and queues it, the audio
// callback parses them and steers the clock before it runs the block.
MidiUartTransport midi_uart;
ceres::MidiParser midi_parser;
ceres::MidiSync   midi_sync;

struct StampedByte
{
    uint8_t  byte;
    uint32_t time;  // System::GetTick() when it arrived
};
static const uint32_t kMidiQueueSize = 512;
StampedByte           midi_queue[kMidiQueueSize];
volatile uint32_t     midi_queue_write, midi_queue_read;

uint32_t midi_byte_ticks;     // One MIDI byte on the wire (320 us), in timer ticks
double   samples_per_timer_tick;
double   block_sample;        // Sample count at the start of the current audio block

// Tempo source
bool  knob_has_tempo = true;  // False after a MIDI clock went away, until knob 2 moves
bool  tempo_from_midi;        // The tempo is still the last MIDI clock tempo (for the status line)
float knob_at_handover;

// MIDI diagnostics, read in the main loop
volatile uint32_t midi_ticks, midi_starts, midi_stops, midi_continues, midi_spp, midi_cc, midi_other;
volatile uint32_t midi_dropped, uart_restarts;


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


// From the UART and USB interrupts
void QueueMidiByte(uint8_t b, uint32_t time)
{
    uint32_t primask = __get_PRIMASK();  // Two producers (UART, USB): keep pushes atomic
    __disable_irq();
    uint32_t w = midi_queue_write;
    if (w - midi_queue_read < kMidiQueueSize) {
        midi_queue[w % kMidiQueueSize] = {b, time};
        midi_queue_write               = w + 1;
    } else {
        midi_dropped++;
    }
    __set_PRIMASK(primask);
}


// UART interrupt. Bytes come in chunks: when the line goes idle (one byte-time after the last
// byte) or when the DMA buffer is half full. Back-date each byte to when it finished arriving.
void MidiUartReceive(uint8_t* data, size_t size, void*)
{
    uint32_t now = System::GetTick();
    if (USART1->ISR & USART_ISR_IDLE)
        now -= midi_byte_ticks;
    for (size_t i = 0; i < size; i++)
        QueueMidiByte(data[i], now - (uint32_t)(size - 1 - i) * midi_byte_ticks);
}


void HandleMidiMessage(const ceres::MidiMsg& m)
{
    switch (m.type) {
        case ceres::MidiMsg::CLOCK:
            midi_ticks++;
            midi_sync.OnClock(m.time, clock, block_sample);
            break;
        case ceres::MidiMsg::START:
            midi_starts++;
            midi_sync.OnStart();
            break;
        case ceres::MidiMsg::CONTINUE:
            midi_continues++;
            midi_sync.OnContinue();
            break;
        case ceres::MidiMsg::STOP:
            midi_stops++;
            midi_sync.OnStop(clock);
            break;
        case ceres::MidiMsg::SONG_POSITION:
            midi_spp++;
            midi_sync.OnSongPosition(m.value());
            break;
        case ceres::MidiMsg::CC: midi_cc++; break;
        default: midi_other++; break;
    }
}


// Audio callback: everything that arrived before this block, in order
void ProcessMidi(uint32_t block_time)
{
    while (midi_queue_read != midi_queue_write) {
        StampedByte sb = midi_queue[midi_queue_read % kMidiQueueSize];
        midi_queue_read++;
        double t = block_sample + (double)(int32_t)(sb.time - block_time) * samples_per_timer_tick;
        ceres::MidiMsg m;
        if (midi_parser.Feed(sb.byte, t, m))
            HandleMidiMessage(m);
    }
    midi_sync.Update(block_sample, clock);
}


// MIDI clock when there is one. Otherwise the pinned tempo or knob 2; after a MIDI clock goes
// away its last tempo stays until knob 2 moves (or a new T command), so losing it is inaudible.
void UpdateTempo(float knob)
{
    if (midi_sync.Locked()) {
        knob_has_tempo   = false;
        tempo_from_midi  = true;
        knob_at_handover = knob;
        return;
    }
    if (tempo_override_new) {
        tempo_override_new = false;
        tempo_from_midi    = false;
        clock.SetTempo(tempo_override);
    }
    if (tempo_override > 0.0f)
        return;
    if (!knob_has_tempo && fabsf(knob - knob_at_handover) * (kMaxBpm - kMinBpm) > kTempoPickupBpm) {
        knob_has_tempo  = true;
        tempo_from_midi = false;
    }
    if (knob_has_tempo) {
        float bpm = UpdateTempoFromKnob(knob, clock.Tempo());
        if (bpm != clock.Tempo())
            clock.SetTempo(bpm);
    }
}


void RunUsbCommand(char cmd, int value)
{
    switch (cmd) {
        case 'T':
            if (value >= kMinBpm && value <= kMaxBpm) {
                tempo_override     = (float)value;
                tempo_override_new = true;
            }
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


static int HexDigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
}


void UsbReceive(uint8_t* buf, uint32_t* len)
{
    uint32_t now = System::GetTick();
    for (uint32_t i = 0; i < *len; i++) {
        char c = buf[i];
        if ((uint8_t)c >= 0xF8) {
            QueueMidiByte((uint8_t)c, now);  // Realtime MIDI, for testing without a MIDI cable
        } else if (usb_cmd_len > 0 && usb_cmd[0] == 'M' && HexDigit(c) >= 0) {
            if (usb_cmd_len < sizeof(usb_cmd) - 1)
                usb_cmd[usb_cmd_len++] = c;
        } else if (usb_cmd_len > 0 && usb_cmd[0] == 'M' && (c == '\n' || c == '\r')) {
            for (uint32_t n = 1; n + 1 < usb_cmd_len; n += 2)
                QueueMidiByte((uint8_t)(HexDigit(usb_cmd[n]) * 16 + HexDigit(usb_cmd[n + 1])), now);
            usb_cmd_len = 0;
        } else if (c == 'B') {
            bootloader_requested = true;
        } else if (c == 'K') {
            tempo_override  = 0.0f;
            knob_has_tempo  = true;
            tempo_from_midi = false;
            fx_override     = -1;
            amount_override = -1.0f;
        } else if (c == 'S') {
            stress = !stress;
        } else if (c == 'T' || c == 'F' || c == 'A' || c == 'M') {
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
    uint32_t block_time = System::GetTick();
    cpu_meter.OnBlockStart();

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    ProcessMidi(block_time);  // Steers the clock before this block runs
    UpdateTempo(tempo_knob.Process());

    int   fx     = fx_override >= 0 ? (int)fx_override : ReadFxSwitch();
    float amount = amount_override >= 0.0f ? (float)amount_override : fx_knob.Process();
    current_fx     = fx;
    current_amount = amount;

    float delay_time = (float)(kDelayTicks * clock.SamplesPerTickNominal());
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

    block_sample += size;
    cpu_meter.OnBlockEnd();
}


// Once per bar over USB serial (two lines, libDaisy's logger takes 128 characters per line), e.g.
//   bar 12  120.02 BPM  midi locked  playing  err +0.04 ms  trim +0.011%  jitter 0.82 ms  fit 192
//     fx delay 40%  cpu 10/11%  midi clk 4417 start 1 cont 0 stop 0 spp 0 cc 0 other 0 drop 0 uart 0
void PrintStatus()
{
    float ms_per_tick = 60000.0f / (clock.Tempo() * Clock::kTicksPerBeat);
    const char* sync  = midi_sync.Locked() ? (midi_sync.TempoChanging() ? "midi locked, tempo moving" : "midi locked")
                                           : (tempo_from_midi ? "free-running" : "internal");
    // PrintLine has no %f: hundredths/thousandths by hand
    int tempo_x100  = (int)roundf(clock.Tempo() * 100.0f);
    int err_x100    = (int)roundf((float)midi_sync.PhaseErrorTicks() * ms_per_tick * 100.0f);
    int trim_x1000  = (int)roundf(clock.RateTrim() * 100000.0f);
    int jitter_x100 = (int)roundf((float)(midi_sync.JitterSamples() / samples_per_timer_tick)
                                  / (float)System::GetTickFreq() * 100000.0f);
    hw.seed.PrintLine("bar %lu  %d.%02d BPM  %s  %s  err %c%d.%02d ms  trim %c%d.%03d%%  jitter %d.%02d ms  fit %d",
                      bar_to_print, tempo_x100 / 100, tempo_x100 % 100, sync, midi_sync.Playing() ? "playing" : "STOPPED",
                      err_x100 < 0 ? '-' : '+', abs(err_x100) / 100, abs(err_x100) % 100,
                      trim_x1000 < 0 ? '-' : '+', abs(trim_x1000) / 1000, abs(trim_x1000) % 1000,
                      jitter_x100 / 100, jitter_x100 % 100, midi_sync.FitTicks());
    hw.seed.PrintLine("  fx %s%s %d%%  cpu %d/%d%%  midi clk %lu start %lu cont %lu stop %lu spp %lu cc %lu other %lu drop %lu uart %lu",
                      stress ? "STRESS " : "", kFxNames[current_fx], (int)(current_amount * 100.0f),
                      (int)(cpu_meter.GetAvgCpuLoad() * 100.0f), (int)(cpu_meter.GetMaxCpuLoad() * 100.0f),
                      midi_ticks, midi_starts, midi_continues, midi_stops, midi_spp, midi_cc, midi_other,
                      midi_dropped, uart_restarts);
    cpu_meter.Reset();  // Max per bar
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

    // MIDI in on the Funbox jack (USART1, D30), with our own parser instead of libDaisy's
    midi_byte_ticks        = System::GetTickFreq() / 3125;  // 10 bits at 31250 baud
    samples_per_timer_tick = (double)sample_rate / (double)System::GetTickFreq();
    midi_sync.Init(sample_rate);
    midi_parser.Reset();
    MidiUartTransport::Config midi_config;
    midi_config.rx = seed::D30;
    midi_config.tx = seed::D29;
    midi_uart.Init(midi_config);
    midi_uart.StartRx(MidiUartReceive, nullptr);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Let the MIDI interrupt (priority 0) cut into the audio callback, so clock bytes get
    // timestamped when they arrive, not when the DSP happens to be done
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 1, 0);  // SAI1 A
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 1, 0);  // SAI1 B

    hw.seed.PrintLine("Ceres phase 1c: MIDI clock sync + loop FX test");

    while (1)
    {
        if (bootloader_requested)
            System::ResetToBootloader();  // STM32 DFU, no BOOT button needed

        // The UART stops listening after an error (e.g. a framing error from a loose cable)
        if (!midi_uart.RxActive()) {
            uart_restarts++;
            midi_uart.StartRx(MidiUartReceive, nullptr);
        }

        if (bar_print_pending) {
            bar_print_pending = false;
            PrintStatus();
        }
    }
}
