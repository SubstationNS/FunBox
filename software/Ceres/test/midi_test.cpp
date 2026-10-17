// Host tests for midi_in.h (parser) and midi_sync.h (clock sync), no hardware needed:
//   c++ -std=c++17 -O2 -I.. midi_test.cpp -o midi_test && ./midi_test
//
// The sync scenarios simulate a master clock with worst-case USB-MIDI jitter, deliver the ticks
// to MidiSync in 48-sample blocks exactly like the pedal does, and measure where the pedal's clock
// is at the moment each master tick really happens.
#include "clock.h"
#include "midi_in.h"
#include "midi_sync.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

using ceres::MidiMsg;
using ceres::MidiParser;
using ceres::MidiSync;

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            failures++;                                      \
        }                                                    \
    } while (0)

static const double kSampleRate = 16000000.0 / 6.0 * 295.0 / 16.0 / 1024.0;  // 48014.3 Hz
static const int    kBlock      = 48;


//
// Parser
//

static std::vector<MidiMsg> Parse(std::vector<uint8_t> bytes)
{
    MidiParser p;
    p.Reset();
    std::vector<MidiMsg> out;
    MidiMsg m;
    for (size_t i = 0; i < bytes.size(); i++)
        if (p.Feed(bytes[i], (double)i, m))
            out.push_back(m);
    return out;
}

static void TestParser()
{
    std::printf("parser\n");

    // Clock in the middle of a CC: both come out, the CC intact
    auto m = Parse({0xB0, 0x07, 0xF8, 0x64});
    CHECK(m.size() == 2 && m[0].type == MidiMsg::CLOCK && m[1].type == MidiMsg::CC && m[1].data1 == 7
              && m[1].data2 == 100, "clock inside CC");
    CHECK(m.size() == 2 && m[0].time == 2.0 && m[1].time == 3.0, "arrival times");

    // Running status
    m = Parse({0xB3, 0x07, 0x64, 0x08, 0x32});
    CHECK(m.size() == 2 && m[1].type == MidiMsg::CC && m[1].channel == 3 && m[1].data1 == 8 && m[1].data2 == 50,
          "running status");

    // Song position with clock and start bytes inside
    m = Parse({0xF2, 0xF8, 0x10, 0xFA, 0x01});
    CHECK(m.size() == 3 && m[0].type == MidiMsg::CLOCK && m[1].type == MidiMsg::START
              && m[2].type == MidiMsg::SONG_POSITION && m[2].value() == 16 + 128,
          "song position with realtime inside");

    // SysEx is skipped, realtime inside it still comes through
    m = Parse({0xF0, 0x01, 0xF8, 0x02, 0x7F, 0xF7, 0x90, 0x3C, 0x40});
    CHECK(m.size() == 2 && m[0].type == MidiMsg::CLOCK && m[1].type == MidiMsg::NOTE_ON && m[1].data1 == 60,
          "sysex skipped");

    // Note on with velocity 0 = note off
    m = Parse({0x90, 0x3C, 0x00});
    CHECK(m.size() == 1 && m[0].type == MidiMsg::NOTE_OFF, "note on vel 0");

    // System common cancels running status
    m = Parse({0xB0, 0x07, 0x64, 0xF3, 0x05, 0x08, 0x32});
    CHECK(m.size() == 1, "song select cancels running status (%zu messages)", m.size());

    // One data byte messages with running status
    m = Parse({0xC0, 0x05, 0x06});
    CHECK(m.size() == 2 && m[1].type == MidiMsg::PROGRAM && m[1].data1 == 6, "program change running status");

    // Stray data bytes before any status, active sensing, transport
    m = Parse({0x10, 0x20, 0xFE, 0xFB, 0xFC});
    CHECK(m.size() == 2 && m[0].type == MidiMsg::CONTINUE && m[1].type == MidiMsg::STOP, "stray data / transport");
}


//
// Sync simulation
//

// Arrival jitter of a clock coming from a DAW through a USB-MIDI interface
struct Jitter
{
    double sigma_ms;      // Random scheduling jitter of the sender
    double frame_ms;      // USB frames: bytes leave the interface on frame boundaries (0 = DIN, no USB)
    double outlier_rate;  // Share of ticks that are held up badly (OS hiccup)
    double outlier_ms;    // ... by up to this much
};

static const Jitter kClean     = {0.0, 0.0, 0.0, 0.0};
static const Jitter kWorstCase = {1.0, 1.0, 0.02, 10.0};

struct MasterEvent
{
    double        time;   // True send time, samples
    MidiMsg::Type type;
    int64_t       index;  // CLOCK: song position of that tick (-1 = not playing), SPP: 16ths
    bool          sent;   // False during a simulated cable pull
};

// The master: a tempo curve, transport, and the ticks it sends
struct Master
{
    std::function<double(double)> bpm_at;  // BPM at a time in seconds
    double ppm = 100.0;  // Its crystal vs ours: it runs this much fast
    double t   = 0.0;    // Next tick, samples
    int64_t position = 0;
    bool    playing  = false;
    std::vector<MasterEvent> events;

    double Period() const
    {
        double bpm = bpm_at(t / kSampleRate) * (1.0 + ppm * 1e-6);
        return kSampleRate * 60.0 / (bpm * 24.0);
    }

    // Send ticks until `seconds`; `connected` = false simulates a pulled cable (nothing arrives)
    void Run(double seconds, bool connected = true)
    {
        while (t < seconds * kSampleRate) {
            events.push_back({t, MidiMsg::CLOCK, playing ? position : -1, connected});
            if (playing)
                position++;
            t += Period();
        }
    }

    // Transport messages go out just before the next tick
    void Send(MidiMsg::Type type, int64_t value = 0)
    {
        double when = t - 0.0005 * kSampleRate;
        if (type == MidiMsg::START) {
            playing  = true;
            position = 0;
        } else if (type == MidiMsg::CONTINUE) {
            playing = true;
        } else if (type == MidiMsg::STOP) {
            playing = false;
        } else if (type == MidiMsg::SONG_POSITION) {
            position = value * 6;
        }
        events.push_back({when, type, value, true});
    }

    // Skip ahead without sending ticks (a master that stops its clock while stopped)
    void Silence(double seconds) { t = seconds * kSampleRate; }
};

struct Result
{
    double lock_time   = -1.0;  // Seconds
    double err_mean_ms = 0.0, err_rms_ms = 0.0, err_max_ms = 0.0;  // After `settle`, vs the arrival bias
    double worst_ms    = 0.0;   // Largest |error| at any time after the first lock
    double max_trim    = 0.0;
    int    locates     = 0;
    int    measured    = 0;
    uint8_t events     = 0;
    float  final_tempo = 0.0f;
    double final_error_ms = 0.0;
    double mean_delay_ms  = 0.0;  // Mean arrival delay the jitter model added (unknowable latency)
    uint32_t tempo_changes = 0;    // Fit restarts
    double worst_at       = 0.0;  // Seconds
};

// Run a master's events through jitter, then MidiSync + Clock in 48-sample blocks.
// Errors are measured at every true tick time from `settle` seconds on. With position_known,
// the clock must be at exactly the master's song position; otherwise only the sub-tick phase counts.
static bool trace = false;  // Print once per beat-ish (set by the TRACE scenario name)

static Result Simulate(const Master& master, const Jitter& jitter, double knob_bpm, double settle,
                       bool position_known, unsigned seed = 1)
{
    std::mt19937                     rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    // Arrival times: sender jitter, USB frames, hiccups; bytes can't overtake each other and
    // each takes 320 us on the wire
    const double ms   = kSampleRate * 0.001;
    const double wire = 0.32 * ms;
    std::vector<double> arrival(master.events.size());
    double last = -1e9, delay_sum = 0.0;
    int    delay_n = 0;
    for (size_t i = 0; i < master.events.size(); i++) {
        double t = master.events[i].time;
        double a = t + std::fabs(gauss(rng)) * jitter.sigma_ms * ms;
        if (jitter.frame_ms > 0.0) {
            double frame = jitter.frame_ms * ms;
            a = std::ceil(a / frame) * frame + uni(rng) * wire;
        }
        if (uni(rng) < jitter.outlier_rate)
            a += (2.0 + uni(rng) * (jitter.outlier_ms - 2.0)) * ms;
        a = a < last + wire ? last + wire : a;
        last       = a;
        arrival[i] = a;
        if (master.events[i].type == MidiMsg::CLOCK) {
            delay_sum += a - t;
            delay_n++;
        }
    }

    Clock clock;
    clock.Init((float)kSampleRate, (float)knob_bpm);
    MidiSync sync;
    sync.Init((float)kSampleRate);

    Result r;
    r.mean_delay_ms = delay_n ? delay_sum / delay_n / ms : 0.0;
    double sum = 0.0, sum2 = 0.0;
    std::vector<double> errs;
    size_t next_arrival = 0, next_truth = 0;
    double end = master.t;
    bool   ever_locked = false;
    double last_transport = -1e9;  // Master time of the last transport message
    double next_trace = 0.0;

    for (double s = 0.0; s < end; s += kBlock) {
        // Everything that arrived before this block starts, like the audio callback sees it
        while (next_arrival < master.events.size() && arrival[next_arrival] < s) {
            const MasterEvent& e = master.events[next_arrival];
            double a = arrival[next_arrival];
            if (e.sent) {
                switch (e.type) {
                    case MidiMsg::CLOCK: sync.OnClock(a, clock, s); break;
                    case MidiMsg::START: sync.OnStart(); break;
                    case MidiMsg::CONTINUE: sync.OnContinue(); break;
                    case MidiMsg::STOP: sync.OnStop(clock); break;
                    case MidiMsg::SONG_POSITION: sync.OnSongPosition((uint16_t)e.index); break;
                    default: break;
                }
            }
            next_arrival++;
        }
        sync.Update(s, clock);
        uint8_t ev = sync.TakeEvents();
        r.events |= ev;
        if (ev & MidiSync::EV_LOCATED)
            r.locates++;
        if ((ev & MidiSync::EV_LOCKED) && r.lock_time < 0.0)
            r.lock_time = s / kSampleRate;
        ever_locked |= sync.Locked();
        double trim = std::fabs(clock.RateTrim());
        r.max_trim  = trim > r.max_trim ? trim : r.max_trim;

        // Where the clock is when each master tick really happens (true time, no jitter)
        double p0 = clock.Position(), inc = clock.Increment();
        while (next_truth < master.events.size() && master.events[next_truth].time < s + kBlock) {
            const MasterEvent& e = master.events[next_truth++];
            if (e.type != MidiMsg::CLOCK) {
                last_transport = e.time;
                continue;
            }
            // Before the pedal could know about a Start / Continue there's nothing to measure
            if (!ever_locked || e.time < s || e.time - last_transport < 0.05 * kSampleRate)
                continue;
            double p   = p0 + (e.time - s) * inc;
            double err;  // In ticks; positive = the pedal is ahead of the master
            if (position_known) {
                if (e.index < 0)
                    continue;  // Master stopped
                err = p - (double)e.index;
            } else {
                err = p - std::round(p);  // Unknown bar position: sub-tick phase only
            }
            double err_ms = err * master.Period() / ms;
            if (std::fabs(err_ms) > r.worst_ms) {
                r.worst_ms = std::fabs(err_ms);
                r.worst_at = e.time / kSampleRate;
            }
            r.final_error_ms = err_ms;
            if (trace && e.time >= next_trace) {
                next_trace = e.time + 0.5 * kSampleRate;
                std::printf("    t %6.2f  master %7.3f  pedal %7.3f  error %+6.2f ms  phase err %+.3f ticks  trim %+.3f%%  fit %3d\n",
                            e.time / kSampleRate, master.bpm_at(e.time / kSampleRate), clock.Tempo(), err_ms,
                            sync.PhaseErrorTicks(), clock.RateTrim() * 100.0, sync.FitTicks());
            }
            if (e.time >= settle * kSampleRate) {
                errs.push_back(err_ms);
                sum += err_ms;
                sum2 += err_ms * err_ms;
            }
        }

        for (int i = 0; i < kBlock; i++)
            clock.Process();
    }

    r.measured = (int)errs.size();
    if (r.measured) {
        r.err_mean_ms = sum / r.measured;
        r.err_rms_ms  = std::sqrt(sum2 / r.measured - r.err_mean_ms * r.err_mean_ms);
        for (double e : errs)
            r.err_max_ms = std::fabs(e - r.err_mean_ms) > r.err_max_ms ? std::fabs(e - r.err_mean_ms) : r.err_max_ms;
    }
    r.final_tempo   = clock.Tempo();
    r.tempo_changes = sync.TempoChanges();
    return r;
}

static void Print(const char* name, const Result& r)
{
    std::printf("  %-34s lock %5.2fs  error: bias %+5.2f ms, rms %.2f ms, max %.2f ms (worst %.2f at %.1fs)  trim %.2f%%  tempo %.3f  changes %u\n",
                name, r.lock_time, r.err_mean_ms, r.err_rms_ms, r.err_max_ms, r.worst_ms, r.worst_at,
                r.max_trim * 100.0, r.final_tempo, r.tempo_changes);
}

static Master Steady(double bpm)
{
    Master m;
    m.bpm_at = [bpm](double) { return bpm; };
    return m;
}

// Steady tempo, Start after a second, then just play
static void TestSteady(double bpm, const Jitter& jitter, const char* jitter_name, double seconds)
{
    Master m = Steady(bpm);
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(seconds);
    Result r = Simulate(m, jitter, 120.0, 5.0, true);
    char name[64];
    std::snprintf(name, sizeof(name), "%.2f BPM, %s, %.0f min", bpm, jitter_name, seconds / 60.0);
    Print(name, r);
    bool clean = jitter.sigma_ms == 0.0;
    double want_max = clean ? 0.05 : 1.5;  // ms around the bias
    CHECK(r.err_max_ms < want_max, "%s: max error %.2f ms", name, r.err_max_ms);
    CHECK(std::fabs(r.err_mean_ms + r.mean_delay_ms) < (clean ? 0.05 : 0.5),
          "%s: bias %.2f ms should be the arrival delay %.2f ms", name, r.err_mean_ms, -r.mean_delay_ms);
    CHECK(r.max_trim <= MidiSync::kMaxTrim + 1e-9, "%s: trim %.4f", name, r.max_trim);
    // Tempo within 0.05 % of the master's (including its crystal offset)
    double master_bpm = bpm * (1.0 + m.ppm * 1e-6);
    CHECK(std::fabs(r.final_tempo - master_bpm) / master_bpm < 0.0005, "%s: tempo %.4f vs %.4f", name,
          r.final_tempo, master_bpm);
}

// The master changes tempo; the pedal follows within a couple of beats
static void TestTempoStep()
{
    Master m;
    m.bpm_at = [](double s) { return s < 30.0 ? 120.0 : 126.0; };
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(60.0);
    Result r = Simulate(m, kWorstCase, 120.0, 34.0, true);
    Print("step 120 -> 126 BPM at 30 s", r);
    CHECK(r.err_max_ms < 1.5, "step: not settled 4 s after the change, max %.2f ms", r.err_max_ms);
    CHECK(r.worst_ms < 25.0, "step: worst error %.2f ms during the change", r.worst_ms);
}

static void TestTempoRamp()
{
    Master m;
    // 120 -> 140 BPM over 16 bars (~30 s), from 20 s on
    m.bpm_at = [](double s) { return s < 20.0 ? 120.0 : (s < 50.0 ? 120.0 + 20.0 * (s - 20.0) / 30.0 : 140.0); };
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(80.0);
    trace = getenv("TRACE_RAMP") != nullptr;
    Result r = Simulate(m, kWorstCase, 120.0, 55.0, true);
    trace = false;
    Print("ramp 120 -> 140 BPM over 30 s", r);
    CHECK(r.err_max_ms < 1.5, "ramp: not settled after the ramp, max %.2f ms", r.err_max_ms);
    CHECK(r.worst_ms < 10.0, "ramp: worst error %.2f ms during the ramp", r.worst_ms);
}

// Cable pulled for 5 s while the master plays on: nothing audible, bar position kept
static void TestDropout()
{
    Master m = Steady(124.0);
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(20.0);
    m.Run(25.0, false);
    m.Run(60.0);
    Result r = Simulate(m, kWorstCase, 120.0, 30.0, true);
    Print("cable pulled 5 s", r);
    CHECK((r.events & MidiSync::EV_LOST) && r.locates == 1, "dropout: events %02x, locates %d", r.events, r.locates);
    CHECK(r.worst_ms < 5.0, "dropout: bar position drifted, worst %.2f ms", r.worst_ms);
    CHECK(r.err_max_ms < 1.5, "dropout: not settled after the return, max %.2f ms", r.err_max_ms);
}

// DAW pause: Stop, relocate (SPP) to bar 9, Continue. Once with a master that keeps its clock
// running while stopped, once with one that stops it.
static void TestStopSppContinue(bool clock_while_stopped)
{
    Master m = Steady(132.0);
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(10.0);
    m.Send(MidiMsg::STOP);
    if (clock_while_stopped)
        m.Run(13.0);
    else
        m.Silence(13.0);
    m.Send(MidiMsg::SONG_POSITION, 8 * 16);  // Bar 9
    m.Send(MidiMsg::CONTINUE);
    m.Run(30.0);
    Result r = Simulate(m, kWorstCase, 120.0, 15.0, true);
    const char* name = clock_while_stopped ? "stop / SPP bar 9 / continue (clock on)" : "stop / SPP bar 9 / continue (clock off)";
    Print(name, r);
    CHECK(r.locates == 2, "%s: %d locates", name, r.locates);
    CHECK(r.err_max_ms < 1.5, "%s: max %.2f ms", name, r.err_max_ms);
}

// A master that restarts its clock on a new grid at Start (a third of a tick later)
static void TestStartRephase()
{
    Master m = Steady(120.0);
    m.Run(5.0);
    m.t += m.Period() / 3.0;
    m.Send(MidiMsg::START);
    m.Run(20.0);
    Result r = Simulate(m, kWorstCase, 120.0, 7.0, true);
    Print("start on a new clock grid (+1/3 tick)", r);
    CHECK(r.err_max_ms < 1.5, "rephase: max %.2f ms", r.err_max_ms);
}

// No Start, pedal at a different tempo: picks up tempo and sub-tick phase
static void TestColdAcquisition(double knob_bpm, double master_bpm)
{
    Master m = Steady(master_bpm);
    m.playing = true;  // Already playing when we plug in, no Start
    m.Run(30.0);
    Result r = Simulate(m, kWorstCase, knob_bpm, 10.0, false);
    char name[64];
    std::snprintf(name, sizeof(name), "cold, knob %.0f, master %.0f BPM", knob_bpm, master_bpm);
    Print(name, r);
    CHECK(r.lock_time >= 0.0 && r.lock_time < 0.5, "%s: lock time %.2f", name, r.lock_time);
    CHECK(r.err_max_ms < 1.5, "%s: max %.2f ms", name, r.err_max_ms);
}

// Start right as the clock appears, from a very different knob tempo: on bar 1 within a few ticks
static void TestStartWithoutTempo()
{
    Master m = Steady(95.0);
    m.Send(MidiMsg::START);
    m.Run(20.0);
    Result r = Simulate(m, kWorstCase, 150.0, 2.0, true);
    Print("start + clock together, knob 150, master 95", r);
    CHECK(r.err_max_ms < 1.5, "start without tempo: max %.2f ms", r.err_max_ms);
}

// Heavy bunching: ticks held up to 15 ms (more than a tick at 120 BPM) must not slip the count
static void TestHeavyBunching()
{
    Jitter brutal = {1.5, 1.0, 0.08, 15.0};
    Master m = Steady(140.0);
    m.Run(1.0);
    m.Send(MidiMsg::START);
    m.Run(300.0);
    Result r = Simulate(m, brutal, 120.0, 5.0, true, 7);
    Print("brutal jitter (8% held up to 15 ms)", r);
    CHECK(r.err_max_ms < 3.0, "brutal: max %.2f ms", r.err_max_ms);
}

int main()
{
    TestParser();

    std::printf("sync (error = where the pedal's clock is when the master's tick really happens;\n"
                "      bias = the average arrival delay, which no receiver can know)\n");
    TestSteady(120.0, kClean, "DIN clean", 120.0);
    for (double bpm : {60.0, 90.0, 120.0, 133.33, 200.0})
        TestSteady(bpm, kWorstCase, "worst USB", bpm == 120.0 ? 3600.0 : 300.0);
    TestTempoStep();
    TestTempoRamp();
    TestDropout();
    TestStopSppContinue(true);
    TestStopSppContinue(false);
    TestColdAcquisition(100.0, 128.0);
    TestColdAcquisition(180.0, 70.0);
    TestStartWithoutTempo();
    TestStartRephase();
    TestHeavyBunching();

    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
