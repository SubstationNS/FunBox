#pragma once
#include "params.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

//
// Ceres saved state ("it plays like I left it"). Design: docs/looper-design.md, "State persistence".
//
//   - A snapshot holds every parameter value, the tempo, the values only MIDI CC can set and the
//     expression morph. Loops aren't saved. Toggles and DIPs aren't either: they're physical truth.
//   - Stored in the top 16 KB of the QSPI flash, far above the program (0x90040000 + < 480 KB),
//     as 4 slots of 4 KB used in turn: each save goes to the next slot with a higher sequence
//     number, so a power cut mid-write only loses that one save, and wear is spread over 4 sectors.
//   - Magic + version + size + CRC: anything that doesn't match exactly (an older firmware's layout,
//     a half-written slot, an erased one) is ignored and the defaults are used instead.
//
// Plain C++ so it runs in the host tests (with a RAM "flash").
//

namespace ceres
{

struct Snapshot
{
    static const uint32_t kMagic   = 0x31535243;  // "CRS1"
    static const uint16_t kVersion = 2;  // 2: + latency

    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;

    float   values[Controls::kCount];
    float   tempo;           // BPM
    uint8_t delay_division;  // MIDI CC 77
    uint8_t preset_bars;     // MIDI CC 29: recordings end after this many bars (0 = off)
    uint8_t reserved[2];
    float   latency;         // Measured round trip, samples (0 = not measured)
    Controls::Morph morph;

    uint32_t crc;  // Over everything above

    // Same settings (sequence and CRC aside)? Values within `tolerance`.
    bool SameAs(const Snapshot& o, float tolerance) const
    {
        for (int i = 0; i < Controls::kCount; i++)
            if (std::fabs(values[i] - o.values[i]) > tolerance)
                return false;
        if (std::fabs(tempo - o.tempo) > 0.05f || delay_division != o.delay_division || preset_bars != o.preset_bars
            || latency != o.latency)
            return false;
        if (morph.valid != o.morph.valid || morph.mask != o.morph.mask)
            return false;
        if (!morph.valid)
            return true;
        for (int i = 0; i < Controls::kCount; i++)
            if (morph.a[i] != o.morph.a[i] || morph.b[i] != o.morph.b[i])
                return false;
        return morph.pedal_a == o.morph.pedal_a && morph.pedal_b == o.morph.pedal_b;
    }
};

// Where the slots live; the pedal backs this with QSPI, the tests with RAM
class SnapshotFlash
{
  public:
    virtual const uint8_t* Read(uint32_t slot)                               = 0;  // Slot contents (4 KB)
    virtual bool           Write(uint32_t slot, const uint8_t* data, uint32_t size) = 0;  // Erase + write
    virtual ~SnapshotFlash() {}
};

class SnapshotStore
{
  public:
    static const uint32_t kSlots = 4;

    void Init(SnapshotFlash* flash)
    {
        flash_     = flash;
        last_slot_ = kSlots - 1;
        last_seq_  = 0;
    }

    // The newest valid snapshot, if any
    bool Load(Snapshot& out)
    {
        bool found = false;
        for (uint32_t s = 0; s < kSlots; s++) {
            Snapshot snap;
            memcpy(&snap, flash_->Read(s), sizeof(snap));
            if (!Valid(snap))
                continue;
            if (!found || snap.sequence > last_seq_) {
                out        = snap;
                last_seq_  = snap.sequence;
                last_slot_ = s;
                found      = true;
            }
        }
        return found;
    }

    // Fills in the header, sequence and CRC, writes to the next slot
    bool Save(Snapshot& snap)
    {
        snap.magic    = Snapshot::kMagic;
        snap.version  = Snapshot::kVersion;
        snap.size     = sizeof(Snapshot);
        snap.sequence = last_seq_ + 1;
        snap.crc      = Crc(snap);
        uint32_t slot = (last_slot_ + 1) % kSlots;
        if (!flash_->Write(slot, (const uint8_t*)&snap, sizeof(snap)))
            return false;
        last_slot_ = slot;
        last_seq_  = snap.sequence;
        return true;
    }

    static uint32_t Crc(const Snapshot& snap)  // zlib's crc32 over everything before the CRC
    {
        const uint8_t* d   = (const uint8_t*)&snap;
        uint32_t       n   = offsetof(Snapshot, crc);
        uint32_t       crc = 0xFFFFFFFF;
        for (uint32_t i = 0; i < n; i++) {
            crc ^= d[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
        return ~crc;
    }

  private:
    static bool Valid(const Snapshot& s)
    {
        return s.magic == Snapshot::kMagic && s.version == Snapshot::kVersion && s.size == sizeof(Snapshot)
               && s.crc == Crc(s);
    }

    SnapshotFlash* flash_;
    uint32_t       last_slot_, last_seq_;
};

} // namespace ceres
