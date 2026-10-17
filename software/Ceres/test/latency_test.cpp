// Host test for latency.h: a sweep through a simulated codec round trip (fractional delay, a
// lowpass like the codec's filters, a DC-blocking highpass, gain, noise) is measured back.
//   c++ -std=c++17 -O2 -I.. latency_test.cpp -o latency_test && ./latency_test
#include "latency.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using ceres::LatencyProbe;

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

static const float kSampleRate = 48014.3f;

// The loopback: delay by `delay` samples (fractional, windowed-sinc), filters, gain, noise
static std::vector<float> Loop(const std::vector<float>& out, double delay, double gain, double noise, unsigned seed)
{
    std::vector<float> in(out.size(), 0.0f);
    for (size_t n = 0; n < out.size(); n++) {
        double t = n - delay, acc = 0.0;
        long   c = (long)std::floor(t);
        for (long k = c - 16; k <= c + 16; k++) {
            if (k < 0 || k >= (long)out.size())
                continue;
            double x = t - k;
            double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(3.14159265358979 * x) / (3.14159265358979 * x);
            double w = 0.5 + 0.5 * std::cos(3.14159265358979 * x / 17.0);
            acc += out[k] * s * w;
        }
        in[n] = (float)acc;
    }
    // Codec-ish: lowpass ~20 kHz, highpass ~5 Hz, gain, noise
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    double lp = 0.0, hp_x = 0.0, hp_y = 0.0, a = 1.0 - std::exp(-2.0 * 3.14159265358979 * 20000.0 / kSampleRate);
    double r = 1.0 - 2.0 * 3.14159265358979 * 5.0 / kSampleRate;
    for (auto& x : in) {
        lp += a * (x - lp);
        double y = lp - hp_x + r * hp_y;
        hp_x = lp;
        hp_y = y;
        x    = (float)(gain * y + noise * g(rng));
    }
    return in;
}

static void Case(double delay, double gain, double noise, bool expect_ok, double tol = 0.3)
{
    std::vector<float> sweep(LatencyProbe::kSweepLength);
    for (uint32_t i = 0; i < sweep.size(); i++)
        sweep[i] = LatencyProbe::Sweep(i, kSampleRate);
    std::vector<float> out(LatencyProbe::kCaptureLength, 0.0f);
    for (uint32_t i = 0; i < sweep.size(); i++)
        out[i] = sweep[i];

    LatencyProbe::Result runs[4];
    for (int k = 0; k < 4; k++) {
        auto in  = Loop(out, delay, gain, noise, 10 + k);
        runs[k]  = LatencyProbe::Measure(in.data(), sweep.data());
    }
    auto r = LatencyProbe::Combine(runs, 4);
    std::printf("  delay %7.2f gain %.2f noise %.3f -> %s %7.2f (match %.2f, level %.2f)\n", delay, gain, noise,
                r.ok ? "ok  " : "FAIL", r.delay, r.match, r.level);
    CHECK(r.ok == expect_ok, "delay %.2f: ok %d, expected %d", delay, r.ok, expect_ok);
    if (expect_ok)
        CHECK(std::fabs(r.delay - delay) < tol, "delay %.2f measured %.2f", delay, r.delay);
}

int main()
{
    std::printf("latency probe\n");
    Case(187.0, 1.0, 0.001, true);
    Case(187.37, 1.0, 0.001, true);
    Case(96.5, 0.8, 0.002, true);
    Case(250.81, 0.5, 0.01, true);    // Quieter and noisier
    Case(1500.25, 1.0, 0.005, true);  // Much longer than expected
    Case(187.0, 0.0, 0.01, false);    // No cable: just noise
    Case(187.0, 0.003, 0.02, false);  // Barely anything coming back
    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
