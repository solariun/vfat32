// vfat32 tests — the handle table (spec §7.1): many handles on one file, H2–H5, read-only attribute (D10, W6).

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

TEST(handles_many_on_one_file_share_size_d1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/shared.bin", ""));
    vfat32::file* w = v.fs.fopen("/shared.bin", "r+");
    REQUIRE(w);
    CHECK_EQ(v.fs.setvbuf(w, nullptr, vfat32::io_nbf, 0), 0);
    vfat32::file* readers[VFAT32_MAX_OPEN_FILES];
    int n = 0;
    for (; n < VFAT32_MAX_OPEN_FILES - 1; ++n) {
        readers[n] = v.fs.fopen("/shared.bin", "r");
        REQUIRE(readers[n]);
    }
    uint8_t data[3000];
    test::rng r(9);
    r.fill(data, sizeof data);
    CHECK_EQ(v.fs.fwrite(data, 1, sizeof data, w), sizeof data);
    for (int i = 0; i < n; ++i) {
        vfat32::stat st;
        CHECK_EQ(v.fs.fstat(v.fs.fileno(readers[i]), &st), 0);
        CHECK_EQ(st.st_size, 3000u);
        uint8_t back[3000];
        CHECK_EQ(v.fs.fread(back, 1, sizeof back, readers[i]), sizeof back);
        CHECK(memcmp(back, data, sizeof back) == 0);
    }
    // a write lands for everybody, including an empty file that just got its first cluster
    for (int i = 0; i < n; ++i) CHECK_EQ(v.fs.fclose(readers[i]), 0);
    CHECK_EQ(v.fs.fclose(w), 0);
    CHECK(test::check_clean(v.fs));
}

TEST(handles_first_cluster_seen_by_all) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* a = v.fs.fopen("/empty.bin", "w");
    vfat32::file* b = v.fs.fopen("/empty.bin", "r");
    REQUIRE(a && b);
    CHECK_EQ(v.fs.fgetc(b), vfat32::eof);  // empty
    v.fs.clearerr(b);
    CHECK_EQ(v.fs.setvbuf(a, nullptr, vfat32::io_nbf, 0), 0);
    CHECK(v.fs.fputs("first", a) >= 0);  // allocates the first cluster through a
    CHECK_EQ(v.fs.fgetc(b), 'f');
    CHECK_EQ(v.fs.fclose(a), 0);
    CHECK_EQ(v.fs.fclose(b), 0);
}

// H4: shrinking through one handle resets the caches of the others — no stale cluster is written later.
TEST(handles_truncate_invalidates_others_h4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    uint8_t data[4000];
    test::rng r(21);
    r.fill(data, sizeof data);
    CHECK(test::write_file(v.fs, "/t.bin", data, sizeof data));
    int a = v.fs.open("/t.bin", vfat32::o_rdwr);
    int b = v.fs.open("/t.bin", vfat32::o_rdwr);
    REQUIRE(a >= 0 && b >= 0);
    uint8_t buf[100];
    CHECK_EQ(v.fs.lseek(b, 3500, vfat32::seek_set), 3500L);
    CHECK_EQ(v.fs.read(b, buf, 100), 100L);  // b caches a cluster near the end
    CHECK_EQ(v.fs.ftruncate(a, 0), 0);        // every cluster freed
    CHECK(test::check_clean(v.fs));
    // somebody else takes the freed clusters
    uint8_t other[4000];
    memset(other, 0x5A, sizeof other);
    CHECK(test::write_file(v.fs, "/other.bin", other, sizeof other));
    CHECK_EQ(v.fs.read(b, buf, 100), 0L);  // b is past the end now
    CHECK_EQ(v.fs.lseek(b, 0, vfat32::seek_set), 0L);
    CHECK_EQ(v.fs.write(a, "new", 3), 3L);
    CHECK_EQ(v.fs.read(b, buf, 100), 3L);
    CHECK(memcmp(buf, "new", 3) == 0);
    CHECK_EQ(v.fs.lseek(b, 2000, vfat32::seek_set), 2000L);
    CHECK_EQ(v.fs.write(b, "tail", 4), 4L);  // zero-fills 3..2000, must not touch /other.bin
    CHECK(test::file_equals(v.fs, "/other.bin", other, sizeof other));
    CHECK_EQ(test::file_size(v.fs, "/t.bin"), 2004);
    CHECK_EQ(v.fs.close(a), 0);
    CHECK_EQ(v.fs.close(b), 0);
    CHECK(test::check_clean(v.fs));
}

#if VFAT32_BUFFER_SIZE > 0
// H4 / D5: a stream that had buffered the old content does not return bytes past the new end.
TEST(handles_truncate_drops_read_buffers_h4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/rb.txt", "abcdefghij"));
    vfat32::file* f = v.fs.fopen("/rb.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.fgetc(f), 'a');
    CHECK_EQ(v.fs.truncate("/rb.txt", 2), 0);
    CHECK_EQ(v.fs.fgetc(f), 'b');
    CHECK_EQ(v.fs.fgetc(f), vfat32::eof);
    CHECK_EQ(v.fs.fclose(f), 0);
}
#endif

// H3: when the table is full, opening fails before any file is created.
TEST(handles_too_many_open_h3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* files[VFAT32_MAX_OPEN_FILES];
    int fds[VFAT32_MAX_OPEN_FILES];
    int nf = 0;
    int nd = 0;
    for (int i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) {
        char path[32];
        snprintf(path, sizeof path, "/h%d.txt", i);
        if (i % 2) {
            files[nf] = v.fs.fopen(path, "w");
            REQUIRE(files[nf]);
            ++nf;
        } else {
            fds[nd] = v.fs.open(path, vfat32::o_wronly | vfat32::o_creat);
            REQUIRE(fds[nd] >= 0);
            ++nd;
        }
    }
    CHECK(v.fs.fopen("/new_one.txt", "w") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::too_many_open);
    CHECK_EQ(v.fs.open("/new_two.txt", vfat32::o_wronly | vfat32::o_creat), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::too_many_open);
    CHECK(!test::exists(v.fs, "/new_one.txt"));
    CHECK(!test::exists(v.fs, "/new_two.txt"));
    // fds are distinct row numbers
    for (int i = 0; i < nd; ++i)
        for (int j = i + 1; j < nd; ++j) CHECK(fds[i] != fds[j]);
    for (int i = 0; i < nf; ++i) CHECK_EQ(v.fs.fclose(files[i]), 0);
    for (int i = 0; i < nd; ++i) CHECK_EQ(v.fs.close(fds[i]), 0);
    vfat32::file* f = v.fs.fopen("/new_one.txt", "w");
    CHECK(f != nullptr);
    if (f) v.fs.fclose(f);
}

// H2: every call validates its handle.
TEST(handles_invalid_pointers_h2) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file fake;
    char b[4];
    CHECK_EQ(v.fs.fclose(&fake), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fread(b, 1, 1, &fake), 0u);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fwrite(b, 1, 1, nullptr), 0u);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fgetc(&fake), vfat32::eof);
    CHECK_EQ(v.fs.fputc('x', &fake), vfat32::eof);
    CHECK_EQ(v.fs.fseek(&fake, 0, vfat32::seek_set), -1);
    CHECK_EQ(v.fs.ftell(&fake), -1L);
    CHECK_EQ(v.fs.fflush(&fake), vfat32::eof);
    CHECK(v.fs.fgets(b, sizeof b, &fake) == nullptr);
    CHECK(v.fs.fputs("x", &fake) < 0);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    vfat32::dir fake_dir;
    CHECK(v.fs.readdir(&fake_dir) == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.closedir(&fake_dir), -1);
    CHECK_EQ(v.fs.closedir(nullptr), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    // a closed row is not valid any more
    vfat32::file* f = v.fs.fopen("/c.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(v.fs.fputc('x', f), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    // handles of another volume are not valid here
    volume other(one_mib);
    REQUIRE(other.ok());
    vfat32::file* g = other.fs.fopen("/o.txt", "w");
    REQUIRE(g);
    CHECK_EQ(v.fs.fputc('x', g), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(other.fs.fclose(g), 0);
}

// D10 / W6: the read-only attribute blocks write access; remove and rename stay allowed.
TEST(handles_read_only_attribute_d10) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/ro.txt", "locked"));
    geometry g = test::read_geometry(v.mem);
    uint8_t* e = test::find_sfn(v.mem, g, g.root, "RO      TXT");
    REQUIRE(e);
    e[11] |= vfat32::attr_read_only;
    const char* write_modes[] = {"w", "r+", "a", "w+", "a+"};
    for (size_t i = 0; i < sizeof write_modes / sizeof write_modes[0]; ++i) {
        CHECK(v.fs.fopen("/ro.txt", write_modes[i]) == nullptr);
        CHECK_EQ(v.fs.last_error(), vfat32::error::access);
    }
    CHECK_EQ(v.fs.open("/ro.txt", vfat32::o_wronly), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::access);
    CHECK_EQ(v.fs.open("/ro.txt", vfat32::o_rdwr), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::access);
    CHECK_EQ(v.fs.truncate("/ro.txt", 0), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::access);
    CHECK(test::text_equals(v.fs, "/ro.txt", "locked"));  // "r" works
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/ro.txt", &st), 0);
    CHECK(st.st_attr & vfat32::attr_read_only);
    CHECK_EQ(st.st_mode & 0222u, 0u);  // K6: no write permission bits
    CHECK_EQ(v.fs.rename("/ro.txt", "/ro2.txt"), 0);
    CHECK_EQ(v.fs.stat("/ro2.txt", &st), 0);
    CHECK(st.st_attr & vfat32::attr_read_only);  // K5: attributes kept
    CHECK_EQ(v.fs.remove("/ro2.txt"), 0);
    CHECK(!test::exists(v.fs, "/ro2.txt"));
    CHECK(test::check_clean(v.fs));
}

// H5: a renamed open file keeps working through its handle (see also test_rename).
TEST(handles_follow_rename_h5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    int fd = v.fs.open("/before.txt", vfat32::o_rdwr | vfat32::o_creat);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, "one", 3), 3L);
    CHECK_EQ(v.fs.rename("/before.txt", "/after.txt"), 0);
    CHECK_EQ(v.fs.write(fd, "two", 3), 3L);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK_EQ(st.st_size, 6u);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::text_equals(v.fs, "/after.txt", "onetwo"));
    CHECK(!test::exists(v.fs, "/before.txt"));
}
