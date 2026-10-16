// tmce_dongle — T-Dongle-S3 firmware: USB drive for Nintendont that pulls the newest
// Training Mode save (saves/GTME.raw) over Wi-Fi at boot, before the Wii sees the drive.
//
// Boot sequence:
//   1. Mount the microSD (FAT32) inside the dongle.
//   2. Read CONFIG/config.txt (SSID, PASSWORD, optional HOST/PORT/GAME/SYNC).
//   3. Join Wi-Fi, find Wii Memcard Manager's TM Sync server (UDP discovery or HOST=),
//      and GET /tmsync/v1/saves/GTME.raw with If-None-Match = last ETag we installed.
//        304 -> nothing to do.  200 -> stream to saves/GTME.tmp, verify size + CRC32,
//        keep the old card as saves/GTME.bak, rename tmp -> GTME.raw, remember the ETag.
//      Any failure or timeout -> skip, card untouched.
//   4. Unmount, turn Wi-Fi off, then present the raw card to the host as a USB drive.
// Every boot appends one line to CONFIG/sync_log.txt so problems are visible from a Mac.
//
// Server protocol: wii-memcard-manager electron/main/wiiSyncServer.ts (TM Sync v1).
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "USB.h"
#include "USBMSC.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <stdio.h>
#include <sys/stat.h>

// T-Dongle-S3 microSD wiring (same as LilyGO's examples and slippi-beamer)
#define SD_CLK 12
#define SD_CMD 16
#define SD_D0  14
#define SD_D1  17
#define SD_D2  21
#define SD_D3  18

#define MNT "/sd"
static const uint32_t SYNC_BUDGET_MS = 20000;   // whole Wi-Fi phase, worst case
static const uint32_t WIFI_JOIN_MS   = 12000;
static const uint16_t TMSYNC_HTTP_PORT = 8642;
static const uint16_t TMSYNC_DISCOVERY_PORT = 8643;

USBMSC MSC;
static sdmmc_card_t card;
static bool card_ok = false;

// USB activity counters, persisted to NVS so the next boot can report what the host did.
static volatile uint32_t usb_reads = 0, usb_writes = 0, usb_startstops = 0;
static volatile uint32_t usb_first_io_ms = 0;
static Preferences prefs;

// ---------------------------------------------------------------------------
// SD card: raw access for USB, FAT mount for the sync phase
// ---------------------------------------------------------------------------
static sdmmc_slot_config_t slot_config() {
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = (gpio_num_t)SD_CLK;
  slot.cmd = (gpio_num_t)SD_CMD;
  slot.d0 = (gpio_num_t)SD_D0;
  slot.d1 = (gpio_num_t)SD_D1;
  slot.d2 = (gpio_num_t)SD_D2;
  slot.d3 = (gpio_num_t)SD_D3;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  return slot;
}

static bool card_init(int freq_khz) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = freq_khz;
  sdmmc_slot_config_t slot = slot_config();
  sdmmc_host_deinit();
  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) return false;
  return sdmmc_card_init(&host, &card) == ESP_OK;
}

static sdmmc_card_t *fat_card = nullptr;

static bool fat_mount() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = slot_config();
  esp_vfs_fat_sdmmc_mount_config_t cfg = {};
  cfg.format_if_mount_failed = false;
  cfg.max_files = 4;
  cfg.allocation_unit_size = 0;
  sdmmc_host_deinit();
  return esp_vfs_fat_sdmmc_mount(MNT, &host, &slot, &cfg, &fat_card) == ESP_OK;
}

static void fat_unmount() {
  if (fat_card) esp_vfs_fat_sdcard_unmount(MNT, fat_card);
  fat_card = nullptr;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static String g_log;  // one line per boot, written to CONFIG/sync_log.txt
static void note(const String &s) {
  if (g_log.length()) g_log += " | ";
  g_log += s;
}

static String read_text(const char *path, size_t max = 2048) {
  FILE *f = fopen(path, "r");
  if (!f) return "";
  String out;
  char buf[128];
  size_t n;
  while (out.length() < max && (n = fread(buf, 1, sizeof(buf), f)) > 0) out.concat(buf, n);
  fclose(f);
  return out;
}

static bool write_text(const char *path, const String &s, const char *mode = "w") {
  FILE *f = fopen(path, mode);
  if (!f) return false;
  fwrite(s.c_str(), 1, s.length(), f);
  fclose(f);
  return true;
}

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

// CRC32 (IEEE, reflected 0xEDB88320) — matches wiiSyncServer.ts
static uint32_t crc_table[256];
static void crc_init() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n) {
  while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return c;
}

// ---------------------------------------------------------------------------
// CONFIG/config.txt  (KEY=value, keys case-insensitive, # comments, quotes ok)
// ---------------------------------------------------------------------------
struct Config {
  String ssid, password, host, game = "GTME";
  uint16_t port = TMSYNC_HTTP_PORT;
  bool sync = true;
};

static Config load_config() {
  Config c;
  String text = read_text(MNT "/CONFIG/config.txt");
  int start = 0;
  while (start < (int)text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    start = end + 1;
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq), val = line.substring(eq + 1);
    key.trim();
    key.toUpperCase();
    val.trim();
    if (val.length() >= 2 && val[0] == '"' && val[val.length() - 1] == '"') val = val.substring(1, val.length() - 1);
    if (key == "SSID") c.ssid = val;
    else if (key == "PASSWORD") c.password = val;
    else if (key == "HOST") c.host = val;
    else if (key == "PORT") c.port = val.toInt() ? val.toInt() : TMSYNC_HTTP_PORT;
    else if (key == "GAME") { val.toUpperCase(); if (val.length() == 4) c.game = val; }
    else if (key == "SYNC") { val.toLowerCase(); c.sync = !(val == "false" || val == "0" || val == "off"); }
  }
  return c;
}

// ---------------------------------------------------------------------------
// TM Sync client
// ---------------------------------------------------------------------------
static bool discover(IPAddress &ip, uint16_t &port, uint32_t deadline) {
  WiFiUDP udp;
  if (!udp.begin(TMSYNC_DISCOVERY_PORT + 1)) return false;
  for (int attempt = 0; attempt < 3 && millis() < deadline; attempt++) {
    udp.beginPacket(IPAddress(255, 255, 255, 255), TMSYNC_DISCOVERY_PORT);
    udp.print("TMSYNC_DISCOVER\n");
    udp.endPacket();
    uint32_t until = millis() + 1200;
    while (millis() < until && millis() < deadline) {
      int n = udp.parsePacket();
      if (n > 0) {
        char buf[64] = {0};
        udp.read(buf, sizeof(buf) - 1);
        int p = 0;
        if (sscanf(buf, "TMSYNC_HERE 1 %d", &p) == 1 && p > 0) {
          ip = udp.remoteIP();
          port = (uint16_t)p;
          udp.stop();
          return true;
        }
      }
      delay(20);
    }
  }
  udp.stop();
  return false;
}

static void sync_save(const Config &cfg, uint32_t deadline) {
  mkdir(MNT "/saves", 0775);
  const String raw = String(MNT "/saves/") + cfg.game + ".raw";
  const String tmp = String(MNT "/saves/") + cfg.game + ".tmp";
  const String bak = String(MNT "/saves/") + cfg.game + ".bak";
  const char *etag_path = MNT "/CONFIG/last_etag.txt";

  // Find the server.
  String host = cfg.host;
  uint16_t port = cfg.port;
  if (!host.length()) {
    IPAddress ip;
    if (!discover(ip, port, deadline)) { note("no TM Sync server found"); return; }
    host = ip.toString();
  }
  note("server " + host + ":" + String(port));

  // Only send our last ETag if the card we installed is still there.
  String etag = exists(raw.c_str()) ? read_text(etag_path, 64) : "";
  etag.trim();

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  String url = "http://" + host + ":" + String(port) + "/tmsync/v1/saves/" + cfg.game + ".raw";
  if (!http.begin(url)) { note("bad url"); return; }
  if (etag.length()) http.addHeader("If-None-Match", "\"" + etag + "\"");
  const char *want[] = {"ETag", "X-TMSync-CRC32"};
  http.collectHeaders(want, 2);

  int code = http.GET();
  if (code == 304) { note("up to date"); http.end(); return; }
  if (code != 200) { note("HTTP " + String(code)); http.end(); return; }

  int len = http.getSize();
  String new_etag = http.header("ETag");
  new_etag.replace("\"", "");
  uint32_t want_crc = strtoul(http.header("X-TMSync-CRC32").c_str(), nullptr, 16);
  if (len <= 0) { note("no length"); http.end(); return; }

  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) { note("can't write tmp"); http.end(); return; }
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[4096];
  int got = 0;
  uint32_t crc = 0xFFFFFFFFu;
  bool ok = true;
  while (got < len) {
    if (millis() > deadline + 15000) { ok = false; note("download timeout"); break; }  // allow big cards a bit longer
    size_t avail = s->available();
    if (!avail) {
      if (!s->connected()) { ok = false; note("connection dropped"); break; }
      delay(2);
      continue;
    }
    int n = s->readBytes(buf, min((size_t)(len - got), min(avail, sizeof(buf))));
    if (n <= 0) continue;
    crc = crc_update(crc, buf, n);
    if (fwrite(buf, 1, n, f) != (size_t)n) { ok = false; note("write failed"); break; }
    got += n;
  }
  fclose(f);
  http.end();
  crc ^= 0xFFFFFFFFu;

  if (ok && got != len) { ok = false; note("short download"); }
  if (ok && want_crc && crc != want_crc) { ok = false; note("CRC mismatch"); }
  if (!ok) { remove(tmp.c_str()); return; }

  // Install: old card -> .bak, new card -> .raw
  remove(bak.c_str());
  if (exists(raw.c_str())) rename(raw.c_str(), bak.c_str());
  if (rename(tmp.c_str(), raw.c_str()) != 0) {
    note("rename failed");
    if (exists(bak.c_str())) rename(bak.c_str(), raw.c_str());
    return;
  }
  write_text(etag_path, new_etag + "\n");
  note("installed " + cfg.game + ".raw (" + String(len) + " bytes, etag " + new_etag + ")");
}

static String previous_session() {
  prefs.begin("tmce", true);
  uint32_t r = prefs.getUInt("reads", 0), w = prefs.getUInt("writes", 0), ss = prefs.getUInt("ss", 0), f = prefs.getUInt("first", 0);
  bool any = prefs.isKey("reads");
  prefs.end();
  if (!any) return "";
  return "prev session: " + String(r) + " reads, " + String(w) + " writes, " + String(ss) + " start/stop" + (f ? ", first IO at " + String(f) + "ms" : ", host never read");
}

static void wifi_phase() {
  uint32_t t0 = millis();
  String prev = previous_session();
  if (prev.length()) note(prev);
  if (!fat_mount()) { note("card not FAT32, sync skipped"); return; }
  Config cfg = load_config();
  if (!cfg.sync) note("SYNC=false");
  else if (!cfg.ssid.length()) note("no SSID in CONFIG/config.txt");
  else {
    crc_init();
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
    uint32_t join_deadline = millis() + WIFI_JOIN_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < join_deadline) delay(100);
    if (WiFi.status() != WL_CONNECTED) note("Wi-Fi join failed (" + cfg.ssid + ")");
    else {
      note("wifi " + WiFi.localIP().toString());
      sync_save(cfg, t0 + SYNC_BUDGET_MS);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  note(String(millis() - t0) + "ms");
  write_text(MNT "/CONFIG/sync_log.txt", "boot: " + g_log + "\n", "a");
  fat_unmount();
}

// ---------------------------------------------------------------------------
// USB mass storage
// ---------------------------------------------------------------------------
static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  usb_reads++;
  if (!usb_first_io_ms) usb_first_io_ms = millis();
  if (!card_ok) return -1;
  if (sdmmc_read_sectors(&card, buffer, lba, bufsize / card.csd.sector_size) != ESP_OK) return -1;
  return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  usb_writes++;
  if (!usb_first_io_ms) usb_first_io_ms = millis();
  if (!card_ok) return -1;
  if (sdmmc_write_sectors(&card, buffer, lba, bufsize / card.csd.sector_size) != ESP_OK) return -1;
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) { usb_startstops++; return true; }

void setup() {
  wifi_phase();  // never blocks longer than ~35 s, and never fatal

  for (int i = 0; i < 10 && !card_ok; i++) {
    card_ok = card_init(SDMMC_FREQ_HIGHSPEED) || card_init(SDMMC_FREQ_DEFAULT);
    if (!card_ok) delay(500);
  }

  // Present as a plain single-function flash drive (class defined at interface level),
  // not the Arduino default "miscellaneous / IAD composite" device class.
  USB.usbClass(0);
  USB.usbSubClass(0);
  USB.usbProtocol(0);
  USB.usbAttributes(0x80);  // bus powered
  USB.usbPower(500);
  USB.productName("TM-CE Dongle");
  USB.manufacturerName("JohnFio");

  MSC.vendorID("JOHNFIO");
  MSC.productID("TM-CE DONGLE");
  MSC.productRevision("0.4");
  MSC.onStartStop(onStartStop);
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);
  MSC.mediaPresent(card_ok);
  MSC.begin(card_ok ? card.csd.capacity : 0, card_ok ? card.csd.sector_size : 512);
  USB.begin();
}

void loop() {
  // Persist USB activity counters every 2 s (only when they changed) for the next boot's log.
  static uint32_t saved_r = UINT32_MAX, saved_w = UINT32_MAX, saved_ss = UINT32_MAX;
  if (usb_reads != saved_r || usb_writes != saved_w || usb_startstops != saved_ss) {
    saved_r = usb_reads; saved_w = usb_writes; saved_ss = usb_startstops;
    prefs.begin("tmce", false);
    prefs.putUInt("reads", saved_r);
    prefs.putUInt("writes", saved_w);
    prefs.putUInt("ss", saved_ss);
    prefs.putUInt("first", usb_first_io_ms);
    prefs.end();
  }
  delay(2000);
}
