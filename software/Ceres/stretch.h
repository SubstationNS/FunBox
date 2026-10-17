#pragma once
#include <cmath>
#include <cstdint>

//
// Pitch-neutral time stretch for loops (WSOLA: waveform-similarity overlap-add).
//
// The loop is read in grains of 1024 samples (21 ms, Hann window), a new one every 512 output
// samples, each read at normal speed (so the pitch stays). Where a grain comes from follows a
// target position that moves at the stretch ratio (the clock's idea of where the loop is). Each
// new grain is placed within +-8 ms of its target, at the spot that lines up best with the sound
// the previous grain would have continued with (normalized cross-correlation), so grains join
// without phase cancellation.
//
// The search goes coarse (every 8th lag on every 8th sample), medium (every 2nd around the best),
// then fine (every lag next to it): ~10k multiply-adds per grain, ~94 grains per second.
// Two players can be staggered (Reset's `phase`) so their searches never land in the same block.
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class WsolaPlayer
{
  public:
    static const uint32_t kGrain  = 1024;
    static const uint32_t kHop    = kGrain / 2;
    static const uint32_t kSearch = 384;           // +-8 ms around the target
    static const uint32_t kMatch  = kGrain - kHop;  // The overlap the alignment is judged on

    WsolaPlayer()
    {
        for (uint32_t n = 0; n < kGrain; n++)  // Periodic Hann: two at 50 % overlap sum to exactly 1
            window_[n] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * n / kGrain);
    }

    // Start at source position `target` (seamless with reading the loop directly there).
    // phase (0..kHop-1): how far into the current grain pair to start, to stagger players.
    void Reset(const float* src, uint32_t len, double target, uint32_t phase = 0)
    {
        src_ = src;
        len_ = len;
        uint32_t t = Wrap((int64_t)std::floor(target));
        phase %= kHop;
        q_old_ = Wrap((int64_t)t - (int64_t)kHop - phase);  // Both grains read the target right now
        n_old_ = kHop + phase;
        q_new_ = Wrap((int64_t)t - phase);
        n_new_ = phase;
    }

    // Swap the source (undo / redo, a re-rendered loop) without resetting
    void SetSource(const float* src, uint32_t len)
    {
        src_ = src;
        len_ = len;
        q_old_ %= len;
        q_new_ %= len;
    }

    // One output sample. target: where the clock wants the loop now (source samples), ratio: how
    // fast that moves (source samples per output sample).
    float Process(double target, double ratio)
    {
        if (n_new_ == kHop) {  // The new grain is half way: it becomes the old one, start the next
            q_old_ = q_new_;
            n_old_ = kHop;
            // Aim the grain's middle at where the target will be by then
            double aim = target + (ratio - 1.0) * (kGrain / 2);
            q_new_     = Search(Wrap((int64_t)std::floor(aim)), Wrap((int64_t)q_old_ + kHop));
            n_new_     = 0;
        }
        float y = At(q_old_, n_old_) * window_[n_old_] + At(q_new_, n_new_) * window_[n_new_];
        n_old_++;
        n_new_++;
        return y;
    }

  private:
    float At(uint32_t q, uint32_t n) const
    {
        uint32_t i = q + n;
        return src_[i >= len_ ? i - len_ : i];
    }

    uint32_t Wrap(int64_t i) const
    {
        int64_t r = i % (int64_t)len_;
        return (uint32_t)(r < 0 ? r + len_ : r);
    }

    // Best start for the new grain around `target`: where the source looks most like `natural`
    // (what the previous grain would have continued with)
    uint32_t Search(uint32_t target, uint32_t natural)
    {
        // Gather the reference and the candidate region (wrapping around the loop end)
        float ref_energy = 0.0f;
        for (uint32_t i = 0; i < kMatch; i++) {
            ref_[i] = At(natural, i);
            ref_energy += ref_[i] * ref_[i];
        }
        uint32_t start = Wrap((int64_t)target - kSearch);
        for (uint32_t i = 0; i < 2 * kSearch + kMatch; i++)
            cand_[i] = At(start, i);
        if (ref_energy < 1e-9f || len_ < 4 * kGrain)
            return target;  // Silence (or a tiny loop): nothing to line up

        // Coarse: every 8th lag on every 8th sample, then every 2nd around it, then every one
        int best = Best(0, (int)(2 * kSearch), 8, 8);
        best     = Best(best - 8, best + 8, 2, 2);
        best     = Best(best - 1, best + 1, 1, 1);
        return Wrap((int64_t)start + best);
    }

    // Best lag in [lo, hi] (clamped) every `lag_step`, comparing every `step`-th sample
    int Best(int lo, int hi, int lag_step, uint32_t step) const
    {
        lo = lo < 0 ? 0 : lo;
        hi = hi > (int)(2 * kSearch) ? (int)(2 * kSearch) : hi;
        int   best       = lo;
        float best_score = -1e30f;
        for (int lag = lo; lag <= hi; lag += lag_step) {
            float s = Score((uint32_t)lag, step);
            if (s > best_score) {
                best_score = s;
                best       = lag;
            }
        }
        return best;
    }

    // Normalized correlation (squared, keeping the sign: no square root needed to compare)
    // of the candidate at `lag` with the reference
    float Score(uint32_t lag, uint32_t step) const
    {
        float dot = 0.0f, energy = 1e-12f;
        const float* c = cand_ + lag;
        for (uint32_t i = 0; i < kMatch; i += step) {
            dot += c[i] * ref_[i];
            energy += c[i] * c[i];
        }
        return dot * std::fabs(dot) / energy;  // The reference's energy is the same for every lag
    }

    const float* src_ = nullptr;
    uint32_t     len_ = 1;
    uint32_t     q_old_ = 0, n_old_ = kHop, q_new_ = 0, n_new_ = 0;
    float        window_[kGrain];
    float        ref_[kMatch];
    float        cand_[2 * kSearch + kMatch];
};


//
// Re-render a loop at another length (tempo) with the same stretcher: the result plays at normal
// speed at the new tempo. Run in steps so it can be spread out / aborted. The end is crossfaded
// into the start (like a recording's seam), so it loops seamlessly.
//
class LoopRenderer
{
  public:
    static const uint32_t kSeam = 480;  // 10 ms

    // dst: new_len + kSeam samples of room
    void Start(const float* src, uint32_t len, float* dst, uint32_t new_len)
    {
        dst_     = dst;
        new_len_ = new_len;
        ratio_   = (double)len / (double)new_len;
        out_     = 0;
        player_.Reset(src, len, 0.0);
        done_ = false;
    }

    // Renders up to `samples` more; returns true when finished
    bool Step(uint32_t samples)
    {
        uint32_t end = out_ + samples > new_len_ + kSeam ? new_len_ + kSeam : out_ + samples;
        for (; out_ < end; out_++)
            dst_[out_] = player_.Process(out_ * ratio_, ratio_);
        if (out_ == new_len_ + kSeam && !done_) {
            for (uint32_t i = 0; i < kSeam; i++) {  // The tail past the end fades into the start
                float g  = ((float)i + 0.5f) / kSeam;
                dst_[i] = dst_[i] * g + dst_[new_len_ + i] * (1.0f - g);
            }
            done_ = true;
        }
        return done_;
    }

    bool     Done() const { return done_; }
    uint32_t Progress() const { return out_; }

  private:
    WsolaPlayer player_;
    float*      dst_ = nullptr;
    uint32_t    new_len_ = 0, out_ = 0;
    double      ratio_ = 1.0;
    bool        done_ = true;
};

} // namespace ceres
