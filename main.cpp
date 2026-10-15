#include <Arduino.h>
// tmce_dongle — milestone 1: T-Dongle-S3 as a plain USB drive backed by its microSD.
// Based on LilyGO's examples/usb_mass_storage (MIT). No Wi-Fi yet: this is the Nintendont speed test.
#include "USB.h"
#include "USBMSC.h"
#include "SD_MMC.h"

#define SD_MMC_D0_PIN  14
#define SD_MMC_D1_PIN  17
#define SD_MMC_D2_PIN  21
#define SD_MMC_D3_PIN  18
#define SD_MMC_CLK_PIN 12
#define SD_MMC_CMD_PIN 16

USBMSC MSC;

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t sec = SD_MMC.sectorSize();
  if (!sec) return 0;
  for (uint32_t x = 0; x < bufsize / sec; x++) {
    if (!SD_MMC.writeRAW(buffer + sec * x, lba + x)) return 0;
  }
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t sec = SD_MMC.sectorSize();
  if (!sec) return 0;
  for (uint32_t x = 0; x < bufsize / sec; x++) {
    if (!SD_MMC.readRAW((uint8_t *)buffer + sec * x, lba + x)) return 0;
  }
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) { return true; }

void setup() {
  SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN, SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
  // 4-bit bus at 40 MHz high-speed; fall back to default speed if the card refuses.
  if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED) &&
      !SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT)) {
    while (true) delay(1000);  // no card: never enumerate
  }
  MSC.vendorID("JOHNFIO");
  MSC.productID("TM-CE DONGLE");
  MSC.productRevision("0.1");
  MSC.onStartStop(onStartStop);
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);
  MSC.mediaPresent(true);
  MSC.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());
  USB.begin();
}

void loop() { delay(1000); }
