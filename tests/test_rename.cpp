// vfat32 tests — rename (spec D7, K5, H5).

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

namespace {

vfat32::timestamp clock_now;
vfat32::timestamp test_clock() { return clock_now; }

}  // namespace

TEST(rename_same_dir_and_across_dirs_k5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/a.txt", "payload"));
    CHECK_EQ(v.fs.rename("/a.txt", "/b.txt"), 0);
    CHECK(!test::exists(v.fs, "/a.txt"));
    CHECK(test::text_equals(v.fs, "/b.txt", "payload"));
    CHECK_EQ(v.fs.mkdir("/d1"), 0);
    CHECK_EQ(v.fs.mkdir("/d2"), 0);
    CHECK_EQ(v.fs.rename("/b.txt", "/d1/A much longer name.txt"), 0);
    CHECK(test::text_equals(v.fs, "/d1/A much longer name.txt", "payload"));
    CHECK_EQ(v.fs.rename("/d1/A much longer name.txt", "/d2/y.txt"), 0);
    CHECK(test::text_equals(v.fs, "/d2/y.txt", "payload"));
    CHECK_EQ(test::count_entries(v.fs, "/d1"), 0);
    CHECK_EQ(test::count_entries(v.fs, "/"), 2);
    CHECK(test::check_clean(v.fs));
}

TEST(rename_replaces_existing_file_d7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/src.txt", "new content"));
    char old[2000];
    memset(old, 'o', sizeof old);
    CHECK(test::write_file(v.fs, "/dst.txt", old, sizeof old));
    uint32_t before = test::free_clusters(v.fs);
    CHECK_EQ(v.fs.rename("/src.txt", "/dst.txt"), 0);
    CHECK(!test::exists(v.fs, "/src.txt"));
    CHECK(test::text_equals(v.fs, "/dst.txt", "new content"));
    CHECK_EQ(test::free_clusters(v.fs), before + 4);  // the old target's clusters are freed
    CHECK_EQ(test::count_entries(v.fs, "/"), 1);
    CHECK(test::check_clean(v.fs));
}

TEST(rename_open_target_busy_d7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/src.txt", "s"));
    CHECK(test::write_text(v.fs, "/dst.txt", "d"));
    vfat32::file* f = v.fs.fopen("/dst.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.rename("/src.txt", "/dst.txt"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK(test::text_equals(v.fs, "/src.txt", "s"));
    CHECK(test::text_equals(v.fs, "/dst.txt", "d"));
    CHECK_EQ(v.fs.fclose(f), 0);
}

TEST(rename_directory_rules_d7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/a"), 0);
    CHECK_EQ(v.fs.mkdir("/b"), 0);
    CHECK(test::write_text(v.fs, "/b/inside", "i"));
    CHECK(test::write_text(v.fs, "/file", "f"));
    CHECK_EQ(v.fs.rename("/a", "/b"), -1);  // onto a non-empty directory
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_empty);
    CHECK_EQ(v.fs.rename("/file", "/a"), -1);  // file onto directory
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
    CHECK_EQ(v.fs.rename("/a", "/file"), -1);  // directory onto file
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK_EQ(v.fs.mkdir("/a/q"), 0);
    CHECK_EQ(v.fs.rename("/a", "/a/q/a2"), -1);  // into its own subtree
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.rename("/a", "/a/x"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.mkdir("/empty"), 0);
    CHECK_EQ(v.fs.rename("/a", "/empty"), 0);  // onto an empty directory: replaced
    CHECK(!test::exists(v.fs, "/a"));
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/empty/q", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK(test::check_clean(v.fs));
}

TEST(rename_missing_and_invalid) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.rename("/missing", "/x"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(test::write_text(v.fs, "/f", "f"));
    CHECK_EQ(v.fs.rename("/f", "/nodir/f"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK_EQ(v.fs.rename("/f", "/bad|name"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.rename("/", "/root2"), -1);
    CHECK(test::text_equals(v.fs, "/f", "f"));
    CHECK_EQ(v.fs.rename("/f", "/f"), 0);  // POSIX: same name, nothing happens
    CHECK(test::text_equals(v.fs, "/f", "f"));
    CHECK_EQ(test::count_entries(v.fs, "/"), 1);
}

TEST(rename_case_only_k5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/case.txt", "c"));
    CHECK_EQ(v.fs.rename("/case.txt", "/CASE.TXT"), 0);
    CHECK(test::dir_contains(v.fs, "/", "CASE.TXT"));
    CHECK(!test::dir_contains(v.fs, "/", "case.txt"));
    CHECK_EQ(test::count_entries(v.fs, "/"), 1);
    CHECK(test::text_equals(v.fs, "/case.txt", "c"));
    CHECK_EQ(v.fs.rename("/CASE.TXT", "/Case.Txt"), 0);  // now needs an LFN
    CHECK(test::dir_contains(v.fs, "/", "Case.Txt"));
    CHECK_EQ(test::count_entries(v.fs, "/"), 1);
    CHECK(test::check_clean(v.fs));
}

// H5 / D7: an open file may be renamed; its handle writes into the renamed file.
TEST(rename_open_file_handle_follows_h5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/other"), 0);
    vfat32::file* f = v.fs.fopen("/o.txt", "w");
    REQUIRE(f);
    CHECK(v.fs.fputs("one", f) >= 0);
    CHECK_EQ(v.fs.rename("/o.txt", "/other/A renamed long name.txt"), 0);
    CHECK(v.fs.fputs("two", f) >= 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/other/A renamed long name.txt", "onetwo"));
    CHECK(!test::exists(v.fs, "/o.txt"));
    CHECK(test::check_clean(v.fs));
}

// K5: moving a directory rewrites its ".."; attributes, size, chain and creation time are kept.
TEST(rename_moves_dir_dotdot_and_keeps_metadata_k5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    v.fs.set_clock(test_clock);
    clock_now.year = 2020;
    clock_now.month = 2;
    clock_now.day = 3;
    clock_now.hour = 4;
    clock_now.minute = 5;
    clock_now.second = 6;
    CHECK_EQ(v.fs.mkdir("/m1"), 0);
    CHECK_EQ(v.fs.mkdir("/m2"), 0);
    CHECK_EQ(v.fs.mkdir("/m1/sub"), 0);
    CHECK(test::write_text(v.fs, "/m1/sub/file.txt", "deep"));
    vfat32::stat before;
    CHECK_EQ(v.fs.stat("/m1/sub/file.txt", &before), 0);
    clock_now.year = 2022;
    CHECK_EQ(v.fs.rename("/m1/sub", "/m2/moved"), 0);
    geometry g = test::read_geometry(v.mem);
    uint8_t* m2 = test::find_sfn(v.mem, g, g.root, "M2         ");
    REQUIRE(m2);
    uint32_t m2c = test::entry_cluster(m2);
    uint8_t* moved = test::find_sfn(v.mem, g, m2c, "MOVED      ");
    REQUIRE(moved);
    uint32_t mc = test::entry_cluster(moved);
    CHECK_EQ(test::entry_cluster(test::dir_slot(v.mem, g, mc, 1)), m2c);  // ".." rewritten
    CHECK_EQ(test::entry_cluster(test::dir_slot(v.mem, g, mc, 0)), mc);
    CHECK(test::write_text(v.fs, "/m2/moved/../marker", "m"));  // ".." resolves to the new parent
    CHECK(test::exists(v.fs, "/m2/marker"));
    CHECK(test::text_equals(v.fs, "/m2/moved/file.txt", "deep"));
    CHECK_EQ(test::count_entries(v.fs, "/m1"), 0);
    // moving to the root writes 0 into ".." (F13)
    CHECK_EQ(v.fs.rename("/m2/moved", "/top"), 0);
    uint8_t* top = test::find_sfn(v.mem, g, g.root, "TOP        ");
    REQUIRE(top);
    CHECK_EQ(test::entry_cluster(test::dir_slot(v.mem, g, test::entry_cluster(top), 1)), 0u);
    // a renamed file keeps its creation time
    CHECK_EQ(v.fs.rename("/top/file.txt", "/top/renamed.txt"), 0);
    vfat32::stat after;
    CHECK_EQ(v.fs.stat("/top/renamed.txt", &after), 0);
    CHECK_EQ(after.st_created.year, before.st_created.year);
    CHECK_EQ(after.st_created.second, before.st_created.second);
    CHECK_EQ(after.st_size, before.st_size);
    CHECK(test::check_clean(v.fs));
}

TEST(rename_with_open_dir_handle_listing_continues) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/d"), 0);
    CHECK(test::write_text(v.fs, "/d/x", "x"));
    vfat32::dir* d = v.fs.opendir("/d");
    REQUIRE(d);
    CHECK_EQ(v.fs.rename("/d", "/e"), 0);
    int n = 0;
    while (vfat32::dirent* e = v.fs.readdir(d)) {
        CHECK_STR(e->d_name, "x");
        ++n;
    }
    CHECK_EQ(n, 1);
    CHECK_EQ(v.fs.closedir(d), 0);
    CHECK(test::text_equals(v.fs, "/e/x", "x"));
}
