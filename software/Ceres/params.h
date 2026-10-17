#pragma once
#include <cmath>
#include <cstdint>

//
// Ceres parameters with several sources each (knob, expression pedal, MIDI CC), and the A/B
// expression morph. Design: docs/looper-design.md, "Expression" and "State persistence".
//
//   - Pickup (DeepMind-style): a knob or the pedal is ignored until it has moved past a small
//     threshold (pot noise, an unplugged jack, a curious kid's finger on a knob that doesn't move it
//     far don't count). After that it takes over a parameter once it crosses the parameter's
//     current value, never with a jump. The source that crossed last owns the parameter; the one
//     that lost it has to move again (past the threshold) before it can take it back, so a knob
//     resting where the pedal passes by doesn't grab it.
//   - MIDI CC jumps straight to its value: a CC is a deliberate value (and DAW automation would
//     feel dead with pickup).
//   - Morph: DIP 4 on captures state A (the morphable values + pedal position), DIP 4 off captures
//     B. Parameters that differ between A and B follow the pedal from A to B between the two
//     pedal positions (clamped outside). Toggling DIP 4 without changing anything clears the morph.
//     DIP 4 already on at power-up is ignored until it's toggled.
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

class Controls
{
  public:
    enum Id
    {
        T1_LEVEL,
        T2_LEVEL,
        T1_FILTER,
        T2_FILTER,
        FX_AMOUNT,
        kCount
    };

    enum Owner : uint8_t
    {
        NONE,   // Restored value, nothing has taken over yet
        KNOB,
        PEDAL,
        CC,
    };

    // Events from Update(), for LED feedback
    enum Event : uint16_t
    {
        TAKEOVER_T1   = 1 << 0,  // A knob or the pedal just took over a Track 1 parameter
        TAKEOVER_T2   = 1 << 1,
        TAKEOVER_FX   = 1 << 2,
        LEARN_A       = 1 << 3,  // DIP 4 on: state A captured
        LEARN_B       = 1 << 4,  // DIP 4 off: morph built
        MORPH_CLEARED = 1 << 5,  // DIP 4 off, nothing differed: no morph
    };

    struct Morph
    {
        bool    valid;
        uint8_t mask;  // Bit per Id: follows the pedal
        float   a[kCount], b[kCount];
        float   pedal_a, pedal_b;
    };

    static constexpr float kMoveThreshold  = 0.03f;   // A control has to move this far to count
    static constexpr float kCatch          = 0.008f;  // This close to the value counts as crossing it
    static constexpr float kDiffers        = 0.02f;   // A and B differ for a parameter
    static constexpr float kPedalMinTravel = 0.05f;   // A and B pedal positions must be this far apart
    static const int       kSettleBlocks   = 200;     // Readings ramp up from 0 after power-on (ADC
                                                      // smoothing): ignore them for 200 ms

    // values: restored (from a snapshot), or nullptr for defaults. Restored values stay until a
    // control takes over by pickup; with defaults (nothing saved yet) the knobs apply right away.
    void Init(const float* values, const Morph* morph)
    {
        static const float kDefaults[kCount] = {0.7071f, 0.7071f, 0.5f, 0.5f, 0.0f};  // Unity, flat, FX off
        for (int i = 0; i < kCount; i++) {
            p_[i].value      = values ? values[i] : kDefaults[i];
            p_[i].owner      = NONE;
            p_[i].knob_prev  = 0.0f;
            p_[i].pedal_prev = 0.0f;
            p_[i].knob_ref_valid = false;
            p_[i].knob_armed     = values == nullptr;  // Nothing to protect: knobs apply at once
            p_[i].pedal_armed    = false;
        }
        if (morph)
            morph_ = *morph;
        else
            morph_ = Morph{};
        pedal_ref_valid_ = false;
        dip_valid_       = false;
        settle_          = kSettleBlocks;
        last_pedal_      = 0.0f;
        for (int i = 0; i < kCount; i++)
            last_knobs_[i] = 0.0f;
        learning_        = false;
    }

    // Once per audio block: raw knob readings (by Id), the expression pedal, DIP 4.
    // Returns Event flags.
    uint16_t Update(const float* knobs, float pedal, bool dip4)
    {
        if (settle_ > 0) {  // Power-on: the readings aren't real yet, take the references afterwards
            settle_--;
            return 0;
        }
        uint16_t events = Learn(dip4, pedal, knobs);
        if (!pedal_ref_valid_) {
            for (int i = 0; i < kCount; i++)
                p_[i].pedal_ref = pedal;
            pedal_ref_valid_ = true;
        }

        for (int i = 0; i < kCount; i++) {
            Param& p    = p_[i];
            bool   took = false;

            // Knob
            if (!p.knob_ref_valid) {
                p.knob_ref       = knobs[i];
                p.knob_ref_valid = true;
                p.knob_prev      = knobs[i];
                if (p.knob_armed) {  // Defaults: the knob is the value
                    p.owner = KNOB;
                    p.value = knobs[i];
                }
            } else if (!p.knob_armed && std::fabs(knobs[i] - p.knob_ref) > kMoveThreshold) {
                p.knob_armed = true;  // Crossing is judged from where it started
                p.knob_prev  = p.knob_ref;
            }
            if (p.knob_armed) {
                if (p.owner != KNOB && Crossed(p.knob_prev, knobs[i], p.value)) {
                    TakeOver(i, KNOB, knobs, pedal);
                    took = true;
                }
                if (p.owner == KNOB)
                    p.value = knobs[i];
                p.knob_prev = knobs[i];
            }

            // Pedal, for parameters in the morph (not while learning: you're setting it up)
            if (MorphActive() && (morph_.mask & (1 << i))) {
                float m = MorphValue(i, pedal);
                if (!p.pedal_armed && std::fabs(pedal - p.pedal_ref) > kMoveThreshold) {
                    p.pedal_armed = true;
                    p.pedal_prev  = MorphValue(i, p.pedal_ref);
                }
                if (p.pedal_armed) {
                    if (p.owner != PEDAL && Crossed(p.pedal_prev, m, p.value)) {
                        TakeOver(i, PEDAL, knobs, pedal);
                        took = true;
                    }
                    if (p.owner == PEDAL)
                        p.value = m;
                    p.pedal_prev = m;
                }
            }

            if (took)
                events |= i == FX_AMOUNT ? TAKEOVER_FX : ((i == T1_LEVEL || i == T1_FILTER) ? TAKEOVER_T1 : TAKEOVER_T2);
        }
        return events;
    }

    // MIDI CC: straight to the value (0..1)
    void Cc(Id id, float value)
    {
        p_[id].value = value;
        if (p_[id].owner != CC)
            TakeOver(id, CC, last_knobs_, last_pedal_);
    }

    float Value(Id id) const { return p_[id].value; }
    Owner GetOwner(Id id) const { return p_[id].owner; }
    bool  Learning() const { return learning_; }
    bool  MorphActive() const { return morph_.valid && !learning_; }
    const Morph& GetMorph() const { return morph_; }
    const float* Values() const { return values_cache(); }

    float MorphValue(int i, float pedal) const
    {
        float t = (pedal - morph_.pedal_a) / (morph_.pedal_b - morph_.pedal_a);
        t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return morph_.a[i] + t * (morph_.b[i] - morph_.a[i]);
    }

  private:
    struct Param
    {
        float value;
        Owner owner;
        float knob_ref, knob_prev, pedal_ref, pedal_prev;
        bool  knob_ref_valid, knob_armed, pedal_armed;
    };

    // `owner` takes parameter i; the others have to move again before they can take it back
    void TakeOver(int i, Owner owner, const float* knobs, float pedal)
    {
        Param& p = p_[i];
        p.owner  = owner;
        if (owner != KNOB) {
            p.knob_armed = false;
            p.knob_ref   = knobs[i];
        }
        if (owner != PEDAL) {
            p.pedal_armed = false;
            p.pedal_ref   = pedal;
        }
    }

    // Did a source moving from a to b pass (or land on) v?
    static bool Crossed(float a, float b, float v)
    {
        return (a - v) * (b - v) <= 0.0f || std::fabs(b - v) < kCatch;
    }

    // DIP 4 edges: on = capture A, off = capture B and build the morph
    uint16_t Learn(bool dip, float pedal, const float* knobs)
    {
        for (int i = 0; i < kCount; i++)
            last_knobs_[i] = knobs[i];
        last_pedal_ = pedal;
        if (!dip_valid_) {  // On at power-up: ignored until toggled
            dip_prev_  = dip;
            dip_valid_ = true;
            return 0;
        }
        if (dip == dip_prev_)
            return 0;
        dip_prev_ = dip;
        if (dip) {
            learning_ = true;
            for (int i = 0; i < kCount; i++)
                learn_.a[i] = p_[i].value;
            learn_.pedal_a = pedal;
            return LEARN_A;
        }
        if (!learning_)
            return 0;
        learning_ = false;
        Morph m   = learn_;
        m.mask    = 0;
        for (int i = 0; i < kCount; i++) {
            m.b[i] = p_[i].value;
            if (std::fabs(m.b[i] - m.a[i]) > kDiffers)
                m.mask |= 1 << i;
        }
        m.pedal_b = pedal;
        m.valid   = m.mask != 0 && std::fabs(m.pedal_b - m.pedal_a) > kPedalMinTravel;
        if (!m.valid) {
            morph_ = Morph{};
            return MORPH_CLEARED;
        }
        morph_ = m;
        // The pedal is at B and everything sits at B: the pedal owns the morphed parameters now
        for (int i = 0; i < kCount; i++)
            if (m.mask & (1 << i)) {
                TakeOver(i, PEDAL, knobs, pedal);
                p_[i].pedal_armed = true;
                p_[i].pedal_prev  = m.b[i];
            }
        return LEARN_B;
    }

    const float* values_cache() const
    {
        for (int i = 0; i < kCount; i++)
            cache_[i] = p_[i].value;
        return cache_;
    }

    Param         p_[kCount];
    Morph         morph_, learn_;
    bool          pedal_ref_valid_;
    float         last_knobs_[kCount], last_pedal_;
    bool          dip_prev_, dip_valid_, learning_;
    int           settle_;
    mutable float cache_[kCount];
};

} // namespace ceres
