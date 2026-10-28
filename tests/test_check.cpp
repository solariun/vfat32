// vfat32 tests — integrity check and repair (spec §11): damage injected in the raw image, found, repaired (C1–C4).

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

namespace {

uint8_t* root_entry(volume& v, const geometry& g, const char* sfn11) {
    return test::find_sfn(v.mem, g, g.root, sfn11);
}

// A free cluster from the end of the volume.
uint32_t last_free_cluster(const uint8_t* m, const geometry& g) {
    for (uint32_t c = g.clusters + 1; c >= 2; --c)
        if (test::fat_get(m, g, c) == 0) return c;
    return 0;
}

struct fixture {
    volume v;
    geometry g;
    uint8_t a[1500];  // 3 clusters
    uint8_t b[1000];  // 2 clusters
    fixture() : v(one_mib) {
        test::rng r(42);
        r.fill(a, sizeof a);
        r.fill(b, sizeof b);
        test::write_file(v.fs, "/A.BIN", a, sizeof a);
        test::write_file(v.fs, "/B.BIN", b, sizeof b);
        v.fs.mkdir("/D1");
        v.fs.mkdir("/D1/D2");
        test::write_text(v.fs, "/D1/D2/C.TXT", "nested");
        g = test::read_geometry(v.mem);
    }
};

// Runs check without repair, expects failure; then repair; then a clean check. Returns the first report.
vfat32::check_report damage_cycle(volume& v, bool& repaired_clean) {
    vfat32::check_report r;
    test::clear_report(r);
    int rc = v.fs.check(&r, false);
    CHECK_EQ(rc, -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::corrupt);
    CHECK(r.errors() > 0);
    vfat32::check_report fix;
    test::clear_report(fix);
    int rc2 = v.fs.check(&fix, true);
    CHECK_EQ(rc2, 0);  // C3
    CHECK(fix.repaired > 0);
    repaired_clean = rc2 == 0 && test::check_clean(v.fs);
    CHECK(repaired_clean);
    return r;
}

}  // namespace

TEST(check_clean_volume_counts) {
    fixture f;
    REQUIRE(f.v.ok());
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(f.v.fs.check(&r), 0);
    CHECK_EQ(r.errors(), 0u);
    CHECK_EQ(r.files, 3u);
    CHECK_EQ(r.directories, 2u);  // subdirectories only
    vfat32::statvfs s;
    CHECK_EQ(f.v.fs.statvfs(&s), 0);
    CHECK_EQ(r.free_clusters, s.f_bfree);
    CHECK_EQ(r.used_clusters + r.free_clusters + r.bad_clusters, s.f_blocks);
    CHECK_EQ(r.used_clusters, 1u + 3 + 2 + 1 + 1 + 1);  // root, A, B, D1, D2, C
    CHECK_EQ(r.bad_clusters, 0u);
    CHECK_EQ(r.free_extents, 1u);
    CHECK_EQ(r.fragmented, 0u);
    CHECK_EQ(r.repaired, 0u);
    CHECK(test::no_marks(f.v.mem, f.g));  // C1
}

TEST(check_report_errors_sums_error_counters) {
    vfat32::check_report r;
    test::clear_report(r);
    r.files = 5;
    r.used_clusters = 7;
    r.repaired = 3;
    CHECK_EQ(r.errors(), 0u);
    r.lost_clusters = 1;
    r.cross_links = 2;
    r.fsinfo_errors = 1;
    CHECK_EQ(r.errors(), 4u);
}

TEST(check_lost_cluster) {
    fixture f;
    REQUIRE(f.v.ok());
    uint32_t c = last_free_cluster(f.v.mem, f.g);
    REQUIRE(c);
    test::fat_put(f.v.mem, f.g, c, test::fat_eoc);  // allocated, owned by nobody
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK_EQ(r.lost_clusters, 1u);
    CHECK_EQ(test::fat_get(f.v.mem, f.g, c), 0u);
    CHECK(test::no_marks(f.v.mem, f.g));
    CHECK(test::file_equals(f.v.fs, "/A.BIN", f.a, sizeof f.a));
}

TEST(check_lost_chain_freed) {
    fixture f;
    REQUIRE(f.v.ok());
    uint32_t c = last_free_cluster(f.v.mem, f.g);
    REQUIRE(c > 10);
    test::fat_put(f.v.mem, f.g, c - 2, c - 1);
    test::fat_put(f.v.mem, f.g, c - 1, c);
    test::fat_put(f.v.mem, f.g, c, test::fat_eoc);
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK_EQ(r.lost_clusters, 3u);
    uint32_t free_before = test::free_clusters(f.v.fs);
    CHECK_EQ(test::fat_get(f.v.mem, f.g, c - 2), 0u);
    CHECK(free_before > 0);
}

TEST(check_cross_link) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
    uint8_t* eb = root_entry(f.v, f.g, "B       BIN");
    REQUIRE(ea && eb);
    uint32_t a1 = test::entry_cluster(ea);
    uint32_t a2 = test::fat_get(f.v.mem, f.g, a1);
    uint32_t b1 = test::entry_cluster(eb);
    test::fat_put(f.v.mem, f.g, b1, a2);  // B now runs into A's chain
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.cross_links >= 1u);
    CHECK(test::file_equals(f.v.fs, "/A.BIN", f.a, sizeof f.a));  // the first chain is kept whole
    CHECK(test::exists(f.v.fs, "/B.BIN"));
    CHECK(test::no_marks(f.v.mem, f.g));
}

TEST(check_chain_loop) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
    REQUIRE(ea);
    uint32_t a1 = test::entry_cluster(ea);
    uint32_t a2 = test::fat_get(f.v.mem, f.g, a1);
    uint32_t a3 = test::fat_get(f.v.mem, f.g, a2);
    test::fat_put(f.v.mem, f.g, a3, a1);  // a loop: must terminate and be found
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.cross_links >= 1u);
    CHECK_EQ(test::file_size(f.v.fs, "/A.BIN"), 1500);
    CHECK(test::file_equals(f.v.fs, "/A.BIN", f.a, sizeof f.a));
}

TEST(check_size_too_large) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
    REQUIRE(ea);
    test::put32(ea + 28, 1500 + 2 * f.g.cluster_bytes);
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK_EQ(r.size_errors, 1u);
    long size = test::file_size(f.v.fs, "/A.BIN");
    CHECK(size >= 1500 && size <= 3 * 512);  // set from the chain
}

TEST(check_size_too_small_frees_extra_clusters) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
    REQUIRE(ea);
    uint32_t free_before = test::free_clusters(f.v.fs);
    test::put32(ea + 28, 1);  // 1 byte with 3 clusters
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK_EQ(r.size_errors, 1u);
    CHECK_EQ(test::file_size(f.v.fs, "/A.BIN"), 1);
    CHECK_EQ(test::free_clusters(f.v.fs), free_before + 2);
    uint8_t one;
    CHECK_EQ(test::read_file(f.v.fs, "/A.BIN", &one, 1), 1L);
    CHECK_EQ(one, f.a[0]);
}

TEST(check_zero_size_with_cluster_and_size_without) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* eb = root_entry(f.v, f.g, "B       BIN");
    REQUIRE(eb);
    test::put32(eb + 28, 0);  // size 0 but a chain (W1)
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.size_errors >= 1u);
    CHECK(test::write_text(f.v.fs, "/E.BIN", ""));
    uint8_t* ee = root_entry(f.v, f.g, "E       BIN");
    REQUIRE(ee);
    test::put32(ee + 28, 700);  // size but no cluster
    r = damage_cycle(f.v, ok);
    CHECK(r.size_errors >= 1u);
    CHECK_EQ(test::file_size(f.v.fs, "/E.BIN"), 0);
}

TEST(check_bad_chain_into_free_and_out_of_range) {
    for (int variant = 0; variant < 3; ++variant) {
        fixture f;
        REQUIRE(f.v.ok());
        uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
        REQUIRE(ea);
        uint32_t a1 = test::entry_cluster(ea);
        uint32_t target = variant == 0 ? last_free_cluster(f.v.mem, f.g)
                        : variant == 1 ? f.g.clusters + 10
                                       : test::fat_bad;
        if (variant == 2) {
            uint32_t a2 = test::fat_get(f.v.mem, f.g, a1);
            test::fat_put(f.v.mem, f.g, a2, test::fat_bad);  // a bad cluster inside the chain
        } else {
            test::fat_put(f.v.mem, f.g, a1, target);
        }
        bool ok = false;
        vfat32::check_report r = damage_cycle(f.v, ok);
        CHECK(r.bad_chains >= 1u);
        long size = test::file_size(f.v.fs, "/A.BIN");
        CHECK(size >= 0 && size <= 1500);
        uint8_t first[512];
        if (size >= 512) {
            CHECK_EQ(test::read_file(f.v.fs, "/A.BIN", first, sizeof first), 512L);
            CHECK(memcmp(first, f.a, 512) == 0);  // the part before the cut is kept
        }
    }
}

TEST(check_fat_copy_mismatch) {
    fixture f;
    REQUIRE(f.v.ok());
    REQUIRE_EQ(f.g.nfats, 2u);
    uint32_t c = last_free_cluster(f.v.mem, f.g);
    test::fat_put(f.v.mem, f.g, c, test::fat_eoc, 1);  // only the second copy
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.fat_mismatches >= 1u);
    CHECK_EQ(r.lost_clusters, 0u);  // the first copy decides
    const uint8_t* fat1 = f.v.mem + f.g.fat_offset;
    const uint8_t* fat2 = fat1 + static_cast<size_t>(f.g.fat_sectors) * f.g.bps;
    CHECK(memcmp(fat1, fat2, (f.g.clusters + 2) * 4) == 0);
}

TEST(check_fsinfo_free_count) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* fsi = f.v.mem + f.g.fsinfo * f.g.bps;
    uint32_t right = test::le32(fsi + 488);
    test::put32(fsi + 488, 12345);
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK_EQ(r.fsinfo_errors, 1u);
    CHECK_EQ(test::le32(fsi + 488), right);
}

TEST(check_bad_dotdot_and_dot) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* d1e = root_entry(f.v, f.g, "D1         ");
    REQUIRE(d1e);
    uint32_t d1 = test::entry_cluster(d1e);
    uint8_t* d2e = test::find_sfn(f.v.mem, f.g, d1, "D2         ");
    REQUIRE(d2e);
    uint32_t d2 = test::entry_cluster(d2e);
    uint8_t* dotdot = test::dir_slot(f.v.mem, f.g, d2, 1);
    REQUIRE(dotdot);
    test::set_entry_cluster(dotdot, 0);  // should be d1
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.bad_entries >= 1u);
    CHECK_EQ(test::entry_cluster(dotdot), d1);
    CHECK(test::text_equals(f.v.fs, "/D1/D2/C.TXT", "nested"));
    uint8_t* dot = test::dir_slot(f.v.mem, f.g, d1, 0);
    test::set_entry_cluster(dot, d2);  // should be d1
    r = damage_cycle(f.v, ok);
    CHECK(r.bad_entries >= 1u);
    CHECK_EQ(test::entry_cluster(dot), d1);
    CHECK(test::text_equals(f.v.fs, "/D1/D2/C.TXT", "nested"));
}

TEST(check_directory_with_size) {
    fixture f;
    REQUIRE(f.v.ok());
    uint8_t* d1e = root_entry(f.v, f.g, "D1         ");
    REQUIRE(d1e);
    test::put32(d1e + 28, 4096);
    bool ok = false;
    vfat32::check_report r = damage_cycle(f.v, ok);
    CHECK(r.size_errors >= 1u);
    CHECK_EQ(test::le32(d1e + 28), 0u);
}

// C1: upper bits already set: without repair only reserved_bits is reported; repair clears them.
TEST(check_reserved_bits_c1) {
    const uint32_t bits[] = {0x10000000u, 0x80000000u, 0xF0000000u};
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; ++i) {
        fixture f;
        REQUIRE(f.v.ok());
        uint8_t* ea = root_entry(f.v, f.g, "A       BIN");
        REQUIRE(ea);
        uint32_t a1 = test::entry_cluster(ea);
        test::fat_put(f.v.mem, f.g, a1, test::fat_raw(f.v.mem, f.g, a1) | bits[i]);
        vfat32::check_report r;
        test::clear_report(r);
        CHECK_EQ(f.v.fs.check(&r, false), -1);
        CHECK(r.reserved_bits >= 1u);
        if (!CHECK_EQ(r.errors(), r.reserved_bits)) test::print_report(r);
        CHECK_EQ(test::fat_raw(f.v.mem, f.g, a1) & 0xF0000000u, bits[i]);  // left as found
        vfat32::check_report fix;
        test::clear_report(fix);
        CHECK_EQ(f.v.fs.check(&fix, true), 0);
        CHECK_EQ(test::fat_raw(f.v.mem, f.g, a1) & 0xF0000000u, 0u);
        CHECK(test::check_clean(f.v.fs));
        CHECK(test::file_equals(f.v.fs, "/A.BIN", f.a, sizeof f.a));
    }
}

// C4: repair needs every handle closed; a plain check runs with handles open.
TEST(check_handles_open_c4) {
    fixture f;
    REQUIRE(f.v.ok());
    vfat32::file* h = f.v.fs.fopen("/A.BIN", "r+");
    REQUIRE(h);
    CHECK(f.v.fs.fputs("pending", h) >= 0);
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(f.v.fs.check(&r, false), 0);
    test::clear_report(r);
    CHECK_EQ(f.v.fs.check(&r, true), -1);
    CHECK_EQ(f.v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(f.v.fs.fclose(h), 0);
    vfat32::dir* d = f.v.fs.opendir("/D1");
    REQUIRE(d);
    CHECK_EQ(f.v.fs.check(&r, true), -1);
    CHECK_EQ(f.v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(f.v.fs.closedir(d), 0);
    CHECK_EQ(f.v.fs.check(&r, true), 0);
}

TEST(check_fragmented_and_extents_info) {
    volume v(one_mib);
    REQUIRE(v.ok());
    int a = v.fs.open("/X.BIN", vfat32::o_wronly | vfat32::o_creat);
    int b = v.fs.open("/Y.BIN", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(a >= 0 && b >= 0);
    uint8_t block[512];
    memset(block, 1, sizeof block);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(v.fs.write(a, block, sizeof block), 512L);
        CHECK_EQ(v.fs.write(b, block, sizeof block), 512L);
    }
    CHECK_EQ(v.fs.close(a), 0);
    CHECK_EQ(v.fs.close(b), 0);
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(v.fs.check(&r), 0);
    CHECK_EQ(r.fragmented, 2u);
    CHECK_EQ(r.free_extents, 1u);
    CHECK_EQ(v.fs.remove("/X.BIN"), 0);  // leaves holes between Y's clusters
    test::clear_report(r);
    CHECK_EQ(v.fs.check(&r), 0);
    CHECK_EQ(r.fragmented, 1u);
    CHECK_EQ(r.free_extents, 5u);  // 4 holes + the tail
}

// M7: an image whose previous session was interrupted after allocating and before linking.
TEST(check_after_dirty_reset_m7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/keep.txt", "kept"));
    geometry g = test::read_geometry(v.mem);
    uint8_t* snap = static_cast<uint8_t*>(malloc(v.size));
    REQUIRE(snap);
    memcpy(snap, v.mem, v.size);
    uint32_t c = last_free_cluster(snap, g);
    test::fat_put(snap, g, c, test::fat_eoc);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(snap, v.size, vfat32::mount_mode::reuse), 0);
    CHECK(fs.was_dirty());
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(fs.check(&r, true), 0);
    CHECK_EQ(r.lost_clusters, 1u);
    CHECK(test::text_equals(fs, "/keep.txt", "kept"));
    fs.umount();
    free(snap);
}

// C2: a subdirectory that cannot be entered (wrong "..", no repair) does not stop the walk: the entries after it
// are still verified, and nothing is called lost except what really is.
TEST(check_walk_continues_after_bad_subdir_c2) {
    test::volume v(256 * 1024);
    REQUIRE_EQ(v.fs.mkdir("/a"), 0);
    REQUIRE_EQ(v.fs.mkdir("/a/inner"), 0);
    REQUIRE(test::write_text(v.fs, "/a/inner/x.txt", "inside a broken subtree\n"));
    REQUIRE(test::write_text(v.fs, "/z.txt", "after the bad directory\n"));
    // Corrupt /a's ".." (slot 1 of its first cluster): it must name the root (0).
    test::geometry g = test::read_geometry(v.mem);
    const uint8_t* a_entry = test::find_sfn(v.mem, g, g.root, "A          ");
    REQUIRE(a_entry);
    uint32_t a_cluster = test::entry_cluster(a_entry);
    REQUIRE(a_cluster >= 2);
    uint8_t* dotdot = test::dir_slot(v.mem, g, a_cluster, 1);
    REQUIRE(dotdot);
    test::set_entry_cluster(dotdot, 0x77);  // points elsewhere
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(v.fs.check(&r), -1);
    CHECK_EQ(r.bad_entries, 1u);
    CHECK_EQ(r.files, 1u);  // /z.txt still verified after the bad directory
    CHECK_EQ(r.size_errors, 0u);
    CHECK_EQ(r.cross_links, 0u);
    CHECK_EQ(r.bad_chains, 0u);
    // repair fixes ".." and finds everything in place
    REQUIRE_EQ(v.fs.check(&r, true), 0);
    CHECK(test::text_equals(v.fs, "/a/inner/x.txt", "inside a broken subtree\n"));
}
