// defrag_demo — fragment a volume on purpose, then defragment it step by step.
//
// Eight files are grown in turns, a few bytes each, so their clusters interleave;
// then every other file is removed, leaving holes. check() reports how many
// chains are fragmented and how many separate runs of free space exist.
// defrag(64) moves at most 64 clusters per call and returns 1 while work
// remains, so a program (or an RTOS task) can do other things between steps —
// the volume is consistent after every call (spec §12, D4).
//
// Build: c++ -std=c++11 -Isrc examples/pc/defrag_demo.cpp src/*.cpp -o defrag_demo

#include <stdio.h>
#include <string.h>

#include "vfat32.hpp"

namespace {

const size_t volume_size = 512 * 1024;
uint8_t volume[volume_size];

const int file_count = 8;
const int rounds = 40;
const size_t chunk = 700;  // > one 512-byte cluster per round per file
const uint32_t moves_per_step = 64;

uint8_t pattern(int file, size_t offset) { return static_cast<uint8_t>(file * 31 + offset * 7); }

void name_of(int i, char* out, size_t size) { snprintf(out, size, "/file_%d.bin", i); }

void print_state(vfat32::vfs& fs, const char* when) {
    vfat32::check_report r;
    int clean = fs.check(&r);
    printf("%-16s fragmented chains: %3u   free extents: %3u   used clusters: %4u   %s\n", when,
           static_cast<unsigned>(r.fragmented), static_cast<unsigned>(r.free_extents),
           static_cast<unsigned>(r.used_clusters), clean == 0 ? "clean" : "NOT clean");
}

// Re-reads every remaining file and compares it with the pattern it was written with.
bool verify(vfat32::vfs& fs) {
    uint8_t buf[chunk];
    for (int i = 1; i < file_count; i += 2) {
        char path[32];
        name_of(i, path, sizeof path);
        vfat32::file* f = fs.fopen(path, "r");
        if (!f) return false;
        size_t offset = 0;
        size_t n;
        while ((n = fs.fread(buf, 1, sizeof buf, f)) > 0) {
            for (size_t k = 0; k < n; ++k) {
                if (buf[k] != pattern(i, offset + k)) {
                    fs.fclose(f);
                    return false;
                }
            }
            offset += n;
        }
        fs.fclose(f);
        if (offset != rounds * chunk) return false;
    }
    return true;
}

}  // namespace

int main() {
    vfat32::vfs fs;
    if (fs.mount(volume, volume_size, vfat32::mount_mode::format) != 0) return 1;

    // Interleaved growth: every round appends one chunk to each file in turn.
    uint8_t buf[chunk];
    for (int round = 0; round < rounds; ++round) {
        for (int i = 0; i < file_count; ++i) {
            char path[32];
            name_of(i, path, sizeof path);
            vfat32::file* f = fs.fopen(path, "a");
            if (!f) return 1;
            for (size_t k = 0; k < chunk; ++k) buf[k] = pattern(i, round * chunk + k);
            fs.fwrite(buf, 1, chunk, f);
            fs.fclose(f);
        }
    }
    print_state(fs, "interleaved:");

    for (int i = 0; i < file_count; i += 2) {  // remove every other file: holes in the free space
        char path[32];
        name_of(i, path, sizeof path);
        fs.remove(path);
    }
    print_state(fs, "after removes:");

    int step = 0;
    int r;
    while ((r = fs.defrag(moves_per_step)) == 1) {
        ++step;
        if (step % 4 == 0) {
            char label[32];
            snprintf(label, sizeof label, "step %d:", step);
            print_state(fs, label);
        }
    }
    if (r != 0) {
        printf("defrag failed: %s\n", vfat32::vfs::strerror(fs.last_error()));
        return 1;
    }
    printf("defrag finished after %d incremental steps of up to %u moves\n", step + 1,
           static_cast<unsigned>(moves_per_step));
    print_state(fs, "defragmented:");

    bool ok = verify(fs);
    printf("file contents after defrag: %s\n", ok ? "identical" : "DIFFERENT");
    fs.umount();
    return ok ? 0 : 1;
}
