// image_file — a volume that survives between runs, kept in an image file.
//
// Each run: load the image (if it exists) into memory, ask needs_format(),
// mount with reuse (or format the first time), report was_dirty(), append one
// line to /boot_count.txt, run check() and print its report, unmount (marks
// the volume clean), and write the memory back to the file.
//
// The image is a real FAT32: inspect it with the host's tools, e.g.
//   macOS:  fsck_msdos -n vfat32.img
//   Linux:  fsck.fat -n vfat32.img     mdir -i vfat32.img ::
// (Volumes under 65,525 clusters are FAT32 by layout only: Linux and macOS
// accept them, Windows does not — spec G1. This example uses 40 MiB, enough
// clusters to be FAT32 by every rule.)
//
// Build: c++ -std=c++11 -Isrc examples/pc/image_file.cpp src/*.cpp -o image_file
// Run:   ./image_file [image]      (default: vfat32.img)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfat32.hpp"

namespace {

const size_t volume_size = 40 * 1024 * 1024;
const char* const default_image = "vfat32.img";
const int line_size = 96;

// Loads the image into `memory`; returns false when there is no such file.
bool load_image(const char* path, uint8_t* memory, size_t size) {
    FILE* in = ::fopen(path, "rb");
    if (!in) return false;
    size_t got = ::fread(memory, 1, size, in);
    ::fclose(in);
    printf("loaded %s: %u bytes\n", path, static_cast<unsigned>(got));
    return true;
}

bool save_image(const char* path, const uint8_t* memory, size_t size) {
    FILE* out = ::fopen(path, "wb");
    if (!out) return false;
    bool ok = ::fwrite(memory, 1, size, out) == size;
    ok = (::fclose(out) == 0) && ok;
    return ok;
}

void print_report(const vfat32::check_report& r) {
    printf("check: %u files, %u directories, %u used / %u free clusters, %u free extents, %u fragmented\n",
           static_cast<unsigned>(r.files), static_cast<unsigned>(r.directories),
           static_cast<unsigned>(r.used_clusters), static_cast<unsigned>(r.free_clusters),
           static_cast<unsigned>(r.free_extents), static_cast<unsigned>(r.fragmented));
    printf("       errors %u (lost %u, cross-linked %u, bad chains %u, size %u, entries %u, FAT copies %u), "
           "repaired %u\n",
           static_cast<unsigned>(r.errors()), static_cast<unsigned>(r.lost_clusters),
           static_cast<unsigned>(r.cross_links), static_cast<unsigned>(r.bad_chains),
           static_cast<unsigned>(r.size_errors), static_cast<unsigned>(r.bad_entries),
           static_cast<unsigned>(r.fat_mismatches), static_cast<unsigned>(r.repaired));
}

int count_lines(vfat32::vfs& fs, const char* path) {
    vfat32::file* f = fs.fopen(path, "r");
    if (!f) return 0;
    int lines = 0;
    char line[line_size];
    while (fs.fgets(line, line_size, f)) ++lines;
    fs.fclose(f);
    return lines;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : default_image;

    uint8_t* memory = static_cast<uint8_t*>(calloc(1, volume_size));
    if (!memory) {
        printf("out of memory\n");
        return 1;
    }

    bool loaded = load_image(path, memory, volume_size);
    if (!loaded) printf("%s does not exist yet: starting from blank memory\n", path);
    bool needs = vfat32::vfs::needs_format(memory, volume_size);
    printf("needs_format: %s\n", needs ? "yes (no complete FAT32 here)" : "no (a complete FAT32 is there)");

    vfat32::vfs fs;
    vfat32::mount_mode mode = needs ? vfat32::mount_mode::format : vfat32::mount_mode::reuse;
    if (fs.mount(memory, volume_size, mode) != 0) {
        printf("mount failed: %s\n", vfat32::vfs::strerror(fs.last_error()));
        free(memory);
        return 1;
    }
    printf("mounted (%s); was_dirty: %s\n", needs ? "formatted" : "reused",
           fs.was_dirty() ? "yes — the last session did not umount" : "no");

    int boots = count_lines(fs, "/boot_count.txt") + 1;
    vfat32::file* f = fs.fopen("/boot_count.txt", "a");
    if (f) {
        fs.fprintf(f, "run %d\n", boots);
        fs.fclose(f);
    }
    printf("this is run %d\n", boots);

    vfat32::check_report report;
    int clean = fs.check(&report);
    print_report(report);
    printf("volume is %s\n", clean == 0 ? "clean" : "NOT clean");

    fs.umount();  // sets the clean bit: the next run's was_dirty() is false
    if (!save_image(path, memory, volume_size)) {
        printf("could not write %s\n", path);
        free(memory);
        return 1;
    }
    printf("wrote %s (%u bytes); try: fsck_msdos -n %s  (macOS) or fsck.fat -n %s (Linux)\n", path,
           static_cast<unsigned>(volume_size), path, path);
    free(memory);
    return clean == 0 ? 0 : 1;
}
