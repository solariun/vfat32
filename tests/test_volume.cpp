// vfat32 tests — volume lifecycle: probe (§5.1), format geometry (§4.1–§4.4), mount modes, clean bit, umount (§5.2).

#include "test_harness.hpp"

using test::geometry;
using test::le16;
using test::le32;
using test::one_mib;
using test::volume;

namespace {

// F5, written out from the spec.
uint32_t f5_fat_sectors(uint32_t total, uint32_t rsvd, uint32_t nfats, uint32_t spc, uint32_t bps,
                        uint32_t& clusters) {
    const uint32_t max_clusters = 0x0FFFFFF5;
    uint32_t fat = 0;
    for (;;) {
        uint32_t c = (total - rsvd - nfats * fat) / spc;
        if (c > max_clusters) c = max_clusters;
        uint32_t need = static_cast<uint32_t>((static_cast<uint64_t>(c + 2) * 4 + bps - 1) / bps);
        if (need <= fat) break;
        fat = need;
    }
    clusters = (total - rsvd - nfats * fat) / spc;
    if (clusters > max_clusters) clusters = max_clusters;
    return fat;
}

vfat32::timestamp fixed_now;
vfat32::timestamp fixed_clock() { return fixed_now; }

// Formats `size` bytes into a fresh buffer with options and returns it unmounted (caller frees).
uint8_t* formatted_image(size_t size, const vfat32::format_options* options = nullptr) {
    uint8_t* m = static_cast<uint8_t*>(calloc(1, size));
    vfat32::vfs fs;
    if (fs.mount(m, size, vfat32::mount_mode::format, options) != 0) {
        free(m);
        return nullptr;
    }
    fs.umount();
    return m;
}

}  // namespace

TEST(volume_needs_format_blank_garbage_tiny) {
    const size_t size = 64 * 1024;
    uint8_t* m = static_cast<uint8_t*>(calloc(1, size));
    REQUIRE(m);
    CHECK(vfat32::vfs::needs_format(m, size));
    test::rng r(1);
    r.fill(m, size);
    CHECK(vfat32::vfs::needs_format(m, size));
    CHECK(vfat32::vfs::needs_format(m, 100));  // P1
    free(m);
}

TEST(volume_needs_format_false_on_complete_fat32) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(!vfat32::vfs::needs_format(v.mem, v.size));  // also while mounted
    CHECK_EQ(v.fs.umount(), 0);
    CHECK(!vfat32::vfs::needs_format(v.mem, v.size));
    CHECK(vfat32::vfs::needs_format(v.mem, v.size / 2));  // P9: the volume does not fit
    CHECK(vfat32::vfs::needs_format(v.mem, 511));         // P1
}

TEST(volume_needs_format_memory_larger_than_volume) {
    const size_t big = 2 * one_mib;
    uint8_t* m = static_cast<uint8_t*>(calloc(1, big));
    REQUIRE(m);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format), 0);
    REQUIRE_EQ(fs.umount(), 0);
    CHECK(!vfat32::vfs::needs_format(m, big));  // P9: larger memory is fine
    vfat32::vfs fs2;
    CHECK_EQ(fs2.mount(m, big, vfat32::mount_mode::reuse), 0);
    vfat32::statvfs s;
    CHECK_EQ(fs2.statvfs(&s), 0);
    geometry g = test::read_geometry(m);
    CHECK_EQ(s.f_blocks, g.clusters);  // the volume, not the memory
    fs2.umount();
    free(m);
}

// One P-rule broken at a time on a valid image: needs_format must say true.
TEST(volume_probe_rules_each_violation_detected) {
    uint8_t* good = formatted_image(one_mib);
    REQUIRE(good);
    geometry g = test::read_geometry(good);
    uint8_t* m = static_cast<uint8_t*>(malloc(one_mib));
    REQUIRE(m);
    const int cases = 27;
    for (int i = 0; i < cases; ++i) {
        memcpy(m, good, one_mib);
        const char* rule = "";
        switch (i) {
            case 0: rule = "P2 byte 510"; m[510] = 0; break;
            case 1: rule = "P2 byte 511"; m[511] = 0; break;
            case 2: rule = "P3 jmp opcode"; m[0] = 0; break;
            case 3: rule = "P3 EB without 90"; m[2] = 0; break;
            case 4: rule = "P4 bps 256"; test::put16(m + 11, 256); break;
            case 5: rule = "P4 bps 768"; test::put16(m + 11, 768); break;
            case 6: rule = "P5 spc 3"; m[13] = 3; break;
            case 7: rule = "P5 spc 0"; m[13] = 0; break;
            case 8: rule = "P6 rsvd 0"; test::put16(m + 14, 0); break;
            case 9: rule = "P6 nfats 0"; m[16] = 0; break;
            case 10: rule = "P7 RootEntCnt"; test::put16(m + 17, 512); break;
            case 11: rule = "P7 TotSec16"; test::put16(m + 19, 100); break;
            case 12: rule = "P7 FATSz16"; test::put16(m + 22, 1); break;
            case 13: rule = "P7 FATSz32 0"; test::put32(m + 36, 0); break;
            case 14: rule = "P7 FSVer"; test::put16(m + 42, 1); break;
            case 15: rule = "P8 media 0x00"; m[21] = 0x00; break;
            case 16: rule = "P8 media 0xF5"; m[21] = 0xF5; break;
            case 17: rule = "P9 TotSec32 beyond memory"; test::put32(m + 32, g.total + 1); break;
            case 18: rule = "P10 data beyond volume"; test::put32(m + 36, g.total); break;
            case 19: rule = "P10 FAT too small"; test::put32(m + 36, 1); break;
            case 20: rule = "P10 active FAT out of range"; test::put16(m + 40, 0x0085); break;
            case 21: rule = "P11 RootClus 0"; test::put32(m + 44, 0); break;
            case 22: rule = "P11 RootClus 1"; test::put32(m + 44, 1); break;
            case 23: rule = "P11 RootClus past end"; test::put32(m + 44, g.clusters + 2); break;
            case 24: rule = "P11 FAT[root] free"; test::fat_put(m, g, g.root, 0); break;
            case 25: rule = "P11 FAT[root] bad"; test::fat_put(m, g, g.root, test::fat_bad); break;
            case 26: rule = "P12 FAT[0] media mismatch"; test::fat_put(m, g, 0, 0x0FFFFFF0); break;
        }
        if (!vfat32::vfs::needs_format(m, one_mib)) {
            printf("    rule not enforced: %s\n", rule);
            CHECK(vfat32::vfs::needs_format(m, one_mib));
        }
        vfat32::vfs fs;
        if (fs.mount(m, one_mib, vfat32::mount_mode::reuse) == 0) {
            printf("    reuse accepted a broken image: %s\n", rule);
            CHECK(false);
            fs.umount();
        } else {
            CHECK_EQ(fs.last_error(), vfat32::error::no_filesystem);
        }
    }
    free(m);
    free(good);
}

// Things the probe must NOT require (§5.1, F8, informational fields).
TEST(volume_probe_accepts_valid_variants) {
    uint8_t* good = formatted_image(one_mib);
    REQUIRE(good);
    geometry g = test::read_geometry(good);
    uint8_t* m = static_cast<uint8_t*>(malloc(one_mib));
    REQUIRE(m);
    for (int i = 0; i < 6; ++i) {
        memcpy(m, good, one_mib);
        const char* what = "";
        switch (i) {
            case 0: what = "FSInfo zeroed (F8)"; memset(m + g.fsinfo * g.bps, 0, g.bps); break;
            case 1: what = "ExtFlags active FAT 1, no mirroring"; test::put16(m + 40, 0x0081); break;
            case 2:
                what = "media F0 with matching FAT[0]";
                m[21] = 0xF0;
                test::fat_put(m, g, 0, 0x0FFFFFF0);
                break;
            case 3: what = "E9 jump"; m[0] = 0xE9; m[1] = 0x10; m[2] = 0x00; break;
            case 4: what = "OEM name changed"; memcpy(m + 3, "OTHEROEM", 8); break;
            case 5: what = "FilSysType changed"; memcpy(m + 82, "        ", 8); break;
        }
        if (vfat32::vfs::needs_format(m, one_mib)) {
            printf("    valid variant rejected: %s\n", what);
            CHECK(!vfat32::vfs::needs_format(m, one_mib));
        }
    }
    free(m);
    free(good);
}

TEST(volume_mount_modes) {
    uint8_t* m = static_cast<uint8_t*>(calloc(1, one_mib));
    REQUIRE(m);
    {
        vfat32::vfs fs;
        CHECK_EQ(fs.mount(m, one_mib, vfat32::mount_mode::reuse), -1);
        CHECK_EQ(fs.last_error(), vfat32::error::no_filesystem);
        CHECK(!fs.mounted());
        CHECK_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format_if_needed), 0);
        CHECK(fs.mounted());
        CHECK(test::write_text(fs, "/keep.txt", "kept"));
        CHECK_EQ(fs.umount(), 0);
        CHECK(!fs.mounted());
    }
    {
        vfat32::vfs fs;
        REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::reuse), 0);
        CHECK(test::text_equals(fs, "/keep.txt", "kept"));
        CHECK_EQ(fs.umount(), 0);
    }
    {
        vfat32::vfs fs;
        REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format_if_needed), 0);
        CHECK(test::text_equals(fs, "/keep.txt", "kept"));  // a complete FAT32 is kept
        CHECK_EQ(fs.umount(), 0);
    }
    {
        vfat32::vfs fs;
        REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format), 0);
        CHECK(!test::exists(fs, "/keep.txt"));  // always formats
        CHECK_EQ(fs.umount(), 0);
    }
    free(m);
}

TEST(volume_mount_null_and_double_mount) {
    vfat32::vfs fs;
    CHECK_EQ(fs.mount(nullptr, one_mib, vfat32::mount_mode::format), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::invalid);
    uint8_t* m = static_cast<uint8_t*>(calloc(1, one_mib));
    REQUIRE(m);
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format), 0);
    CHECK_EQ(fs.mount(m, one_mib, vfat32::mount_mode::reuse), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::busy);
    CHECK_EQ(fs.umount(), 0);
    free(m);
}

TEST(volume_not_mounted_errors) {
    vfat32::vfs fs;
    CHECK(!fs.mounted());
    CHECK(fs.fopen("/a", "w") == nullptr);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK_EQ(fs.mkdir("/d"), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK_EQ(fs.open("/a", vfat32::o_rdonly), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK(fs.opendir("/") == nullptr);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK_EQ(fs.umount(), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK_EQ(fs.format(), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    vfat32::statvfs s;
    CHECK_EQ(fs.statvfs(&s), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(fs.check(&r), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
    CHECK_EQ(fs.defrag(), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::not_mounted);
}

// §4.2 / §4.3 / F10 for the default options on 1 MiB (2048 sectors).
TEST(volume_boot_sector_fsinfo_and_fat_bytes) {
    uint8_t* m = formatted_image(one_mib);
    REQUIRE(m);
    CHECK_EQ(m[0], 0xEB);
    CHECK_EQ(m[1], 0x58);
    CHECK_EQ(m[2], 0x90);
    CHECK(memcmp(m + 3, "MSWIN4.1", 8) == 0);
    CHECK_EQ(le16(m + 11), 512);                // F1
    CHECK_EQ(m[13], 1);                         // F2: <= 260 MiB -> 512 B clusters
    CHECK_EQ(le16(m + 14), 8);                  // F3: < 4096 sectors -> 8
    CHECK_EQ(m[16], 2);                         // F4
    CHECK_EQ(le16(m + 17), 0);
    CHECK_EQ(le16(m + 19), 0);
    CHECK_EQ(m[21], 0xF8);
    CHECK_EQ(le16(m + 22), 0);
    CHECK_EQ(le16(m + 24), 63);
    CHECK_EQ(le16(m + 26), 255);
    CHECK_EQ(le32(m + 28), 0u);
    CHECK_EQ(le32(m + 32), 2048u);
    uint32_t clusters = 0;
    uint32_t fat = f5_fat_sectors(2048, 8, 2, 1, 512, clusters);
    CHECK_EQ(fat, 16u);
    CHECK_EQ(le32(m + 36), fat);                // F5
    CHECK_EQ(le16(m + 40), 0);                  // mirroring
    CHECK_EQ(le16(m + 42), 0);
    CHECK_EQ(le32(m + 44), 2u);
    CHECK_EQ(le16(m + 48), 1);
    CHECK_EQ(le16(m + 50), 6);                  // F3 backup
    CHECK_EQ(m[64], 0x80);
    CHECK_EQ(m[66], 0x29);
    CHECK(memcmp(m + 71, "NO NAME    ", 11) == 0);
    CHECK(memcmp(m + 82, "FAT32   ", 8) == 0);
    CHECK_EQ(m[510], 0x55);
    CHECK_EQ(m[511], 0xAA);
    CHECK(memcmp(m, m + 6 * 512, 512) == 0);    // backup boot sector at 6

    const uint8_t* fsi = m + 512;
    CHECK_EQ(le32(fsi + 0), 0x41615252u);
    CHECK_EQ(le32(fsi + 484), 0x61417272u);
    CHECK_EQ(le32(fsi + 488), clusters - 1);    // free: everything but the root
    CHECK_EQ(le32(fsi + 508), 0xAA550000u);
    CHECK_EQ(le32(m + 7 * 512), 0x41615252u);   // backup FSInfo at 7

    geometry g = test::read_geometry(m);
    CHECK_EQ(g.clusters, clusters);
    CHECK_EQ(test::fat_raw(m, g, 0), 0x0FFFFFF8u);   // F10
    CHECK_EQ(test::fat_raw(m, g, 1), 0x0FFFFFFFu);   // clean after umount
    CHECK(test::fat_get(m, g, 2) >= test::fat_eoc_min);
    CHECK_EQ(test::fat_get(m, g, 3), 0u);
    CHECK(memcmp(m + g.fat_offset, m + g.fat_offset + g.fat_sectors * g.bps, g.fat_sectors * g.bps) == 0);
    const uint8_t* root = m + g.data_offset;
    bool zero = true;
    for (uint32_t i = 0; i < g.cluster_bytes; ++i)
        if (root[i]) zero = false;
    CHECK(zero);  // F7: root zeroed, no label entry for NO NAME
    free(m);
}

TEST(volume_label_and_volume_id_options) {
    vfat32::format_options o;
    o.volume_id = 0x12345678;
    strcpy(o.volume_label, "MYVOL");
    uint8_t* m = formatted_image(one_mib, &o);
    REQUIRE(m);
    CHECK_EQ(le32(m + 67), 0x12345678u);
    CHECK(memcmp(m + 71, "MYVOL      ", 11) == 0);
    geometry g = test::read_geometry(m);
    const uint8_t* slot0 = m + g.data_offset;
    CHECK(memcmp(slot0, "MYVOL      ", 11) == 0);  // F7: label entry in the root
    CHECK_EQ(slot0[11], vfat32::attr_volume_id);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::reuse), 0);
    CHECK_EQ(test::count_entries(fs, "/"), 0);  // D11: never returned
    fs.umount();
    free(m);
}

TEST(volume_id_from_clock) {
    fixed_now.year = 2026;
    fixed_now.month = 10;
    fixed_now.day = 28;
    fixed_now.hour = 13;
    fixed_now.minute = 7;
    fixed_now.second = 30;
    uint8_t* m = static_cast<uint8_t*>(calloc(1, one_mib));
    REQUIRE(m);
    vfat32::vfs fs;
    fs.set_clock(fixed_clock);
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format), 0);
    uint32_t date = ((2026 - 1980) << 9) | (10 << 5) | 28;
    uint32_t time = (13 << 11) | (7 << 5) | (30 / 2);
    CHECK_EQ(le32(m + 67), date * 65536 + time);
    fs.umount();
    free(m);
}

TEST(volume_geometry_options_f1_to_f4) {
    struct row {
        size_t size;
        uint16_t bps;
        uint8_t spc;
        uint8_t fats;
        uint16_t rsvd;
        uint32_t want_spc;
        uint32_t want_rsvd;
        uint32_t want_backup;
        uint32_t want_fats;
    };
    const row rows[] = {
        {32 * 1024, 512, 0, 2, 0, 1, 2, 0, 2},       // < 128 sectors: 2 reserved, no backup
        {one_mib, 512, 0, 2, 0, 1, 8, 6, 2},          // < 4096 sectors: 8
        {4 * one_mib, 512, 0, 2, 0, 1, 32, 6, 2},     // >= 4096 sectors: 32
        {one_mib, 4096, 0, 2, 0, 1, 8, 6, 2},         // 256 sectors of 4 KiB, cluster = one sector
        {one_mib, 512, 8, 2, 0, 8, 8, 6, 2},          // forced spc
        {one_mib, 512, 0, 1, 0, 1, 8, 6, 1},          // one FAT
        {one_mib, 512, 0, 2, 16, 1, 16, 6, 2},        // forced reserved >= 8: backup
        {one_mib, 512, 0, 2, 4, 1, 4, 0, 2},          // forced reserved < 8: no backup
        {one_mib, 512, 0, 2, 7, 1, 7, 0, 2},
        {one_mib, 1024, 0, 2, 0, 1, 8, 6, 2},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        const row& r = rows[i];
        vfat32::format_options o;
        o.bytes_per_sector = r.bps;
        o.sectors_per_cluster = r.spc;
        o.fat_copies = r.fats;
        o.reserved_sectors = r.rsvd;
        uint8_t* m = formatted_image(r.size, &o);
        if (!m) {
            printf("    row %lu: format failed\n", static_cast<unsigned long>(i));
            CHECK(m != nullptr);
            continue;
        }
        geometry g = test::read_geometry(m);
        CHECK_EQ(g.bps, r.bps);
        CHECK_EQ(g.spc, r.want_spc);
        CHECK_EQ(g.rsvd, r.want_rsvd);
        CHECK_EQ(g.backup, r.want_backup);
        CHECK_EQ(g.nfats, r.want_fats);
        CHECK_EQ(g.total, static_cast<uint32_t>(r.size / r.bps));
        uint32_t clusters = 0;
        CHECK_EQ(g.fat_sectors, f5_fat_sectors(g.total, g.rsvd, g.nfats, g.spc, g.bps, clusters));
        CHECK_EQ(g.clusters, clusters);
        if (g.backup) CHECK(memcmp(m, m + g.backup * g.bps, 512) == 0);
        CHECK(!vfat32::vfs::needs_format(m, r.size));
        free(m);
    }
}

// F2: > 260 MiB uses 4 KiB clusters.
TEST(volume_geometry_auto_cluster_above_260_mib) {
    const size_t size = 270 * one_mib;
    uint8_t* m = formatted_image(size);
    REQUIRE(m);
    geometry g = test::read_geometry(m);
    CHECK_EQ(g.spc, 8u);
    CHECK_EQ(g.rsvd, 32u);
    uint32_t clusters = 0;
    CHECK_EQ(g.fat_sectors, f5_fat_sectors(g.total, g.rsvd, g.nfats, g.spc, g.bps, clusters));
    free(m);
}

TEST(volume_invalid_format_options) {
    for (int i = 0; i < 7; ++i) {
        vfat32::format_options o;
        switch (i) {
            case 0: o.bytes_per_sector = 1000; break;
            case 1: o.bytes_per_sector = 256; break;
            case 2: o.sectors_per_cluster = 3; break;
            case 3: o.fat_copies = 0; break;
            case 4: o.fat_copies = 3; break;
            case 5: o.reserved_sectors = 1; break;
            case 6: o.bytes_per_sector = 4096; o.sectors_per_cluster = 16; break;  // 64 KiB > 32 KiB
        }
        uint8_t* m = static_cast<uint8_t*>(calloc(1, one_mib));
        vfat32::vfs fs;
        CHECK_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format, &o), -1);
        CHECK_EQ(fs.last_error(), vfat32::error::invalid);
        free(m);
    }
}

TEST(volume_format_defaults_struct) {
    vfat32::format_options o;
    CHECK_EQ(o.bytes_per_sector, 512);
    CHECK_EQ(o.sectors_per_cluster, 0);
    CHECK_EQ(o.fat_copies, 2);
    CHECK_EQ(o.reserved_sectors, 0);
    CHECK_EQ(o.volume_id, 0u);
}

// F6: 6 sectors (3 KiB) is the smallest with the defaults; 5 with one FAT copy.
TEST(volume_minimum_size_f6) {
    {
        volume v(3072);
        CHECK(v.ok());
        if (v.ok()) {
            vfat32::statvfs s;
            CHECK_EQ(v.fs.statvfs(&s), 0);
            CHECK_EQ(s.f_blocks, 2u);
            CHECK_EQ(s.f_bfree, 1u);
            CHECK(test::write_text(v.fs, "/a", "x"));
            CHECK(test::text_equals(v.fs, "/a", "x"));
            CHECK(test::check_clean(v.fs));
        }
    }
    {
        volume v(2560);
        CHECK_EQ(v.rc, -1);
        CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    }
    vfat32::format_options one;
    one.fat_copies = 1;
    {
        volume v(2560, &one);
        CHECK(v.ok());
    }
    {
        volume v(2048, &one);
        CHECK_EQ(v.rc, -1);
        CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    }
}

// F7: format leaves the data region past the root alone.
TEST(volume_format_does_not_touch_data_region) {
    uint8_t* m = static_cast<uint8_t*>(malloc(one_mib));
    REQUIRE(m);
    memset(m, 0xAA, one_mib);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::format), 0);
    geometry g = test::read_geometry(m);
    CHECK_EQ(test::cluster_data(m, g, 3)[0], 0xAA);
    CHECK_EQ(test::cluster_data(m, g, g.clusters + 1)[g.cluster_bytes - 1], 0xAA);
    CHECK_EQ(test::cluster_data(m, g, 2)[0], 0x00);
    CHECK_EQ(test::fat_get(m, g, 3), 0u);
    CHECK(test::check_clean(fs));
    fs.umount();
    free(m);
}

// F10 / M7: the clean bit and was_dirty().
TEST(volume_clean_bit_and_was_dirty) {
    volume v(one_mib);
    REQUIRE(v.ok());
    geometry g = test::read_geometry(v.mem);
    CHECK(!v.fs.was_dirty());
    CHECK_EQ(test::fat_raw(v.mem, g, 1) & 0x08000000u, 0u);  // in use while mounted
    CHECK(test::write_text(v.fs, "/data.txt", "retained"));
    uint8_t* snapshot = static_cast<uint8_t*>(malloc(v.size));
    REQUIRE(snapshot);
    memcpy(snapshot, v.mem, v.size);  // "reset" with RAM retained
    {
        vfat32::vfs fs;
        REQUIRE_EQ(fs.mount(snapshot, v.size, vfat32::mount_mode::reuse), 0);
        CHECK(fs.was_dirty());
        CHECK(test::text_equals(fs, "/data.txt", "retained"));
        CHECK_EQ(fs.umount(), 0);
    }
    CHECK_EQ(test::fat_raw(snapshot, g, 1) & 0x08000000u, 0x08000000u);
    {
        vfat32::vfs fs;
        REQUIRE_EQ(fs.mount(snapshot, v.size, vfat32::mount_mode::reuse), 0);
        CHECK(!fs.was_dirty());
        fs.umount();
    }
    CHECK_EQ(v.fs.umount(), 0);
    CHECK_EQ(test::fat_raw(v.mem, g, 1), 0x0FFFFFFFu);
    free(snapshot);
}

#if VFAT32_USE_HEAP
TEST(volume_heap_mount_m2) {
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(64 * 1024), 0);
    CHECK(fs.mounted());
    CHECK(test::write_text(fs, "/heap.txt", "from calloc"));
    CHECK(test::text_equals(fs, "/heap.txt", "from calloc"));
    vfat32::statvfs s;
    CHECK_EQ(fs.statvfs(&s), 0);
    CHECK(s.f_blocks > 0);
    CHECK_EQ(fs.umount(), 0);
    CHECK(!fs.mounted());
    REQUIRE_EQ(fs.mount(64 * 1024), 0);  // fresh memory again
    CHECK(!test::exists(fs, "/heap.txt"));
    CHECK_EQ(fs.mount(64 * 1024), -1);
    CHECK_EQ(fs.last_error(), vfat32::error::busy);
}
#endif

TEST(volume_format_member_m3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/x.txt", "x"));
    vfat32::file* f = v.fs.fopen("/x.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.format(), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK(test::exists(v.fs, "/x.txt"));
    CHECK_EQ(v.fs.fclose(f), 0);
    vfat32::format_options o;
    o.fat_copies = 1;
    CHECK_EQ(v.fs.format(&o), 0);
    CHECK(v.fs.mounted());
    CHECK(!test::exists(v.fs, "/x.txt"));
    geometry g = test::read_geometry(v.mem);
    CHECK_EQ(g.nfats, 1u);
    CHECK(test::write_text(v.fs, "/y.txt", "y"));
    CHECK(test::check_clean(v.fs));
}

TEST(volume_statvfs_m6) {
    volume v(one_mib);
    REQUIRE(v.ok());
    geometry g = test::read_geometry(v.mem);
    vfat32::statvfs s;
    REQUIRE_EQ(v.fs.statvfs(&s), 0);
    CHECK_EQ(s.f_bsize, 512u);
    CHECK_EQ(s.f_frsize, 512u);
    CHECK_EQ(s.f_blocks, g.clusters);
    CHECK_EQ(s.f_bfree, g.clusters - 1);
    CHECK_EQ(s.f_bavail, g.clusters - 1);
    CHECK_EQ(s.f_namemax, static_cast<uint32_t>(VFAT32_NAME_MAX));
    uint8_t data[1500];
    memset(data, 'z', sizeof data);
    CHECK(test::write_file(v.fs, "/z.bin", data, sizeof data));
    CHECK_EQ(test::free_clusters(v.fs), g.clusters - 1 - 3);
    CHECK_EQ(v.fs.remove("/z.bin"), 0);
    CHECK_EQ(test::free_clusters(v.fs), g.clusters - 1);
    // FSInfo follows at once (D2)
    CHECK_EQ(le32(v.mem + g.fsinfo * g.bps + 488), g.clusters - 1);
}

// F8: mount recounts the free clusters and repairs FSInfo.
TEST(volume_mount_recounts_fsinfo_f8) {
    uint8_t* m = formatted_image(one_mib);
    REQUIRE(m);
    geometry g = test::read_geometry(m);
    test::put32(m + g.fsinfo * g.bps + 488, 7);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, one_mib, vfat32::mount_mode::reuse), 0);
    CHECK_EQ(test::free_clusters(fs), g.clusters - 1);
    CHECK_EQ(fs.umount(), 0);
    CHECK_EQ(le32(m + g.fsinfo * g.bps + 488), g.clusters - 1);
    free(m);
}

// D6: umount flushes and closes every handle; later use fails with bad_handle.
TEST(volume_umount_flushes_and_closes_d6) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/pending.txt", "w");
    REQUIRE(f);
    CHECK(v.fs.fputs("buffered", f) >= 0);
    int fd = v.fs.open("/fd.bin", vfat32::o_wronly | vfat32::o_creat);
    CHECK(fd >= 0);
    vfat32::dir* d = v.fs.opendir("/");
    CHECK(d != nullptr);
    CHECK_EQ(v.fs.umount(), 0);
    REQUIRE_EQ(v.fs.mount(v.mem, v.size, vfat32::mount_mode::reuse), 0);
    CHECK(test::text_equals(v.fs, "/pending.txt", "buffered"));
    CHECK_EQ(v.fs.fputc('x', f), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.close(fd), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK(v.fs.readdir(d) == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
}

#if VFAT32_BUFFER_SIZE > 0
// M5: sync flushes streams; the clean bit stays clear.
TEST(volume_sync_m5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    geometry g = test::read_geometry(v.mem);
    vfat32::file* f = v.fs.fopen("/s.txt", "w");
    REQUIRE(f);
    CHECK(v.fs.fputs("synced", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/s.txt"), 0);  // still in the stream buffer
    CHECK_EQ(v.fs.sync(), 0);
    CHECK_EQ(test::file_size(v.fs, "/s.txt"), 6);
    CHECK_EQ(test::fat_raw(v.mem, g, 1) & 0x08000000u, 0u);
    CHECK_EQ(v.fs.fclose(f), 0);
}
#endif

TEST(volume_strerror_texts) {
    for (int e = 0; e <= static_cast<int>(vfat32::error::corrupt); ++e) {
        const char* s = vfat32::vfs::strerror(static_cast<vfat32::error>(e));
        CHECK(s != nullptr);
        if (s) CHECK(s[0] != 0);
    }
}
