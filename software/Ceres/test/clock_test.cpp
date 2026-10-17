// Host-side test for clock.h (no hardware needed):
//   c++ -std=c++17 -O2 -I.. clock_test.cpp -o clock_test && ./clock_test
#include "clock.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);            \
            std::printf("\n");                   \
            failures++;                          \
        }                                        \
    } while (0)

static const float kSampleRate = 48000.0f;

// Beats land on the expected sample, every time, at tempos that divide evenly and ones that don't
static void TestBeatPositions(float bpm, uint64_t seconds)
{
    Clock clock;
    clock.Init(kSampleRate, bpm);

    const double samples_per_beat = 60.0 / bpm * kSampleRate;
    const uint64_t total = (uint64_t)(seconds * kSampleRate);
    uint64_t beats = 0, bars = 0;
    double worst_error = 0.0;

    for (uint64_t n = 0; n < total; n++) {
        uint8_t e = clock.Process();
        if (e & Clock::BEAT) {
            double expected = beats * samples_per_beat;
            double error    = std::fabs((double)n - expected);
            if (error > worst_error)
                worst_error = error;
            if ((beats % Clock::kBeatsPerBar == 0) != bool(e & Clock::BAR))
                CHECK(false, "%.1f BPM: BAR flag wrong on beat %llu", bpm, (unsigned long long)beats);
            beats++;
        }
        if (e & Clock::BAR)
            bars++;
    }

    uint64_t expected_beats = (uint64_t)std::floor((total - 1) / samples_per_beat) + 1;
    CHECK(beats == expected_beats, "%.1f BPM: %llu beats, expected %llu", bpm,
          (unsigned long long)beats, (unsigned long long)expected_beats);
    // A beat may land up to one sample late (it's reported on the sample that crosses it)
    CHECK(worst_error <= 1.0, "%.1f BPM: beat off by %.3f samples after %llus", bpm, worst_error,
          (unsigned long long)seconds);
    std::printf("  %6.1f BPM, %5llus: %llu beats, %llu bars, worst beat error %.3f samples\n", bpm,
                (unsigned long long)seconds, (unsigned long long)beats, (unsigned long long)bars, worst_error);
}

// Changing tempo mid-beat keeps the position continuous (no jump, no lost or doubled tick)
static void TestTempoChangeIsContinuous()
{
    Clock clock;
    clock.Init(kSampleRate, 120.0f);
    uint32_t ticks = 0;
    for (int n = 0; n < 30000; n++)  // 1.25 beats at 120
        if (clock.Process() & Clock::TICK)
            ticks++;
    uint32_t before = clock.Tick();
    clock.SetTempo(90.0f);
    for (int n = 0; n < 48000; n++)  // one second at 90 BPM = 36 ticks
        if (clock.Process() & Clock::TICK)
            ticks++;
    CHECK(ticks == clock.Tick() + 1, "tick events %u vs counter %u", ticks, clock.Tick() + 1);
    uint32_t gained = clock.Tick() - before;
    CHECK(gained == 36 || gained == 35, "gained %u ticks in 1 s at 90 BPM, expected ~36", gained);
    std::printf("  tempo change 120 -> 90: continuous, %u ticks in 1 s\n", gained);
}

// Reset() reports bar 1 on the very next sample
static void TestReset()
{
    Clock clock;
    clock.Init(kSampleRate, 133.0f);
    for (int n = 0; n < 12345; n++)
        clock.Process();
    clock.Reset();
    uint8_t e = clock.Process();
    CHECK(e == (Clock::TICK | Clock::BEAT | Clock::BAR), "after Reset got events %u", e);
    CHECK(clock.Tick() == 0 && clock.Bar() == 0, "after Reset tick %u bar %u", clock.Tick(), clock.Bar());
    std::printf("  reset: bar 1 on the next sample\n");
}

// Rate trim speeds the clock up by the given fraction
static void TestRateTrim()
{
    Clock clock;
    clock.Init(kSampleRate, 120.0f);
    clock.SetRateTrim(0.002f);
    for (int n = 0; n < 48000 * 60; n++)  // one minute: 120 beats, +0.2 % = 120.24 beats
        clock.Process();
    double beats = clock.Tick() / (double)Clock::kTicksPerBeat;
    CHECK(std::fabs(beats - 120.24) < 0.05, "trimmed clock did %.3f beats in 1 min", beats);
    std::printf("  rate trim +0.2 %%: %.3f beats per minute at 120 BPM\n", beats);
}

int main()
{
    std::printf("clock.h tests\n");
    TestBeatPositions(120.0f, 3600);  // One hour, divides evenly
    TestBeatPositions(133.0f, 3600);  // One hour, doesn't divide evenly
    TestBeatPositions(60.0f, 600);
    TestBeatPositions(200.0f, 600);
    TestTempoChangeIsContinuous();
    TestReset();
    TestRateTrim();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
