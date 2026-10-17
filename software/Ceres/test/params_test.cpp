// Host tests for params.h (pickup, MIDI CC, A/B morph) and persist.h (snapshot slots):
//   c++ -std=c++17 -O2 -I.. params_test.cpp -o params_test && ./params_test
#include "params.h"
#include "persist.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using ceres::Controls;
using ceres::Snapshot;
using ceres::SnapshotStore;

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            failures++;                                      \
        }                                                    \
    } while (0)

static bool Near(float a, float b, float tol = 0.002f) { return std::fabs(a - b) <= tol; }

// The physical controls of a pedal, fed to Controls once per "block"
struct Panel
{
    Controls c;
    float    knobs[Controls::kCount] = {0.2f, 0.9f, 0.5f, 0.5f, 0.0f};
    float    pedal = 0.0f;
    bool     dip4  = false;
    uint16_t events = 0;

    // Power-on: run through the settling time with the readings ramping up from 0, like the ADC does
    void PowerOn()
    {
        for (int b = 0; b < Controls::kSettleBlocks; b++) {
            float k[Controls::kCount];
            float f = b < 50 ? b / 50.0f : 1.0f;
            for (int i = 0; i < Controls::kCount; i++)
                k[i] = knobs[i] * f;
            events |= c.Update(k, pedal * f, dip4);
        }
    }

    void Step(int blocks = 1)
    {
        for (int i = 0; i < blocks; i++)
            events |= c.Update(knobs, pedal, dip4);
    }
    // Move a knob there in small steps, like a hand does
    void Turn(int id, float to, int steps = 50)
    {
        float from = knobs[id];
        for (int s = 1; s <= steps; s++) {
            knobs[id] = from + (to - from) * s / steps;
            Step();
        }
    }
    void Sweep(float to, int steps = 50)
    {
        float from = pedal;
        for (int s = 1; s <= steps; s++) {
            pedal = from + (to - from) * s / steps;
            Step();
        }
    }
};

static void TestDefaults()
{
    std::printf("params: nothing saved yet\n");
    Panel p;
    p.c.Init(nullptr, nullptr);
    p.PowerOn();
    p.Step();
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.2f) && Near(p.c.Value(Controls::T2_LEVEL), 0.9f),
          "knobs apply right away: %.3f %.3f", p.c.Value(Controls::T1_LEVEL), p.c.Value(Controls::T2_LEVEL));
    p.Turn(Controls::T1_LEVEL, 0.6f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.6f), "and follow: %.3f", p.c.Value(Controls::T1_LEVEL));
}

static void TestRestoreAndPickup()
{
    std::printf("params: restored, pickup\n");
    float saved[Controls::kCount] = {0.7f, 0.7f, 0.5f, 0.5f, 0.3f};
    Panel p;
    p.c.Init(saved, nullptr);
    p.PowerOn();
    p.Step(10);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.7f) && p.c.GetOwner(Controls::T1_LEVEL) == Controls::NONE,
          "restored value stays through the power-on ramp, knob elsewhere: %.3f", p.c.Value(Controls::T1_LEVEL));

    // Pot noise / a brush that doesn't really move it: nothing
    for (int i = 0; i < 200; i++) {
        p.knobs[Controls::T1_LEVEL] = 0.2f + 0.02f * std::sin(i * 0.7f);
        p.Step();
    }
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.7f), "noise doesn't take over: %.3f", p.c.Value(Controls::T1_LEVEL));

    // Moved, but away from the value: still the restored value
    p.Turn(Controls::T1_LEVEL, 0.05f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.7f) && p.c.GetOwner(Controls::T1_LEVEL) == Controls::NONE,
          "moving away doesn't take over");
    // Toward it, past it: takes over at the crossing, then follows
    p.events = 0;
    p.Turn(Controls::T1_LEVEL, 0.65f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.7f), "not there yet: %.3f", p.c.Value(Controls::T1_LEVEL));
    p.Turn(Controls::T1_LEVEL, 0.8f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.8f) && p.c.GetOwner(Controls::T1_LEVEL) == Controls::KNOB,
          "crossed: knob owns it: %.3f", p.c.Value(Controls::T1_LEVEL));
    CHECK(p.events & Controls::TAKEOVER_T1, "takeover reported (LED flash)");
    // Others untouched
    CHECK(Near(p.c.Value(Controls::FX_AMOUNT), 0.3f), "other parameters keep their values");
}

// The knob is above the saved value: its power-on ramp from 0 passes through it, which must not count
static void TestPowerOnRamp()
{
    std::printf("params: power-on ramp\n");
    float saved[Controls::kCount] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    Panel p;
    for (int i = 0; i < Controls::kCount; i++)
        p.knobs[i] = 0.9f;
    p.c.Init(saved, nullptr);
    p.PowerOn();
    p.Step(100);
    bool all = true;
    for (int i = 0; i < Controls::kCount; i++)
        all &= Near(p.c.Value((Controls::Id)i), 0.5f) && p.c.GetOwner((Controls::Id)i) == Controls::NONE;
    CHECK(all, "ramp through the saved values doesn't take over (T1 %.3f)", p.c.Value(Controls::T1_LEVEL));
}

static void TestCc()
{
    std::printf("params: MIDI CC\n");
    Panel p;
    p.c.Init(nullptr, nullptr);
    p.PowerOn();
    p.Step();
    p.c.Cc(Controls::T2_FILTER, 0.9f);
    p.Step(5);
    CHECK(Near(p.c.Value(Controls::T2_FILTER), 0.9f), "CC jumps: %.3f", p.c.Value(Controls::T2_FILTER));
    p.Turn(Controls::T2_FILTER, 0.6f);
    CHECK(Near(p.c.Value(Controls::T2_FILTER), 0.9f), "knob below doesn't steal it");
    p.Turn(Controls::T2_FILTER, 0.95f);
    CHECK(Near(p.c.Value(Controls::T2_FILTER), 0.95f), "knob takes it back after crossing");
}

static void TestMorph()
{
    std::printf("params: A/B morph\n");
    Panel p;
    p.dip4 = true;  // On at power-up: ignored
    p.c.Init(nullptr, nullptr);
    p.PowerOn();
    p.knobs[Controls::T1_LEVEL] = 0.7f;
    p.knobs[Controls::T2_LEVEL] = 0.0f;
    p.Step(5);
    CHECK(!p.c.Learning(), "DIP 4 on at power-up is ignored");
    p.dip4 = false;
    p.Step();
    CHECK(!p.c.MorphActive(), "turning it off: no morph");

    // Toggle without changes: cleared
    p.dip4 = true;
    p.Step();
    p.dip4 = false;
    p.events = 0;
    p.Step();
    CHECK((p.events & Controls::MORPH_CLEARED) && !p.c.MorphActive(), "toggle with no change: cleared");

    // Crossfader: A = T1 up, T2 down, pedal heel (0.1); B = T1 down, T2 up, pedal toe (0.9)
    p.pedal = 0.1f;
    p.Step();
    p.dip4 = true;
    p.events = 0;
    p.Step();
    CHECK((p.events & Controls::LEARN_A) && p.c.Learning(), "DIP 4 on: A captured");
    p.Turn(Controls::T1_LEVEL, 0.0f);
    p.Turn(Controls::T2_LEVEL, 0.7f);
    p.Sweep(0.9f);
    p.dip4 = false;
    p.events = 0;
    p.Step();
    CHECK((p.events & Controls::LEARN_B) && p.c.MorphActive(), "DIP 4 off: morph built");
    CHECK(p.c.GetMorph().mask == ((1 << Controls::T1_LEVEL) | (1 << Controls::T2_LEVEL)),
          "only the two levels are mapped (mask %02x)", p.c.GetMorph().mask);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.0f) && Near(p.c.Value(Controls::T2_LEVEL), 0.7f), "nothing jumped");

    p.Sweep(0.5f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.35f, 0.01f) && Near(p.c.Value(Controls::T2_LEVEL), 0.35f, 0.01f),
          "half way: both at 0.35 (%.3f %.3f)", p.c.Value(Controls::T1_LEVEL), p.c.Value(Controls::T2_LEVEL));
    p.Sweep(0.0f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.7f) && Near(p.c.Value(Controls::T2_LEVEL), 0.0f),
          "past the heel position: clamped to A (%.3f %.3f)", p.c.Value(Controls::T1_LEVEL), p.c.Value(Controls::T2_LEVEL));
    CHECK(Near(p.c.Value(Controls::T1_FILTER), 0.5f), "unmapped parameters don't move");

    // Knob takes T1 over (after crossing), the pedal takes it back once its morph value crosses
    p.Turn(Controls::T1_LEVEL, 0.75f);  // Knob from 0.0 up past 0.7
    CHECK(p.c.GetOwner(Controls::T1_LEVEL) == Controls::KNOB, "knob took T1 over");
    p.Turn(Controls::T1_LEVEL, 0.5f);
    CHECK(Near(p.c.Value(Controls::T1_LEVEL), 0.5f), "knob sets it: %.3f", p.c.Value(Controls::T1_LEVEL));
    p.Sweep(0.2f);  // Morph value for T1 goes 0.7 -> 0.61: doesn't reach 0.5
    CHECK(p.c.GetOwner(Controls::T1_LEVEL) == Controls::KNOB, "pedal not there yet");
    p.Sweep(0.7f);  // Morph value 0.61 -> 0.175: crosses 0.5
    CHECK(p.c.GetOwner(Controls::T1_LEVEL) == Controls::PEDAL && Near(p.c.Value(Controls::T1_LEVEL), 0.175f, 0.01f),
          "pedal took it back: %.3f", p.c.Value(Controls::T1_LEVEL));
    // The knob rests at 0.5 now; the pedal sweeping T1 through 0.5 again keeps it
    p.Sweep(0.1f);
    p.Sweep(0.9f);
    CHECK(p.c.GetOwner(Controls::T1_LEVEL) == Controls::PEDAL && Near(p.c.Value(Controls::T1_LEVEL), 0.0f),
          "a knob resting where the pedal passes doesn't grab it (owner %d, %.3f)", p.c.GetOwner(Controls::T1_LEVEL),
          p.c.Value(Controls::T1_LEVEL));
}

static void TestMorphRestored()
{
    std::printf("params: morph restored at power-up\n");
    Controls::Morph m{};
    m.valid = true;
    m.mask  = 1 << Controls::FX_AMOUNT;
    m.a[Controls::FX_AMOUNT] = 0.0f;
    m.b[Controls::FX_AMOUNT] = 1.0f;
    m.pedal_a = 0.0f;
    m.pedal_b = 1.0f;
    float saved[Controls::kCount] = {0.7f, 0.7f, 0.5f, 0.5f, 0.4f};
    Panel p;
    p.pedal = 0.9f;  // Pedal left somewhere else
    p.c.Init(saved, &m);
    p.PowerOn();
    p.Step(20);
    CHECK(Near(p.c.Value(Controls::FX_AMOUNT), 0.4f), "pedal not moved: saved value stays (%.3f)", p.c.Value(Controls::FX_AMOUNT));
    p.Sweep(0.7f);  // Morph 0.9 -> 0.7: doesn't reach 0.4
    CHECK(Near(p.c.Value(Controls::FX_AMOUNT), 0.4f), "moved, not crossed yet");
    p.Sweep(0.2f);
    CHECK(Near(p.c.Value(Controls::FX_AMOUNT), 0.2f), "crossed: follows (%.3f)", p.c.Value(Controls::FX_AMOUNT));
}

//
// Snapshot store
//

struct RamFlash : ceres::SnapshotFlash
{
    std::vector<uint8_t> mem = std::vector<uint8_t>(4 * 4096, 0xFF);  // Erased flash
    int  writes      = 0;
    bool fail_next   = false;
    int  torn_bytes  = -1;  // Simulate a power cut: only this many bytes get written

    const uint8_t* Read(uint32_t slot) override { return &mem[slot * 4096]; }
    bool Write(uint32_t slot, const uint8_t* data, uint32_t size) override
    {
        writes++;
        memset(&mem[slot * 4096], 0xFF, 4096);
        if (fail_next) {
            fail_next = false;
            return false;
        }
        uint32_t n = torn_bytes >= 0 ? (uint32_t)torn_bytes : size;
        memcpy(&mem[slot * 4096], data, n);
        torn_bytes = -1;
        return true;
    }
};

static Snapshot MakeSnap(float level)
{
    Snapshot s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < Controls::kCount; i++)
        s.values[i] = level;
    s.tempo = 120.0f;
    return s;
}

static void TestStore()
{
    std::printf("persist: snapshot slots\n");
    RamFlash      flash;
    SnapshotStore store;
    store.Init(&flash);
    Snapshot got;
    CHECK(!store.Load(got), "erased flash: nothing to load");

    for (int k = 1; k <= 6; k++) {
        Snapshot s = MakeSnap(0.1f * k);
        CHECK(store.Save(s), "save %d", k);
    }
    SnapshotStore fresh;
    fresh.Init(&flash);
    CHECK(fresh.Load(got) && Near(got.values[0], 0.6f) && got.sequence == 6, "newest wins after 6 saves (%.2f, seq %u)",
          got.values[0], got.sequence);

    // Power cut in the middle of the next save: that slot is garbage, the previous one loads
    flash.torn_bytes = 20;
    Snapshot s7 = MakeSnap(0.7f);
    fresh.Save(s7);
    SnapshotStore after_cut;
    after_cut.Init(&flash);
    CHECK(after_cut.Load(got) && Near(got.values[0], 0.6f), "torn write: falls back to the last good one (%.2f)", got.values[0]);
    // And saving continues past it
    Snapshot s8 = MakeSnap(0.8f);
    after_cut.Save(s8);
    SnapshotStore again;
    again.Init(&flash);
    CHECK(again.Load(got) && Near(got.values[0], 0.8f), "next save after the cut loads (%.2f)", got.values[0]);

    // A firmware with another layout: ignored
    Snapshot old = MakeSnap(0.3f);
    again.Save(old);
    Snapshot* raw = (Snapshot*)&flash.mem[0];
    for (uint32_t sl = 0; sl < 4; sl++) {
        raw = (Snapshot*)&flash.mem[sl * 4096];
        raw->version = 99;
        raw->crc     = SnapshotStore::Crc(*raw);
    }
    SnapshotStore newer;
    newer.Init(&flash);
    CHECK(!newer.Load(got), "other version: not loaded, defaults instead");

    // Rotation: 4 saves touch 4 different slots
    RamFlash      f2;
    SnapshotStore st;
    st.Init(&f2);
    for (int k = 0; k < 4; k++) {
        Snapshot s = MakeSnap(0.5f);
        st.Save(s);
    }
    int used = 0;
    for (int sl = 0; sl < 4; sl++)
        used += f2.mem[sl * 4096] != 0xFF;
    CHECK(used == 4, "4 saves spread over the 4 slots (%d)", used);

    // SameAs: tiny noise isn't a change
    Snapshot a = MakeSnap(0.5f), b = MakeSnap(0.503f);
    CHECK(a.SameAs(b, 0.005f) && !a.SameAs(MakeSnap(0.52f), 0.005f), "change detection tolerance");
}

int main()
{
    TestDefaults();
    TestRestoreAndPickup();
    TestPowerOnRamp();
    TestCc();
    TestMorph();
    TestMorphRestored();
    TestStore();
    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
