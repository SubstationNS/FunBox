#pragma once
#include "clock.h"
#include <cmath>
#include <cstdint>

//
// Steers the internal Clock from incoming MIDI clock (the "flywheel PLL").
//
// Each 0xF8 is timestamped on arrival (in audio samples). A straight line is fitted through the
// arrival times of the last two bars of ticks: its slope is the tempo, and its end is where the
// newest tick "really" was, with the jitter averaged out. The clock gets the fitted tempo, and
// its phase is pulled onto the line with a small rate trim (clamped to +-0.2 %), so it never jumps.
//
// A real tempo change shows up as the newest ticks drifting off the line through the older ones.
// When that drift is too big to be jitter, the fit restarts from the recent ticks, so the tempo
// re-follows within about half a second instead of lagging two bars behind. While the master's
// tempo is moving (until two bars pass without a change), the phase is pulled in faster and the
// trim may go to +-2 %: the loops can't follow a tempo change anyway until the stretcher exists.
//
// Ticks are counted, not inferred from their timing, so even badly bunched-up ticks (USB-MIDI)
// never shift the bar position.
//
// Transport: Start / Continue locate the clock on the next tick (bar 1, or the song position),
// Stop freezes the song position, Song Position Pointer moves it. Losing the clock changes
// nothing audible: the clock keeps running at the last tempo, and the bar position is kept
// when it comes back (only the sub-tick phase gets corrected).
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class MidiSync
{
  public:
    enum Event : uint8_t
    {
        EV_LOCKED   = 1 << 0,  // Clock found, now steering
        EV_LOST     = 1 << 1,  // Clock gone, free-running
        EV_START    = 1 << 2,  // Start received; the clock moves to bar 1 on the next tick (EV_LOCATED)
        EV_CONTINUE = 1 << 3,  // Continue received; the clock moves to the song position on the next tick
        EV_STOP     = 1 << 4,
        EV_LOCATED  = 1 << 5,  // The clock was moved to the song position (first tick after Start / Continue)
    };

    static const int    kWindow       = 192;    // Ticks in the fit: 2 bars
    static const int    kMinFit       = 8;      // Ticks before the first fit
    static const int    kRecent       = 12;     // Ticks checked for drift (tempo change detection)
    static constexpr double kDetectSigmas = 4.5;  // How unlikely the drift must be as jitter
    static constexpr double kLossSeconds  = 0.3;  // No tick for this long = clock lost
    static constexpr double kPhaseSeconds = 1.5;  // Phase errors are corrected over about this time
    static constexpr double kMaxTrim      = 0.002;  // +-0.2 % rate correction, about 3.5 cents
    static constexpr double kChangingPhaseSeconds = 0.4;  // ... while the master's tempo is changing
    static constexpr double kChangingMaxTrim      = 0.02;
    static constexpr double kJitterFloorMs = 0.1;   // Don't trust the fit to be better than this

    void Init(float sample_rate)
    {
        sample_rate_    = sample_rate;
        head_ = count_  = 0;
        newest_         = 0;
        locked_         = false;
        playing_        = true;  // Without MIDI transport the pedal just plays
        position_known_ = false;
        locate_pending_ = false;
        from_locate_    = false;
        changing_       = false;
        detections_     = 0;
        song_position_  = 0;
        last_tick_time_ = 0.0;
        tempo_          = 120.0f;
        phase_error_    = 0.0;
        sigma_          = kJitterFloorMs * 0.001 * sample_rate;
        events_         = 0;
    }

    // All times are in samples, on the same time base as `now`: the sample the clock is at
    // (the start of the audio block being processed). Messages must be fed in arrival order.
    void OnClock(double t, Clock& clock, double now)
    {
        last_tick_time_ = t;

        if (locate_pending_) {
            // This tick is the song position. If the clock was already running before the Start /
            // Continue and the tick arrived on its grid, the fitted line knows better than the
            // (jittered) arrival when it really happened, and the fit carries on.
            locate_pending_ = false;
            double when     = t;
            bool   on_grid  = false;
            if (locked_ && count_ >= kMinFit) {
                double t0, period;
                Fit(0, count_ - 1, t0, period);
                double predicted = t0 + period * count_;
                // Generous: USB-MIDI often holds single ticks up by several ms. A master that
                // really did restart its clock on a new grid gets caught by the drift detection.
                on_grid          = std::fabs(t - predicted) < 0.4 * period;
                when             = on_grid ? predicted : t;
            }
            if (!on_grid) {
                count_       = 0;
                from_locate_ = true;
            }
            Push(t, song_position_);
            // The clock has moved on a little since then
            clock.Locate((double)song_position_ + (now - when) * clock.Increment());
            position_known_ = true;
            phase_error_    = 0.0;
            events_ |= EV_LOCATED;
            if (on_grid)
                Steer(clock, now);
            else
                clock.SetRateTrim(0.0f);
            return;
        }

        if (count_ == 0) {
            // First tick after power-on or a clock loss: it belongs to the nearest clock tick
            from_locate_ = false;
            Push(t, llround(PositionAt(t, clock, now)));
        } else {
            Push(t, newest_ + 1);
        }

        if (count_ >= kMinFit)
            Steer(clock, now);
    }

    void OnStart()
    {
        playing_        = true;
        song_position_  = 0;
        locate_pending_ = true;
        events_ |= EV_START;
    }

    void OnContinue()
    {
        playing_        = true;
        locate_pending_ = true;
        events_ |= EV_CONTINUE;
    }

    void OnStop(const Clock& clock)
    {
        if (playing_)  // The next tick would have been this position
            song_position_ = count_ > 0 ? newest_ + 1 : (int64_t)clock.Tick() + 1;
        playing_        = false;
        locate_pending_ = false;
        events_ |= EV_STOP;
    }

    // Song Position Pointer, in 16th notes (6 ticks); takes effect on the next Continue
    void OnSongPosition(uint16_t sixteenths) { song_position_ = (int64_t)sixteenths * 6; }

    // Once per audio block, after the messages: clock loss detection
    void Update(double now, Clock& clock)
    {
        if (count_ > 0 && now - last_tick_time_ > kLossSeconds * sample_rate_) {
            count_       = 0;
            phase_error_ = 0.0;
            clock.SetRateTrim(0.0f);  // Keep the tempo, drop the phase correction
            if (locked_) {
                locked_ = false;
                events_ |= EV_LOST;
            }
        }
    }

    bool    Locked() const { return locked_; }          // Receiving clock and steering
    bool    Playing() const { return playing_; }        // False after a MIDI Stop, until Start / Continue
    bool    TempoChanging() const { return changing_; }  // The master's tempo is moving
    bool    PositionKnown() const { return position_known_; }  // Bar position came from Start / Continue
    float   Tempo() const { return tempo_; }            // Last fitted tempo
    double  PhaseErrorTicks() const { return phase_error_; }  // Fitted MIDI position - clock position
    double  JitterSamples() const { return sigma_; }    // RMS deviation of the ticks from the fit
    int     FitTicks() const { return count_; }         // Ticks in the fit right now
    uint32_t TempoChanges() const { return detections_; }  // Times the fit restarted for a tempo change
    uint8_t TakeEvents()
    {
        uint8_t e = events_;
        events_   = 0;
        return e;
    }

  private:
    void Steer(Clock& clock, double now)
    {
        double t0, period;
        Fit(0, count_ - 1, t0, period);
        UpdateJitter(t0, period);

        if (count_ >= 3 * kRecent && DriftDetected()) {
            Drop(count_ - kRecent);  // Tempo changed: start over from the recent ticks
            Fit(0, count_ - 1, t0, period);
            changing_ = true;
            detections_++;
        } else if (count_ == kWindow) {
            changing_ = false;  // Two bars without a change
        }

        float bpm = (float)(sample_rate_ * 60.0 / (period * Clock::kTicksPerBeat));
        if (!(bpm > 20.0f && bpm < 400.0f))
            return;  // Nonsense (e.g. ticks bunched up right after the clock appeared)
        tempo_ = bpm;

        double t_end = t0 + period * (count_ - 1);  // Where the newest tick really was
        if (!locked_) {
            locked_ = true;
            events_ |= EV_LOCKED;
            clock.SetRateTrim(0.0f);
            clock.SetTempo(bpm);  // The only tempo step: going from the knob tempo to the MIDI tempo
            double error = (double)newest_ - PositionAt(t_end, clock, now);
            if (from_locate_) {
                // Just after a Start / Continue the position is known exactly. If the clock ran
                // at the wrong tempo until now, put it right while the song is only a few ticks in.
                if (std::fabs(error) > 0.25)
                    clock.Locate((double)newest_ + (now - t_end) * clock.Increment());
            } else {
                newest_ -= llround(error);  // Bar position unknown or kept: only fix the sub-tick phase
            }
        }
        clock.SetTempo(bpm);

        phase_error_    = (double)newest_ - PositionAt(t_end, clock, now);
        double seconds  = changing_ ? kChangingPhaseSeconds : kPhaseSeconds;
        double max_trim = changing_ ? kChangingMaxTrim : kMaxTrim;
        double trim     = phase_error_ * period / (seconds * sample_rate_);
        trim            = trim > max_trim ? max_trim : (trim < -max_trim ? -max_trim : trim);
        clock.SetRateTrim((float)trim);
    }

    // Where the clock was at time t (slightly in the past or future of now)
    static double PositionAt(double t, const Clock& clock, double now)
    {
        return clock.Position() - (now - t) * clock.Increment();
    }

    // Window entry i (0 = oldest) and bookkeeping. Entries are consecutive ticks; the newest
    // one belongs to clock tick newest_.
    double At(int i) const { return times_[(head_ + kWindow - count_ + i) % kWindow]; }

    void Push(double t, int64_t index)
    {
        times_[head_] = t;
        head_         = (head_ + 1) % kWindow;
        if (count_ < kWindow)
            count_++;
        newest_ = index;
    }

    void Drop(int oldest) { count_ -= oldest; }

    // Least-squares line through entries first..last: time(i) = t0 + period * (i - first)
    void Fit(int first, int last, double& t0, double& period) const
    {
        int    n   = last - first + 1;
        double ref = At(first), xm = 0.5 * (n - 1), tm = 0.0, sxt = 0.0;
        for (int i = 0; i < n; i++)
            tm += At(first + i) - ref;
        tm /= n;
        for (int i = 0; i < n; i++)
            sxt += (i - xm) * (At(first + i) - ref - tm);
        double sxx = n * ((double)n * n - 1.0) / 12.0;
        period     = sxt / sxx;
        t0         = ref + tm - period * xm;
    }

    void UpdateJitter(double t0, double period)
    {
        double sum = 0.0;
        for (int i = 0; i < count_; i++) {
            double r = At(i) - (t0 + period * i);
            sum += r * r;
        }
        double floor = kJitterFloorMs * 0.001 * sample_rate_;
        sigma_       = std::sqrt(sum / (count_ - 2));
        sigma_       = sigma_ < floor ? floor : sigma_;
    }

    // Do the newest kRecent ticks sit off the line through the older ones by more than jitter explains?
    bool DriftDetected() const
    {
        int    m = count_ - kRecent;
        double t0, period;
        Fit(0, m - 1, t0, period);
        double clip = 3.0 * sigma_, sum = 0.0;
        for (int i = m; i < count_; i++) {
            double r = At(i) - (t0 + period * i);
            sum += r > clip ? clip : (r < -clip ? -clip : r);  // A single late tick isn't a tempo change
        }
        double mean = sum / kRecent;
        // Spread of that mean without a tempo change: the recent ticks' own jitter, plus the
        // uncertainty of extending the older line out to them
        double d   = 0.5 * (m + kRecent);
        double var = 1.0 / kRecent + 1.0 / m + 12.0 * d * d / ((double)m * m * m - m);
        return std::fabs(mean) > kDetectSigmas * sigma_ * std::sqrt(var);
    }

    float   sample_rate_;
    double  times_[kWindow];
    int     head_, count_;
    int64_t newest_;
    bool    locked_, playing_, position_known_, locate_pending_, from_locate_, changing_;
    int64_t song_position_;  // In ticks: where the next Continue starts
    double  last_tick_time_;
    float   tempo_;
    double  phase_error_, sigma_;
    uint8_t events_;
    uint32_t detections_;
};

} // namespace ceres
