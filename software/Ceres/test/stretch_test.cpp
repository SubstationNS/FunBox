// Host test for stretch.h (WSOLA): pitch stays, timing follows the target, no clicks, rendered
// loops wrap seamlessly. Also writes WAVs to listen to (pass a directory).
//   c++ -std=c++17 -O2 -I.. stretch_test.cpp -o stretch_test && ./stretch_test [wav dir]
#include "stretch.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using ceres::LoopRenderer;
using ceres::WsolaPlayer;

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

static const double kSr = 48014.3;
static const double kPi = 3.14159265358979;

// Frequency from zero crossings (with interpolation) over the middle of the signal
static double Frequency(const std::vector<float>& x)
{
    double first = -1, last = -1;
    int    crossings = 0;
    for (size_t i = x.size() / 4; i + 1 < x.size() * 3 / 4; i++)
        if (x[i] <= 0 && x[i + 1] > 0) {
            double t = i + x[i] / (x[i] - x[i + 1]);
            if (first < 0)
                first = t;
            last = t;
            crossings++;
        }
    return crossings > 1 ? (crossings - 1) / ((last - first) / kSr) : 0.0;
}

static double MaxStep(const std::vector<float>& x, size_t from)
{
    double m = 0;
    for (size_t i = from + 1; i < x.size(); i++)
        m = std::fmax(m, std::fabs((double)x[i] - x[i - 1]));
    return m;
}

// Stretch a looped source in real time: target moves at `ratio`
static std::vector<float> Play(const std::vector<float>& src, double ratio, size_t out_len)
{
    WsolaPlayer p;
    p.Reset(src.data(), (uint32_t)src.size(), 0.0);
    std::vector<float> out(out_len);
    for (size_t n = 0; n < out_len; n++)
        out[n] = p.Process(std::fmod(n * ratio, (double)src.size()), ratio);
    return out;
}

static void WriteWav(const std::string& path, const std::vector<float>& x)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    uint32_t rate = 48000, data = (uint32_t)x.size() * 2, riff = 36 + data, fmt_len = 16, byte_rate = rate * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt_len, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (float s : x) {
        float   c = s > 1 ? 1 : (s < -1 ? -1 : s);
        int16_t v = (int16_t)(c * 32767);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// A 2-bar plucked riff at 120 BPM (Karplus-Strong), like fx_render's
static std::vector<float> Riff(double bpm)
{
    const float notes[] = {110.0f, 0, 164.8f, 220.0f, 0, 196.0f, 164.8f, 0, 146.8f, 0, 220.0f, 0, 246.9f, 220.0f, 196.0f, 0,
                           110.0f, 0, 164.8f, 220.0f, 0, 261.6f, 246.9f, 0, 220.0f, 0, 196.0f, 164.8f, 0, 146.8f, 0, 0};
    double six = 60.0 / bpm / 4.0 * kSr;
    size_t len = (size_t)std::lround(32 * six);
    std::vector<float> out(len, 0.0f), str;
    uint32_t seed = 1;
    for (int step = 0; step < 32; step++) {
        float hz = notes[step];
        if (hz == 0)
            continue;
        size_t start = (size_t)(step * six), n = (size_t)(kSr / hz);
        str.assign(n, 0);
        for (auto& s : str) {
            seed = seed * 1664525u + 1013904223u;
            s    = (float)(int32_t)seed / 2147483648.0f;
        }
        for (size_t k = 0; k < (size_t)(six * 2.5); k++) {
            size_t i = k % n;
            float  y = str[i];
            str[i]   = 0.996f * 0.5f * (y + str[(i + 1) % n]);
            out[(start + k) % len] += 0.35f * y;
        }
    }
    return out;
}

static void TestPitch()
{
    std::printf("stretch: pitch stays\n");
    std::vector<float> src((size_t)(2.0 * kSr));
    // A sine that fits the loop a whole number of times, so the source itself has no seam
    double hz = 440.0 * kSr / src.size();  // Exactly 440 cycles in the loop
    for (size_t i = 0; i < src.size(); i++)
        src[i] = 0.5f * (float)std::sin(2 * kPi * hz * i / kSr);
    double natural = 0.5 * 2 * kPi * hz / kSr;
    for (double r : {0.5, 0.8, 0.97, 1.0, 1.05, 1.25, 2.0}) {
        auto   out  = Play(src, r, (size_t)(3 * kSr));
        double f    = Frequency(out);
        double step = MaxStep(out, 2048);
        std::printf("  ratio %.2f: %.2f Hz (source %.2f), biggest step %.4f (sine alone %.4f)\n", r, f, hz, step, natural);
        CHECK(std::fabs(f / hz - 1.0) < 0.003, "ratio %.2f: pitch %.2f Hz vs %.2f", r, f, hz);
        CHECK(step < 1.3 * natural, "ratio %.2f: click, step %.4f", r, step);
    }
}

// Clicks in the source (one per beat) come out where the target puts them
static void TestTiming()
{
    std::printf("stretch: timing follows the target\n");
    const double beat = 60.0 / 120.0 * kSr;
    std::vector<float> src((size_t)(4 * beat), 0.0f);
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 200; k++)  // Short decaying burst
            src[(size_t)(b * beat) + k] = 0.8f * (float)std::exp(-k / 40.0) * (float)std::sin(k * 0.9);
    for (double r : {0.8, 1.25}) {
        auto out = Play(src, r, (size_t)(4 * beat / r));
        double worst = 0;
        for (int b = 1; b < 4; b++) {
            double expect = b * beat / r;
            // Onset: the first sample above a threshold near the expected position
            size_t lo = (size_t)(expect - 0.03 * kSr), hi = (size_t)(expect + 0.03 * kSr);
            size_t at = lo;
            while (at < hi && std::fabs(out[at]) < 0.2)
                at++;
            worst = std::fmax(worst, std::fabs(at - expect) / kSr * 1000.0);
        }
        std::printf("  ratio %.2f: beats within %.1f ms of where the clock puts them\n", r, worst);
        CHECK(worst < 10.0, "ratio %.2f: beats off by %.1f ms", r, worst);
    }
}

static void TestRender(const char* dir)
{
    std::printf("stretch: re-rendered loops\n");
    auto src = Riff(120.0);
    for (double bpm : {100.0, 150.0}) {
        uint32_t new_len = (uint32_t)std::lround(src.size() * 120.0 / bpm);
        std::vector<float> dst(new_len + LoopRenderer::kSeam);
        LoopRenderer r;
        auto t0 = std::chrono::steady_clock::now();
        r.Start(src.data(), (uint32_t)src.size(), dst.data(), new_len);
        while (!r.Step(4096)) {
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // Seamless: play it around 3 times and look at the wrap
        std::vector<float> looped;
        for (int k = 0; k < 3; k++)
            looped.insert(looped.end(), dst.begin(), dst.begin() + new_len);
        double wrap = 0;
        for (int k = 1; k < 3; k++)
            wrap = std::fmax(wrap, std::fabs((double)looped[k * new_len] - looped[k * new_len - 1]));
        double typical = 0;
        for (size_t i = 1; i < new_len; i++)
            typical = std::fmax(typical, std::fabs((double)dst[i] - dst[i - 1]));
        std::printf("  120 -> %.0f BPM: %u samples, seam step %.4f (biggest anywhere %.4f), %.1f ms on this computer\n", bpm,
                    new_len, wrap, typical, ms);
        CHECK(wrap <= typical, "%.0f BPM: seam step %.4f bigger than anything else (%.4f)", bpm, wrap, typical);
        if (dir) {
            WriteWav(std::string(dir) + "/stretch_" + std::to_string((int)bpm) + "bpm.wav", looped);
        }
    }
    if (dir) {
        std::vector<float> orig;
        for (int k = 0; k < 3; k++)
            orig.insert(orig.end(), src.begin(), src.end());
        WriteWav(std::string(dir) + "/stretch_120bpm_original.wav", orig);
        // Real time: the tempo knob swept 120 -> 80 -> 160 over 12 s
        WsolaPlayer p;
        p.Reset(src.data(), (uint32_t)src.size(), 0.0);
        std::vector<float> out((size_t)(12 * kSr));
        double pos = 0;
        for (size_t n = 0; n < out.size(); n++) {
            double t   = n / kSr;
            double bpm = t < 6 ? 120 - 40 * t / 6 : 80 + 80 * (t - 6) / 6;
            double r   = bpm / 120.0;
            pos        = std::fmod(pos + r, (double)src.size());
            out[n]     = p.Process(pos, r);
        }
        WriteWav(std::string(dir) + "/stretch_sweep_120_80_160.wav", out);
    }
}

static void TestCost()
{
    std::vector<float> src((size_t)(4 * kSr));
    uint32_t seed = 3;
    for (auto& s : src) {
        seed = seed * 1664525u + 1013904223u;
        s    = (float)(int32_t)seed / 4294967296.0f;
    }
    auto t0  = std::chrono::steady_clock::now();
    auto out = Play(src, 1.13, (size_t)(10 * kSr));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("stretch: 10 s of audio in %.1f ms on this computer (%.2f %% of real time)\n", ms, ms / 100.0);
}

int main(int argc, char** argv)
{
    TestPitch();
    TestTiming();
    TestRender(argc > 1 ? argv[1] : nullptr);
    TestCost();
    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
