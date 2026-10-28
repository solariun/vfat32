// vfat32 tests — defragmentation (spec §12): contiguous chains in tree order, free space at the end, in place,
// incremental (D4), preconditions (D-1), contents and structure intact.

#include <time.h>

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

namespace {

const int max_tracked = 64;

struct tracked {
    char path[96];
    uint8_t* data;
    size_t size;
    bool alive;
};

struct workload {
    tracked files[max_tracked];
    int count;
    workload() : count(0) { memset(files, 0, sizeof files); }
    ~workload() {
        for (int i = 0; i < count; ++i) free(files[i].data);
    }
    int add(const char* path) {
        tracked& t = files[count];
        snprintf(t.path, sizeof t.path, "%s", path);
        t.data = nullptr;
        t.size = 0;
        t.alive = true;
        return count++;
    }
};

// Appends n random bytes through an unbuffered descriptor, so allocations of different files interleave.
bool append(vfat32::vfs& fs, tracked& t, size_t n, test::rng& r) {
    uint8_t* grown = static_cast<uint8_t*>(realloc(t.data, t.size + n));
    if (!grown) return false;
    t.data = grown;
    r.fill(t.data + t.size, n);
    int fd = fs.open(t.path, vfat32::o_wronly | vfat32::o_creat | vfat32::o_append);
    if (fd < 0) return false;
    long w = fs.write(fd, t.data + t.size, n);
    fs.close(fd);
    if (w != static_cast<long>(n)) return false;
    t.size += n;
    return true;
}

bool verify(vfat32::vfs& fs, workload& w) {
    bool ok = true;
    for (int i = 0; i < w.count; ++i) {
        tracked& t = w.files[i];
        if (t.alive) {
            if (!test::file_equals(fs, t.path, t.data, t.size)) {
                printf("    content differs: %s\n", t.path);
                ok = false;
            }
        } else if (test::exists(fs, t.path)) {
            printf("    removed file is back: %s\n", t.path);
            ok = false;
        }
    }
    return ok;
}

// Builds a fragmented tree: interleaved appends, every other file removed, directories growing in between.
bool build_fragmented(vfat32::vfs& fs, workload& w, uint32_t seed) {
    test::rng r(seed);
    if (fs.mkdir("/sub") || fs.mkdir("/sub/deep") || fs.mkdir("/other")) return false;
    char path[96];
    for (int i = 0; i < 10; ++i) {
        snprintf(path, sizeof path, "/f%02d.bin", i);
        w.add(path);
    }
    for (int i = 0; i < 5; ++i) {
        snprintf(path, sizeof path, "/sub/s%02d.bin", i);
        w.add(path);
    }
    for (int i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "/sub/deep/Deep File %d.dat", i);
        w.add(path);
    }
    for (int i = 0; i < 2; ++i) {
        snprintf(path, sizeof path, "/other/o%d.txt", i);
        w.add(path);
    }
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < w.count; ++i)
            if (!append(fs, w.files[i], r.below(900) + 1, r)) return false;
        if (round == 3) {  // grow /sub's directory chain between data clusters
            for (int k = 0; k < 40; ++k) {
                snprintf(path, sizeof path, "/sub/padding entry with a long name %d", k);
                if (!test::write_text(fs, path, "")) return false;
            }
            if (fs.mkdir("/sub/late") != 0) return false;
        }
    }
    for (int i = 0; i < w.count; i += 2) {
        if (fs.remove(w.files[i].path) != 0) return false;
        w.files[i].alive = false;
    }
    if (w.count + 2 > max_tracked) return false;
    w.add("/sub/late/after.bin");
    w.add("/other/Another long file name.bin");
    for (int round = 0; round < 4; ++round)
        for (int i = 0; i < w.count; ++i)
            if (w.files[i].alive && !append(fs, w.files[i], r.below(1200) + 1, r)) return false;
    return true;
}

// Every used cluster comes before every free cluster (spec §12: free space is one run at the end).
bool packed(const uint8_t* m, const geometry& g) {
    bool free_seen = false;
    for (uint32_t c = 2; c <= g.clusters + 1; ++c) {
        uint32_t v = test::fat_get(m, g, c);
        if (v == 0)
            free_seen = true;
        else if (free_seen)
            return false;
    }
    return true;
}

}  // namespace

TEST(defrag_full_pass_d2) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 1234));
    REQUIRE(verify(v.fs, w));
    vfat32::check_report before;
    test::clear_report(before);
    REQUIRE_EQ(v.fs.check(&before), 0);
    CHECK(before.fragmented >= 5u);  // the workload really is fragmented
    CHECK(before.free_extents >= 2u);
    int sub_entries = test::count_entries(v.fs, "/sub");
    clock_t t0 = clock();
    CHECK_EQ(v.fs.defrag(), 0);
    double ms = 1000.0 * static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;
    printf("    defrag: %u used clusters, %u fragmented chains, %.1f ms\n", static_cast<unsigned>(before.used_clusters), static_cast<unsigned>(before.fragmented),
           ms);
    vfat32::check_report after;
    test::clear_report(after);
    CHECK_EQ(v.fs.check(&after), 0);
    CHECK_EQ(after.errors(), 0u);
    CHECK_EQ(after.fragmented, 0u);
    CHECK(after.free_extents <= 1u);
    CHECK_EQ(after.used_clusters, static_cast<unsigned>(before.used_clusters));
    CHECK_EQ(after.files, static_cast<unsigned>(before.files));
    CHECK_EQ(after.directories, static_cast<unsigned>(before.directories));
    geometry g = test::read_geometry(v.mem);
    CHECK(packed(v.mem, g));
    CHECK_EQ(g.root, 2u);  // D-2: the root chain first
    CHECK(verify(v.fs, w));
    CHECK_EQ(test::count_entries(v.fs, "/sub"), sub_entries);
    CHECK(test::write_text(v.fs, "/sub/late/../deep/../new.txt", "after"));  // ".." still right
    CHECK(test::text_equals(v.fs, "/sub/new.txt", "after"));
    CHECK(test::no_marks(v.mem, g));
}

TEST(defrag_second_pass_changes_nothing) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 99));
    REQUIRE_EQ(v.fs.defrag(), 0);
    geometry g = test::read_geometry(v.mem);
    size_t from = g.fat_offset;
    size_t len = v.size - from;
    uint8_t* snap = static_cast<uint8_t*>(malloc(len));
    REQUIRE(snap);
    memcpy(snap, v.mem + from, len);
    CHECK_EQ(v.fs.defrag(), 0);
    CHECK(memcmp(snap, v.mem + from, len) == 0);
    free(snap);
}

// D4: one move per call, consistent between calls, other calls (even writes) in between.
TEST(defrag_incremental_d4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 777));
    test::rng r(5);
    int steps = 0;
    int rc = 1;
    bool clean_all_along = true;
    while (rc == 1 && steps < 50000) {
        rc = v.fs.defrag(1);
        ++steps;
        if (rc < 0) break;
        if (steps % 7 == 0 && !test::check_clean(v.fs)) {
            clean_all_along = false;
            break;
        }
        if (steps < 300 && steps % 25 == 0) {  // writes between steps
            int i = static_cast<int>(r.below(static_cast<uint32_t>(w.count)));
            if (w.files[i].alive) CHECK(append(v.fs, w.files[i], r.below(700) + 1, r));
        }
    }
    printf("    incremental defrag: %d calls\n", steps);
    CHECK_EQ(rc, 0);
    CHECK(clean_all_along);
    CHECK(steps > 10);
    vfat32::check_report after;
    test::clear_report(after);
    CHECK_EQ(v.fs.check(&after), 0);
    CHECK_EQ(after.fragmented, 0u);
    CHECK(after.free_extents <= 1u);
    CHECK(verify(v.fs, w));
}

TEST(defrag_bounded_steps_return_one) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 31337));
    CHECK_EQ(v.fs.defrag(10), 1);  // more work remains
    CHECK(test::check_clean(v.fs));
    CHECK(verify(v.fs, w));
    int rc = 1;
    int calls = 0;
    while (rc == 1 && calls < 10000) {
        rc = v.fs.defrag(50);
        ++calls;
    }
    CHECK_EQ(rc, 0);
    CHECK(verify(v.fs, w));
}

// D-1: no handle may be open; the volume must check clean.
TEST(defrag_preconditions_d1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 4));
    vfat32::file* f = v.fs.fopen(w.files[1].path, "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.defrag(), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(v.fs.fclose(f), 0);
    vfat32::dir* d = v.fs.opendir("/sub");
    REQUIRE(d);
    CHECK_EQ(v.fs.defrag(), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(v.fs.closedir(d), 0);
    geometry g = test::read_geometry(v.mem);
    uint32_t c = 0;
    for (uint32_t k = g.clusters + 1; k >= 2; --k)
        if (test::fat_get(v.mem, g, k) == 0) {
            c = k;
            break;
        }
    REQUIRE(c);
    uint8_t* snap = static_cast<uint8_t*>(malloc(v.size));
    REQUIRE(snap);
    test::fat_put(v.mem, g, c, test::fat_eoc);  // a lost cluster
    memcpy(snap, v.mem, v.size);
    CHECK_EQ(v.fs.defrag(), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::corrupt);
    CHECK(memcmp(snap + g.fat_offset, v.mem + g.fat_offset, v.size - g.fat_offset) == 0);  // nothing moved
    free(snap);
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(v.fs.check(&r, true), 0);
    CHECK_EQ(v.fs.defrag(), 0);
    CHECK(verify(v.fs, w));
}

TEST(defrag_then_remount_reuse) {
    volume v(one_mib);
    REQUIRE(v.ok());
    workload w;
    REQUIRE(build_fragmented(v.fs, w, 2024));
    REQUIRE_EQ(v.fs.defrag(), 0);
    REQUIRE_EQ(v.fs.umount(), 0);
    CHECK(!vfat32::vfs::needs_format(v.mem, v.size));
    REQUIRE_EQ(v.fs.mount(v.mem, v.size, vfat32::mount_mode::reuse), 0);
    CHECK(!v.fs.was_dirty());
    CHECK(test::check_clean(v.fs));
    CHECK(verify(v.fs, w));
}

TEST(defrag_empty_volume) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.defrag(), 0);
    CHECK_EQ(v.fs.defrag(1), 0);
    CHECK(test::check_clean(v.fs));
}

// D4: in place by swapping — works with no free cluster at all.
TEST(defrag_full_volume_no_free_space) {
    volume v(64 * 1024);
    REQUIRE(v.ok());
    int a = v.fs.open("/A.BIN", vfat32::o_wronly | vfat32::o_creat);
    int b = v.fs.open("/B.BIN", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(a >= 0 && b >= 0);
    static uint8_t da[64 * 1024];
    static uint8_t db[64 * 1024];
    test::rng r(8);
    r.fill(da, sizeof da);
    r.fill(db, sizeof db);
    size_t na = 0;
    size_t nb = 0;
    for (;;) {
        long wa = v.fs.write(a, da + na, 512);
        if (wa > 0) na += static_cast<size_t>(wa);
        long wb = v.fs.write(b, db + nb, 512);
        if (wb > 0) nb += static_cast<size_t>(wb);
        if (wa < 512 || wb < 512) break;
    }
    CHECK_EQ(v.fs.close(a), 0);
    CHECK_EQ(v.fs.close(b), 0);
    CHECK_EQ(test::free_clusters(v.fs), 0u);
    vfat32::check_report r1;
    test::clear_report(r1);
    REQUIRE_EQ(v.fs.check(&r1), 0);
    CHECK_EQ(r1.fragmented, 2u);
    CHECK_EQ(v.fs.defrag(), 0);
    vfat32::check_report r2;
    test::clear_report(r2);
    CHECK_EQ(v.fs.check(&r2), 0);
    CHECK_EQ(r2.fragmented, 0u);
    CHECK_EQ(r2.free_extents, 0u);
    CHECK(test::file_equals(v.fs, "/A.BIN", da, na));
    CHECK(test::file_equals(v.fs, "/B.BIN", db, nb));
}

// A bigger volume, to see the cost (D-7) and the result at scale.
TEST(defrag_four_mib_random_tree) {
    volume v(4 * one_mib);
    REQUIRE(v.ok());
    workload w;
    test::rng r(55);
    v.fs.mkdir("/a");
    v.fs.mkdir("/a/b");
    v.fs.mkdir("/c");
    const char* dirs[] = {"", "/a", "/a/b", "/c"};
    char path[96];
    for (int i = 0; i < 48; ++i) {
        snprintf(path, sizeof path, "%s/file number %d.bin", dirs[i % 4], i);
        w.add(path);
    }
    for (int round = 0; round < 12; ++round)
        for (int i = 0; i < w.count; ++i)
            if (w.files[i].alive) REQUIRE(append(v.fs, w.files[i], r.below(4000) + 1, r));
    for (int i = 0; i < w.count; i += 3) {
        REQUIRE_EQ(v.fs.remove(w.files[i].path), 0);
        w.files[i].alive = false;
    }
    vfat32::check_report before;
    test::clear_report(before);
    REQUIRE_EQ(v.fs.check(&before), 0);
    clock_t t0 = clock();
    CHECK_EQ(v.fs.defrag(), 0);
    double ms = 1000.0 * static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;
    printf("    4 MiB defrag: %u used of %u clusters, %u fragmented chains, %.1f ms\n", static_cast<unsigned>(before.used_clusters),
           static_cast<unsigned>(before.used_clusters + before.free_clusters), static_cast<unsigned>(before.fragmented), ms);
    vfat32::check_report after;
    test::clear_report(after);
    CHECK_EQ(v.fs.check(&after), 0);
    CHECK_EQ(after.fragmented, 0u);
    CHECK(after.free_extents <= 1u);
    geometry g = test::read_geometry(v.mem);
    CHECK(packed(v.mem, g));
    CHECK(verify(v.fs, w));
}
