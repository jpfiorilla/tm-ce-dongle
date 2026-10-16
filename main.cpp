// tmce_dongle — milestone 1: T-Dongle-S3 as a plain USB drive backed by its microSD.
// Talks to the card at the sector level (no filesystem mount), so any card format works:
// FAT32, exFAT, whatever. The host (Mac / Wii) is the only thing that interprets the filesystem.
#include <Arduino.h>
#include "USB.h"
#include "USBMSC.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

// T-Dongle-S3 microSD wiring (same as LilyGO's examples and slippi-beamer)
#define SD_CLK 12
#define SD_CMD 16
#define SD_D0  14
#define SD_D1  17
#define SD_D2  21
#define SD_D3  18

USBMSC MSC;
static sdmmc_card_t card;
static bool card_ok = false;

static bool card_init(int freq_khz) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = freq_khz;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = (gpio_num_t)SD_CLK;
  slot.cmd = (gpio_num_t)SD_CMD;
  slot.d0 = (gpio_num_t)SD_D0;
  slot.d1 = (gpio_num_t)SD_D1;
  slot.d2 = (gpio_num_t)SD_D2;
  slot.d3 = (gpio_num_t)SD_D3;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_host_deinit();
  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) return false;
  return sdmmc_card_init(&host, &card) == ESP_OK;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!card_ok) return -1;
  uint32_t sec = card.csd.sector_size;
  if (sdmmc_read_sectors(&card, buffer, lba, bufsize / sec) != ESP_OK) return -1;
  return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (!card_ok) return -1;
  uint32_t sec = card.csd.sector_size;
  if (sdmmc_write_sectors(&card, buffer, lba, bufsize / sec) != ESP_OK) return -1;
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) { return true; }

void setup() {
  // Try for ~5 s: high-speed first, then default speed (some cards refuse 40 MHz).
  for (int i = 0; i < 10 && !card_ok; i++) {
    card_ok = card_init(SDMMC_FREQ_HIGHSPEED) || card_init(SDMMC_FREQ_DEFAULT);
    if (!card_ok) delay(500);
  }

  MSC.vendorID("JOHNFIO");
  MSC.productID("TM-CE DONGLE");
  MSC.productRevision("0.2");
  MSC.onStartStop(onStartStop);
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);
  // Always enumerate, even with no card, so the host at least sees the device.
  MSC.mediaPresent(card_ok);
  MSC.begin(card_ok ? card.csd.capacity : 0, card_ok ? card.csd.sector_size : 512);
  USB.begin();
}

void loop() { delay(1000); }
