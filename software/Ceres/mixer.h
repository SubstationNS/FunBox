#pragma once
#include <cmath>
#include <cstdint>

//
// Ceres output stage: per-track DJ filter and level, output limiter.
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

//
// Bipolar DJ filter for a track's playback. Knob: low-pass to the left of noon (down to ~150 Hz),
// high-pass to the right (up to ~3 kHz), flat around noon. Resonance rises a little toward the
// ends. The filter fades in over the first bit of travel past noon, so there's no step there.
// State-variable filter (topology-preserving), coefficients once per block.
//
class DjFilter
{
  public:
    void Init(float sample_rate)
    {
        sr_    = sample_rate;
        knob_  = 0.5f;
        ic1_ = ic2_ = 0.0f;
        hp_    = false;
        wet_   = 0.0f;
        SetKnob(0.5f);
    }

    // Once per audio block
    void SetKnob(float knob)
    {
        knob_ += 0.15f * (knob - knob_);  // Smooth over ~6 blocks, no zipper noise
        const float kDead = 0.04f, kSpan = 0.5f - kDead;
        float t = 0.0f, fc = 20000.0f;
        if (knob_ < 0.5f - kDead) {
            t   = (0.5f - kDead - knob_) / kSpan;
            fc  = 20000.0f * powf(150.0f / 20000.0f, t);
            hp_ = false;
        } else if (knob_ > 0.5f + kDead) {
            t   = (knob_ - 0.5f - kDead) / kSpan;
            fc  = 20.0f * powf(3000.0f / 20.0f, t);
            hp_ = true;
        }
        wet_ = t < 0.08f ? t / 0.08f : 1.0f;
        fc   = fc > 0.45f * sr_ ? 0.45f * sr_ : fc;
        float q = 0.7f + 0.9f * t;  // A touch of resonance toward the ends
        k_      = 1.0f / q;
        float g = tanf(3.14159265f * fc / sr_);
        a1_     = 1.0f / (1.0f + g * (g + k_));
        a2_     = g * a1_;
        a3_     = g * a2_;
    }

    float Process(float x)
    {
        float v3 = x - ic2_;
        float v1 = a1_ * ic1_ + a2_ * v3;
        float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_     = 2.0f * v1 - ic1_;
        ic2_     = 2.0f * v2 - ic2_;
        float y  = hp_ ? x - k_ * v1 - v2 : v2;
        return x + wet_ * (y - x);
    }

  private:
    float sr_, knob_, k_, a1_, a2_, a3_, ic1_, ic2_, wet_;
    bool  hp_;
};


// Track level knob: silent at 0, unity at about 2 o'clock (0.71), +6 dB at full
inline float LevelGain(float knob) { return 2.0f * knob * knob; }


//
// Output limiter: catches hot overdub stacks before the codec clips. Instant attack, 100 ms
// release, ceiling -1 dBFS; a soft clip above that for anything that still gets through.
//
class Limiter
{
  public:
    void Init(float sample_rate)
    {
        release_ = expf(-1.0f / (0.1f * sample_rate));
        env_     = 0.0f;
    }

    float Process(float x)
    {
        const float kCeiling = 0.89f;
        float a = fabsf(x);
        env_    = a > env_ ? a : env_ * release_ + a * (1.0f - release_);
        float y = env_ > kCeiling ? x * (kCeiling / env_) : x;
        return y > 0.95f ? 0.95f + 0.05f * tanhf((y - 0.95f) * 20.0f) : (y < -0.95f ? -0.95f + 0.05f * tanhf((y + 0.95f) * 20.0f) : y);
    }

    float GainReduction() const { return env_ > 0.89f ? 0.89f / env_ : 1.0f; }

  private:
    float release_, env_;
};

} // namespace ceres
