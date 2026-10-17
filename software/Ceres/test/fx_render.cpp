// Renders the loop FX on a synthetic plucked riff to WAV files, for listening on the computer:
//   c++ -std=c++17 -O2 -I.. fx_render.cpp -o fx_render && ./fx_render <out_dir>
// Uses the same clock.h / fx.h as the pedal, at the pedal's real sample rate.
#include "clock.h"
#include "fx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const float kSampleRate = 16000000.0f / 6.0f * 295.0f / 16.0f / 1024.0f;  // 48014.3 Hz
static const float kBpm        = 120.0f;
static const int   kBars       = 8;

// Karplus-Strong plucks: a simple 2-bar riff in 16ths, repeated
static std::vector<float> MakeRiff(int samples)
{
    const float notes[] = {110.0f, 0, 164.8f, 220.0f, 0, 196.0f, 164.8f, 0,
                           146.8f, 0, 220.0f, 0, 246.9f, 220.0f, 196.0f, 0,
                           110.0f, 0, 164.8f, 220.0f, 0, 261.6f, 246.9f, 0,
                           220.0f, 0, 196.0f, 164.8f, 0, 146.8f, 0, 0};
    const double samples_per_16th = 60.0 / kBpm / 4.0 * kSampleRate;

    std::vector<float> out(samples, 0.0f);
    std::vector<float> string;
    uint32_t seed = 1;
    for (int step = 0; step * samples_per_16th < samples; step++) {
        float hz = notes[step % 32];
        if (hz == 0.0f)
            continue;
        int start = (int)(step * samples_per_16th);
        int len   = (int)(kSampleRate / hz);
        string.assign(len, 0.0f);
        for (auto& s : string) {
            seed = seed * 1664525u + 1013904223u;
            s    = (float)(int32_t)seed / 2147483648.0f;
        }
        int ring = (int)(samples_per_16th * 2.5);  // Let notes overlap a little
        for (int n = 0; n < ring && start + n < samples; n++) {
            int   i    = n % len;
            float next = string[(i + 1) % len];
            float y    = string[i];
            string[i]  = 0.996f * 0.5f * (y + next);
            out[start + n] += 0.35f * y;
        }
    }
    return out;
}

static void WriteWav(const std::string& path, const std::vector<float>& x)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::perror(path.c_str());
        std::exit(1);
    }
    uint32_t rate = 48000, data = (uint32_t)x.size() * 2, riff = 36 + data, fmt_len = 16, byte_rate = rate * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt_len, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (float s : x) {
        float   c = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        int16_t v = (int16_t)(c * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

enum Fx { TAPE, DELAY, REPEAT };

// amount(bar_position 0..kBars) lets a render sweep the knob
template <typename AmountFn>
static void Render(const std::string& path, Fx fx, AmountFn amount_at, const std::vector<float>& dry)
{
    std::vector<float> tape_buf(4096), delay_buf(120000), repeat_buf(50000);
    ceres::TapeFx     tape;
    ceres::SyncDelay  delay;
    ceres::BeatRepeat repeat;
    tape.Init(kSampleRate, tape_buf.data(), tape_buf.size());
    delay.Init(kSampleRate, delay_buf.data(), delay_buf.size());
    repeat.Init(kSampleRate, repeat_buf.data(), repeat_buf.size());

    Clock clock;
    clock.Init(kSampleRate, kBpm);
    delay.SetTime((float)(18 * clock.SamplesPerTick()));  // Dotted 1/8

    std::vector<float> out(dry.size());
    float peak = 0.0f;
    for (size_t n = 0; n < dry.size(); n++) {
        uint8_t events = clock.Process();
        float   a      = amount_at((float)clock.Tick() / Clock::kTicksPerBar);
        float   y      = 0.0f;
        switch (fx) {
            case TAPE: y = tape.Process(dry[n], a); break;
            case DELAY: y = delay.Process(dry[n], a); break;
            case REPEAT: y = repeat.Process(dry[n], a, events, clock.Tick()); break;
        }
        if (!std::isfinite(y)) {
            std::printf("FAIL %s: non-finite output at sample %zu\n", path.c_str(), n);
            std::exit(1);
        }
        peak   = std::fabs(y) > peak ? std::fabs(y) : peak;
        out[n] = y;
    }
    WriteWav(path, out);
    std::printf("  %-28s peak %.2f\n", path.substr(path.rfind('/') + 1).c_str(), peak);
}

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : ".";
    int samples = (int)(kBars * 4 * 60.0 / kBpm * kSampleRate);
    auto dry = MakeRiff(samples);

    std::printf("Rendering %d bars at %.0f BPM to %s\n", kBars, kBpm, dir.c_str());
    WriteWav(dir + "/00_dry.wav", dry);
    Render(dir + "/01_tape_50.wav", TAPE, [](float) { return 0.5f; }, dry);
    Render(dir + "/02_tape_100.wav", TAPE, [](float) { return 1.0f; }, dry);
    Render(dir + "/03_delay_40.wav", DELAY, [](float) { return 0.4f; }, dry);
    Render(dir + "/04_delay_100.wav", DELAY, [](float) { return 1.0f; }, dry);
    // Knob held at each step for 2 bars: 1/4, 1/8, 1/16, 1/32
    Render(dir + "/05_repeat_steps.wav", REPEAT, [](float bar) {
        const float steps[] = {0.2f, 0.4f, 0.7f, 0.9f};
        return steps[(int)bar / 2 % 4];
    }, dry);
    // Expression-style build-up: off for 2 bars, sweep 0 -> full over 4 bars, off again (the drop)
    Render(dir + "/06_repeat_buildup.wav", REPEAT, [](float bar) {
        if (bar < 2.0f || bar >= 6.0f) return 0.0f;
        return (bar - 2.0f) / 4.0f;
    }, dry);
    std::printf("done\n");
}
