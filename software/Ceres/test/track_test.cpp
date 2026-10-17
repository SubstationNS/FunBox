// Host tests for track.h and footswitch.h, no hardware needed:
//   c++ -std=c++17 -O2 -I.. track_test.cpp -o track_test && ./track_test
//
// A Track runs against the real Clock in 48-sample blocks like on the pedal. Inputs are sines,
// so what the loop plays back can be compared with what was played into it.
#include "clock.h"
#include "footswitch.h"
#include "track.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using ceres::Footswitch;
using ceres::Track;

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
static const double kPi         = 3.14159265358979;
static const double kBehind     = 2.0;  // Overdubs land this many samples behind the playhead

static double SineA(double n) { return 0.30 * std::sin(2.0 * kPi * 220.0 * n / kSampleRate); }
static double SineB(double n) { return 0.20 * std::sin(2.0 * kPi * 330.7 * n / kSampleRate); }
static double SineC(double n) { return 0.15 * std::sin(2.0 * kPi * 497.3 * n / kSampleRate); }

struct Rig
{
    Clock            clock;
    Track            track;
    Track::Settings  settings;
    std::vector<float> a, b, side_a, side_b, hold;
    uint64_t         n = 0;
    std::function<double(double)> input = [](double) { return 0.0; };
    bool             transport = true;
    int64_t          rec_start = -1;  // Sample where the recording started
    std::vector<float>  out;          // Every output sample
    std::vector<double> pos;          // Track position used for each sample
    double           bpm;

    explicit Rig(double bpm_ = 120.0, double capacity_seconds = 20.0) : bpm(bpm_)
    {
        uint32_t cap  = (uint32_t)(capacity_seconds * kSampleRate);
        uint32_t side = (uint32_t)(1.5 * kSampleRate);
        a.assign(cap, 0.0f);
        b.assign(cap, 0.0f);
        side_a.assign(side, 0.0f);
        side_b.assign(side, 0.0f);
        clock.Init((float)kSampleRate, (float)bpm);
        hold.assign((size_t)(8 * kSampleRate), 0.0f);
        track.Init((float)kSampleRate, a.data(), b.data(), cap, side_a.data(), side_b.data(), side, hold.data(), (uint32_t)hold.size());
    }

    ceres::LoopRenderer renderer;
    int  render_delay = 20;  // Blocks the "main loop" takes before it renders (it's busy elsewhere)
    int  render_wait  = 0;

    void Block()
    {
        // The main loop's part: re-render when asked (here: instantly, after a little while)
        if (track.Rendering() && ++render_wait > render_delay) {
            if (track.RunRender(renderer))
                render_wait = 0;
        }
        track.BeginBlock(clock, settings, transport);
        for (int i = 0; i < kBlock; i++) {
            uint8_t ev = clock.Process();
            pos.push_back(track.Position());
            Track::State before = track.GetState();
            float y = track.Process((float)input((double)n), ev, clock);
            if (before == Track::ARMED && track.GetState() == Track::RECORDING)
                rec_start = (int64_t)n;
            out.push_back(y);
            n++;
        }
    }

    void Run(double seconds)
    {
        uint64_t end = n + (uint64_t)(seconds * kSampleRate);
        while (n < end)
            Block();
    }

    // Run until the clock reaches a position (in bars, 0-based)
    void RunToBar(double bar)
    {
        while (clock.Position() < bar * Clock::kTicksPerBar)
            Block();
    }

    double SamplesPerBar() const { return kSampleRate * 60.0 / bpm * 4.0; }

    // RMS of (output - model) over [from, to) samples; model gets the sample index and the
    // track position (samples into the loop) of that sample
    double RmsError(uint64_t from, uint64_t to, std::function<double(uint64_t, double)> model) const
    {
        double sum = 0.0;
        for (uint64_t i = from; i < to; i++) {
            double e = out[i] - model(i, pos[i]);
            sum += e * e;
        }
        return std::sqrt(sum / (double)(to - from));
    }

    double Rms(uint64_t from, uint64_t to) const
    {
        return RmsError(from, to, [](uint64_t, double) { return 0.0; });
    }

    // Largest sample-to-sample step over [from, to): a click shows up as a big one
    double MaxStep(uint64_t from, uint64_t to) const
    {
        double m = 0.0;
        for (uint64_t i = from + 1; i < to; i++)
            m = std::fmax(m, std::fabs((double)out[i] - (double)out[i - 1]));
        return m;
    }

    // Record a loop: arm now, tap at `tap_bar` (clock bars), with input `in`
    void Record(std::function<double(double)> in, double tap_bar)
    {
        input = in;
        if (clock.Position() < 0.25 * Clock::kTicksPerBar)
            RunToBar(0.25);  // Arm during bar 1, not on its very first sample
        track.Tap(clock);
        RunToBar(tap_bar);
        track.Tap(clock);
    }

    // Recorded input A at loop position q
    double LoopA(double q) const { return SineA((double)rec_start + q); }
};

//
// Footswitch gestures
//

static void TestFootswitch()
{
    std::printf("footswitch\n");
    Footswitch fs;
    double t = 0.0;
    auto step = [&](bool down, double dt) {
        uint8_t e = 0;
        for (double end = t + dt; t < end; t += 0.001)
            e |= fs.Update(down, t);
        return e;
    };
    step(false, 0.1);
    CHECK(step(true, 0.05) == Footswitch::TAP, "tap on press");
    CHECK(step(false, 0.1) == 0, "release: nothing");
    CHECK(step(true, 0.05) == Footswitch::DOUBLE_TAP, "second press soon after: double tap");
    step(false, 0.1);
    CHECK(step(true, 0.05) == Footswitch::TAP, "third press: a new tap, not another double");
    step(false, 0.6);
    uint8_t e = step(true, 1.1);
    CHECK(e == (Footswitch::TAP | Footswitch::HOLD_UNDO), "hold 1 s: tap then undo (%02x)", e);
    e = step(true, 1.0);
    CHECK(e == Footswitch::HOLD_CLEAR, "hold 2 s: clear (%02x)", e);
    step(false, 0.1);
    CHECK(step(true, 0.05) == Footswitch::TAP, "press after a hold is a tap, not a double");
}

//
// Track
//

static void TestRecordAndPlay()
{
    std::printf("track: record / play\n");
    Rig r;
    r.Record(SineA, 1.5);  // Armed during bar 1 -> records bar 2 -> tap halfway through bar 2 -> ends at bar 3
    CHECK(r.rec_start >= 0, "recording started");
    double start_bar = (double)r.rec_start / r.SamplesPerBar();
    CHECK(std::fabs(start_bar - 1.0) < 0.001, "recording starts on bar line 2, got %.4f bars", start_bar);
    CHECK(r.track.GetState() == Track::RECORDING && r.track.EndTicks() == 96, "ends after one bar (end %u)", r.track.EndTicks());
    r.RunToBar(2.0 + 0.01);
    CHECK(r.track.GetState() == Track::PLAYING, "playing after the bar line");
    CHECK(r.track.LengthTicks() == 96, "one bar long (%u ticks)", r.track.LengthTicks());
    double len_bars = r.track.LengthSamples() / r.SamplesPerBar();
    CHECK(std::fabs(len_bars - 1.0) < 1.0 / r.SamplesPerBar() + 1e-9, "one bar of samples (%u)", r.track.LengthSamples());

    // Play it back for a few passes: it should be exactly what was played in (input now silent)
    r.input = [](double) { return 0.0; };
    uint64_t from = r.n + 1000;
    r.Run(8.0);
    double err = r.RmsError(from, r.n, [&](uint64_t, double q) {
        // Away from the seam (first 5 ms are crossfaded with the tail)
        return q < 300 ? r.out[0] * 0 + 0.0 : r.LoopA(q);
    });
    double err_noseam = 0.0;
    {
        double sum = 0.0;
        uint64_t cnt = 0;
        for (uint64_t i = from; i < r.n; i++) {
            if (r.pos[i] < 300)
                continue;
            double e = r.out[i] - r.LoopA(r.pos[i]);
            sum += e * e;
            cnt++;
        }
        err_noseam = std::sqrt(sum / cnt);
    }
    (void)err;
    CHECK(err_noseam < 1e-3, "playback = recording, rms error %.5f", err_noseam);

    // Lined up with the clock: loop position 0 on every bar line
    double off = 0.0;
    for (uint64_t i = from; i < r.n; i++)
        if (i > 0 && r.pos[i] < r.pos[i - 1]) {  // Wrap
            double bar = (double)i / r.SamplesPerBar();
            off        = std::fmax(off, std::fabs(bar - std::round(bar)) * r.SamplesPerBar());
        }
    CHECK(off < 2.0, "loop wraps on the bar lines (worst %.2f samples off)", off);

    // Seamless: the 220 Hz sine doesn't fit the loop length a whole number of times, so
    // without the seam crossfade there would be a step at every wrap
    double step = r.MaxStep(from, r.n);
    double natural = 0.30 * 2.0 * kPi * 220.0 / kSampleRate;
    CHECK(step < 2.0 * natural, "no click at the seam: biggest step %.4f (sine alone %.4f)", step, natural);
    std::printf("  1 bar loop: playback error %.5f rms, seam step %.4f (sine alone %.4f), bar alignment %.2f samples\n",
                err_noseam, step, natural, off);
}

static void TestEndings()
{
    std::printf("track: endings\n");
    struct Case { const char* name; double tap_bar; bool pow2; uint32_t expect_ticks; };
    const Case cases[] = {
        {"tap mid bar 3", 3.5, false, 3 * 96},          // Recording from bar 2: ends at bar 5 = 3 bars
        {"tap 40 ms late (snaps back)", 3.02, false, 2 * 96},
        {"tap 150 ms late", 3.075, false, 3 * 96},
        {"pow2: tap in bar 3 of the loop", 3.5, true, 4 * 96},
        {"pow2: tap in bar 5 of the loop", 5.5, true, 8 * 96},
        {"pow2: tap just after 4 bars", 5.02, true, 4 * 96},
    };
    for (const Case& c : cases) {
        Rig r;
        r.settings.power_of_two = c.pow2;
        r.Record(SineA, c.tap_bar);
        r.Run(17.0);
        CHECK(r.track.GetState() == Track::PLAYING && r.track.LengthTicks() == c.expect_ticks, "%s: %u ticks, expected %u",
              c.name, r.track.LengthTicks(), c.expect_ticks);
        std::printf("  %-34s -> %u bars%s\n", c.name, r.track.LengthTicks() / 96,
                    r.track.LengthTicks() % 96 ? " (+ part)" : "");
    }

    // Double tap: end on the next 1/8 instead
    {
        Rig r;
        r.settings.grid_ticks = 12;
        r.Record(SineA, 2.3);  // 1.3 bars into the recording (bar line at 1.0)
        r.Run(0.3);            // + 0.15 bar = 1.45 bars recorded = 139.2 ticks -> next 1/8 = 144
        r.track.DoubleTap(r.clock);
        r.Run(2.0);
        CHECK(r.track.LengthTicks() == 144, "double tap: %u ticks, expected 144 (1.5 bars)", r.track.LengthTicks());
        std::printf("  double tap, grid 1/8, 1.45 bars in    -> %u eighths\n", r.track.LengthTicks() / 12);
    }
    // The bar line came before the second tap: the loop stays as it ended, no stop
    {
        Rig r;
        r.Record(SineA, 2.95);
        r.RunToBar(3.04);
        r.track.DoubleTap(r.clock);
        r.Run(1.0);
        CHECK(r.track.GetState() == Track::PLAYING && r.track.LengthTicks() == 2 * 96,
              "late double tap: state %d, %u ticks", r.track.GetState(), r.track.LengthTicks());
    }
    // Armed tap again: cancel
    {
        Rig r;
        r.RunToBar(0.25);
        r.track.Tap(r.clock);
        r.track.Tap(r.clock);
        r.Run(3.0);
        CHECK(r.track.GetState() == Track::EMPTY, "tap while armed cancels");
    }
    // Out of memory: 5 s of room at 120 BPM -> ends at the last bar line, 2 bars
    {
        Rig r(120.0, 5.0);
        r.input = SineA;
        r.RunToBar(0.25);
        r.track.Tap(r.clock);
        r.Run(10.0);
        CHECK(r.track.GetState() == Track::PLAYING && r.track.LengthTicks() == 2 * 96,
              "memory full: state %d, %u ticks", r.track.GetState(), r.track.LengthTicks());
    }
    // Waits for the MIDI transport
    {
        Rig r;
        r.transport = false;
        r.RunToBar(0.25);
        r.track.Tap(r.clock);
        r.Run(5.0);
        CHECK(r.track.GetState() == Track::ARMED, "armed waits while the MIDI transport is stopped");
        r.transport = true;
        r.Run(2.1);
        CHECK(r.track.GetState() == Track::RECORDING, "and records once it plays");
    }
}

// Amplitude of a sine at hz in the output over [from, to): measured in short windows and averaged,
// so it doesn't matter that the loop's passes are at different phases against the clock
static double Amplitude(const Rig& r, uint64_t from, uint64_t to, double hz)
{
    const uint64_t kWin = 4096;
    double sum = 0;
    int    n   = 0;
    for (uint64_t w = from; w + kWin <= to; w += kWin) {
        double c = 0, s2 = 0;
        for (uint64_t i = w; i < w + kWin; i++) {
            double ph = 2.0 * kPi * hz * i / kSampleRate;
            c += r.out[i] * std::cos(ph);
            s2 += r.out[i] * std::sin(ph);
        }
        sum += 2.0 * std::sqrt(c * c + s2 * s2) / kWin;
        n++;
    }
    return n ? sum / n : 0.0;
}

// One-bar loop of A, then overdubs
static Rig* LoopOfA(double bpm = 120.0)
{
    Rig* r = new Rig(bpm);
    r->Record(SineA, 1.5);
    r->RunToBar(2.2);
    r->input = [](double) { return 0.0; };
    return r;
}

// Error vs a model over the next `bars`, skipping the seam and fades around punch points
static double CheckModel(Rig& r, double bars, std::function<double(uint64_t, double)> model)
{
    uint64_t from = r.n;
    r.Run(bars * r.SamplesPerBar() / kSampleRate);
    double   sum = 0.0;
    uint64_t cnt = 0;
    for (uint64_t i = from; i < r.n; i++) {
        double q = r.pos[i];
        if (q < 300 || q > r.track.LengthSamples() - 300)
            continue;
        double e = r.out[i] - model(i, q);
        sum += e * e;
        cnt++;
    }
    return std::sqrt(sum / cnt);
}

static void TestOverdubUndo()
{
    std::printf("track: overdub / undo / redo\n");
    Rig& r = *LoopOfA();
    uint32_t L = r.track.LengthSamples();

    // Overdub B for exactly one pass (from bar line to bar line)
    r.RunToBar(3.0);
    r.input = SineB;
    r.track.Tap(r.clock);
    uint64_t dub_start = r.n;
    r.RunToBar(4.0);
    r.track.Tap(r.clock);
    r.input = [](double) { return 0.0; };
    CHECK(r.track.GetState() == Track::PLAYING, "dub ended");
    // B was written at loop position q at the time the playhead was there
    double q0       = r.pos[dub_start];
    auto   a_plus_b = [&](uint64_t, double q) { return r.LoopA(q) + SineB((double)dub_start + q + kBehind - q0); };
    double e1 = CheckModel(r, 2.0, a_plus_b);
    CHECK(e1 < 0.01, "after overdub: A + B, error %.4f", e1);
    CHECK(r.track.CanUndo(), "undo available");

    r.track.Undo();
    r.Run(0.05);
    double e2 = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e2 < 0.01, "undo: back to A, error %.4f", e2);
    CHECK(r.track.CanRedo(), "redo available");

    r.track.Undo();  // Hold again = redo
    r.Run(0.05);
    double e3 = CheckModel(r, 2.0, a_plus_b);
    CHECK(e3 < 0.01, "redo: A + B again, error %.4f", e3);
    std::printf("  overdub %.4f, undo %.4f, redo %.4f (rms error vs. expected)\n", e1, e2, e3);

    // Hold to undo while playing: the press starts an overdub (C), the hold cancels it and undoes B
    r.input = SineC;
    r.track.Tap(r.clock);
    r.Run(1.0);
    r.track.Undo();
    r.input = [](double) { return 0.0; };
    r.Run(0.1);
    double e4 = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e4 < 0.01 && r.track.GetState() == Track::PLAYING, "hold while playing: C cancelled, B undone -> A, error %.4f", e4);
    std::printf("  hold-undo while playing (cancels the overdub the press started): %.4f\n", e4);
    (void)L;
    delete &r;
}

// The device sequence: overdub one pass with the input still playing, play a while, then hold
// (the press starts a new overdub, the hold cancels it and undoes the first one)
static void TestHoldUndoAfterPlaying()
{
    std::printf("track: hold-undo a while after an overdub\n");
    Rig r;
    r.input = SineA;  // Input on the whole time
    r.Record(SineA, 2.5);
    r.RunToBar(7.0);
    r.track.Tap(r.clock);
    r.RunToBar(9.0);
    r.track.Tap(r.clock);
    r.RunToBar(11.0);
    r.track.Tap(r.clock);  // The hold's press...
    r.Run(1.0);
    r.track.Undo();        // ...1 s later
    r.input = [](double) { return 0.0; };
    r.Run(0.1);
    double e = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e < 0.01, "back to the first take, error %.4f", e);
    std::printf("  overdub, 2 bars later hold-undo -> first take: %.4f\n", e);
}

static void TestDoubleTapCancel()
{
    std::printf("track: double tap while playing\n");
    Rig& r = *LoopOfA();
    r.RunToBar(3.3);
    r.input = SineB;
    r.track.Tap(r.clock);  // Starts an overdub...
    r.Run(0.25);
    r.track.DoubleTap(r.clock);  // ...which the double tap takes back, then stops
    r.input = [](double) { return 0.0; };
    r.Run(0.01);
    CHECK(r.track.GetState() == Track::STOPPED, "stopped (state %d)", r.track.GetState());
    uint64_t from = r.n;
    r.Run(1.0);
    CHECK(r.Rms(from, r.n) < 1e-6, "silent when stopped");
    r.track.Tap(r.clock);  // Play again from the top at the next bar
    r.RunToBar(std::ceil(r.clock.Position() / 96.0) + 0.05);
    CHECK(r.track.GetState() == Track::PLAYING, "plays again from the next bar");
    double e = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e < 0.01, "the cancelled overdub left nothing behind, error %.4f", e);
    std::printf("  overdub cancelled, loop untouched: %.4f\n", e);
    delete &r;
}

static void TestBackToBackOverdubs()
{
    std::printf("track: overdubs back to back\n");
    Rig& r = *LoopOfA();
    r.RunToBar(3.1);
    // B for 0.5 s, gap 0.1 s, C for 0.5 s: B's undo copy is still running when C starts
    r.input = SineB;
    r.track.Tap(r.clock);
    uint64_t b0 = r.n;
    r.Run(0.5);
    r.track.Tap(r.clock);
    uint64_t b1 = r.n;
    r.input = [](double) { return 0.0; };
    r.Run(0.1);
    r.input = SineC;
    r.track.Tap(r.clock);
    uint64_t c0 = r.n;
    r.Run(0.5);
    r.track.Tap(r.clock);
    uint64_t c1 = r.n;
    r.input = [](double) { return 0.0; };
    r.Run(2.0);

    uint32_t L = r.track.LengthSamples();
    double   s0 = (double)r.rec_start;
    (void)s0;
    // Where (in loop positions) each overdub went: recorded from b0 at position pos[b0]
    double qb = r.pos[b0], qc = r.pos[c0];
    auto layer = [&](double q, double q0, uint64_t t0, uint64_t t1, double (*f)(double)) {
        double d = std::fmod(q + kBehind - q0 + L, (double)L);
        return d < (double)(t1 - t0) ? f((double)t0 + d) : 0.0;
    };
    auto all = [&](uint64_t, double q) { return r.LoopA(q) + layer(q, qb, b0, b1, SineB) + layer(q, qc, c0, c1, SineC); };
    auto no_c = [&](uint64_t, double q) { return r.LoopA(q) + layer(q, qb, b0, b1, SineB); };
    double e1 = CheckModel(r, 2.0, all);
    r.track.Undo();
    r.Run(0.05);
    double e2 = CheckModel(r, 2.0, no_c);
    CHECK(e1 < 0.02, "A + B + C, error %.4f", e1);
    CHECK(e2 < 0.02, "undo takes back C only, error %.4f", e2);
    std::printf("  A+B+C %.4f, undo -> A+B %.4f\n", e1, e2);
    delete &r;
}

// Long overdub across the seam for several passes, then an undo right away (while the undo
// copy may still be running: it happens once the copy is done)
static void TestLongOverdub()
{
    std::printf("track: long overdub, quick undo\n");
    Rig& r = *LoopOfA();
    r.RunToBar(3.6);
    r.input = SineB;
    r.track.Tap(r.clock);
    uint64_t from = r.n;
    r.Run(4.0 * r.SamplesPerBar() / kSampleRate);  // 4 passes
    r.track.Tap(r.clock);
    r.input = [](double) { return 0.0; };
    double natural = (0.30 * 220.0 + 0.20 * 330.7) * 2.0 * kPi / kSampleRate;
    double step    = r.MaxStep(from, r.n);
    CHECK(step < 2.0 * natural, "no clicks overdubbing across the seam: %.4f", step);
    // Four layers of B, each one loop length later than the last
    double qd  = r.pos[from];
    uint32_t L = r.track.LengthSamples();
    double e0 = CheckModel(r, 1.0, [&](uint64_t, double q) {
        double d = std::fmod(q + kBehind - qd + 2.0 * L, (double)L), sum = r.LoopA(q);
        for (int k = 0; k < 4; k++)
            sum += SineB((double)from + d + (double)k * L);
        return sum;
    });
    CHECK(e0 < 0.02, "four layers of B stacked up: error %.4f", e0);
    r.track.Undo();  // Immediately
    r.Run(0.5);
    double e = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e < 0.01, "quick undo takes back the whole overdub: error %.4f", e);
    std::printf("  4 passes across the seam: step %.4f (signal %.4f); quick undo -> A: %.4f\n", step, natural, e);
    delete &r;
}

// With a 200-sample round trip: the first take starts 100 samples (the input half) after the
// bar line, overdubs land 200 (+2) behind the playhead
static void TestLatency()
{
    std::printf("track: latency compensation\n");
    const uint32_t kRt = 200;
    Rig r;
    r.settings.latency = kRt;
    r.Block();  // Settings reach the track
    r.Record(SineA, 1.5);
    r.RunToBar(2.2);
    r.input = [](double) { return 0.0; };
    double e1 = CheckModel(r, 2.0, [&](uint64_t, double q) { return SineA((double)r.rec_start + kRt / 2 + q); });
    CHECK(e1 < 0.01, "first take shifted by the input half: error %.4f", e1);

    r.RunToBar(4.0);
    r.input = SineB;
    r.track.Tap(r.clock);
    uint64_t dub_start = r.n;
    r.RunToBar(4.6);
    r.track.Tap(r.clock);
    uint64_t dub_end = r.n;
    r.input = [](double) { return 0.0; };
    double q0 = r.pos[dub_start];
    uint32_t L = r.track.LengthSamples();
    double e2 = CheckModel(r, 2.0, [&](uint64_t, double q) {
        double d = std::fmod(q + kBehind + kRt - q0 + L, (double)L);
        double b = d < (double)(dub_end - dub_start) - 300 && d > 300 ? SineB((double)dub_start + d) : 0.0;
        return SineA((double)r.rec_start + kRt / 2 + q) + b;
    });
    CHECK(e2 < 0.02, "overdub a round trip behind the playhead: error %.4f", e2);
    std::printf("  round trip %u: first take %.4f, overdub %.4f (rms error vs. expected)\n", kRt, e1, e2);
}

static void TestStopModes()
{
    std::printf("track: stop modes\n");
    // Stop now: silent within ~5 ms
    {
        Rig& r = *LoopOfA();
        r.RunToBar(3.4);
        r.settings.stop_mode = Track::STOP_NOW;
        r.track.Tap(r.clock);
        r.track.DoubleTap(r.clock);
        uint64_t t = r.n;
        r.Run(0.5);
        CHECK(r.Rms(t + (uint64_t)(0.006 * kSampleRate), r.n) < 1e-6, "stop now: silent after 6 ms");
        delete &r;
    }
    // End of bar: plays on to the bar line
    {
        Rig& r = *LoopOfA();
        r.RunToBar(3.4);
        r.settings.stop_mode = Track::STOP_END_OF_BAR;
        r.Block();  // Settings reach the track with the next block, as on the pedal
        r.track.Tap(r.clock);
        r.track.DoubleTap(r.clock);
        uint64_t req = r.n;
        r.RunToBar(4.0);
        uint64_t bar = r.n;
        r.Run(0.5);
        CHECK(r.Rms(req + 500, bar - 500) > 0.1, "end of bar: still playing before the bar line");
        CHECK(r.Rms(bar + (uint64_t)(0.006 * kSampleRate), r.n) < 1e-6, "end of bar: silent after it");
        delete &r;
    }
    // End of loop, on a 2-bar loop: plays to the end of the loop, not just the bar
    {
        Rig r;
        r.Record(SineA, 2.5);
        r.RunToBar(3.2);  // Loop from bar 1 (0-based) to 3, now 0.2 into its first bar
        r.input = [](double) { return 0.0; };
        r.settings.stop_mode = Track::STOP_END_OF_LOOP;
        r.Block();
        r.track.Tap(r.clock);
        r.track.DoubleTap(r.clock);
        r.RunToBar(4.5);
        CHECK(r.track.GetState() == Track::PLAYING && r.track.StopPending(), "end of loop: past the bar, still playing");
        r.RunToBar(5.1);
        CHECK(r.track.GetState() == Track::STOPPED, "end of loop: stopped at the loop end");
    }
}

// The clock is nudged (as MIDI sync does): the loop follows without drifting or clicking
static void TestFollowsClock()
{
    std::printf("track: follows the clock\n");
    Rig& r = *LoopOfA();
    double worst = 0.0;
    uint64_t from = r.n;
    for (int k = 0; k < 120; k++) {  // 2 minutes, trim wandering +-0.15 %
        r.clock.SetRateTrim(0.0015f * (float)std::sin(k * 0.3));
        r.Run(1.0);
        // Where the clock says the loop should be vs where it is
        double expected = std::fmod(r.clock.Position() - (double)r.rec_start / r.SamplesPerBar() * 96.0, 96.0) / 96.0
                          * r.track.LengthSamples();
        double err = std::fabs(expected - r.track.Position());
        err        = std::fmin(err, r.track.LengthSamples() - err);
        worst      = std::fmax(worst, err);
    }
    double natural = 0.30 * 2.0 * kPi * 220.0 / kSampleRate * 1.003;
    double step    = r.MaxStep(from, r.n);
    CHECK(worst < 0.002 * kSampleRate, "loop stays with the clock: worst %.1f samples", worst);
    CHECK(step < 2.0 * natural, "no clicks while following: step %.4f", step);
    std::printf("  2 min of +-0.15 %% trim: worst %.2f ms off the clock, biggest step %.4f\n", worst / kSampleRate * 1000, step);
    delete &r;
}

// Different tempo: the loop is time-stretched, pitch stays, still on the clock's bar lines
static void TestOtherTempo()
{
    std::printf("track: other tempo (time stretch)\n");
    Rig& r = *LoopOfA();
    r.clock.SetTempo(132.0f);  // 10 % faster
    r.Run(0.5);
    CHECK(r.track.Stretching(), "stretching at a 10 %% faster tempo");
    uint64_t from = r.n;
    r.Run(6.0);
    double a220 = Amplitude(r, from, r.n, 220.0);
    CHECK(std::fabs(a220 - 0.30) < 0.03, "pitch unchanged: the 220 Hz loop is still 220 Hz (amplitude %.3f)", a220);
    double natural = 0.30 * 2.0 * kPi * 220.0 / kSampleRate;
    double step    = r.MaxStep(from, r.n);
    CHECK(step < 1.5 * natural, "no clicks while stretched: %.4f", step);
    // On the bar lines: the loop's start comes around on every bar (it's a 1-bar loop)
    double worst = 0;
    double spb   = kSampleRate * 60.0 / 132.0 * 4.0;
    for (uint64_t i = from + 1; i < r.n; i++)
        if (r.pos[i] < r.pos[i - 1]) {
            double bars = (i - r.rec_start * 0.0) / spb;
            (void)bars;
        }
    double off = std::fmod(r.clock.Position() - (double)r.rec_start / r.SamplesPerBar() * 96.0, 96.0) / 96.0 * r.track.LengthSamples()
                 - r.track.Position();
    worst = std::fabs(off);
    CHECK(worst < 2.0, "position follows the clock (%.1f samples off)", worst);
    r.clock.SetTempo(120.0f);
    r.Run(1.0);
    CHECK(r.track.Following(), "plays directly again back at 120");
    uint64_t back = r.n;
    r.Run(2.0);
    double e = CheckModel(r, 2.0, [&](uint64_t, double q) { return r.LoopA(q); });
    CHECK(e < 0.01, "and exactly as recorded, error %.4f", e);
    CHECK(r.MaxStep(back - 2000, r.n) < 1.5 * natural, "no click switching back");
    std::printf("  132 BPM: 220 Hz amplitude %.3f (0.30 recorded), steps %.4f; back at 120: error %.4f\n", a220, step, e);
    delete &r;
}

// Overdub while stretched: the loop is re-rendered at the new tempo, the overdub goes into it;
// undo brings back the original at its own tempo
static void TestStretchedOverdub()
{
    std::printf("track: overdub at another tempo\n");
    Rig& r = *LoopOfA();
    r.Run(0.1);
    uint32_t len120 = r.track.LengthSamples();
    r.clock.SetTempo(100.0f);
    r.RunToBar(std::ceil(r.clock.Position() / 96.0) + 0.02);
    r.input = SineB;
    r.track.Tap(r.clock);
    CHECK(r.track.Rendering(), "overdub held while the loop gets re-rendered");
    r.RunToBar(std::ceil(r.clock.Position() / 96.0) + 0.02);  // One pass of the 1-bar loop
    r.track.Tap(r.clock);
    r.input = [](double) { return 0.0; };
    r.Run(1.0);
    CHECK(!r.track.Rendering() && r.track.Renders() == 1, "rendered (%u)", r.track.Renders());
    uint32_t len100 = r.track.LengthSamples();
    double   want   = len120 * 120.0 / 100.0;
    CHECK(std::fabs(len100 - want) < 2.0, "new length for 100 BPM: %u (want %.0f)", len100, want);
    CHECK(!r.track.Stretching(), "the new loop plays directly at 100");
    uint64_t from = r.n;
    r.Run(4.0);
    double a220 = Amplitude(r, from, r.n, 220.0), a330 = Amplitude(r, from, r.n, 330.7);
    std::printf("  100 BPM: 220 Hz (re-rendered loop) %.3f, 330.7 Hz (overdub) %.3f\n", a220, a330);
    CHECK(std::fabs(a220 - 0.30) < 0.04, "re-rendered loop at its level: %.3f", a220);
    CHECK(std::fabs(a330 - 0.20) < 0.02, "the overdub is in, at its level: %.3f", a330);

    r.track.Undo();
    r.Run(0.5);
    CHECK(r.track.LengthSamples() == len120 && r.track.Stretching(), "undo: the original, stretched again (len %u)",
          r.track.LengthSamples());
    from = r.n;
    r.Run(3.0);
    a220 = Amplitude(r, from, r.n, 220.0);
    a330 = Amplitude(r, from, r.n, 330.7);
    CHECK(std::fabs(a220 - 0.30) < 0.04 && a330 < 0.02, "undo: just the original (220 %.3f, 330 %.3f)", a220, a330);
    r.track.Undo();
    r.Run(0.5);
    CHECK(r.track.LengthSamples() == len100 && !r.track.Stretching(), "redo: the re-rendered one again");
    std::printf("  undo -> original %u samples (stretched), redo -> %u samples\n", len120, len100);
    delete &r;
}

// Double tap / hold within the cancel window: nothing is rendered, the previous undo stays
static void TestStretchedCancel()
{
    std::printf("track: cancelling an overdub at another tempo\n");
    Rig& r = *LoopOfA();
    r.RunToBar(3.0);  // An overdub at 120 first, so there's something to undo
    r.input = SineB;
    r.track.Tap(r.clock);
    r.RunToBar(4.0);
    r.track.Tap(r.clock);
    r.input = [](double) { return 0.0; };
    r.Run(2.0);
    CHECK(r.track.CanUndo(), "undo point from the 120 overdub");
    r.clock.SetTempo(110.0f);
    r.Run(0.5);
    r.input = SineC;
    r.track.Tap(r.clock);
    r.Run(0.2);
    r.track.DoubleTap(r.clock);  // Take it back, stop
    r.Run(2.0);
    CHECK(r.track.Renders() == 0 && r.track.CanUndo(), "double tap: nothing rendered, undo point kept");
    r.track.Tap(r.clock);  // Play again
    r.RunToBar(std::ceil(r.clock.Position() / 96.0) + 0.1);
    r.Run(0.5);
    // Hold while stretched: the press's overdub is taken back, then the 120 overdub is undone
    r.track.Tap(r.clock);
    r.Run(1.0);
    r.track.Undo();
    r.input = [](double) { return 0.0; };
    r.Run(0.5);
    CHECK(r.track.Renders() == 0 && r.track.CanRedo(), "hold: no render, the earlier overdub undone (redo available)");
    uint64_t from = r.n;
    r.Run(3.0);
    double a330 = Amplitude(r, from, r.n, 330.7), a497 = Amplitude(r, from, r.n, 497.3);
    CHECK(a330 < 0.02 && a497 < 0.02, "left with just the first take (330 %.3f, 497 %.3f)", a330, a497);
    delete &r;
}

// MIDI transport: Stop pauses, Continue resumes at the song position, Start starts over
static void TestTransport()
{
    std::printf("track: MIDI transport\n");
    Rig r;
    r.Record(SineA, 2.5);  // 2-bar loop from bar 1
    r.RunToBar(4.3);
    r.input = [](double) { return 0.0; };
    r.track.Pause();
    r.transport = false;
    uint64_t t = r.n;
    r.Run(2.0);
    CHECK(r.Rms(t + 300, r.n) < 1e-6, "paused: silent");
    // Continue at bar 8 (0-based 7): loop bar = (7 - 1) % 2 = 0 -> loop position = start
    r.clock.Locate(7 * 96.0);
    r.transport = true;
    r.track.Resume(false, r.clock);
    r.Block();
    CHECK(r.track.GetState() == Track::PLAYING, "resumed");
    CHECK(r.track.Position() < 100, "at the song position (loop pos %.0f)", r.track.Position());
    // Start while playing: crossfades back to the top
    r.RunToBar(8.5);
    r.clock.Locate(0.0);
    r.track.Resume(true, r.clock);
    r.Run(0.02);
    CHECK(r.track.Position() < 0.025 * kSampleRate, "Start: back at the top (pos %.0f)", r.track.Position());
    // A track you stopped yourself stays stopped
    r.track.RequestStop(Track::STOP_NOW);
    r.Run(0.1);
    r.track.Pause();
    r.track.Resume(true, r.clock);
    r.Run(0.1);
    CHECK(r.track.GetState() == Track::STOPPED, "footswitch-stopped track doesn't auto-resume");
}

int main()
{
    TestFootswitch();
    TestRecordAndPlay();
    TestEndings();
    TestOverdubUndo();
    TestHoldUndoAfterPlaying();
    TestDoubleTapCancel();
    TestBackToBackOverdubs();
    TestLongOverdub();
    TestLatency();
    TestStopModes();
    TestFollowsClock();
    TestOtherTempo();
    TestStretchedOverdub();
    TestStretchedCancel();
    TestTransport();
    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
