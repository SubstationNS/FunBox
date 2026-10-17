#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include "clock.h"
#include "fx.h"
#include "midi_in.h"
#include "midi_sync.h"
#include "footswitch.h"
#include "track.h"
#include "mixer.h"
#include "updater.h"
#include "params.h"
#include "persist.h"

//
// Ceres - MIDI-synced dual-track bar looper for the Funbox v3.
// Design: docs/looper-design.md
//
// Phases 1-5: clock (MIDI synced), two tracks, dual-mono routing, loop FX.
//   Footswitch 1 / 2: Track 1 / 2. Tap = record / overdub / play, double tap = stop (or end the
//   recording on the grid), hold 1 s = undo / redo, hold 2 s = clear. Both together = stop / start all.
//   Knob 1 / 3: Track 1 / 2 level. Knob 4 / 6: Track 1 / 2 filter (LP <- noon -> HP).
//   Knob 2: tempo (60-200 BPM), only without MIDI clock and while both tracks are empty.
//   Knob 5: FX amount on the loops, 3-way 2: Tape / Delay / Repeat.
//   3-way 1: grid for double-tap endings (1/4, 1/8, 1/16). 3-way 3: stop mode (now, end of bar, end of loop).
//   DIP 1: split (In L / Out L = Track 1, In R / Out R = Track 2). DIP 2: power-of-two loop lengths.
//   DIP 4: expression learn (on = state A, off = state B: the pedal morphs what differs).
//   Knobs and the pedal use pickup; MIDI CC (map in the README) jumps. The settings are saved to
//   QSPI 10 s after the last change and restored at power-up.
//   Dry always passes at unity. Status over USB serial once per bar.
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter  tempo_knob, fx_knob, level_knob[2], filter_knob[2];

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
// Delay time per MIDI CC 77: 1/4, dotted 1/8 (default), 1/8, 1/8 triplet, 1/16 (24 ticks per beat)
static const uint32_t kDelayTicks[]  = {24, 18, 12, 8, 6};
volatile uint8_t      delay_division = 1;

float DSY_SDRAM_BSS tape_buf[2][kTapeBufSize];
float DSY_SDRAM_BSS delay_buf[2][kDelayBufSize];
float DSY_SDRAM_BSS repeat_buf[2][kRepeatBufSize];

ceres::TapeFx     tape[2];
ceres::SyncDelay  delay_fx[2];
ceres::BeatRepeat repeat[2];

// Tracks: loop + undo buffer each, 75 s at 48 kHz (about 37 bars at 120 BPM), plus the small
// side buffers that make overdubs cancellable. 59 MB of the 64 MB SDRAM.
static const uint32_t kLoopCapacity = 3600000;
static const uint32_t kSideSize     = 72000;  // 1.5 s, more than Track::kCancelSeconds
float DSY_SDRAM_BSS loop_buf[2][2][kLoopCapacity];
float DSY_SDRAM_BSS side_buf[2][2][kSideSize];

// Parameters (pickup, CC, morph) and their saved state
ceres::Controls      controls;
ceres::SnapshotStore store;
ceres::Snapshot      saved_snap, pending_snap;
uint32_t             pending_since;
volatile uint32_t    saves, save_failures;
volatile uint8_t     preset_bars;        // MIDI CC 29
volatile int8_t      grid_override = -1; // MIDI CC 30 (ticks), until 3-way 1 moves
int8_t               grid_switch_last = -1;
bool                 tempo_restored;
uint32_t             led_flash_until[2];  // Block counter: takeover / learn flashes

ceres::Track      tracks[2];
ceres::Footswitch footswitch[2];
ceres::DjFilter   filters[2];
ceres::Limiter    limiters[2];
ceres::Slew       level_slew[2];
bool              both_latch;   // Both footswitches went down together: ignore them until released
bool              start_pending;  // MIDI Start seen, waiting for the clock to be located
static const double kBothSeconds = 0.08;  // Presses this close together count as both at once

// Set in the audio callback, printed in the main loop (no USB from the interrupt)
volatile bool     bar_print_pending;
volatile uint32_t bar_to_print;
volatile int      current_fx;
volatile float    current_amount;
volatile float    track_rms[2];  // Loop output level over the last bar
float             track_sum[2];
uint32_t          track_count;
bool              split_mode;

// Dev commands over USB serial (letter + number + newline, except B/K/S):
//   B         reboot into the STM32's ROM DFU bootloader (flash.sh, to install the Daisy bootloader)
//   b         reboot into the Daisy bootloader's DFU, waiting until flashed
//   W<size>,<crc32>\n + <size> raw bytes (hex numbers): firmware update. The image (a BOOT_SRAM
//             build) is written to QSPI where the Daisy bootloader loads it from, then Ceres
//             restarts into it. Used by flash.sh; doesn't need the bootloader's DFU mode.
//   T<bpm>    pin the tempo, ignoring knob 2 (e.g. T120); MIDI clock still wins
//   K         hand tempo, FX type and amount back to the controls
//   F<0-2>    pin the FX type: 0 tape, 1 delay, 2 repeat
//   A<0-100>  pin the FX amount in percent
//   S         stress test on/off: all three FX in series on both channels
//   M<hex>    inject MIDI bytes as if they came in on the MIDI jack (e.g. Mf20010 = SPP bar 2)
//   raw bytes 0xF8-0xFF are injected directly (clock, start, continue, stop), timestamped on arrival
//   D<1-2>    footswitch 1 / 2 down, U<1-2> up (e.g. D1, U1: a tap)
//   I<0-1>    test tone instead of the inputs: 220 Hz on In L, 330 Hz on In R (-10 dBFS)
//   N         pin the knob and pedal readings (the bare Seed's knobs float): levels unity,
//             filters flat, FX 0, pedal 0; K releases. X<k><vvv> sets pinned control k (0-4 =
//             knobs 1, 3, 4, 6, 5; 5 = pedal) to vvv %, e.g. X0050. Y1 / Y0: DIP 4 on / off.
//   R         restart (tests the saved state). Z: back to defaults (morph cleared, knobs apply)
//   Q         check the QSPI chip, lift a write protection (see updater.h)
volatile bool  bootloader_requested, daisy_bootloader_requested;

// Firmware update over USB serial (updater.h); the image collects in SDRAM
uint8_t DSY_SDRAM_BSS  update_buf[ceres::FirmwareUpdater::kMaxSize];
ceres::FirmwareUpdater updater;
volatile bool          qspi_probe_requested;
volatile float tempo_override;    // 0 = use knob 2
volatile bool  tempo_override_new;
volatile int   fx_override = -1;  // -1 = use 3-way 2
volatile float amount_override = -1.0f;  // < 0 = use knob 5
volatile bool  stress;
volatile bool  fs_injected[2];
volatile bool  test_tone;
volatile bool  neutral_mix;
float          pinned_raw[6] = {0.7071f, 0.7071f, 0.5f, 0.5f, 0.0f, 0.0f};  // N mode: knobs 1, 3, 4, 6, 5, pedal
volatile bool  dip4_injected;
volatile bool  reset_requested, defaults_requested;
float          tone_phase[2];
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
float    clock_sample_rate;   // The codec's real rate

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


void HandleCc(uint8_t cc, uint8_t value);

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
        case ceres::MidiMsg::CC:
            midi_cc++;
            HandleCc(m.data1, m.data2);
            break;
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
// While a track has a loop (or is recording one) the tempo is locked: loops can't stretch yet.
// Clearing them hands the tempo back to knob 2 once it moves.
void UpdateTempo(float knob)
{
    if (tempo_restored) {  // Saved tempo: knob 2 only takes over once it moves
        static int settle;  // ...from where it really is: its reading ramps up from 0 at power-on
        knob_has_tempo   = false;
        knob_at_handover = knob;
        if (++settle < ceres::Controls::kSettleBlocks)
            return;
        tempo_restored = false;
    }
    bool loops = tracks[0].GetState() != ceres::Track::EMPTY || tracks[1].GetState() != ceres::Track::EMPTY;
    if (midi_sync.Locked() || loops) {
        knob_has_tempo   = false;
        tempo_from_midi |= midi_sync.Locked();
        knob_at_handover = knob;
        tempo_override_new = false;
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
        case 'D':
        case 'U':
            if (value == 1 || value == 2)
                fs_injected[value - 1] = cmd == 'D';
            break;
        case 'I': test_tone = value != 0; break;
        case 'X':  // X<k><vvv>: pinned raw control k (0-4 knobs 1, 3, 4, 6, 5; 5 pedal) at vvv %
            if (value / 1000 <= 5 && value % 1000 <= 100)
                pinned_raw[value / 1000] = (value % 1000) / 100.0f;
            break;
        case 'Y': dip4_injected = value != 0; break;
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
        if (updater.Receive((uint8_t)c))  // W command and firmware image bytes
            continue;
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
        } else if (c == 'b') {
            daisy_bootloader_requested = true;
        } else if (c == 'K') {
            tempo_override  = 0.0f;
            knob_has_tempo  = true;
            tempo_from_midi = false;
            fx_override     = -1;
            amount_override = -1.0f;
            neutral_mix     = false;
        } else if (c == 'S') {
            stress = !stress;
        } else if (c == 'N') {
            neutral_mix = true;
        } else if (c == 'R') {
            reset_requested = true;
        } else if (c == 'Z') {
            defaults_requested = true;
        } else if (c == 'Q') {
            qspi_probe_requested = true;
        } else if (c == 'T' || c == 'F' || c == 'A' || c == 'M' || c == 'D' || c == 'U' || c == 'I' || c == 'X' || c == 'Y') {
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


// The switches: 3-way 1 = grid, 3-way 3 = stop mode, DIP 2 = power of two
ceres::Track::Settings ReadTrackSettings()
{
    ceres::Track::Settings st;
    st.grid_ticks   = hw.switches[Funbox::SWITCH_1_LEFT].Pressed() ? 24 : (hw.switches[Funbox::SWITCH_1_RIGHT].Pressed() ? 6 : 12);
    st.stop_mode    = hw.switches[Funbox::SWITCH_3_LEFT].Pressed() ? ceres::Track::STOP_NOW
                      : (hw.switches[Funbox::SWITCH_3_RIGHT].Pressed() ? ceres::Track::STOP_END_OF_LOOP
                                                                       : ceres::Track::STOP_END_OF_BAR);
    st.power_of_two = hw.switches[Funbox::SWITCH_DIP_2].Pressed();
    // MIDI CC 30 overrides the grid toggle until the toggle moves
    if (grid_switch_last != (int8_t)st.grid_ticks) {
        if (grid_switch_last >= 0)
            grid_override = -1;
        grid_switch_last = (int8_t)st.grid_ticks;
    }
    if (grid_override > 0)
        st.grid_ticks = (uint32_t)grid_override;
    st.preset_bars = preset_bars;
    return st;
}


// Both tracks: stop all if anything plays, else start all
void StopOrStartAll(const ceres::Track::Settings& st)
{
    bool any_playing = false;
    for (auto& t : tracks)
        any_playing |= t.GetState() == ceres::Track::PLAYING || t.GetState() == ceres::Track::OVERDUBBING;
    for (auto& t : tracks) {
        if (any_playing)
            t.RequestStop(st.stop_mode);
        else
            t.RequestPlay();
    }
}


// MIDI CC map (omni). Buttons act when the value goes from below 64 to 64 or more.
//   20 / 21  Track 1 / 2 tap (record / overdub / play)   22 / 23  stop (stop mode)
//   24 / 25  undo / redo                                  26 / 27  clear
//   28       stop all / start all                         29       preset length in bars (0 = off)
//   30       grid: 0-42 1/4, 43-85 1/8, 86-127 1/16 (until 3-way 1 moves)
//   7 / 8    Track 1 / 2 level      74 / 75  Track 1 / 2 filter      76  FX amount
//   77       delay time: 1/4, dotted 1/8, 1/8, 1/8 triplet, 1/16 (value / 26)
void HandleCc(uint8_t cc, uint8_t value)
{
    static uint8_t last[128];
    bool pressed = value >= 64 && last[cc & 127] < 64;
    last[cc & 127] = value;
    float v = value / 127.0f;
    ceres::Track::Settings st = ReadTrackSettings();
    switch (cc) {
        case 20: case 21: if (pressed) tracks[cc - 20].Tap(clock); break;
        case 22: case 23: if (pressed) tracks[cc - 22].RequestStop(st.stop_mode); break;
        case 24: case 25: if (pressed) tracks[cc - 24].Undo(); break;
        case 26: case 27: if (pressed) tracks[cc - 26].Clear(); break;
        case 28: if (pressed) StopOrStartAll(st); break;
        case 29: preset_bars = value > 64 ? 64 : value; break;
        case 30: grid_override = value < 43 ? 24 : (value < 86 ? 12 : 6); break;
        case 7: controls.Cc(ceres::Controls::T1_LEVEL, v); break;
        case 8: controls.Cc(ceres::Controls::T2_LEVEL, v); break;
        case 74: controls.Cc(ceres::Controls::T1_FILTER, v); break;
        case 75: controls.Cc(ceres::Controls::T2_FILTER, v); break;
        case 76: controls.Cc(ceres::Controls::FX_AMOUNT, v); break;
        case 77: delay_division = value / 26 > 4 ? 4 : value / 26; break;
        default: break;
    }
}


void HandleFootswitches(double now, const ceres::Track::Settings& st)
{
    uint8_t ev[2];
    bool    down[2];
    down[0] = hw.switches[Funbox::FOOTSWITCH_1].Pressed() || fs_injected[0];
    down[1] = hw.switches[Funbox::FOOTSWITCH_2].Pressed() || fs_injected[1];
    for (int t = 0; t < 2; t++)
        ev[t] = footswitch[t].Update(down[t], now);

    if (both_latch) {
        if (!down[0] && !down[1])
            both_latch = false;
        return;
    }
    for (int t = 0; t < 2; t++) {
        if (ev[t] & ceres::Footswitch::TAP) {
            int other = 1 - t;
            if (down[other] && now - footswitch[other].LastPress() < kBothSeconds) {
                tracks[other].RevertPress();  // Its press was the first half of this
                StopOrStartAll(st);
                both_latch = true;
                return;
            }
            tracks[t].Tap(clock);
        }
        if (ev[t] & ceres::Footswitch::DOUBLE_TAP)
            tracks[t].DoubleTap(clock);
        if (ev[t] & ceres::Footswitch::HOLD_UNDO)
            tracks[t].Undo();
        if (ev[t] & ceres::Footswitch::HOLD_CLEAR)
            tracks[t].Clear();
    }
}


// MIDI transport to the tracks: Stop pauses playing loops; once Start / Continue has put the
// clock on the song position, they resume there (Start: from the top)
void HandleTransport()
{
    uint8_t ev = midi_sync.TakeEvents();
    if (ev & ceres::MidiSync::EV_START)
        start_pending = true;
    if (ev & ceres::MidiSync::EV_STOP) {
        start_pending = false;
        for (auto& t : tracks)
            t.Pause();
    }
    if (ev & ceres::MidiSync::EV_LOCATED) {
        for (auto& t : tracks)
            t.Resume(start_pending, clock);
        start_pending = false;
    }
}


// LED for a track: record = bright, overdub = fast blink, play = dim, armed = blink on the beat,
// stop pending = blink dim; a dip on every beat (longer on bar 1). Empty: a faint beat flash.
float TrackLed(const ceres::Track& t, uint32_t blocks)
{
    uint32_t tick       = clock.TickInBeat();
    bool     bar_one    = clock.BeatInBar() == 0;
    bool     beat_flash = tick < (bar_one ? kBarFlashTicks : kBeatFlashTicks);
    switch (t.GetState()) {
        case ceres::Track::EMPTY: return beat_flash ? (bar_one ? 0.3f : 0.1f) : 0.0f;
        case ceres::Track::ARMED:
        case ceres::Track::ARMED_PLAY: return tick < Clock::kTicksPerBeat / 2 ? 1.0f : 0.0f;
        case ceres::Track::RECORDING: return beat_flash ? 0.4f : 1.0f;
        case ceres::Track::OVERDUBBING: return (blocks / 60) & 1 ? 1.0f : 0.2f;
        case ceres::Track::PLAYING:
            if (t.StopPending())
                return tick < Clock::kTicksPerBeat / 2 ? 0.35f : 0.0f;
            return beat_flash ? (bar_one ? 0.0f : 0.1f) : 0.35f;
        default: return 0.04f;  // Stopped
    }
}


static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    uint32_t block_time = System::GetTick();
    static uint32_t blocks;
    blocks++;
    cpu_meter.OnBlockStart();

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    ProcessMidi(block_time);  // Steers the clock before this block runs
    HandleTransport();
    UpdateTempo(tempo_knob.Process());

    ceres::Track::Settings st = ReadTrackSettings();
    HandleFootswitches(block_sample / clock_sample_rate, st);
    split_mode = hw.switches[Funbox::SWITCH_DIP_1].Pressed();

    // Knobs 1, 3, 4, 6, 5 and the pedal into the parameters (pickup, morph)
    float raw[ceres::Controls::kCount];
    float pedal = hw.GetExpression();
    raw[ceres::Controls::T1_LEVEL]  = level_knob[0].Process();
    raw[ceres::Controls::T2_LEVEL]  = level_knob[1].Process();
    raw[ceres::Controls::T1_FILTER] = filter_knob[0].Process();
    raw[ceres::Controls::T2_FILTER] = filter_knob[1].Process();
    raw[ceres::Controls::FX_AMOUNT] = fx_knob.Process();
    if (neutral_mix) {
        for (int k = 0; k < ceres::Controls::kCount; k++)
            raw[k] = pinned_raw[k];
        pedal = pinned_raw[5];
    }
    if (defaults_requested) {
        defaults_requested = false;
        controls.Init(nullptr, nullptr);
        delay_division = 1;
        preset_bars    = 0;
    }
    uint16_t cev = controls.Update(raw, pedal, hw.switches[Funbox::SWITCH_DIP_4].Pressed() || dip4_injected);
    // LED feedback: a short flash on the track a control just took over; both for learn A / B
    if (cev & (ceres::Controls::TAKEOVER_T1 | ceres::Controls::TAKEOVER_FX))
        led_flash_until[0] = blocks + 80;
    if (cev & (ceres::Controls::TAKEOVER_T2 | ceres::Controls::TAKEOVER_FX))
        led_flash_until[1] = blocks + 80;
    if (cev & (ceres::Controls::LEARN_A | ceres::Controls::LEARN_B))
        led_flash_until[0] = led_flash_until[1] = blocks + 400;
    if (cev & ceres::Controls::MORPH_CLEARED)
        led_flash_until[0] = led_flash_until[1] = blocks + 150;

    int   fx     = fx_override >= 0 ? (int)fx_override : ReadFxSwitch();
    float amount = amount_override >= 0.0f ? (float)amount_override : controls.Value(ceres::Controls::FX_AMOUNT);
    current_fx     = fx;
    current_amount = amount;

    float delay_time = (float)(kDelayTicks[delay_division] * clock.SamplesPerTickNominal());
    delay_fx[0].SetTime(delay_time);
    delay_fx[1].SetTime(delay_time);

    float level[2];
    for (int t = 0; t < 2; t++) {
        level[t] = ceres::LevelGain(controls.Value((ceres::Controls::Id)(ceres::Controls::T1_LEVEL + t)));
        filters[t].SetKnob(controls.Value((ceres::Controls::Id)(ceres::Controls::T1_FILTER + t)));
        tracks[t].BeginBlock(clock, st, midi_sync.Playing());
    }

    int channels = split_mode ? 2 : 1;  // Not split: one mix, on both outputs
    for (size_t i = 0; i < size; i++) {
        uint8_t events = clock.Process();
        if (events & Clock::BAR) {
            bar_to_print      = clock.Bar() + 1;
            bar_print_pending = true;
            for (int t = 0; t < 2; t++) {
                track_rms[t] = sqrtf(track_sum[t] / (track_count + 1));
                track_sum[t] = 0.0f;
            }
            track_count = 0;
        }
        uint32_t tick = clock.Tick();

        float dry[2] = {in[0][i], in[1][i]};
        if (test_tone) {
            for (int ch = 0; ch < 2; ch++) {
                tone_phase[ch] += (ch == 0 ? 220.0f : 330.0f) / clock_sample_rate;
                tone_phase[ch] -= tone_phase[ch] >= 1.0f ? 1.0f : 0.0f;
                dry[ch] = 0.316f * sinf(2.0f * 3.14159265f * tone_phase[ch]);
            }
        }

        // Split: In L -> Track 1, In R -> Track 2. Otherwise In L feeds both.
        float loop[2];
        for (int t = 0; t < 2; t++) {
            float y = tracks[t].Process(dry[split_mode ? t : 0], events, clock);
            y       = filters[t].Process(y) * level_slew[t].Process(level[t]);
            loop[t] = y;
            track_sum[t] += y * y;
        }
        track_count++;
        if (!split_mode)
            loop[0] += loop[1];

        // Loop FX on the loops only, one instance per output
        for (int ch = 0; ch < channels; ch++) {
            float x = loop[ch];
            if (stress) {
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
            out[ch][i] = limiters[ch].Process(dry[ch] + x);
        }
        if (!split_mode)
            out[1][i] = out[0][i];
    }

    // While learning (DIP 4 on) both LEDs breathe slowly over the track state
    float learn = controls.Learning() ? 0.5f + 0.5f * sinf(blocks * 0.004f) : 0.0f;
    led1.Set(blocks < led_flash_until[0] ? 1.0f : fmaxf(TrackLed(tracks[0], blocks), learn));
    led2.Set(blocks < led_flash_until[1] ? 1.0f : fmaxf(TrackLed(tracks[1], blocks), learn));
    led1.Update();
    led2.Update();

    block_sample += size;
    cpu_meter.OnBlockEnd();
}


static const char* kStateNames[] = {"empty", "armed", "RECORDING", "playing", "OVERDUB", "stopped", "armed-play"};

// One track for the status line, e.g. "T1 playing 4 bars undo -18 dB" or "T2 RECORDING 2+48/96"
void FormatTrack(char* out, size_t size, int n)
{
    const ceres::Track& t = tracks[n];
    uint32_t ticks = t.HasLoop() ? t.LengthTicks() : t.RecordedTicks();
    char len[24] = "";
    if (t.HasLoop() || t.GetState() == ceres::Track::RECORDING) {
        if (ticks % 96 == 0)
            snprintf(len, sizeof(len), " %lu bar%s", ticks / 96, ticks == 96 ? "" : "s");
        else
            snprintf(len, sizeof(len), " %lu+%lu/96", ticks / 96, ticks % 96);
    }
    char end[16] = "";
    if (t.GetState() == ceres::Track::RECORDING && t.EndTicks())
        snprintf(end, sizeof(end), " ->%lu", t.EndTicks() / 96);
    int db = track_rms[n] > 1e-5f ? (int)roundf(20.0f * log10f(track_rms[n])) : -99;
    snprintf(out, size, "T%d %s%s%s%s%s%s %ddB", n + 1, kStateNames[t.GetState()], len, end,
             t.StopPending() ? " stopping" : "", t.CanUndo() ? " undo" : (t.CanRedo() ? " redo" : ""),
             t.PausedByMidi() ? " (midi)" : "", db);
}


// Once per bar over USB serial (three lines, libDaisy's logger takes 128 characters per line), e.g.
//   bar 12  120.02 BPM  midi locked  playing  err +0.04 ms  trim +0.011%  jitter 0.82 ms  fit 192
//     T1 playing 4 bars undo -18dB | T2 empty -99dB | mono
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
    char t1[56], t2[56];
    FormatTrack(t1, sizeof(t1), 0);
    FormatTrack(t2, sizeof(t2), 1);
    hw.seed.PrintLine("  %s | %s | %s", t1, t2, split_mode ? "split" : "mono");
    // Parameters: value in %, owner (- restored, k knob, p pedal, c MIDI CC)
    static const char kOwner[] = {'-', 'k', 'p', 'c'};
    char params[96];
    int  n = 0;
    static const char* kNames[] = {"L1", "L2", "F1", "F2", "FX"};
    for (int i = 0; i < ceres::Controls::kCount; i++)
        n += snprintf(params + n, sizeof(params) - n, "%s %d%c ", kNames[i],
                      (int)roundf(controls.Value((ceres::Controls::Id)i) * 100.0f), kOwner[controls.GetOwner((ceres::Controls::Id)i)]);
    const ceres::Controls::Morph& mo = controls.GetMorph();
    hw.seed.PrintLine("  %smorph %s%02x%s  delay /%lu  preset %d  saves %lu%s", params, mo.valid ? "" : "off ",
                      mo.mask, controls.Learning() ? " LEARNING" : "", kDelayTicks[delay_division], (int)preset_bars, saves,
                      save_failures ? " (failures!)" : "");
    hw.seed.PrintLine("  fx %s%s %d%%  cpu %d/%d%%  midi clk %lu start %lu cont %lu stop %lu spp %lu cc %lu other %lu drop %lu uart %lu",
                      stress ? "STRESS " : "", kFxNames[current_fx], (int)(current_amount * 100.0f),
                      (int)(cpu_meter.GetAvgCpuLoad() * 100.0f), (int)(cpu_meter.GetMaxCpuLoad() * 100.0f),
                      midi_ticks, midi_starts, midi_continues, midi_stops, midi_spp, midi_cc, midi_other,
                      midi_dropped, uart_restarts);
    cpu_meter.Reset();  // Max per bar
}


// Snapshot slots: the top 16 KB of the 8 MB QSPI chip, far above the program
class QspiSnapshotFlash : public ceres::SnapshotFlash
{
  public:
    static const uint32_t kBase = 0x7FC000;

    const uint8_t* Read(uint32_t slot) override
    {
        uint8_t* p = (uint8_t*)hw.seed.qspi.GetData(kBase + slot * 4096);
        dsy_dma_invalidate_cache_for_buffer(p, 4096);
        return p;
    }

    bool Write(uint32_t slot, const uint8_t* data, uint32_t size) override
    {
        uint32_t addr = 0x90000000 + kBase + slot * 4096;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (hw.seed.qspi.Erase(addr, addr + 4096) == QSPIHandle::Result::OK
                && hw.seed.qspi.Write(addr, size, (uint8_t*)data) == QSPIHandle::Result::OK)
                return memcmp(Read(slot), data, size) == 0;
            updater.Unprotect(false);  // The chip may have been locked (see updater.h); once more
        }
        return false;
    }
};
QspiSnapshotFlash snapshot_flash;


// What would be saved right now
ceres::Snapshot CurrentSettings()
{
    ceres::Snapshot snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < ceres::Controls::kCount; i++)
        snap.values[i] = controls.Value((ceres::Controls::Id)i);
    snap.tempo          = clock.Tempo();
    snap.delay_division = delay_division;
    snap.preset_bars    = preset_bars;
    snap.morph          = controls.GetMorph();
    return snap;
}


// Main loop, every 100 ms: save 10 s after the last change (one write per change, however long
// a knob is swept). Not while learning: the morph isn't finished.
void SaveWhenSettled()
{
    static uint32_t last_check;
    uint32_t now = System::GetNow();
    if (now - last_check < 100)
        return;
    last_check = now;
    ceres::Snapshot snap = CurrentSettings();
    if (!snap.SameAs(pending_snap, 0.005f)) {
        pending_snap  = snap;
        pending_since = now;
        return;
    }
    if (snap.SameAs(saved_snap, 0.005f) || now - pending_since < 10000 || controls.Learning())
        return;
    if (store.Save(snap)) {
        saved_snap = snap;
        saves++;
        hw.seed.PrintLine("saved settings (#%lu)", snap.sequence);
    } else {
        saved_snap = snap;  // Don't retry every 100 ms
        save_failures++;
        hw.seed.PrintLine("saving settings FAILED");
    }
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
    level_knob[0].Init(hw.knob[Funbox::KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
    level_knob[1].Init(hw.knob[Funbox::KNOB_3], 0.0f, 1.0f, Parameter::LINEAR);
    filter_knob[0].Init(hw.knob[Funbox::KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
    filter_knob[1].Init(hw.knob[Funbox::KNOB_6], 0.0f, 1.0f, Parameter::LINEAR);

    led1.Init(hw.seed.GetPin(Funbox::LED_1), false);
    led2.Init(hw.seed.GetPin(Funbox::LED_2), false);

    float sample_rate = hw.AudioSampleRate();
    if (sample_rate == 48000.0f)
        sample_rate = kTrueSampleRate48k;
    clock.Init(sample_rate, 120.0f);
    clock_sample_rate = sample_rate;

    // Saved settings: values stay until a control takes over by pickup; nothing saved: defaults
    store.Init(&snapshot_flash);
    ceres::Snapshot restored;
    bool have_snapshot = store.Load(restored);
    if (have_snapshot) {
        controls.Init(restored.values, &restored.morph);
        clock.SetTempo(restored.tempo >= kMinBpm && restored.tempo <= kMaxBpm ? restored.tempo : 120.0f);
        tempo_restored = true;
        delay_division = restored.delay_division <= 4 ? restored.delay_division : 1;
        preset_bars    = restored.preset_bars;
        saved_snap     = restored;
    } else {
        controls.Init(nullptr, nullptr);
        memset(&saved_snap, 0, sizeof(saved_snap));
    }
    pending_snap = saved_snap;

    for (int t = 0; t < 2; t++) {
        tracks[t].Init(sample_rate, loop_buf[t][0], loop_buf[t][1], kLoopCapacity, side_buf[t][0], side_buf[t][1], kSideSize);
        filters[t].Init(sample_rate);
        limiters[t].Init(sample_rate);
        level_slew[t].Init(20.0f, sample_rate);
    }

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

    updater.Init(&hw.seed, update_buf);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Let the MIDI interrupt (priority 0) cut into the audio callback, so clock bytes get
    // timestamped when they arrive, not when the DSP happens to be done
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 1, 0);  // SAI1 A
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 1, 0);  // SAI1 B

    hw.seed.PrintLine("Ceres: two tracks, MIDI clock sync, loop FX");
    if (have_snapshot)
        hw.seed.PrintLine("restored settings #%lu (%d BPM)", restored.sequence, (int)restored.tempo);
    else
        hw.seed.PrintLine("no saved settings, defaults");

    while (1)
    {
        if (bootloader_requested)
            System::ResetToBootloader();  // STM32 DFU, no BOOT button needed
        if (daisy_bootloader_requested)  // Daisy bootloader, stays in DFU until something is flashed
            System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);

        // The UART stops listening after an error (e.g. a framing error from a loose cable)
        if (!midi_uart.RxActive()) {
            uart_restarts++;
            midi_uart.StartRx(MidiUartReceive, nullptr);
        }

        if (updater.Pending())
            updater.Run();
        if (reset_requested)
            NVIC_SystemReset();
        if (!updater.Pending())
            SaveWhenSettled();
        if (qspi_probe_requested) {
            qspi_probe_requested = false;
            updater.Unprotect(false);
        }

        if (bar_print_pending) {
            bar_print_pending = false;
            PrintStatus();
        }
    }
}
