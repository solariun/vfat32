// vfat32 — bare-metal link test for check_embedded (spec §15.1): the whole API on a 4 KiB RAM volume,
// built without exceptions, RTTI or heap (VFAT32_USE_HEAP=0), linked with --gc-sections.
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32.hpp"

// No C++ runtime on bare metal: the vtable of an abstract class needs this symbol (spec X4).
extern "C" void __cxa_pure_virtual() {
    for (;;) {
    }
}

namespace {

const size_t volume_bytes = 4096;

struct no_lock : vfat32::lock_interface {
    void lock() override { ++depth; }
    void unlock() override { --depth; }
    volatile int depth = 0;
};

uint8_t g_volume[volume_bytes];
vfat32::vfs g_fs;
no_lock g_lock;
volatile int g_result;

}  // namespace

int main() {
    g_fs.set_lock(&g_lock);
    vfat32::format_options options;
    options.fat_copies = 1;
    int rc = g_fs.mount(g_volume, sizeof g_volume, vfat32::mount_mode::format_if_needed, &options);
    g_fs.mkdir("/d");
    vfat32::file* f = g_fs.fopen("/d/log.txt", "a");
    g_fs.fprintf(f, "boot %d\n", rc);
    g_fs.fputs("hello\n", f);
    g_fs.fclose(f);
    char line[16];
    f = g_fs.fopen("/d/log.txt", "r");
    while (g_fs.fgets(line, sizeof line, f)) g_result += line[0];
    g_fs.fclose(f);
    int fd = g_fs.open("/raw", vfat32::o_rdwr | vfat32::o_creat);
    g_fs.write(fd, line, sizeof line);
    g_fs.lseek(fd, 0, vfat32::seek_set);
    g_fs.read(fd, line, sizeof line);
    g_fs.close(fd);
    vfat32::dir* d = g_fs.opendir("/");
    while (vfat32::dirent* e = g_fs.readdir(d)) g_result += e->d_name[0];
    g_fs.closedir(d);
    g_fs.rename("/raw", "/d/raw");
    vfat32::check_report report;
    g_result += g_fs.check(&report, true);
    g_result += g_fs.defrag(4);
    g_fs.remove("/d/raw");
    g_result += g_fs.umount();
    return g_result;
}
