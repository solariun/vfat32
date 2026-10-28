// vfat32 tests — directories and paths (spec §6, §8): mkdir/rmdir/remove/unlink, readdir (D11, K8), S1–S4, F13, F15.

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

TEST(dir_mkdir_nested_and_errors_k1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/a"), 0);
    CHECK_EQ(v.fs.mkdir("/a/b"), 0);
    CHECK_EQ(v.fs.mkdir("/a/b/c"), 0);
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/a/b/c", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(st.st_size, 0u);
    CHECK(st.st_attr & vfat32::attr_directory);
    CHECK_EQ(v.fs.mkdir("/a/b"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    CHECK_EQ(v.fs.mkdir("/x/y"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(test::write_text(v.fs, "/f", "file"));
    CHECK_EQ(v.fs.mkdir("/f"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    CHECK_EQ(v.fs.mkdir("/f/sub"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK_EQ(v.fs.mkdir("/"), -1);
    CHECK_EQ(v.fs.mkdir("/bad:name"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK(test::check_clean(v.fs));
}

TEST(dir_rmdir_rules_k2) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/p"), 0);
    CHECK_EQ(v.fs.mkdir("/p/q"), 0);
    CHECK_EQ(v.fs.rmdir("/p"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_empty);
    CHECK_EQ(v.fs.rmdir("/"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    vfat32::dir* d = v.fs.opendir("/p/q");
    REQUIRE(d);
    CHECK_EQ(v.fs.rmdir("/p/q"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(v.fs.closedir(d), 0);
    CHECK(test::write_text(v.fs, "/p/file", "x"));
    CHECK_EQ(v.fs.rmdir("/p/file"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK_EQ(v.fs.rmdir("/p/missing"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    uint32_t before = test::free_clusters(v.fs);
    CHECK_EQ(v.fs.rmdir("/p/q"), 0);
    CHECK_EQ(test::free_clusters(v.fs), before + 1);
    CHECK(!test::exists(v.fs, "/p/q"));
    CHECK_EQ(v.fs.rmdir("/p"), -1);  // still holds a file
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_empty);
    CHECK_EQ(v.fs.unlink("/p/file"), 0);
    CHECK_EQ(v.fs.rmdir("/p"), 0);  // only deleted slots left: empty
    CHECK_EQ(test::count_entries(v.fs, "/"), 0);
    CHECK(test::check_clean(v.fs));
}

TEST(dir_remove_and_unlink_k3_k4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/d"), 0);
    CHECK(test::write_text(v.fs, "/d/f.txt", "f"));
    CHECK_EQ(v.fs.unlink("/d"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
    CHECK_EQ(v.fs.remove("/d"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_empty);
    vfat32::file* f = v.fs.fopen("/d/f.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.remove("/d/f.txt"), -1);  // D8
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(v.fs.unlink("/d/f.txt"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::busy);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(v.fs.remove("/d/f.txt"), 0);
    CHECK_EQ(v.fs.remove("/d"), 0);
    CHECK(!test::exists(v.fs, "/d"));
    CHECK_EQ(v.fs.remove("/d"), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK_EQ(v.fs.remove("/"), -1);
    CHECK(test::check_clean(v.fs));
}

// D11 / K8: every name exactly once, never "." or "..", with d_type and d_size.
TEST(dir_readdir_lists_everything_once_d11) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const char* dirs[] = {"/", "/sub"};
    CHECK_EQ(v.fs.mkdir("/sub"), 0);
    for (int di = 0; di < 2; ++di) {
        const int files = 10;
        char path[64];
        for (int i = 0; i < files; ++i) {
            snprintf(path, sizeof path, "%s/file %d.txt", di ? dirs[di] : "", i);
            char data[32];
            memset(data, 'x', sizeof data);
            CHECK(test::write_file(v.fs, path, data, static_cast<size_t>(i)));
        }
        snprintf(path, sizeof path, "%s/inner", di ? dirs[di] : "");
        CHECK_EQ(v.fs.mkdir(path), 0);
        int seen[files];
        memset(seen, 0, sizeof seen);
        int inner = 0;
        int other = 0;
        vfat32::dir* d = v.fs.opendir(dirs[di]);
        REQUIRE(d);
        for (int pass = 0; pass < 2; ++pass) {
            while (vfat32::dirent* e = v.fs.readdir(d)) {
                int n = -1;
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    ++other;
                } else if (di == 0 && strcmp(e->d_name, "sub") == 0) {
                    CHECK_EQ(e->d_type, vfat32::dt_dir);
                } else if (strcmp(e->d_name, "inner") == 0) {
                    ++inner;
                    CHECK_EQ(e->d_type, vfat32::dt_dir);
                    CHECK_EQ(e->d_size, 0u);
                } else if (sscanf(e->d_name, "file %d.txt", &n) == 1 && n >= 0 && n < files) {
                    ++seen[n];
                    CHECK_EQ(e->d_type, vfat32::dt_reg);
                    CHECK_EQ(e->d_size, static_cast<uint32_t>(n));
                } else {
                    ++other;
                    printf("    unexpected entry \"%s\"\n", e->d_name);
                }
            }
            if (pass == 0) v.fs.rewinddir(d);
        }
        CHECK_EQ(v.fs.closedir(d), 0);
        CHECK_EQ(other, 0);
        CHECK_EQ(inner, 2);
        for (int i = 0; i < files; ++i) CHECK_EQ(seen[i], 2);
    }
    CHECK_EQ(test::count_entries(v.fs, "/sub/inner"), 0);
}

TEST(dir_opendir_errors_and_limits_k8) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/file", "x"));
    CHECK(v.fs.opendir("/file") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK(v.fs.opendir("/missing") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    vfat32::dir* dirs[VFAT32_MAX_OPEN_DIRS];
    for (int i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i) {
        dirs[i] = v.fs.opendir("/");
        REQUIRE(dirs[i]);
    }
    CHECK(v.fs.opendir("/") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::too_many_open);
    for (int i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i) CHECK_EQ(v.fs.closedir(dirs[i]), 0);
    CHECK_EQ(v.fs.closedir(dirs[0]), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    vfat32::dir* d = v.fs.opendir("/");
    REQUIRE(d);
    CHECK(v.fs.readdir(d) != nullptr);
    CHECK(v.fs.readdir(d) == nullptr);  // end
    CHECK(v.fs.readdir(d) == nullptr);
    CHECK_EQ(v.fs.closedir(d), 0);
}

// S1–S4.
TEST(dir_path_rules_s1_to_s4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/a"), 0);
    CHECK_EQ(v.fs.mkdir("a/b"), 0);  // S1: leading / optional
    CHECK(test::write_text(v.fs, "//a///b//f.txt", "deep"));  // S1: repeated separators
    CHECK(test::text_equals(v.fs, "/a/b/f.txt", "deep"));
    CHECK(test::text_equals(v.fs, "a/b/f.txt", "deep"));
    CHECK(test::text_equals(v.fs, "/a/./b/./f.txt", "deep"));      // S2
    CHECK(test::text_equals(v.fs, "/a/b/../b/f.txt", "deep"));
    CHECK(test::text_equals(v.fs, "/../a/b/f.txt", "deep"));        // .. of the root is the root
    CHECK(test::text_equals(v.fs, "/a/b/../../a/b/f.txt", "deep"));
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("", &st), 0);  // S1: "" and "/" are the root
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(st.st_size, 0u);
    CHECK_EQ(v.fs.stat("/", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(v.fs.stat("/a/b/..", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(v.fs.stat("/a/b/.", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(v.fs.stat("/a/b/", &st), 0);  // S3: trailing / on a directory
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(v.fs.stat("/a/b/f.txt/", &st), -1);  // S3: trailing / on a file
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK(v.fs.fopen("/a/new.txt/", "w") == nullptr);  // S3: would create a file
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
    CHECK(!test::exists(v.fs, "/a/new.txt"));
    CHECK_EQ(v.fs.stat("/missing/x", &st), -1);  // S4
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK_EQ(v.fs.stat("/a/b/f.txt/x", &st), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_directory);
    CHECK_EQ(v.fs.stat("/a/missing", &st), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK_EQ(v.fs.mkdir("/a/c/"), 0);  // trailing / when creating a directory is fine
    CHECK(test::exists(v.fs, "/a/c"));
}

TEST(dir_stat_root_dir_file_k6) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/d"), 0);
    CHECK(test::write_text(v.fs, "/d/f", "12345"));
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(st.st_modified.year, 0);  // K6: zero stamps for the root
    CHECK_EQ(v.fs.stat("/d", &st), 0);
    CHECK(vfat32::s_isdir(st.st_mode));
    CHECK_EQ(st.st_size, 0u);
    CHECK_EQ(v.fs.stat("/d/f", &st), 0);
    CHECK(vfat32::s_isreg(st.st_mode));
    CHECK_EQ(st.st_size, 5u);
    CHECK_EQ(st.st_modified.year, 1980);  // no clock: 1980-01-01 00:00:00 (F19)
    CHECK_EQ(st.st_modified.month, 1);
    CHECK_EQ(st.st_modified.day, 1);
    CHECK((st.st_mode & 0777u) == 0666u || (st.st_mode & 0777u) == 0777u);
}

// F15: a directory grows by whole clusters; every entry stays reachable.
TEST(dir_growth_many_entries_f15) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/big"), 0);
    const int n = 200;
    char path[64];
    for (int i = 0; i < n; ++i) {
        snprintf(path, sizeof path, "/big/F%03d.TXT", i);
        char content[16];
        int len = snprintf(content, sizeof content, "#%d", i);
        REQUIRE(test::write_file(v.fs, path, content, static_cast<size_t>(len)));
    }
    for (int i = 0; i < n; i += 3) {  // long names reuse holes and span clusters
        snprintf(path, sizeof path, "/big/a much longer name number %d.data", i);
        REQUIRE(test::write_text(v.fs, path, "long"));
    }
    geometry g = test::read_geometry(v.mem);
    uint8_t* e = test::find_sfn(v.mem, g, g.root, "BIG        ");
    REQUIRE(e);
    CHECK(test::chain_length(v.mem, g, test::entry_cluster(e)) > 13u);  // 202+ slots of 16 per cluster
    CHECK_EQ(test::count_entries(v.fs, "/big"), n + (n + 2) / 3);
    for (int i = 0; i < n; ++i) {
        snprintf(path, sizeof path, "/big/f%03d.txt", i);
        char content[16];
        int len = snprintf(content, sizeof content, "#%d", i);
        CHECK(test::file_equals(v.fs, path, content, static_cast<size_t>(len)));
    }
    // delete half, the holes are reused (F15)
    for (int i = 0; i < n; i += 2) {
        snprintf(path, sizeof path, "/big/F%03d.TXT", i);
        CHECK_EQ(v.fs.remove(path), 0);
    }
    uint32_t chain = test::chain_length(v.mem, g, test::entry_cluster(e));
    for (int i = 0; i < n; i += 2) {
        snprintf(path, sizeof path, "/big/G%03d.TXT", i);
        CHECK(test::write_file(v.fs, path, "", 0));
    }
    CHECK_EQ(test::chain_length(v.mem, g, test::entry_cluster(e)), chain);  // no growth
    CHECK(test::check_clean(v.fs));
}

// F13: "." is the directory, ".." its parent, 0 when the parent is the root.
TEST(dir_dot_entries_raw_f13) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/p"), 0);
    CHECK_EQ(v.fs.mkdir("/p/q"), 0);
    geometry g = test::read_geometry(v.mem);
    uint8_t* pe = test::find_sfn(v.mem, g, g.root, "P          ");
    REQUIRE(pe);
    uint32_t p = test::entry_cluster(pe);
    CHECK(p >= 3);
    CHECK_EQ(test::le32(pe + 28), 0u);  // directory size 0
    uint8_t* dot = test::dir_slot(v.mem, g, p, 0);
    uint8_t* dotdot = test::dir_slot(v.mem, g, p, 1);
    REQUIRE(dot && dotdot);
    CHECK(memcmp(dot, ".          ", 11) == 0);
    CHECK(memcmp(dotdot, "..         ", 11) == 0);
    CHECK(dot[11] & vfat32::attr_directory);
    CHECK_EQ(test::entry_cluster(dot), p);
    CHECK_EQ(test::entry_cluster(dotdot), 0u);
    uint8_t* qe = test::find_sfn(v.mem, g, p, "Q          ");
    REQUIRE(qe);
    uint32_t q = test::entry_cluster(qe);
    CHECK_EQ(test::entry_cluster(test::dir_slot(v.mem, g, q, 0)), q);
    CHECK_EQ(test::entry_cluster(test::dir_slot(v.mem, g, q, 1)), p);
    // the rest of a new directory cluster is zero (F12)
    uint8_t* rest = test::dir_slot(v.mem, g, q, 2);
    REQUIRE(rest);
    bool zero = true;
    for (uint32_t i = 0; i < g.cluster_bytes - 2 * test::slot_bytes; ++i)
        if (rest[i]) zero = false;
    CHECK(zero);
}

TEST(dir_delete_marks_slots_e5_f15) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/Long Name To Delete.txt", "bye"));
    geometry g = test::read_geometry(v.mem);
    int used = 0;
    for (uint32_t i = 0;; ++i) {
        uint8_t* s = test::dir_slot(v.mem, g, g.root, i);
        if (!s || s[0] == 0) break;
        ++used;
    }
    CHECK(used >= 2);  // LFN slots + 8.3 slot
    CHECK_EQ(v.fs.remove("/Long Name To Delete.txt"), 0);
    for (int i = 0; i < used; ++i) CHECK_EQ(test::dir_slot(v.mem, g, g.root, static_cast<uint32_t>(i))[0], 0xE5);
    CHECK_EQ(test::count_entries(v.fs, "/"), 0);
}
