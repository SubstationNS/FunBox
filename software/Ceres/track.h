#pragma once
#include "clock.h"
#include <cmath>
#include <cstdint>
#include <cstring>

//
// Ceres loop track: quantized record / play / overdub / stop, undo / redo, clear.
// Design: docs/looper-design.md, "Recording and playback" and "Track behaviour".
//
//   - Recording starts on a bar line and ends on a bar line (tap), a power-of-two bar count
//     (DIP 2) or a grid step (double tap). A tap just after a boundary ends the loop on it.
//   - Length is kept in clock ticks and samples. Playback follows the clock: the position is
//     pulled toward where the clock says the loop should be (rate nudges up to +-0.2 %, a
//     crossfaded jump if it's far off), so loops stay locked to MIDI clock for hours. If the
//     tempo is no longer the recorded one, the loop plays at its own speed and restarts on
//     every loop boundary of the clock (until the stretcher exists).
//   - Seams: the recording runs 5 ms past the end, and that tail is crossfaded into the start,
//     so the loop wraps without a click. Starts, stops and punch in/out are 5 ms fades.
//   - Undo: two buffers per track, the playing one and the undo/redo one. An overdub writes in
//     place; while it can still be cancelled (the first 1.2 s: a double tap or a hold), the
//     samples it overwrites are saved to a side buffer, so cancelling restores them and the
//     previous undo point survives. After that the overdub is committed: the pre-overdub loop
//     is copied into the undo buffer in the background, ahead of the playhead.
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class Track
{
  public:
    enum State : uint8_t
    {
        EMPTY,
        ARMED,        // Recording starts on the next bar line
        RECORDING,
        PLAYING,
        OVERDUBBING,
        STOPPED,
        ARMED_PLAY,   // Playback restarts from the top on the next bar line
    };

    enum StopMode : uint8_t
    {
        STOP_NOW,
        STOP_END_OF_BAR,
        STOP_END_OF_LOOP,
    };

    struct Settings
    {
        uint32_t grid_ticks   = 12;        // Double-tap ending step: 24 (1/4), 12 (1/8), 6 (1/16)
        bool     power_of_two = false;     // Tap ending rounds up to 1, 2, 4, 8, ... bars
        StopMode stop_mode    = STOP_NOW;
    };

    static constexpr float  kFadeSeconds   = 0.005f;  // Seams, start/stop, punch in/out
    static constexpr float  kSnapSeconds   = 0.08f;   // A tap this soon after a boundary ends the loop on it
    static constexpr float  kCancelSeconds = 1.2f;    // An overdub can be cancelled for this long
    static constexpr double kFollowRange   = 0.006;   // Tempo within 0.6 % of the recorded one: follow the clock
    static constexpr double kFollowSeconds = 0.25;    // Phase errors are pulled in over about this time
    static constexpr double kMaxNudge      = 0.002;   // ... with at most +-0.2 % speed change
    static constexpr float  kJumpSeconds   = 0.010f;  // Further off than this: crossfade to the right spot
    static const uint32_t   kCopyPerBlock  = 2048;    // Background copy speed (samples per audio block)
    static const uint32_t   kTicksPerBar   = Clock::kTicksPerBar;

    // a, b: loop and undo buffers, `capacity` samples each. side_a, side_b: `side_size` samples
    // each, at least kCancelSeconds (two, so a new overdub can start while the last one is being committed)
    void Init(float sample_rate, float* a, float* b, uint32_t capacity, float* side_a, float* side_b,
              uint32_t side_size)
    {
        sr_          = sample_rate;
        buf_[0]      = a;
        buf_[1]      = b;
        capacity_    = capacity;
        side_buf_[0] = side_a;
        side_buf_[1] = side_b;
        side_size_   = side_size;
        fade_len_    = (uint32_t)(kFadeSeconds * sample_rate);
        fade_step_   = 1.0f / (float)fade_len_;
        Clear();
    }

    //
    // Footswitch / MIDI CC actions. Call between audio blocks (before BeginBlock / Process).
    //

    void Tap(const Clock& clock)
    {
        before_press_          = state_;
        press_started_session_ = false;
        press_ended_recording_ = false;
        switch (state_) {
            case EMPTY: state_ = ARMED; break;
            case ARMED: state_ = EMPTY; break;
            case RECORDING:
                if (end_ticks_ == 0) {
                    EndRecording(kTicksPerBar, settings_.power_of_two, clock);
                    press_ended_recording_ = true;
                }
                break;
            case PLAYING:
                if (stop_pending_) {
                    stop_pending_ = false;  // Changed your mind: keep playing
                } else {
                    StartDub();
                    press_started_session_ = true;
                }
                break;
            case OVERDUBBING: EndDub(); break;
            case STOPPED:
                state_          = ARMED_PLAY;
                paused_by_midi_ = false;  // You took over: no auto-resume
                break;
            case ARMED_PLAY: state_ = STOPPED; break;
        }
    }

    // The second tap of a double tap (instead of a second Tap)
    void DoubleTap(const Clock& clock)
    {
        switch (state_) {
            case RECORDING: EndRecording(settings_.grid_ticks, false, clock); break;  // End on the grid instead
            case OVERDUBBING:
                if (press_started_session_)
                    CancelSession();  // The first tap only started this
                RequestStop(settings_.stop_mode);
                break;
            case PLAYING:
                // If the first tap ended the recording and the loop already closed, leave it playing
                if (!press_ended_recording_)
                    RequestStop(settings_.stop_mode);
                break;
            default: break;
        }
    }

    // Held 1 s: take back what the press did, then undo / redo the last overdub.
    // Armed or recording: discard.
    void Undo()
    {
        if (state_ == ARMED || state_ == RECORDING) {
            Clear();
            return;
        }
        RevertPress();
        if (state_ == EMPTY)
            return;
        if (job_ == COPY_UNDO)
            undo_pending_ = true;  // The undo buffer is still being filled, happens when it's done
        else if (alt_valid_)
            SwapBuffers();
    }

    // The press turned out to be part of another gesture (both footswitches): take back its action
    void RevertPress()
    {
        if (before_press_ == EMPTY && state_ == ARMED)
            state_ = EMPTY;
        else if (before_press_ == ARMED && state_ == EMPTY)
            state_ = ARMED;
        else if (before_press_ == PLAYING && state_ == OVERDUBBING && press_started_session_)
            CancelSession();
        else if (before_press_ == STOPPED && state_ == ARMED_PLAY)
            state_ = STOPPED;
        else if (before_press_ == ARMED_PLAY && state_ == STOPPED)
            state_ = ARMED_PLAY;
        else if (before_press_ == RECORDING && state_ == RECORDING)
            end_ticks_ = 0;
        before_press_          = state_;
        press_started_session_ = false;
    }

    void Clear()
    {
        state_          = EMPTY;
        cur_            = 0;
        len_            = 0;
        len_ticks_      = 0;
        rec_count_      = 0;
        rec_ticks_      = 0;
        end_ticks_      = 0;
        post_roll_left_ = 0;
        p_              = 0.0;
        rate_           = 1.0;
        follow_         = true;
        xf_left_        = 0;
        env_            = 0.0f;
        dub_on_         = false;
        dub_env_        = 0.0f;
        last_idx_valid_ = false;
        side_phase_     = false;
        side_sel_       = 0;
        job_            = NONE;
        alt_valid_      = false;
        undone_         = false;
        undo_pending_   = false;
        stop_pending_   = false;
        paused_by_midi_ = false;
        before_press_   = EMPTY;
        press_started_session_ = press_ended_recording_ = false;
    }

    // Stop per stop mode (double tap, stop all)
    void RequestStop(StopMode mode)
    {
        if (state_ == ARMED_PLAY) {
            state_ = STOPPED;
            return;
        }
        if (state_ != PLAYING && state_ != OVERDUBBING)
            return;
        EndDub();
        state_ = PLAYING;
        if (mode == STOP_NOW) {
            state_        = STOPPED;
            stop_pending_ = false;
        } else {
            stop_pending_ = true;
            stop_mode_    = mode;
        }
    }

    // Start all: stopped loops restart from the top on the next bar line
    void RequestPlay()
    {
        if (state_ == STOPPED) {
            state_          = ARMED_PLAY;
            paused_by_midi_ = false;
        }
    }

    // MIDI Stop: playing loops fade out and will resume with the transport
    void Pause()
    {
        if (state_ == PLAYING || state_ == OVERDUBBING) {
            EndDub();
            state_          = STOPPED;
            stop_pending_   = false;
            paused_by_midi_ = true;
        }
    }

    // MIDI Start / Continue, once the clock has been located. from_top (Start): loops start
    // over at the current clock position, playing ones crossfade there.
    void Resume(bool from_top, const Clock& clock)
    {
        if (state_ == RECORDING) {
            anchor_ = (int64_t)clock.Tick() - rec_ticks_;  // It started that long ago, in the new position
            return;
        }
        if (len_ == 0)
            return;
        if (from_top)
            anchor_ = (int64_t)clock.Tick();
        if (paused_by_midi_ && state_ == STOPPED) {
            paused_by_midi_ = false;
            state_          = PLAYING;
            p_              = Expected(clock);  // Silent until now, no crossfade needed
            last_idx_valid_ = false;
        }
    }

    //
    // Audio
    //

    // Once per audio block, before the samples
    void BeginBlock(const Clock& clock, const Settings& settings, bool transport_playing)
    {
        settings_  = settings;
        transport_ = transport_playing;
        RunJob();

        if (side_phase_ && side_elapsed_ >= (uint32_t)(kCancelSeconds * sr_))
            Commit(NextWriteIndex());  // Too late to cancel now

        if (len_ == 0 || state_ == RECORDING || !(Audible() || env_ > 0.0f))
            return;
        double ratio = clock.Increment() * (double)len_ / (double)len_ticks_;
        follow_      = std::fabs(ratio - 1.0) < kFollowRange;
        if (follow_) {
            if (p_ >= (double)len_)
                p_ = std::fmod(p_, (double)len_);
            double expected = Expected(clock);
            double err      = expected - p_;
            if (err > 0.5 * len_)
                err -= len_;
            else if (err < -0.5 * len_)
                err += len_;
            if (std::fabs(err) > kJumpSeconds * sr_ && post_roll_left_ == 0) {
                Jump(expected, cur_);
                rate_ = ratio;
            } else {
                double nudge = err / (kFollowSeconds * sr_);
                nudge        = nudge > kMaxNudge ? kMaxNudge : (nudge < -kMaxNudge ? -kMaxNudge : nudge);
                rate_        = ratio + nudge;
            }
        } else {
            rate_ = 1.0;  // Other tempo: own speed, restarted on every loop boundary
        }
    }

    // Per sample, after clock.Process(). Returns the loop's output (before level / filter).
    float Process(float in, uint8_t events, const Clock& clock)
    {
        bool tick = events & Clock::TICK, bar = events & Clock::BAR;

        switch (state_) {
            case ARMED:
                if (bar && transport_)
                    StartRecording(clock);
                break;
            case RECORDING:
                if (tick && rec_count_ > 0) {
                    rec_ticks_++;
                    if (end_ticks_ != 0 && rec_ticks_ >= end_ticks_)
                        Finish(rec_count_, end_ticks_);
                }
                break;
            case ARMED_PLAY:
                if (bar && transport_) {
                    anchor_         = (int64_t)clock.Tick();
                    p_              = 0.0;
                    last_idx_valid_ = false;
                    state_          = PLAYING;
                }
                break;
            case PLAYING:
            case OVERDUBBING:
                if (tick) {
                    bool loop_start = LoopTick(clock) == 0;
                    if (stop_pending_ && ((stop_mode_ == STOP_END_OF_BAR && bar) || (stop_mode_ == STOP_END_OF_LOOP && loop_start))) {
                        stop_pending_ = false;
                        state_        = STOPPED;
                    }
                    if (!follow_ && loop_start)
                        Jump(0.0, cur_);
                }
                break;
            default: break;
        }

        if (state_ == RECORDING) {
            buf_[cur_][rec_count_++] = in;
            if (rec_count_ + fade_len_ + 1 >= capacity_)
                ForceEnd(clock);
            return 0.0f;
        }
        if (len_ == 0 || state_ == ARMED)
            return 0.0f;

        // Recording the tail past the end, for the seam crossfade
        if (post_roll_left_ > 0) {
            buf_[cur_][rec_count_++] = in;
            if (--post_roll_left_ == 0)
                BakeSeam();
        }

        float target = Audible() ? 1.0f : 0.0f;
        env_         = Ramp(env_, target);
        if (env_ == 0.0f && target == 0.0f) {
            dub_env_ = 0.0f;
            return 0.0f;  // Stopped
        }

        float y = Read(cur_, p_);
        if (xf_left_ > 0) {  // Crossfade from the old position / buffer
            float g = (float)xf_left_ / (float)fade_len_;
            y += g * (Read(xf_buf_, xf_p_) - y);
            xf_p_ += rate_;
            if (follow_ && xf_p_ >= (double)len_)
                xf_p_ -= len_;
            xf_left_--;
        }

        dub_env_ = Ramp(dub_env_, dub_on_ ? 1.0f : 0.0f);
        if (dub_env_ > 0.0f && p_ < (double)len_)
            WriteDub(in * dub_env_);
        else
            last_idx_valid_ = false;  // Not writing: the next overdub starts at the playhead
        if (side_phase_)
            side_elapsed_++;

        p_ += rate_;
        if (follow_ && p_ >= (double)len_)
            p_ -= len_;

        return y * env_;
    }

    //
    // State, for LEDs and the status line
    //
    State    GetState() const { return state_; }
    bool     HasLoop() const { return len_ > 0; }
    uint32_t LengthTicks() const { return len_ticks_; }
    uint32_t LengthSamples() const { return len_; }
    uint32_t RecordedTicks() const { return rec_ticks_; }
    uint32_t EndTicks() const { return end_ticks_; }      // Recording: where it will end (0 = not yet)
    bool     StopPending() const { return stop_pending_; }
    bool     PausedByMidi() const { return paused_by_midi_; }
    bool     CanUndo() const { return alt_valid_ && !undone_; }
    bool     CanRedo() const { return alt_valid_ && undone_; }
    bool     Busy() const { return job_ != NONE; }        // Background copy running
    bool     Following() const { return follow_; }
    double   Position() const { return p_; }              // Samples into the loop

  private:
    enum Job : uint8_t { NONE, COPY_UNDO, RESTORE };

    bool Audible() const { return state_ == PLAYING || state_ == OVERDUBBING; }

    float Ramp(float v, float target) const
    {
        if (v < target)
            return v + fade_step_ > target ? target : v + fade_step_;
        if (v > target)
            return v - fade_step_ < target ? target : v - fade_step_;
        return v;
    }

    static int64_t Mod(int64_t a, int64_t m)
    {
        int64_t r = a % m;
        return r < 0 ? r + m : r;
    }

    int64_t LoopTick(const Clock& clock) const { return Mod((int64_t)clock.Tick() - anchor_, len_ticks_); }

    // Where the clock says the loop should be, in samples
    double Expected(const Clock& clock) const
    {
        double t = std::fmod(clock.Position() - (double)anchor_, (double)len_ticks_);
        if (t < 0.0)
            t += len_ticks_;
        return t * (double)len_ / (double)len_ticks_;
    }

    // Hermite read around the loop (the seam is baked, so wrapping is continuous). Past the
    // end (other tempo, waiting for the restart) it fades out and goes silent.
    float Read(int b, double pos) const
    {
        if (pos >= (double)len_) {
            if (follow_)
                pos = std::fmod(pos, (double)len_);
            else
                return 0.0f;
        }
        const float* x  = buf_[b];
        uint32_t     i  = (uint32_t)pos;
        float        f  = (float)(pos - (double)i);
        uint32_t     im = i == 0 ? len_ - 1 : i - 1;
        uint32_t     i1 = i + 1 >= len_ ? i + 1 - len_ : i + 1;
        uint32_t     i2 = i1 + 1 >= len_ ? i1 + 1 - len_ : i1 + 1;
        float        xm1 = x[im], x0 = x[i], x1 = x[i1], x2 = x[i2];
        float        c1 = 0.5f * (x1 - xm1);
        float        c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float        c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        float        y  = ((c3 * f + c2) * f + c1) * f + x0;
        if (!follow_ && (double)len_ - pos < (double)fade_len_)
            y *= (float)(((double)len_ - pos) / fade_len_);
        return y;
    }

    void Jump(double to, int from_buf)
    {
        xf_p_           = p_;
        xf_buf_         = from_buf;
        xf_left_        = fade_len_;
        p_              = to;
        last_idx_valid_ = false;
        if (side_phase_)
            Commit((uint32_t)to % len_);  // The saved samples are no longer one run: can't cancel any more
    }

    //
    // Recording
    //

    void StartRecording(const Clock& clock)
    {
        state_     = RECORDING;
        cur_       = 0;
        rec_count_ = 0;
        rec_ticks_ = 0;
        end_ticks_ = 0;
        anchor_    = (int64_t)clock.Tick();
        alt_valid_ = false;
        undone_    = false;
        job_       = NONE;
    }

    // End on the boundary of `step` ticks that's coming (or the one just passed, if the tap was
    // only a moment late). Power of two: round the bar count up.
    void EndRecording(uint32_t step, bool power_of_two, const Clock& clock)
    {
        double   frac  = clock.Position() - std::floor(clock.Position());
        double   pos   = (double)rec_ticks_ + frac;  // Ticks recorded so far
        uint32_t prev  = (uint32_t)(pos / step) * step;
        double   since = pos - prev;
        double   spt   = clock.SamplesPerTick();
        double   snap  = std::fmin(kSnapSeconds * sr_, 0.25 * step * spt);
        uint32_t target = (prev > 0 && since * spt <= snap) ? prev : prev + step;
        if (power_of_two) {
            uint32_t bars = (target + kTicksPerBar - 1) / kTicksPerBar, p2 = 1;
            while (p2 < bars)
                p2 <<= 1;
            target = p2 * kTicksPerBar;
        }
        if ((double)target <= pos) {
            uint32_t back = (uint32_t)std::lround((pos - target) * spt);
            Finish(rec_count_ > back ? rec_count_ - back : rec_count_, target);
        } else {
            end_ticks_ = target;
        }
    }

    // Out of memory: end on the last bar line (or grid step, or the last tick)
    void ForceEnd(const Clock& clock)
    {
        uint32_t step = rec_ticks_ >= kTicksPerBar ? kTicksPerBar : (rec_ticks_ >= settings_.grid_ticks ? settings_.grid_ticks : 1);
        uint32_t target = rec_ticks_ / step * step;
        if (target == 0) {
            Clear();
            return;
        }
        double   frac = clock.Position() - std::floor(clock.Position());
        uint32_t back = (uint32_t)std::lround(((double)(rec_ticks_ - target) + frac) * clock.SamplesPerTick());
        Finish(rec_count_ > back ? rec_count_ - back : rec_count_, target);
    }

    // The loop is `samples` / `ticks` long; recording carries on for the seam tail
    void Finish(uint32_t samples, uint32_t ticks)
    {
        len_            = samples;
        len_ticks_      = ticks;
        end_ticks_      = 0;
        state_          = PLAYING;
        p_              = (double)(rec_count_ - samples);  // Already this far into the next time around
        rate_           = 1.0;
        follow_         = true;
        last_idx_valid_ = false;
        env_            = 0.0f;
        uint32_t past   = rec_count_ - samples;
        post_roll_left_ = past < fade_len_ ? fade_len_ - past : 0;
        if (post_roll_left_ == 0)
            BakeSeam();
    }

    // Crossfade the tail recorded past the end into the start
    void BakeSeam()
    {
        float* x = buf_[cur_];
        for (uint32_t i = 0; i < fade_len_; i++) {
            float g = ((float)i + 0.5f) / (float)fade_len_;
            x[i]    = x[i] * g + x[len_ + i] * (1.0f - g);
        }
    }

    //
    // Overdub sessions, undo
    //

    // Overdubs go in just behind the interpolator's read window (it reads 1 before to 2 after
    // the playhead): written any closer, the sound being played in would leak straight into
    // the output, depending on where the playhead's fraction happens to sit. 2 samples = 42 us.
    static const uint32_t kWriteBehind = 2;

    uint32_t WriteIndex() const
    {
        uint32_t i = (uint32_t)p_ % len_;
        return i >= kWriteBehind ? i - kWriteBehind : i + len_ - kWriteBehind;
    }

    uint32_t NextWriteIndex() const
    {
        if (last_idx_valid_)
            return last_idx_ + 1 >= len_ ? 0 : last_idx_ + 1;
        return WriteIndex();
    }

    void StartDub()
    {
        state_  = OVERDUBBING;
        dub_on_ = true;
        if (side_phase_)
            return;  // Still in the last one's cancel window (punched out and in again): same session
        if (job_ == RESTORE)
            RunJobToEnd();  // Rare: a cancel is still being undone, and it needs the side buffer we'd reuse
        side_sel_ ^= 1;     // The other side buffer: the last one may still feed a background copy
        side_phase_   = true;
        side_start_   = NextWriteIndex();
        side_count_   = 0;
        side_elapsed_ = 0;
    }

    void EndDub()
    {
        if (state_ == OVERDUBBING)
            state_ = PLAYING;
        dub_on_ = false;
        if (side_phase_)
            Commit(NextWriteIndex());
    }

    // The overdub can't be cancelled any more: fill the undo buffer with the loop as it was
    // before it, in the background, starting at `start` (just ahead of the playhead)
    void Commit(uint32_t start)
    {
        side_phase_      = false;
        job_             = COPY_UNDO;
        job_start_       = start % len_;
        job_done_        = 0;
        copy_side_       = side_sel_;
        copy_side_start_ = side_start_;
        copy_side_count_ = side_count_;
        alt_valid_       = false;
        undone_          = false;
        RunJob();  // The first chunk now, so the playhead's next writes are covered
    }

    // Take an overdub back that's still in its cancel window
    void CancelSession()
    {
        dub_on_  = false;
        dub_env_ = 0.0f;
        if (state_ == OVERDUBBING)
            state_ = PLAYING;
        if (!side_phase_)
            return;
        side_phase_ = false;
        if (side_count_ == 0)
            return;
        // Restore from just ahead of the playhead (it never catches up)
        uint32_t k0 = (NextWriteIndex() + len_ - side_start_) % len_;
        job_        = RESTORE;
        job_start_  = k0 < side_count_ ? k0 : 0;
        job_done_   = 0;
        copy_side_  = side_sel_;
        restore_start_ = side_start_;
        restore_count_ = side_count_;
        restore_buf_   = cur_;
    }

    void SwapBuffers()
    {
        int old = cur_;
        cur_ ^= 1;
        undone_ = !undone_;
        Jump(p_, old);  // Crossfade over to the other version
    }

    // Overdub write at the playhead. With the rate a hair off 1.0 the index sometimes moves by
    // 0 or 2: write every index passed, so no slot is skipped.
    void WriteDub(float x)
    {
        uint32_t idx = WriteIndex();
        if (!last_idx_valid_) {
            WriteAt(idx, x);
        } else {
            uint32_t d = (idx + len_ - last_idx_) % len_;
            d          = d > 4 ? 1 : d;
            for (uint32_t j = 1; j <= d; j++)
                WriteAt((last_idx_ + j) % len_, x);
        }
        last_idx_       = idx;
        last_idx_valid_ = true;
    }

    void WriteAt(uint32_t i, float x)
    {
        float* loop = buf_[cur_];
        if (side_phase_) {
            uint32_t k = (i + len_ - side_start_) % len_;
            if (k == side_count_ && side_count_ < side_size_ && side_count_ < len_) {
                side_buf_[side_sel_][side_count_++] = loop[i];  // Save what this overdub overwrites
            } else if (k > side_count_ || side_count_ >= side_size_) {
                Commit(i);  // Not one run any more (or out of room): commit from here
            }
            // k < side_count_: saved already (short loop, second time around)
        }
        if (job_ == COPY_UNDO && (i + len_ - job_start_) % len_ >= job_done_)
            return;  // Not in the undo buffer yet (only after a jump): skip rather than corrupt it
        loop[i] += x;
    }

    // Value the loop had before the overdub that's being committed
    float PreDubValue(uint32_t i) const
    {
        uint32_t k = (i + len_ - copy_side_start_) % len_;
        return k < copy_side_count_ ? side_buf_[copy_side_][k] : buf_[cur_][i];
    }

    void RunJob()
    {
        if (job_ == NONE)
            return;
        if (job_ == COPY_UNDO) {
            // Indices counted along, no division per sample: this runs 2048 samples per block
            const float* loop = buf_[cur_];
            const float* side = side_buf_[copy_side_];
            float*       alt  = buf_[cur_ ^ 1];
            uint32_t     n    = len_ - job_done_ < kCopyPerBlock ? len_ - job_done_ : kCopyPerBlock;
            uint32_t     i    = (job_start_ + job_done_) % len_;
            uint32_t     k    = (i + len_ - copy_side_start_) % len_;  // Position in the side buffer
            for (uint32_t j = 0; j < n; j++) {
                alt[i] = k < copy_side_count_ ? side[k] : loop[i];  // The value before the overdub
                if (++i >= len_)
                    i = 0;
                if (++k >= len_)
                    k = 0;
            }
            job_done_ += n;
            if (job_done_ >= len_) {
                job_       = NONE;
                alt_valid_ = true;
                if (undo_pending_) {
                    undo_pending_ = false;
                    SwapBuffers();
                }
            }
        } else {
            float*       loop = buf_[restore_buf_];  // (an undo may have swapped buffers meanwhile)
            const float* side = side_buf_[copy_side_];
            uint32_t     n    = restore_count_ - job_done_ < kCopyPerBlock ? restore_count_ - job_done_ : kCopyPerBlock;
            uint32_t     k    = (job_start_ + job_done_) % restore_count_;
            uint32_t     i    = (restore_start_ + k) % len_;
            for (uint32_t j = 0; j < n; j++) {
                loop[i] = side[k];
                if (++k >= restore_count_) {
                    k = 0;
                    i = restore_start_;
                } else if (++i >= len_) {
                    i = 0;
                }
            }
            job_done_ += n;
            if (job_done_ >= restore_count_)
                job_ = NONE;
        }
    }

    void RunJobToEnd()
    {
        while (job_ != NONE)
            RunJob();
    }

    float    sr_;
    float*   buf_[2];
    float*   side_buf_[2];
    uint32_t capacity_, side_size_, fade_len_;
    float    fade_step_;
    Settings settings_;
    bool     transport_ = true;

    State    state_, before_press_;
    bool     press_started_session_, press_ended_recording_;
    int      cur_;  // Buffer being played; the other is undo / redo

    // Recording
    uint32_t rec_count_, rec_ticks_, end_ticks_, post_roll_left_;

    // Loop and playback
    uint32_t len_, len_ticks_;
    int64_t  anchor_;  // Clock tick where the loop starts
    double   p_, rate_;
    bool     follow_;
    double   xf_p_;
    int      xf_buf_;
    uint32_t xf_left_;
    float    env_;
    bool     stop_pending_, paused_by_midi_;
    StopMode stop_mode_;

    // Overdub
    bool     dub_on_;
    float    dub_env_;
    uint32_t last_idx_;
    bool     last_idx_valid_;
    bool     side_phase_;  // The current overdub can still be cancelled
    int      side_sel_;
    uint32_t side_start_, side_count_, side_elapsed_;

    // Background jobs
    Job      job_;
    uint32_t job_start_, job_done_;
    int      copy_side_, restore_buf_;
    uint32_t copy_side_start_, copy_side_count_, restore_start_, restore_count_;
    bool     alt_valid_, undone_, undo_pending_;
};

} // namespace ceres
