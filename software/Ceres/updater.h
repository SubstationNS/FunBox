#pragma once
#include "daisy_seed.h"
#include <cstdlib>
#include <cstring>

//
// Firmware update over USB serial, without DFU. Used by Ceres itself and by the small loader
// (loader/) that flash.sh --install puts in internal flash for the first install / recovery.
//
// The host (update.py) sends  W<size>,<crc32>\n  (hex) followed by <size> raw bytes: a BOOT_SRAM
// build. It collects in SDRAM, then the main loop checks the CRC, writes it to QSPI where the Daisy
// bootloader loads programs from (0x90040000), reads it back to verify, and (in a bootloader-loaded
// program) restarts into it.
//
// Also fixes a QSPI chip that refuses to erase: see Unprotect().
//

namespace ceres
{

class FirmwareUpdater
{
  public:
    static const uint32_t kMaxSize       = 480 * 1024;  // A BOOT_SRAM program can't be bigger
    static const uint32_t kAppQspiOffset = 0x40000;     // Programs live at 0x90040000

    // buf: kMaxSize bytes, in SDRAM
    void Init(daisy::DaisySeed* seed, uint8_t* buf)
    {
        seed_      = seed;
        buf_       = buf;
        receiving_ = ready_ = false;
        line_len_  = 0;
    }

    // From the USB receive callback, byte by byte. Returns true if the byte belonged to an update
    // (the W line or the image), so the caller doesn't parse it as anything else.
    bool Receive(uint8_t c)
    {
        if (receiving_) {
            buf_[received_++] = c;
            if (received_ == size_) {
                receiving_ = false;
                ready_     = true;
            }
            return true;
        }
        if (line_len_ > 0) {  // In a W line
            if (c == '\n' || c == '\r') {
                line_[line_len_] = 0;
                char*    comma   = strchr(line_, ',');
                uint32_t size    = (uint32_t)strtoul(&line_[1], nullptr, 16);
                if (comma && size > 0 && size <= kMaxSize && !ready_) {
                    size_      = size;
                    crc_       = (uint32_t)strtoul(comma + 1, nullptr, 16);
                    received_  = 0;
                    receiving_ = true;
                }
                line_len_ = 0;
            } else if (line_len_ < sizeof(line_) - 1) {
                line_[line_len_++] = (char)c;
            }
            return true;
        }
        if (c == 'W') {
            line_[0]  = 'W';
            line_len_ = 1;
            return true;
        }
        return false;
    }

    bool Pending() const { return ready_; }

    // Main loop: write a received image. Blocks for a few seconds (audio keeps running).
    void Run()
    {
        ready_        = false;
        uint32_t crc  = Crc32(buf_, size_);
        if (crc != crc_) {
            seed_->PrintLine("update: CRC mismatch (got %08lx, expected %08lx), nothing written", crc, crc_);
            return;
        }
        seed_->PrintLine("update: %lu bytes, CRC ok, writing QSPI", size_);
        if (Unprotect(false) != 0x40) {
            seed_->PrintLine("update: QSPI is write protected, nothing written");
            return;
        }
        uint32_t start = 0x90000000 + kAppQspiOffset;
        if (seed_->qspi.Erase(start, start + size_) != daisy::QSPIHandle::Result::OK) {
            seed_->PrintLine("update: QSPI erase FAILED (status %d)", (int)seed_->qspi.GetStatus());
            return;
        }
        if (seed_->qspi.Write(start, size_, buf_) != daisy::QSPIHandle::Result::OK) {
            seed_->PrintLine("update: QSPI write FAILED (status %d)", (int)seed_->qspi.GetStatus());
            return;
        }
        uint8_t* written = (uint8_t*)seed_->qspi.GetData(kAppQspiOffset);
        dsy_dma_invalidate_cache_for_buffer(written, size_);
        if (Crc32(written, size_) != crc) {
            seed_->PrintLine("update: verify FAILED (QSPI reads back different)");
            return;
        }
#ifdef BOOT_APP
        seed_->PrintLine("update: written and verified, restarting");
        daisy::System::Delay(100);  // Let the message out
        NVIC_SystemReset();         // The bootloader loads the new image
#else
        seed_->PrintLine("update: written and verified (no Daisy bootloader yet: flash.sh installs it)");
#endif
    }

    // The QSPI chip can end up block-protected (status register BP bits) with its status register
    // locked (SRWD) while WP# is low. WP# is IO2, low in single-line mode, so libDaisy's own status
    // write at init bounces and every erase is refused (this is also what broke the Daisy
    // bootloader's DFU erase). Fix: drive IO2 high as a plain GPIO, write the status register to
    // 0x40 (quad enable, nothing protected), give the pin back to QSPI.
    // Returns the status register afterwards (0x40 = good). force: write it even if it looks fine.
    uint8_t Unprotect(bool force)
    {
        QSPI_HandleTypeDef h = {};
        h.Instance           = QUADSPI;
        h.Timeout            = 100;
        h.State              = HAL_QSPI_STATE_BUSY_MEM_MAPPED;  // So the abort really aborts
        HAL_QSPI_Abort(&h);                                     // Leave memory-mapped mode
        h.State = HAL_QSPI_STATE_READY;

        uint8_t id[3] = {}, before = 0, after = 0;
        Command(&h, 0x9F, id, 3, true);       // JEDEC ID
        Command(&h, 0x05, &before, 1, true);  // Status register
        after = before;
        if (force || (before & 0xBC) != 0 || !(before & 0x40)) {
            __HAL_RCC_GPIOF_CLK_ENABLE();
            GPIO_InitTypeDef g = {};
            g.Pin              = GPIO_PIN_7;  // QSPI IO2 = WP#
            g.Mode             = GPIO_MODE_OUTPUT_PP;
            g.Pull             = GPIO_NOPULL;
            g.Speed            = GPIO_SPEED_FREQ_LOW;
            HAL_GPIO_Init(GPIOF, &g);
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_7, GPIO_PIN_SET);
            daisy::System::Delay(1);
            uint8_t sr = 0x40;
            Command(&h, 0x06, nullptr, 0, false);  // Write enable
            Command(&h, 0x01, &sr, 1, false);      // Write status register
            for (int i = 0; i < 100; i++) {        // Up to ~100 ms
                Command(&h, 0x05, &after, 1, true);
                if (!(after & 1))
                    break;
                daisy::System::Delay(1);
            }
        }
        seed_->PrintLine("qspi: id %02x %02x %02x, status %02x -> %02x%s", id[0], id[1], id[2], before, after,
                         after == 0x40 ? " (ok)" : " (STILL PROTECTED)");
        seed_->qspi.Init(seed_->qspi_config);  // Pins back to QSPI, memory-mapped again
        return after;
    }

    static uint32_t Crc32(const uint8_t* data, uint32_t size)  // Same as zlib's crc32
    {
        uint32_t crc = 0xFFFFFFFF;
        for (uint32_t i = 0; i < size; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
        return ~crc;
    }

  private:
    static bool Command(QSPI_HandleTypeDef* h, uint8_t cmd, uint8_t* data, uint32_t n, bool read)
    {
        QSPI_CommandTypeDef c = {};
        c.InstructionMode     = QSPI_INSTRUCTION_1_LINE;
        c.Instruction         = cmd;
        c.AddressMode         = QSPI_ADDRESS_NONE;
        c.DataMode            = n ? QSPI_DATA_1_LINE : QSPI_DATA_NONE;
        c.NbData              = n;
        c.SIOOMode            = QSPI_SIOO_INST_EVERY_CMD;
        if (HAL_QSPI_Command(h, &c, 100) != HAL_OK)
            return false;
        if (n && (read ? HAL_QSPI_Receive(h, data, 100) : HAL_QSPI_Transmit(h, data, 100)) != HAL_OK)
            return false;
        return true;
    }

    daisy::DaisySeed* seed_;
    uint8_t*          buf_;
    volatile bool     receiving_, ready_;
    volatile uint32_t size_, crc_, received_;
    char              line_[32];
    uint32_t          line_len_;
};

} // namespace ceres
