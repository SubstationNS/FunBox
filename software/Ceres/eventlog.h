#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

//
// Ceres black-box log: short text entries kept in QSPI flash across power cycles, read out later
// over USB serial (L command, test/log.py). For finding out what happened on a gig or in the
// enclosure, where USB isn't connected.
//
//   - 64 KB ring of 128-byte entries (512 entries), each with a sequence number, the boot number,
//     the time since boot and a CRC. Entries are programmed into erased flash, so appending costs
//     no erase; a 4 KB sector is erased only when the ring reaches it (wear: ~1 erase per sector
//     per day even with a summary every minute).
//   - A torn entry (power cut while writing) fails its CRC and is skipped.
//
// Plain C++ so it runs in the host tests (with a RAM "flash").
//

namespace ceres
{

class LogFlash
{
  public:
    virtual const uint8_t* Read(uint32_t offset)                                   = 0;
    virtual bool           Program(uint32_t offset, const uint8_t* data, uint32_t size) = 0;  // Into erased flash
    virtual bool           EraseSector(uint32_t offset)                            = 0;  // 4 KB
    virtual ~LogFlash() {}
};

class EventLog
{
  public:
    static const uint32_t kSize        = 64 * 1024;
    static const uint32_t kSectorSize  = 4096;
    static const uint32_t kEntrySize   = 128;
    static const uint32_t kEntries     = kSize / kEntrySize;
    static const uint32_t kPerSector   = kSectorSize / kEntrySize;
    static const uint32_t kTextMax     = kEntrySize - 16;  // Including the terminating 0

    struct Entry
    {
        uint32_t seq;
        uint32_t uptime_ms;
        uint16_t boot;
        uint16_t reserved;
        char     text[kTextMax];
        uint32_t crc;  // Over everything above
    };
    static_assert(sizeof(Entry) == kEntrySize, "entry layout");

    // Finds where the log left off; this power-up gets the next boot number
    void Init(LogFlash* flash)
    {
        flash_     = flash;
        next_      = 0;
        seq_       = 0;
        boot_      = 1;
        bool found = false;
        for (uint32_t i = 0; i < kEntries; i++) {
            Entry e;
            if (!Get(i, e))
                continue;
            if (!found || e.seq > seq_) {
                seq_  = e.seq;
                next_ = (i + 1) % kEntries;
                boot_ = (uint16_t)(e.boot + 1);
                found = true;
            }
        }
        // A power cut can leave the next slot half-written: move on to an erased one
        while (!Erased(next_) && next_ % kPerSector != 0)
            next_ = (next_ + 1) % kEntries;
    }

    bool Append(uint32_t uptime_ms, const char* text)
    {
        Entry e;
        memset(&e, 0, sizeof(e));
        e.seq       = ++seq_;
        e.uptime_ms = uptime_ms;
        e.boot      = boot_;
        strncpy(e.text, text, kTextMax - 1);
        e.crc = Crc(e);
        if (next_ % kPerSector == 0 && !flash_->EraseSector(next_ * kEntrySize))
            return false;
        bool ok = flash_->Program(next_ * kEntrySize, (const uint8_t*)&e, sizeof(e));
        next_   = (next_ + 1) % kEntries;
        return ok;
    }

    // Oldest first. Calls fn(entry) for every valid one.
    template <typename Fn>
    void ForEach(Fn fn)
    {
        for (uint32_t k = 0; k < kEntries; k++) {
            Entry e;
            if (Get((next_ + k) % kEntries, e))
                fn(e);
        }
    }

    uint16_t Boot() const { return boot_; }

  private:
    bool Get(uint32_t i, Entry& e)
    {
        memcpy(&e, flash_->Read(i * kEntrySize), sizeof(e));
        return e.seq != 0xFFFFFFFF && e.crc == Crc(e) && e.text[kTextMax - 1] == 0;
    }

    bool Erased(uint32_t i)
    {
        const uint8_t* p = flash_->Read(i * kEntrySize);
        for (uint32_t k = 0; k < kEntrySize; k++)
            if (p[k] != 0xFF)
                return false;
        return true;
    }

    static uint32_t Crc(const Entry& e)
    {
        const uint8_t* d   = (const uint8_t*)&e;
        uint32_t       crc = 0xFFFFFFFF;
        for (uint32_t i = 0; i < offsetof(Entry, crc); i++) {
            crc ^= d[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
        return ~crc;
    }

    LogFlash* flash_;
    uint32_t  next_, seq_;
    uint16_t  boot_;
};

} // namespace ceres
