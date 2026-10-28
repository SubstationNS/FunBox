#pragma once
#include "clock.h"
#include "stretch.h"
#include <cmath>
#include <cstdint>
#include <cstring>

//
// Ceres loop track: quantized record / play / overdub / stop, undo / redo, clear.
// Design: docs/looper-design.md, "Recording and playback" and "Track behaviour".
//
//   - Recording starts on a bar line and ends on the bar line nearest the tap (tap early: it
//     records on to it; tap late: it's trimmed back to it), a power-of-two bar count (DIP 2) or
//     the nearest grid step (double tap). A late tap is only applied once the double-tap window
//     has passed, so a double tap can still choose the grid instead.
//   - Length is kept in clock ticks and samples. Playback follows the clock: the position is
//     pulled toward where the clock says the loop should be (rate nudges up to +-0.2 %, a
//     crossfaded jump if it's far off), so loops stay locked to MIDI clock for hours. If the
//     tempo is more than 0.6 % off the recorded one, the loop is time-stretched instead
//     (stretch.h, pitch unchanged), still following the clock.
//   - Overdubbing a stretched loop: the overdub is held aside while the loop is re-rendered at
//     the current tempo into the undo buffer (in the main loop, after the cancel window), then
//     playback crossfades to the new loop and the held overdub is written into it. Undo brings
//     back the original recording, at its own tempo.
//   - Latency: what you play in comes back a round trip after what you heard went out. Overdubs
//     are written that far behind the playhead; first recordings skip half of it (the input side)
//     so the downbeat you played lands on the bar line.
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
        uint32_t preset_bars  = 0;         // Recordings end by themselves after this many bars (0 = off)
        uint32_t latency      = 0;         // Measured round trip (out -> in), samples (loopback calibration)
    };

    static constexpr float  kFadeSeconds   = 0.005f;  // Seams, start/stop, punch in/out
    static constexpr float  kTrimWaitSeconds = 0.45f;  // A late tap waits this long for a double tap (footswitch: 0.4 s)
    static constexpr float  kCancelSeconds = 1.2f;    // An overdub can be cancelled for this long
    static constexpr double kFollowRange   = 0.006;   // Tempo within 0.6 % of the recorded one: follow the clock
    static constexpr double kFollowSeconds = 0.25;    // Phase errors are pulled in over about this time
    static constexpr double kMaxNudge      = 0.002;   // ... with at most +-0.2 % speed change
    static constexpr float  kJumpSeconds   = 0.010f;  // Further off than this: crossfade to the right spot
    static const uint32_t   kCopyPerBlock  = 2048;    // Background copy speed (samples per audio block)
    static const uint32_t   kTicksPerBar   = Clock::kTicksPerBar;

    // a, b: loop and undo buffers, `capacity` samples each. side_a, side_b: `side_size` samples
    // each, at least kCancelSeconds (two, so a new overdub can start while the last one is being committed)
    // hold: `hold_size` samples, where an overdub on a stretched loop waits for the re-render
    void Init(float sample_rate, float* a, float* b, uint32_t capacity, float* side_a, float* side_b,
              uint32_t side_size, float* hold, uint32_t hold_size)
    {
        hold_buf_    = hold;
        hold_size_   = hold_size;
        stagger_     = 0;
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
                    EndRecording(kTicksPerBar, settings_.power_of_two, true, clock);
                    press_ended_recording_ = true;
                }
                break;
            case PLAYING:
                if (stop_pending_) {
                    stop_pending_ = false;  // Changed your mind: keep playing
                } else {
                    StartDub(clock);
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
            case RECORDING: EndRecording(settings_.grid_ticks, false, false, clock); break;  // End on the grid instead
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
        if (render_ != R_NONE) {
            // A stretched overdub is waiting / rendering. If this press just started it, take it
            // back and undo the layer before; otherwise it is the overdub being undone.
            bool this_press = press_started_session_ && render_ == R_HOLD;
            RevertPress();
            if (render_ != R_NONE)
                AbortStretchedDub();
            if (!this_press)
                return;
        } else {
            RevertPress();
        }
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
        else if (before_press_ == RECORDING && state_ == RECORDING) {
            end_ticks_    = 0;
            trim_pending_ = false;
        }
        before_press_          = state_;
        press_started_session_ = false;
    }

    void Clear()
    {
        state_          = EMPTY;
        trim_pending_   = false;
        cur_            = 0;
        len_            = 0;
        len_ticks_      = 0;
        rec_count_      = 0;
        rec_written_    = 0;
        rec_skip_       = 0;
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
        alt_len_        = 0;
        stretching_     = false;
        mode_xf_left_   = 0;
        swap_pending_   = false;
        if (render_ != R_NONE)
            render_gen_++;  // A render in the main loop stops
        render_         = R_NONE;
        hold_count_     = 0;
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
            stretching_     = false;  // Re-decided (and the stretcher reset) next block
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

        // A stretched overdub: render once the cancel window is over (and no copy is using the
        // undo buffer); switch over once the main loop has rendered
        if (render_ == R_HOLD && hold_elapsed_ >= (uint32_t)(kCancelSeconds * sr_) && job_ == NONE)
            RequestRender(clock);
        if (render_ == R_READY)
            SwitchToRendered(clock);

        if (len_ == 0 || state_ == RECORDING || !(Audible() || env_ > 0.0f))
            return;
        double ratio        = clock.Increment() * (double)len_ / (double)len_ticks_;
        bool   want_stretch = std::fabs(ratio - 1.0) >= kFollowRange;
        double expected     = Expected(clock);
        if (want_stretch != stretching_) {  // Crossfade between playing directly and stretching
            if (want_stretch) {
                wsola_.Reset(buf_[cur_], len_, expected, stagger_);
                if (dub_on_ && render_ == R_NONE)
                    EndDub();  // The tempo moved away during an overdub: punch out
            } else {
                p_              = expected;
                last_idx_valid_ = false;
            }
            stretching_   = want_stretch;
            mode_xf_left_ = fade_len_;
        }
        if (stretching_) {
            p_    = expected;  // The stretcher follows the clock's position directly
            rate_ = ratio;
            return;
        }
        if (p_ >= (double)len_)
            p_ = std::fmod(p_, (double)len_);
        double err = expected - p_;
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
    }

    // Main loop: re-render a loop for a stretched overdub, if one is waiting. Blocks until done
    // (or aborted by an undo / clear). Returns true if it rendered.
    bool RunRender(LoopRenderer& r)
    {
        if (render_ != R_RENDER)
            return false;
        uint32_t gen = render_gen_;
        r.Start(buf_[cur_], len_, buf_[cur_ ^ 1], render_new_len_);
        while (!r.Step(8192))
            if (render_gen_ != gen)
                return false;
        if (render_gen_ != gen)
            return false;
        render_ = R_READY;
        return true;
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
                if (tick && rec_count_ > 0)
                    rec_ticks_++;
                if (trim_pending_) {  // Late tap: no double tap came, trim back to the bar line
                    if (rec_count_ >= trim_deadline_)
                        Finish(trim_samples_, end_ticks_);
                } else if (tick && rec_count_ > 0 && end_ticks_ != 0 && rec_ticks_ >= end_ticks_) {
                    Finish(rec_count_, end_ticks_);
                }
                break;
            case ARMED_PLAY:
                if (bar && transport_) {
                    anchor_         = (int64_t)clock.Tick();
                    p_              = 0.0;
                    last_idx_valid_ = false;
                    state_          = PLAYING;
                    stretching_     = false;  // Re-decided (and the stretcher reset) next block
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
                }
                break;
            default: break;
        }

        if (state_ == RECORDING) {
            rec_count_++;  // Time since the bar line
            if (rec_skip_ > 0)
                rec_skip_--;  // Still the input latency: this was played before the bar line
            else
                buf_[cur_][rec_written_++] = in;
            if (rec_written_ + fade_len_ + 1 >= capacity_)
                ForceEnd(clock);
            return 0.0f;
        }
        if (len_ == 0 || state_ == ARMED)
            return 0.0f;

        // Recording the tail past the end, for the seam crossfade
        if (post_roll_left_ > 0) {
            buf_[cur_][rec_written_++] = in;
            if (--post_roll_left_ == 0)
                BakeSeam();
        }

        float target = Audible() && !swap_pending_ ? 1.0f : 0.0f;
        env_         = Ramp(env_, target);
        if (swap_pending_ && env_ == 0.0f)
            FinishDipSwap();  // Undo / redo while stretching: swapped at the bottom of a short dip
        if (env_ == 0.0f && target == 0.0f) {
            dub_env_ = 0.0f;
            return 0.0f;  // Stopped
        }

        float y;
        bool  need_direct = !stretching_ || mode_xf_left_ > 0;
        bool  need_wsola  = stretching_ || mode_xf_left_ > 0;
        float direct = 0.0f, stretched = 0.0f;
        if (need_direct) {
            direct = Read(cur_, p_, len_);
            if (xf_left_ > 0) {  // Crossfade from the old position / buffer
                float g = (float)xf_left_ / (float)fade_len_;
                direct += g * (Read(xf_buf_, xf_p_, xf_len_) - direct);
                xf_p_ += rate_;
                if (xf_p_ >= (double)xf_len_)
                    xf_p_ -= xf_len_;
                xf_left_--;
            }
        }
        if (need_wsola)
            stretched = wsola_.Process(p_, rate_);
        if (mode_xf_left_ > 0) {  // Between direct playback and the stretcher
            float g = (float)mode_xf_left_ / (float)fade_len_;
            float now = stretching_ ? stretched : direct, before = stretching_ ? direct : stretched;
            y = now + g * (before - now);
            mode_xf_left_--;
        } else {
            y = stretching_ ? stretched : direct;
        }

        dub_env_ = Ramp(dub_env_, dub_on_ ? 1.0f : 0.0f);
        if (render_ == R_HOLD || render_ == R_RENDER) {
            // Overdub on a stretched loop: held until the re-rendered loop is there
            if (dub_env_ > 0.0f && hold_count_ < hold_size_)
                hold_buf_[hold_count_++] = in * dub_env_;
            else if (hold_count_ >= hold_size_ && dub_on_)
                EndDub();  // The render is taking too long: punch out, keep what was played
            hold_elapsed_++;
        } else if (dub_env_ > 0.0f && !stretching_ && p_ < (double)len_) {
            WriteDub(in * dub_env_);
        } else {
            last_idx_valid_ = false;  // Not writing: the next overdub starts at the playhead
        }
        if (side_phase_)
            side_elapsed_++;

        p_ += rate_;
        if (p_ >= (double)len_)
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
    bool     Following() const { return !stretching_; }
    bool     Stretching() const { return stretching_; }
    bool     Rendering() const { return render_ == R_HOLD || render_ == R_RENDER; }  // Stretched overdub waiting
    uint32_t Renders() const { return rendered_; }
    uint32_t RenderFailures() const { return render_failures_; }
    void     SetStagger(uint32_t phase) { stagger_ = phase; }  // Track 2: half a grain hop
    double   Position() const { return p_; }              // Samples into the loop

  private:
    enum Job : uint8_t { NONE, COPY_UNDO, RESTORE, FLUSH };
    enum Render : uint8_t { R_NONE, R_HOLD, R_RENDER, R_READY };

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

    // Hermite read around a loop of `len` (the seam is baked, so wrapping is continuous)
    float Read(int b, double pos, uint32_t len) const
    {
        if (pos >= (double)len)
            pos = std::fmod(pos, (double)len);
        const float* x  = buf_[b];
        uint32_t     i  = (uint32_t)pos;
        float        f  = (float)(pos - (double)i);
        uint32_t     im = i == 0 ? len - 1 : i - 1;
        uint32_t     i1 = i + 1 >= len ? i + 1 - len : i + 1;
        uint32_t     i2 = i1 + 1 >= len ? i1 + 1 - len : i1 + 1;
        float        xm1 = x[im], x0 = x[i], x1 = x[i1], x2 = x[i2];
        float        c1 = 0.5f * (x1 - xm1);
        float        c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float        c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    void Jump(double to, int from_buf)
    {
        xf_p_           = p_;
        xf_buf_         = from_buf;
        xf_len_         = len_;
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
        trim_pending_   = false;
        cur_       = 0;
        rec_count_ = 0;
        rec_written_ = 0;
        rec_skip_  = settings_.latency / 2;
        rec_ticks_ = 0;
        end_ticks_ = 0;
        anchor_    = (int64_t)clock.Tick();
        if (settings_.preset_bars > 0)
            end_ticks_ = settings_.preset_bars * kTicksPerBar;
        alt_valid_ = false;
        undone_    = false;
        job_       = NONE;
    }

    // End on the nearest boundary of `step` ticks: the one coming (tap in the second half of a
    // step) or the one just passed (first half: late, trimmed back). Power of two: round the bar
    // count up. `wait`: a trim back waits out the double-tap window first.
    void EndRecording(uint32_t step, bool power_of_two, bool wait, const Clock& clock)
    {
        trim_pending_   = false;
        double   frac   = clock.Position() - std::floor(clock.Position());
        double   pos    = (double)rec_ticks_ + frac;  // Ticks recorded so far
        uint32_t prev   = (uint32_t)(pos / step) * step;
        double   since  = pos - prev;
        double   spt    = clock.SamplesPerTick();
        uint32_t target = (prev > 0 && since <= 0.5 * step) ? prev : prev + step;
        if (power_of_two) {
            uint32_t bars = (target + kTicksPerBar - 1) / kTicksPerBar, p2 = 1;
            while (p2 < bars)
                p2 <<= 1;
            target = p2 * kTicksPerBar;
        }
        if ((double)target <= pos) {
            uint32_t back    = (uint32_t)std::lround((pos - target) * spt);
            uint32_t samples = rec_count_ > back ? rec_count_ - back : rec_count_;
            if (wait) {
                trim_pending_  = true;
                trim_samples_  = samples;
                trim_deadline_ = rec_count_ + (uint32_t)(kTrimWaitSeconds * sr_);
                end_ticks_     = target;
            } else {
                Finish(samples, target);
            }
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
        trim_pending_   = false;
        state_          = PLAYING;
        p_              = std::fmod((double)(rec_count_ - samples), (double)samples);  // Already this far into the next time around
        rate_           = 1.0;
        follow_         = true;
        stretching_     = false;
        mode_xf_left_   = 0;
        alt_len_        = samples;
        last_idx_valid_ = false;
        env_            = 0.0f;
        // Recording carries on until the tail past the end is in (input latency later than the clock)
        uint32_t want   = samples + fade_len_;
        post_roll_left_ = rec_written_ < want ? want - rec_written_ : 0;
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
        uint32_t latency = settings_.latency < len_ / 2 ? settings_.latency : len_ / 2;
        uint32_t behind  = kWriteBehind + latency;  // What you hear went out a round trip ago
        uint32_t i       = (uint32_t)p_ % len_;
        return i >= behind ? i - behind : i + len_ - behind;
    }

    uint32_t NextWriteIndex() const
    {
        if (last_idx_valid_)
            return last_idx_ + 1 >= len_ ? 0 : last_idx_ + 1;
        return WriteIndex();
    }

    void StartDub(const Clock& clock)
    {
        state_  = OVERDUBBING;
        dub_on_ = true;
        if (render_ != R_NONE)
            return;  // Punched in again while the last stretched overdub waits: same one
        if (stretching_) {
            // Hold the overdub aside; the loop gets re-rendered at this tempo first
            render_       = R_HOLD;
            hold_count_   = 0;
            hold_elapsed_ = 0;
            double t      = std::fmod(clock.Position() - (double)anchor_, (double)len_ticks_);
            hold_p0_      = t < 0.0 ? t + len_ticks_ : t;
            return;
        }
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
        if (side_phase_ && render_ == R_NONE)
            Commit(NextWriteIndex());
    }

    // The overdub can't be cancelled any more: fill the undo buffer with the loop as it was
    // before it, in the background, starting at `start` (just ahead of the playhead)
    void Commit(uint32_t start)
    {
        if (job_ == FLUSH)
            RunJobToEnd();  // Rare: a stretched overdub is still being written in
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
        if (render_ != R_NONE) {
            AbortStretchedDub();
            return;
        }
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
        if (stretching_) {  // Through a short dip (two stretchers' worth of state isn't worth it)
            swap_pending_ = true;
            return;
        }
        int      old     = cur_;
        uint32_t old_len = len_;
        cur_ ^= 1;
        len_     = alt_len_;
        alt_len_ = old_len;
        undone_  = !undone_;
        Jump(p_ * len_ / old_len, old);  // Crossfade over to the other version (maybe another length)
        xf_len_ = old_len;
    }

    void FinishDipSwap()
    {
        swap_pending_    = false;
        uint32_t old_len = len_;
        cur_ ^= 1;
        len_     = alt_len_;
        alt_len_ = old_len;
        undone_  = !undone_;
        p_       = p_ * len_ / old_len;
        stretching_   = false;  // Re-decided next block, with a fresh stretcher on this buffer
        mode_xf_left_ = 0;
    }

    // The cancel window of a stretched overdub is over: ask the main loop for the re-render
    void RequestRender(const Clock& clock)
    {
        uint32_t n = (uint32_t)std::lround(len_ticks_ * clock.SamplesPerTickNominal());
        if (n + LoopRenderer::kSeam + 1 > capacity_ || n < 4 * WsolaPlayer::kGrain) {
            AbortStretchedDub();  // Doesn't fit at this tempo
            if (state_ == OVERDUBBING)
                state_ = PLAYING;
            render_failures_++;
            return;
        }
        render_new_len_ = n;
        alt_valid_      = false;  // The undo buffer is about to hold the new version
        render_         = R_RENDER;
    }

    // The re-rendered loop is ready: play it, write the held overdub into it
    void SwitchToRendered(const Clock& clock)
    {
        alt_len_   = len_;
        len_       = render_new_len_;
        cur_ ^= 1;
        alt_valid_ = true;  // The original, untouched, at its own tempo
        undone_    = false;
        side_phase_ = false;
        p_          = Expected(clock);
        last_idx_valid_ = false;
        // The stretcher keeps playing the original for the crossfade
        if (stretching_) {
            stretching_   = false;
            mode_xf_left_ = fade_len_;
        }
        // Held overdub: sample k was played when the clock was at hold_p0_ ticks + k samples
        uint32_t behind = kWriteBehind + (settings_.latency < len_ / 2 ? settings_.latency : len_ / 2);
        int64_t  i0     = (int64_t)std::llround(hold_p0_ * len_ / len_ticks_) - behind;
        flush_idx0_     = (uint32_t)(((i0 % (int64_t)len_) + len_) % len_);
        flush_buf_      = cur_;
        job_            = hold_count_ > 0 ? FLUSH : NONE;
        job_done_       = 0;
        render_         = R_NONE;
        rendered_++;
    }

    void AbortStretchedDub()
    {
        if (render_ == R_RENDER)
            alt_valid_ = false;  // The render had started overwriting the undo buffer
        render_gen_++;           // A render in the main loop stops
        render_     = R_NONE;
        hold_count_ = 0;
        dub_on_     = false;
        dub_env_    = 0.0f;
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
        if (job_ == FLUSH) {  // The held overdub into the re-rendered loop
            float*   loop = buf_[flush_buf_];
            uint32_t n    = hold_count_ - job_done_ < kCopyPerBlock ? hold_count_ - job_done_ : kCopyPerBlock;
            uint32_t i    = (uint32_t)(((uint64_t)flush_idx0_ + job_done_) % len_);
            for (uint32_t j = 0; j < n; j++) {
                loop[i] += hold_buf_[job_done_ + j];
                if (++i >= len_)
                    i = 0;
            }
            job_done_ += n;
            if (job_done_ >= hold_count_) {
                job_        = NONE;
                hold_count_ = 0;
            }
            return;
        }
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
    bool     trim_pending_ = false;               // Late tap: trimming back once the double-tap window passes
    uint32_t trim_samples_ = 0, trim_deadline_ = 0;
    uint32_t rec_written_, rec_skip_;  // Samples in the buffer; input latency still to skip

    // Loop and playback
    uint32_t len_, len_ticks_;
    int64_t  anchor_;  // Clock tick where the loop starts
    double   p_, rate_;
    bool     follow_;
    double   xf_p_;
    int      xf_buf_;
    uint32_t xf_left_, xf_len_ = 1;
    uint32_t alt_len_ = 0;  // Length of the undo / redo version (a re-render changes it)

    // Time stretch
    WsolaPlayer       wsola_;
    bool              stretching_ = false, swap_pending_ = false;
    uint32_t          stagger_ = 0;  // Grain phase offset vs. the other track (their searches don't coincide)
    uint32_t          mode_xf_left_ = 0;
    float*            hold_buf_ = nullptr;
    uint32_t          hold_size_ = 0, hold_count_ = 0, hold_elapsed_ = 0;
    double            hold_p0_ = 0.0;
    volatile Render   render_ = R_NONE;
    volatile uint32_t render_gen_ = 0;
    uint32_t          render_new_len_ = 0, flush_idx0_ = 0, rendered_ = 0, render_failures_ = 0;
    int               flush_buf_ = 0;
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
