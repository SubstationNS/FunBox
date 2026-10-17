#pragma once
#include <cmath>
#include <cstdint>

//
// Round-trip latency measurement with a loopback cable (Out L -> In L). A short windowed sweep
// goes out, the input is recorded, and the delay is the lag where the recording matches the
// sweep best (normalized cross-correlation), refined to a fraction of a sample with a parabola
// through the peak. Several runs are measured; they have to agree.
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class LatencyProbe
{
  public:
    static const uint32_t kSweepLength   = 2048;  // 43 ms
    static const uint32_t kCaptureLength = 8192;  // Per run: the sweep plus up to ~6000 samples of delay
    static const uint32_t kMaxLag        = kCaptureLength - kSweepLength;
    static constexpr float kAmplitude    = 0.4f;  // -8 dBFS

    // The test signal, sample n of the sweep (200 Hz -> 8 kHz, Hann window)
    static float Sweep(uint32_t n, float sample_rate)
    {
        if (n >= kSweepLength)
            return 0.0f;
        const double f0 = 200.0, f1 = 8000.0, T = kSweepLength / (double)sample_rate;
        double t     = n / (double)sample_rate;
        double phase = 2.0 * 3.14159265358979 * (f0 * t + 0.5 * (f1 - f0) / T * t * t);
        double w     = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * n / (kSweepLength - 1));
        return (float)(kAmplitude * w * std::sin(phase));
    }

    struct Result
    {
        bool  ok;
        float delay;  // Samples, from the first sweep sample going out to it coming back in
        float match;  // Normalized correlation at the peak (1 = perfect copy)
        float level;  // Peak level of the returning sweep, relative to what was sent
    };

    // capture: kCaptureLength input samples, starting with the sample the sweep started on.
    // sweep: kSweepLength samples of Sweep() (precomputed by the caller).
    static Result Measure(const float* capture, const float* sweep)
    {
        double sweep_energy = 0.0;
        for (uint32_t i = 0; i < kSweepLength; i++)
            sweep_energy += (double)sweep[i] * sweep[i];

        // Running energy of the capture window, so normalizing costs nothing extra
        double win = 0.0;
        for (uint32_t i = 0; i < kSweepLength; i++)
            win += (double)capture[i] * capture[i];

        Result r    = {false, 0.0f, 0.0f, 0.0f};
        float  best = -2.0f, before = 0.0f, after = 0.0f, prev = 0.0f;
        uint32_t best_lag = 0;
        for (uint32_t lag = 0; lag <= kMaxLag; lag++) {
            if (lag > 0) {
                double out = capture[lag - 1], in = capture[lag + kSweepLength - 1];
                win += in * in - out * out;
            }
            float dot = 0.0f;
            const float* c = capture + lag;
            for (uint32_t i = 0; i < kSweepLength; i++)
                dot += c[i] * sweep[i];
            float norm = (float)(dot / std::sqrt(sweep_energy * (win > 1e-12 ? win : 1e-12)));
            if (norm > best) {
                best     = norm;
                best_lag = lag;
                before   = prev;
            }
            if (lag == best_lag + 1)
                after = norm;
            prev = norm;
        }
        // Parabola through the peak and its neighbours
        float frac = 0.0f;
        if (best_lag > 0 && best_lag < kMaxLag) {
            float denom = before - 2.0f * best + after;
            if (std::fabs(denom) > 1e-9f)
                frac = 0.5f * (before - after) / denom;
        }
        double peak = 0.0;
        for (uint32_t i = 0; i < kSweepLength; i++)
            peak = std::fmax(peak, std::fabs((double)capture[best_lag + i]));
        r.delay = (float)best_lag + frac;
        r.match = best;
        r.level = (float)(peak / kAmplitude);
        r.ok    = best > 0.6f && r.level > 0.01f;
        return r;
    }

    // Several runs: all must be ok and within a sample of each other. Returns the average.
    static Result Combine(const Result* runs, int n)
    {
        Result r = {true, 0.0f, 1.0f, 1.0f};
        float  lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < n; i++) {
            r.ok &= runs[i].ok;
            r.delay += runs[i].delay / n;
            r.match = std::fmin(r.match, runs[i].match);
            r.level = std::fmin(r.level, runs[i].level);
            lo      = std::fmin(lo, runs[i].delay);
            hi      = std::fmax(hi, runs[i].delay);
        }
        r.ok &= hi - lo <= 1.0f;
        return r;
    }
};

} // namespace ceres
