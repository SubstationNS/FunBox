#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

//
// Ceres loop FX: Tape, Delay, Repeat. Mono, one instance per output.
// Plain C++ (no Daisy dependencies) so they run in the host tests too.
// Buffers are passed in by the caller (SDRAM on the pedal).
//
// Each Process() takes the FX amount 0..1 (knob 5); 0 = off.
//

namespace ceres
{

static const float kPi = 3.14159265f;

// One-pole lowpass coefficient for a cutoff in Hz
inline float OnePoleCoef(float hz, float sample_rate)
{
    return 1.0f - expf(-2.0f * kPi * hz / sample_rate);
}

// Ramps toward a target over a fixed time, used for click-free on/off
class Slew
{
  public:
    void  Init(float ms, float sample_rate) { coef_ = OnePoleCoef(1000.0f / ms, sample_rate); value_ = 0.0f; }
    float Process(float target) { return value_ += coef_ * (target - value_); }
    float Value() const { return value_; }

  private:
    float coef_, value_;
};


// Circular buffer with fractional (Hermite) reads
class DelayLine
{
  public:
    void Init(float* buf, uint32_t size)
    {
        buf_   = buf;
        size_  = size;
        write_ = 0;
        memset(buf_, 0, size_ * sizeof(float));
    }

    void Write(float x)
    {
        buf_[write_] = x;
        if (++write_ >= size_)
            write_ = 0;
    }

    // Sample written `delay` samples before the most recent one (delay >= 1 for Hermite)
    float Read(float delay) const
    {
        float    pos  = (float)write_ - 1.0f - delay;
        while (pos < 0.0f)
            pos += (float)size_;
        int32_t  i    = (int32_t)pos;
        float    f    = pos - (float)i;
        float    xm1  = At(i - 1), x0 = At(i), x1 = At(i + 1), x2 = At(i + 2);
        float    c1   = 0.5f * (x1 - xm1);
        float    c2   = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float    c3   = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    uint32_t Size() const { return size_; }

  private:
    float At(int32_t i) const
    {
        if (i < 0)
            i += size_;
        else if (i >= (int32_t)size_)
            i -= size_;
        return buf_[i];
    }

    float*   buf_;
    uint32_t size_, write_;
};


//
// Tape: wow + flutter (modulated short delay), saturation, treble loss, a little hiss.
// amount 0 = clean, ~0.5 = worn cassette, 1 = seasick. Needs >= 0.05 s of buffer.
//
class TapeFx
{
  public:
    void Init(float sample_rate, float* buf, uint32_t size)
    {
        sample_rate_ = sample_rate;
        line_.Init(buf, size);
        wow_phase_ = flutter_phase_ = 0.0f;
        drift_ = drift_target_ = 0.0f;
        lp_ = 0.0f;
        noise_ = 12345;
        engage_.Init(10.0f, sample_rate);
        drift_coef_ = OnePoleCoef(0.3f, sample_rate);
        last_amount_ = -1.0f;
    }

    float Process(float in, float amount)
    {
        float a = amount;

        // Wow (slow, plus a wandering drift so it isn't a plain vibrato) and flutter (fast)
        wow_phase_ += 0.55f / sample_rate_;
        flutter_phase_ += 7.3f / sample_rate_;
        if (wow_phase_ >= 1.0f)
            wow_phase_ -= 1.0f;
        if (flutter_phase_ >= 1.0f) {
            flutter_phase_ -= 1.0f;
            drift_target_ = Noise();  // New drift target ~7 times a second
        }
        drift_ += drift_coef_ * (drift_target_ - drift_);

        float wow_ms     = a * a * 2.5f * (sinf(2.0f * kPi * wow_phase_) + 0.6f * drift_);
        float flutter_ms = a * 0.12f * sinf(2.0f * kPi * flutter_phase_);
        float center_ms  = 1.0f + a * a * 4.0f;  // Room for the modulation to swing both ways
        float delay      = (center_ms + wow_ms + flutter_ms) * 0.001f * sample_rate_;

        line_.Write(in);
        float x = line_.Read(delay < 1.0f ? 1.0f : delay);

        // Saturation, roughly level-compensated
        float drive = 1.0f + 5.0f * a;
        x = tanhf(x * drive) / (1.0f + 0.35f * (drive - 1.0f));

        // Treble loss: ~18 kHz at 0 down to ~3 kHz at full (only recomputed when the amount moves)
        if (a != last_amount_) {
            last_amount_ = a;
            lp_coef_     = OnePoleCoef(18000.0f * powf(3000.0f / 18000.0f, a), sample_rate_);
        }
        lp_ += lp_coef_ * (x - lp_);

        float tape = lp_ + Noise() * a * a * 0.003f;

        // Crossfade from the untouched input, so engaging/disengaging never clicks
        float g = engage_.Process(a > 0.01f ? 1.0f : 0.0f);
        return in + g * (tape - in);
    }

  private:
    float Noise()  // White noise -1..1
    {
        noise_ = noise_ * 1664525u + 1013904223u;
        return (float)(int32_t)noise_ * (1.0f / 2147483648.0f);
    }

    float     sample_rate_;
    DelayLine line_;
    float     wow_phase_, flutter_phase_, drift_, drift_target_, drift_coef_, lp_, lp_coef_, last_amount_;
    uint32_t  noise_;
    Slew      engage_;
};


//
// Clock-synced tape delay. amount sets mix and feedback together:
// low = subtle slapback, high = long, darkening, self-oscillating tails.
// Time changes (tempo) glide like tape. Needs >= the longest delay time of buffer.
//
class SyncDelay
{
  public:
    void Init(float sample_rate, float* buf, uint32_t size)
    {
        sample_rate_ = sample_rate;
        line_.Init(buf, size);
        delay_ = target_ = 0.25f * sample_rate;
        glide_coef_ = OnePoleCoef(4.0f, sample_rate);   // ~40 ms glide on time changes
        lp_coef_    = OnePoleCoef(3500.0f, sample_rate);  // Each repeat gets darker
        hp_coef_    = OnePoleCoef(90.0f, sample_rate);    // ... and loses low mud
        lp_ = hp_ = 0.0f;
        mix_.Init(20.0f, sample_rate);
    }

    void SetTime(float samples)
    {
        float max = (float)line_.Size() - 4.0f;
        target_   = samples < 1.0f ? 1.0f : (samples > max ? max : samples);
    }

    float Process(float in, float amount)
    {
        delay_ += glide_coef_ * (target_ - delay_);
        float y = line_.Read(delay_);

        // Feedback path: darker and thinner every repeat, saturated so self-oscillation stays bounded
        lp_ += lp_coef_ * (y - lp_);
        hp_ += hp_coef_ * (lp_ - hp_);
        float feedback = amount * 1.08f;  // Just past 1 at full: tails build and sing
        line_.Write(in + tanhf((lp_ - hp_) * feedback));

        float mix = mix_.Process(amount > 0.01f ? 0.15f + 0.45f * amount : 0.0f);
        return in + mix * y;
    }

  private:
    float     sample_rate_;
    DelayLine line_;
    float     delay_, target_, glide_coef_, lp_coef_, hp_coef_, lp_, hp_;
    Slew      mix_;
};


//
// Clock-synced beat repeat. amount picks the slice: off, 1/4, 1/8, 1/16, 1/32.
// Engages on the next slice boundary: the first slice plays live while it's captured,
// then it repeats, retriggered on every clock boundary so it stays locked to tempo.
// Needs >= one beat at the slowest tempo of buffer (1.0 s at 60 BPM).
//
class BeatRepeat
{
  public:
    void Init(float sample_rate, float* buf, uint32_t size)
    {
        buf_  = buf;
        size_ = size;
        memset(buf_, 0, size_ * sizeof(float));
        state_         = OFF;
        slice_ticks_   = 0;
        captured_ticks_ = 0;
        captured_      = 0;
        play_pos_      = 0;
        fade_len_      = (uint32_t)(0.003f * sample_rate);  // 3 ms crossfades
        fade_pos_      = fade_len_;
        fade_from_pos_ = 0;
        fade_from_live_ = true;
    }

    // Slice length in clock ticks for an amount (24 ticks = 1/4 note)
    static uint32_t SliceTicks(float amount)
    {
        if (amount < 0.05f) return 0;
        if (amount < 0.30f) return 24;
        if (amount < 0.55f) return 12;
        if (amount < 0.80f) return 6;
        return 3;
    }

    // clock_events / tick: from Clock::Process() and Clock::Tick() for this sample
    float Process(float in, float amount, uint8_t clock_events, uint32_t tick)
    {
        if (clock_events & 1) {  // Clock::TICK
            uint32_t want = SliceTicks(amount);
            if (want == 0) {
                if (state_ != OFF)
                    Switch(OFF);
            } else if (tick % want == 0) {
                if (state_ == OFF || want > captured_ticks_) {
                    Switch(CAPTURING);  // (Re)capture: this slice plays live
                    captured_       = 0;
                    captured_ticks_ = want;
                } else {
                    Switch(REPEATING);  // Retrigger on every boundary, stays locked to the clock
                    play_pos_ = 0;
                }
                slice_ticks_ = want;
            }
        }

        if (state_ == CAPTURING && captured_ < size_)
            buf_[captured_++] = in;

        float now = Current(in, state_ == REPEATING, play_pos_);
        if (state_ == REPEATING)
            play_pos_++;

        if (fade_pos_ < fade_len_) {  // Crossfade from whatever was playing before the switch
            float from = Current(in, !fade_from_live_, fade_from_pos_);
            if (!fade_from_live_)
                fade_from_pos_++;
            float g = (float)fade_pos_++ / (float)fade_len_;
            return from + g * (now - from);
        }
        return now;
    }

  private:
    enum State { OFF, CAPTURING, REPEATING };

    float Current(float in, bool from_buffer, uint32_t pos) const
    {
        if (!from_buffer)
            return in;
        return pos < captured_ ? buf_[pos] : 0.0f;  // Past the capture (tempo slowed): silence
    }

    void Switch(State next)
    {
        fade_from_live_ = state_ != REPEATING;
        fade_from_pos_  = play_pos_;
        fade_pos_       = 0;
        state_          = next;
    }

    float*   buf_;
    uint32_t size_;
    State    state_;
    uint32_t slice_ticks_, captured_ticks_, captured_, play_pos_;
    uint32_t fade_len_, fade_pos_, fade_from_pos_;
    bool     fade_from_live_;
};

} // namespace ceres
