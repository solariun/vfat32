// vfat32_basic — a FAT32 volume in the RAM of a microcontroller.
//
// Works on ESP32 (64 KiB volume) and on AVR boards with enough RAM such as the
// Mega2560 (4 KiB volume). An Uno/Nano (2 KiB of RAM) is too small: the
// smallest FAT32 volume is 3 KiB (spec F6).
//
// RAM: with the default configuration the vfs object needs about 6 KB (8 handle
// rows with a 512-byte buffer each, 4 directory rows, two 255-unit name buffers).
// That is nothing on an ESP32 but too much for an AVR. The Arduino IDE cannot pass
// -D options per sketch, so on AVR shrink the configuration (spec §13) with
// compiler flags in the board's platform.local.txt, for example:
//   compiler.cpp.extra_flags=-DVFAT32_MAX_OPEN_FILES=2 -DVFAT32_MAX_OPEN_DIRS=1
//                            -DVFAT32_BUFFER_SIZE=32 -DVFAT32_NAME_MAX=32
//                            -DVFAT32_PRINTF_MAX=48 -DVFAT32_USE_HEAP=0
// With those values the object needs well under 1 KB.

#include <vfat32.hpp>

#if defined(__AVR__)
static const size_t volume_size = 4 * 1024;
#else
static const size_t volume_size = 64 * 1024;
#endif
static const int line_size = 48;
static const unsigned long baud_rate = 115200;

static uint8_t volume[volume_size];  // static RAM: zero at power-up, so it is formatted once
static vfat32::vfs fs;               // static: keeps the object off the small AVR stack

static void print_error(const char* what) {
  Serial.print(what);
  Serial.print(" failed: ");
  Serial.println(vfat32::vfs::strerror(fs.last_error()));
}

void setup() {
  Serial.begin(baud_rate);
  while (!Serial) {
  }

  Serial.print("needs_format: ");
  Serial.println(vfat32::vfs::needs_format(volume, volume_size) ? "yes" : "no");

  if (fs.mount(volume, volume_size, vfat32::mount_mode::format_if_needed) != 0) {
    print_error("mount");
    return;
  }

  // Write a file.
  vfat32::file* f = fs.fopen("/hello.txt", "w");
  if (!f) {
    print_error("fopen(w)");
    return;
  }
  fs.fputs("hello from vfat32\n", f);
  fs.fprintf(f, "millis=%lu\n", millis());
  fs.fclose(f);

  // Read it back.
  f = fs.fopen("/hello.txt", "r");
  if (!f) {
    print_error("fopen(r)");
    return;
  }
  char line[line_size];
  while (fs.fgets(line, line_size, f)) Serial.print(line);
  fs.fclose(f);

  // List the root.
  vfat32::dir* d = fs.opendir("/");
  if (d) {
    Serial.println("root:");
    while (vfat32::dirent* e = fs.readdir(d)) {
      Serial.print(e->d_type == vfat32::dt_dir ? "  <DIR> " : "  ");
      Serial.print(e->d_name);
      Serial.print("  ");
      Serial.println(static_cast<unsigned long>(e->d_size));
    }
    fs.closedir(d);
  }

  // Free space.
  struct vfat32::statvfs sv;
  if (fs.statvfs(&sv) == 0) {
    Serial.print("free: ");
    Serial.print(static_cast<unsigned long>(sv.f_bfree));
    Serial.print(" of ");
    Serial.print(static_cast<unsigned long>(sv.f_blocks));
    Serial.print(" clusters of ");
    Serial.print(static_cast<unsigned long>(sv.f_bsize));
    Serial.println(" bytes");
  }
}

void loop() {
}
