// quick_start — the shortest tour of vfat32 on a PC.
//
// A static array becomes a FAT32 volume; we create a directory, write a file with
// fputs/fprintf, read it back with fgets, list the root and print the free space.
//
// Build: c++ -std=c++11 -Isrc examples/pc/quick_start.cpp src/*.cpp -o quick_start

#include <stdio.h>
#include <string.h>

#include "vfat32.hpp"

namespace {

const size_t volume_size = 256 * 1024;  // 256 KiB of RAM for the whole volume
uint8_t volume[volume_size];            // static: zero at start, so it needs formatting
const int line_size = 128;

int fail(const vfat32::vfs& fs, const char* what) {
    printf("%s failed: %s\n", what, vfat32::vfs::strerror(fs.last_error()));
    return 1;
}

}  // namespace

int main() {
    vfat32::vfs fs;

    // 1. Is there already a FAT32 in the memory? (No: it is all zeros.)
    printf("needs_format: %s\n", vfat32::vfs::needs_format(volume, volume_size) ? "yes" : "no");

    // 2. Mount, formatting only if needed.
    if (fs.mount(volume, volume_size, vfat32::mount_mode::format_if_needed) != 0) return fail(fs, "mount");
    printf("mounted %u KiB volume\n", static_cast<unsigned>(volume_size / 1024));

    // 3. A directory and a file inside it.
    if (fs.mkdir("/logs") != 0) return fail(fs, "mkdir");

    vfat32::file* f = fs.fopen("/logs/Hello World.txt", "w");  // a long name: stored with LFN slots
    if (!f) return fail(fs, "fopen(w)");
    fs.fputs("first line\n", f);
    for (int i = 1; i <= 3; ++i) fs.fprintf(f, "line %d of %d\n", i, 3);
    fs.fclose(f);

    // 4. Read it back line by line.
    f = fs.fopen("/logs/hello world.txt", "r");  // names are case-insensitive (ASCII)
    if (!f) return fail(fs, "fopen(r)");
    char line[line_size];
    printf("--- /logs/Hello World.txt ---\n");
    while (fs.fgets(line, line_size, f)) printf("%s", line);
    fs.fclose(f);

    // 5. One more file in the root, then list both directories.
    f = fs.fopen("/readme.txt", "w");  // fits 8.3 exactly: no LFN slot is written
    if (!f) return fail(fs, "fopen(readme)");
    fs.fputs("vfat32 quick start\n", f);
    fs.fclose(f);

    const char* dirs[] = {"/", "/logs"};
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; ++d) {
        vfat32::dir* dp = fs.opendir(dirs[d]);
        if (!dp) return fail(fs, "opendir");
        printf("--- listing of %s ---\n", dirs[d]);
        while (vfat32::dirent* e = fs.readdir(dp)) {
            printf("  %-5s %8u  %s\n", e->d_type == vfat32::dt_dir ? "<DIR>" : "", static_cast<unsigned>(e->d_size),
                   e->d_name);
        }
        fs.closedir(dp);
    }

    // 6. Space left.
    struct vfat32::statvfs sv;
    if (fs.statvfs(&sv) != 0) return fail(fs, "statvfs");
    printf("clusters: %u x %u bytes, %u free (%u bytes)\n", static_cast<unsigned>(sv.f_blocks),
           static_cast<unsigned>(sv.f_bsize), static_cast<unsigned>(sv.f_bfree),
           static_cast<unsigned>(sv.f_bfree * sv.f_bsize));

    // 7. Unmount: flushes everything and marks the volume clean.
    if (fs.umount() != 0) return fail(fs, "umount");
    printf("needs_format after umount: %s\n", vfat32::vfs::needs_format(volume, volume_size) ? "yes" : "no");
    return 0;
}
