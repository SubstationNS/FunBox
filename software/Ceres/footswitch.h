#pragma once
#include <cstdint>

//
// Footswitch gestures: tap, double tap, hold 1 s, hold 2 s.
//
// A tap is reported the moment the switch goes down (no waiting to see whether a second tap
// follows), so punching in is instant. A second press soon after is reported as DOUBLE_TAP
// instead of TAP, and holds are reported while still held; the track then reverts whatever the
// first tap did when it turns out to be part of a bigger gesture.
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class Footswitch
{
  public:
    enum Event : uint8_t
    {
        TAP        = 1 << 0,
        DOUBLE_TAP = 1 << 1,
        HOLD_UNDO  = 1 << 2,  // Held 1 s
        HOLD_CLEAR = 1 << 3,  // Held 2 s (comes after HOLD_UNDO)
    };

    static constexpr double kDoubleTapSeconds = 0.4;
    static constexpr double kUndoSeconds      = 1.0;
    static constexpr double kClearSeconds     = 2.0;

    // Once per audio block: the debounced switch state and the time in seconds
    uint8_t Update(bool pressed, double now)
    {
        uint8_t events = 0;
        if (pressed && !down_) {
            // A press after a double tap or a hold starts over
            bool second = now - last_press_ < kDoubleTapSeconds && !last_was_double_ && !last_was_hold_;
            events |= second ? DOUBLE_TAP : TAP;
            last_was_double_ = second;
            last_was_hold_   = false;
            last_press_      = now;
            held_            = 0;
        } else if (pressed && down_) {
            double held = now - last_press_;
            if (!(held_ & HOLD_UNDO) && held >= kUndoSeconds) {
                events |= HOLD_UNDO;
                held_ |= HOLD_UNDO;
                last_was_hold_ = true;
            }
            if (!(held_ & HOLD_CLEAR) && held >= kClearSeconds) {
                events |= HOLD_CLEAR;
                held_ |= HOLD_CLEAR;
            }
        }
        down_ = pressed;
        return events;
    }

    double LastPress() const { return last_press_; }  // Seconds

  private:
    bool    down_            = false;
    bool    last_was_double_ = false;
    bool    last_was_hold_   = false;
    double  last_press_      = -1e9;
    uint8_t held_            = 0;
};

} // namespace ceres
