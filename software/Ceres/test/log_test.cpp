// Host test for eventlog.h: appending, boot numbers, wrapping around the ring, power cuts mid-write.
//   c++ -std=c++17 -O2 -I.. log_test.cpp -o log_test && ./log_test
#include "eventlog.h"
#include <cstdio>
#include <string>
#include <vector>

using ceres::EventLog;

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

struct RamFlash : ceres::LogFlash
{
    std::vector<uint8_t> mem = std::vector<uint8_t>(EventLog::kSize, 0x5A);  // Never used: garbage
    int erases = 0, torn = -1;
    const uint8_t* Read(uint32_t off) override { return &mem[off]; }
    bool Program(uint32_t off, const uint8_t* d, uint32_t n) override
    {
        uint32_t k = torn >= 0 ? (uint32_t)torn : n;
        torn       = -1;
        for (uint32_t i = 0; i < k; i++)
            mem[off + i] &= d[i];  // Flash programming only clears bits
        return true;
    }
    bool EraseSector(uint32_t off) override
    {
        erases++;
        std::fill(mem.begin() + off, mem.begin() + off + EventLog::kSectorSize, 0xFF);
        return true;
    }
};

static std::vector<std::string> Dump(RamFlash& f, uint16_t* last_boot = nullptr)
{
    EventLog log;
    log.Init(&f);
    std::vector<std::string> out;
    log.ForEach([&](const EventLog::Entry& e) {
        out.push_back(e.text);
        if (last_boot)
            *last_boot = e.boot;
    });
    return out;
}

int main()
{
    std::printf("event log\n");
    RamFlash f;
    {
        EventLog log;
        log.Init(&f);
        CHECK(log.Boot() == 1, "first boot is #1");
        log.Append(10, "boot one");
        log.Append(20, "hello");
    }
    {
        EventLog log;
        log.Init(&f);
        CHECK(log.Boot() == 2, "second power-up is boot #2 (%u)", log.Boot());
        log.Append(5, "boot two");
    }
    auto d = Dump(f);
    CHECK(d.size() == 3 && d[0] == "boot one" && d[2] == "boot two", "entries kept in order across boots (%zu)", d.size());

    // Long text is cut, not overflowing
    {
        EventLog log;
        log.Init(&f);
        std::string longtext(300, 'x');
        log.Append(1, longtext.c_str());
    }
    d = Dump(f);
    CHECK(d.back().size() == EventLog::kTextMax - 1, "long text cut to %u chars (%zu)", EventLog::kTextMax - 1, d.back().size());

    // Wrap: 2000 entries into a 512-entry ring: the newest 512 (minus the sector being reused) survive
    {
        EventLog log;
        log.Init(&f);
        for (int i = 0; i < 2000; i++)
            log.Append(i, ("entry " + std::to_string(i)).c_str());
    }
    d = Dump(f);
    CHECK(!d.empty() && d.back() == "entry 1999", "newest survives the wrap (%s)", d.empty() ? "-" : d.back().c_str());
    CHECK(d.size() >= EventLog::kEntries - EventLog::kPerSector, "about a ring's worth kept (%zu)", d.size());
    bool ordered = true;
    for (size_t i = 1; i < d.size(); i++)
        ordered &= std::stoi(d[i].substr(6)) == std::stoi(d[i - 1].substr(6)) + 1;
    CHECK(ordered, "oldest to newest, no gaps");
    std::printf("  2000 entries: %zu kept, oldest \"%s\", newest \"%s\", %d sector erases\n", d.size(), d.front().c_str(),
                d.back().c_str(), f.erases);

    // Power cut in the middle of writing an entry: that one is lost, the log carries on after it
    {
        EventLog log;
        log.Init(&f);
        f.torn = 40;
        log.Append(1, "torn entry");
    }
    {
        EventLog log;
        log.Init(&f);
        log.Append(2, "after the cut");
    }
    d = Dump(f);
    CHECK(d.back() == "after the cut" && d[d.size() - 2] == "entry 1999", "torn entry skipped, next one fine");

    if (failures)
        std::printf("%d FAILED\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
