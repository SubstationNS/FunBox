#include "daisy_seed.h"
#include "../updater.h"

//
// Ceres loader: a small internal-flash program for the first install and for recovery
// (flash.sh --install). It only receives a Ceres image over USB serial and writes it to QSPI
// (updater.h); then flash.sh puts the Daisy bootloader in internal flash, which loads Ceres.
// Kept separate from Ceres so Ceres can grow past the 128 KB of internal flash.
//
// USB serial: W<size>,<crc32> + image (the update), B = reboot into the STM32 ROM bootloader.
// Prints "loader ready" twice a second; the LED blinks.
//

using namespace daisy;

DaisySeed hw;

uint8_t DSY_SDRAM_BSS  update_buf[ceres::FirmwareUpdater::kMaxSize];
ceres::FirmwareUpdater updater;
volatile bool          bootloader_requested;

void UsbReceive(uint8_t* buf, uint32_t* len)
{
    for (uint32_t i = 0; i < *len; i++)
        if (!updater.Receive(buf[i]) && buf[i] == 'B')
            bootloader_requested = true;
}

int main(void)
{
    hw.Init();
    hw.StartLog(false);
    hw.usb_handle.SetReceiveCallback(UsbReceive, UsbHandle::FS_INTERNAL);
    updater.Init(&hw, update_buf);

    uint32_t last = 0;
    bool     led  = false;
    while (1) {
        if (bootloader_requested)
            System::ResetToBootloader();
        if (updater.Pending())
            updater.Run();
        if (System::GetNow() - last > 500) {
            last = System::GetNow();
            led  = !led;
            hw.SetLed(led);
            hw.PrintLine("loader ready");
        }
    }
}
