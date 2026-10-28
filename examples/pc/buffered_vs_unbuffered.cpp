// buffered_vs_unbuffered — what a stream buffer buys you, measured.
//
// The volume is RAM, so "buffered" does not save device accesses: it saves the
// per-call work (lock, handle check, cluster lookup, size update). This program
// writes the same bytes several ways and prints the throughput of each:
//
//   fputc on a fully buffered stream (io_fbf, the default for fopen)
//   fputc on a stream with a user-supplied 4 KiB buffer (setvbuf)
//   fputc on a line-buffered stream (io_lbf: flushed at every '\n')
//   fputc on an unbuffered stream (io_nbf)
//   write() of one byte on a descriptor (open: always unbuffered)
//   fwrite / write of 4 KiB blocks (large requests bypass the buffer)
//
// Build: c++ -std=c++11 -O2 -Isrc examples/pc/buffered_vs_unbuffered.cpp src/*.cpp -o buffered_vs_unbuffered

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vfat32.hpp"

namespace {

const size_t volume_size = 32 * 1024 * 1024;
uint8_t volume[volume_size];

const size_t bytes_per_test = 8 * 1024 * 1024;  // 8 MiB per method
const size_t block_size = 4096;
const size_t block_rounds = 16;  // block writes are fast: repeat them so the clock can measure them
const size_t user_buffer_size = 4096;
const size_t line_length = 64;  // a '\n' every 64 bytes for the line-buffered test
const double bytes_per_mib = 1024.0 * 1024.0;
const double ns_per_s = 1e9;

char user_buffer[user_buffer_size];
uint8_t block[block_size];

double now_seconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / ns_per_s;
}

bool quiet = false;  // the warm-up pass prints nothing

void report(const char* method, size_t bytes, double seconds) {
    if (quiet) return;
    double mib_s = seconds > 0 ? static_cast<double>(bytes) / bytes_per_mib / seconds : 0;
    printf("  %-44s %9.1f MiB/s  (%.3f s)\n", method, mib_s, seconds);
}

int byte_at(size_t i) { return (i % line_length == line_length - 1) ? '\n' : 'a' + static_cast<int>(i % 26); }

// Write bytes_per_test bytes with fputc on a stream configured by `mode`/`buf`.
bool stream_bytes(vfat32::vfs& fs, const char* path, int mode, char* buf, size_t size, const char* label) {
    vfat32::file* f = fs.fopen(path, "w");
    if (!f) return false;
    if (mode != vfat32::io_fbf || buf) {
        if (fs.setvbuf(f, buf, mode, size) != 0) return false;
    }
    double t0 = now_seconds();
    for (size_t i = 0; i < bytes_per_test; ++i) {
        if (fs.fputc(byte_at(i), f) == vfat32::eof) return false;
    }
    if (fs.fclose(f) != 0) return false;
    report(label, bytes_per_test, now_seconds() - t0);
    return fs.remove(path) == 0;
}

bool fd_bytes(vfat32::vfs& fs, const char* path) {
    int fd = fs.open(path, vfat32::o_wronly | vfat32::o_creat | vfat32::o_trunc);
    if (fd < 0) return false;
    double t0 = now_seconds();
    for (size_t i = 0; i < bytes_per_test; ++i) {
        uint8_t c = static_cast<uint8_t>(byte_at(i));
        if (fs.write(fd, &c, 1) != 1) return false;
    }
    if (fs.close(fd) != 0) return false;
    report("write(fd, 1 byte)            [unbuffered]", bytes_per_test, now_seconds() - t0);
    return fs.remove(path) == 0;
}

bool block_writes(vfat32::vfs& fs, const char* path) {
    memset(block, 'b', block_size);
    const size_t total = bytes_per_test * block_rounds;

    double t0 = now_seconds();
    for (size_t round = 0; round < block_rounds; ++round) {
        vfat32::file* f = fs.fopen(path, "w");  // "w" truncates: every round allocates again
        if (!f) return false;
        for (size_t done = 0; done < bytes_per_test; done += block_size) {
            if (fs.fwrite(block, 1, block_size, f) != block_size) return false;
        }
        if (fs.fclose(f) != 0) return false;
    }
    report("fwrite(4 KiB blocks)         [bypasses buffer]", total, now_seconds() - t0);
    if (fs.remove(path) != 0) return false;

    t0 = now_seconds();
    for (size_t round = 0; round < block_rounds; ++round) {
        int fd = fs.open(path, vfat32::o_wronly | vfat32::o_creat | vfat32::o_trunc);
        if (fd < 0) return false;
        for (size_t done = 0; done < bytes_per_test; done += block_size) {
            if (fs.write(fd, block, block_size) != static_cast<long>(block_size)) return false;
        }
        if (fs.close(fd) != 0) return false;
    }
    report("write(fd, 4 KiB blocks)", total, now_seconds() - t0);
    return fs.remove(path) == 0;
}

// What buffering means for other readers: data sits in the writer's buffer until a flush.
bool show_visibility(vfat32::vfs& fs) {
    vfat32::file* w = fs.fopen("/visible.txt", "w");
    if (!w) return false;
    fs.fputs("pending", w);  // still in w's buffer
    struct vfat32::stat st;
    fs.stat("/visible.txt", &st);
    printf("  after fputs, before fflush: size on volume = %u\n", static_cast<unsigned>(st.st_size));
    fs.fflush(w);
    fs.stat("/visible.txt", &st);
    printf("  after fflush:               size on volume = %u\n", static_cast<unsigned>(st.st_size));

    fs.setvbuf(w, nullptr, vfat32::io_lbf, 0);  // line buffered: each '\n' flushes
    fs.fputs(" line\n", w);
    fs.stat("/visible.txt", &st);
    printf("  io_lbf after a '\\n':        size on volume = %u\n", static_cast<unsigned>(st.st_size));
    fs.fclose(w);
    return fs.remove("/visible.txt") == 0;
}

}  // namespace

int main() {
    memset(volume, 0, volume_size);  // touch every page once, so the first method does not pay the page faults
    vfat32::vfs fs;
    if (fs.mount(volume, volume_size, vfat32::mount_mode::format) != 0) {
        printf("mount failed: %s\n", vfat32::vfs::strerror(fs.last_error()));
        return 1;
    }
    printf("writing %u KiB per method on a %u MiB RAM volume\n", static_cast<unsigned>(bytes_per_test / 1024),
           static_cast<unsigned>(volume_size / (1024 * 1024)));

    quiet = true;  // warm-up: the CPU clock ramps up during the first tens of milliseconds
    stream_bytes(fs, "/warm.bin", vfat32::io_fbf, nullptr, 0, "");
    stream_bytes(fs, "/warm.bin", vfat32::io_nbf, nullptr, 0, "");
    quiet = false;

    bool ok = stream_bytes(fs, "/fbf.bin", vfat32::io_fbf, nullptr, 0, "fputc, io_fbf (internal buffer)")
              && stream_bytes(fs, "/user.bin", vfat32::io_fbf, user_buffer, user_buffer_size,
                              "fputc, io_fbf (4 KiB user buffer)")
              && stream_bytes(fs, "/lbf.bin", vfat32::io_lbf, nullptr, 0, "fputc, io_lbf (flush every 64 B)")
              && stream_bytes(fs, "/nbf.bin", vfat32::io_nbf, nullptr, 0, "fputc, io_nbf             [unbuffered]")
              && fd_bytes(fs, "/fd.bin") && block_writes(fs, "/block.bin");

    printf("visibility of buffered data to other readers:\n");
    ok = ok && show_visibility(fs);

    if (!ok) printf("a step failed: %s\n", vfat32::vfs::strerror(fs.last_error()));
    fs.umount();
    return ok ? 0 : 1;
}
