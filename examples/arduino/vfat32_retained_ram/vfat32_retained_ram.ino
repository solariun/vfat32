// vfat32_retained_ram — a volume that survives resets (ESP32).
//
// The volume lives in RAM that the ESP32 does not clear on a software reset or a
// watchdog reset: __NOINIT_ATTR (ESP-IDF, esp_attr.h) puts it in the .noinit
// section of internal DRAM, which the start-up code leaves untouched. (For RAM that
// also survives deep sleep use RTC_NOINIT_ATTR: RTC slow memory, 8 KiB on the
// classic ESP32, so at most a 4 KiB volume.) On other boards the plain GCC
// .noinit section does the same where the linker script provides it.
// After a reset, mount(reuse) finds the volume again (spec M7):
//
//   - needs_format() is true only on the first power-up (the memory is random).
//   - was_dirty() is true when the previous session did not call umount() — here
//     always, because the sketch resets itself while mounted. An operation cut by
//     the reset may have left an allocated but unlinked cluster: check(&r, true)
//     frees such lost clusters and fixes the free count.
//
// The sketch counts boots in /boots.txt and resets itself every few seconds.
// After a power cycle the RAM holds random bytes: needs_format() is true again.

#include <vfat32.hpp>

#if defined(ESP32)
#include <esp_attr.h>
#include <esp_system.h>
#define RETAINED __NOINIT_ATTR
static const size_t volume_size = 32 * 1024;
#else
#define RETAINED __attribute__((section(".noinit")))
static const size_t volume_size = 4 * 1024;
#endif
static const int line_size = 32;
static const unsigned long baud_rate = 115200;
static const unsigned long reset_after_ms = 5000;

static uint8_t volume[volume_size] RETAINED;
static vfat32::vfs fs;

static void restart() {
#if defined(ESP32)
  esp_restart();
#endif
}

void setup() {
  Serial.begin(baud_rate);
  delay(200);

  bool fresh = vfat32::vfs::needs_format(volume, volume_size);
  Serial.println(fresh ? "no volume in retained RAM: formatting" : "volume found in retained RAM: reusing");
  vfat32::mount_mode mode = fresh ? vfat32::mount_mode::format : vfat32::mount_mode::reuse;
  if (fs.mount(volume, volume_size, mode) != 0) {
    Serial.print("mount failed: ");
    Serial.println(vfat32::vfs::strerror(fs.last_error()));
    return;
  }

  if (fs.was_dirty()) {
    vfat32::check_report r;
    int clean = fs.check(&r, true);  // repair what can be repaired without guessing (spec §11)
    Serial.print("not cleanly unmounted: check repaired ");
    Serial.print(static_cast<unsigned long>(r.repaired));
    Serial.print(" item(s), volume ");
    Serial.println(clean == 0 ? "clean" : "still NOT clean");
  }

  // Count boots.
  unsigned long boots = 0;
  vfat32::file* f = fs.fopen("/boots.txt", "r");
  if (f) {
    char line[line_size];
    if (fs.fgets(line, line_size, f)) boots = strtoul(line, nullptr, 10);
    fs.fclose(f);
  }
  ++boots;
  f = fs.fopen("/boots.txt", "w");
  if (f) {
    fs.fprintf(f, "%lu\n", boots);
    fs.fclose(f);  // the data is in the volume now; only the clean bit is left unset
  }
  Serial.print("boot number ");
  Serial.println(boots);
}

void loop() {
  if (millis() > reset_after_ms) {
    Serial.println("resetting while mounted...");
    Serial.flush();
    restart();
  }
}
