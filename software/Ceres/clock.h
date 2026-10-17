#pragma once
#include <cstdint>

//
// Ceres internal clock (the "flywheel").
//
// Always runs, advanced once per audio sample. Position is counted in MIDI clock ticks
// (24 per beat) on a fixed 4/4 grid, so incoming MIDI clock can later steer it directly:
//   - tempo:  SetTempo() from the measured tick period
//   - phase:  SetRateTrim() nudges the speed by a tiny amount (PLL), never jumps
//   - Start:  Reset() back to bar 1
//
class Clock
{
  public:
    static const uint32_t kTicksPerBeat = 24;  // MIDI clock resolution (PPQN)
    static const uint32_t kBeatsPerBar  = 4;   // Fixed 4/4 grid
    static const uint32_t kTicksPerBar  = kTicksPerBeat * kBeatsPerBar;

    // Event flags returned by Process(), set on the sample where the boundary is crossed
    enum Event : uint8_t
    {
        TICK = 1 << 0,
        BEAT = 1 << 1,
        BAR  = 1 << 2,
    };

    void Init(float sample_rate, float bpm)
    {
        sample_rate_ = sample_rate;
        rate_trim_   = 0.0f;
        SetTempo(bpm);
        Reset();
    }

    void SetTempo(float bpm)
    {
        bpm_ = bpm;
        UpdateIncrement();
    }

    // Small speed correction for syncing, e.g. +0.001 = 0.1 % faster. Pitch-neutral once loops
    // go through the stretcher; until then keep it within about +-0.002.
    void SetRateTrim(float trim)
    {
        rate_trim_ = trim;
        UpdateIncrement();
    }

    // Back to bar 1, beat 1. The next Process() call reports TICK | BEAT | BAR.
    void Reset()
    {
        tick_          = 0;
        frac_          = 0.0;
        pending_start_ = true;
    }

    // Advance by one sample, returns the boundaries crossed on this sample (Event flags)
    uint8_t Process()
    {
        uint8_t events = 0;

        if (pending_start_) {
            pending_start_ = false;
            return TICK | BEAT | BAR;
        }

        frac_ += increment_;
        while (frac_ >= 1.0) {  // At sane tempos this crosses at most one tick per sample
            frac_ -= 1.0;
            tick_++;
            events |= TICK;
            if (tick_ % kTicksPerBeat == 0)
                events |= BEAT;
            if (tick_ % kTicksPerBar == 0)
                events |= BAR;
        }
        return events;
    }

    float    Tempo() const { return bpm_; }
    uint32_t Tick() const { return tick_; }                               // Ticks since Reset()
    uint32_t Bar() const { return tick_ / kTicksPerBar; }                 // 0-based
    uint32_t BeatInBar() const { return (tick_ / kTicksPerBeat) % kBeatsPerBar; }  // 0-based
    uint32_t TickInBeat() const { return tick_ % kTicksPerBeat; }

  private:
    void UpdateIncrement()
    {
        // ticks per sample = beats per second * ticks per beat / samples per second
        increment_ = (double)bpm_ / 60.0 * kTicksPerBeat / sample_rate_ * (1.0 + rate_trim_);
    }

    float    sample_rate_;
    float    bpm_;
    float    rate_trim_;
    double   increment_;  // Ticks per sample
    uint32_t tick_;       // Whole ticks since Reset()
    double   frac_;       // Fraction of the current tick, 0..1
    bool     pending_start_;
};
