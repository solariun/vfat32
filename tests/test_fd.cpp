// vfat32 tests — descriptors, unbuffered (spec §7.4), truncate (W3, K7), time stamps on fsync/fclose (D9, F20).

#include "test_harness.hpp"

using test::one_mib;
using test::volume;

namespace {

vfat32::timestamp clock_now;
vfat32::timestamp test_clock() { return clock_now; }
void set_time(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    clock_now.year = year;
    clock_now.month = month;
    clock_now.day = day;
    clock_now.hour = hour;
    clock_now.minute = minute;
    clock_now.second = second;
}
bool same_time(const vfat32::timestamp& a, const vfat32::timestamp& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute &&
           a.second == b.second;
}

}  // namespace

TEST(fd_open_flags_u1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.open("/x.bin", vfat32::o_rdonly), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    int fd = v.fs.open("/x.bin", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, "hello", 5), 5L);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK_EQ(v.fs.open("/x.bin", vfat32::o_wronly | vfat32::o_creat | vfat32::o_excl), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    fd = v.fs.open("/x.bin", vfat32::o_rdonly);
    REQUIRE(fd >= 0);
    char buf[8] = {0};
    CHECK_EQ(v.fs.read(fd, buf, sizeof buf), 5L);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK_EQ(v.fs.read(fd, buf, sizeof buf), 0L);  // end of file
    CHECK_EQ(v.fs.write(fd, "x", 1), -1L);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.close(fd), 0);
    fd = v.fs.open("/x.bin", vfat32::o_wronly);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.read(fd, buf, 1), -1L);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.write(fd, "J", 1), 1L);  // no truncation without o_trunc
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::text_equals(v.fs, "/x.bin", "Jello"));
    fd = v.fs.open("/x.bin", vfat32::o_rdwr | vfat32::o_trunc);
    REQUIRE(fd >= 0);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK_EQ(st.st_size, 0u);
    CHECK(vfat32::s_isreg(st.st_mode));
    CHECK_EQ(v.fs.close(fd), 0);
    fd = v.fs.open("/excl.bin", vfat32::o_rdwr | vfat32::o_creat | vfat32::o_excl);
    CHECK(fd >= 0);
    CHECK_EQ(v.fs.close(fd), 0);
}

TEST(fd_append_u1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    int fd = v.fs.open("/ap.bin", vfat32::o_wronly | vfat32::o_creat | vfat32::o_append);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, "abc", 3), 3L);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_set), 0L);
    CHECK_EQ(v.fs.write(fd, "def", 3), 3L);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_cur), 6L);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::text_equals(v.fs, "/ap.bin", "abcdef"));
}

TEST(fd_lseek_u2) {
    volume v(one_mib);
    REQUIRE(v.ok());
    int fd = v.fs.open("/s.bin", vfat32::o_rdwr | vfat32::o_creat);
    REQUIRE(fd >= 0);
    uint8_t data[2000];
    test::rng r(5);
    r.fill(data, sizeof data);
    CHECK_EQ(v.fs.write(fd, data, sizeof data), 2000L);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_cur), 2000L);
    CHECK_EQ(v.fs.lseek(fd, 700, vfat32::seek_set), 700L);
    uint8_t b[10];
    CHECK_EQ(v.fs.read(fd, b, 10), 10L);
    CHECK(memcmp(b, data + 700, 10) == 0);
    CHECK_EQ(v.fs.lseek(fd, -20, vfat32::seek_cur), 690L);
    CHECK_EQ(v.fs.lseek(fd, -1, vfat32::seek_end), 1999L);
    CHECK_EQ(v.fs.read(fd, b, 10), 1L);
    CHECK_EQ(b[0], data[1999]);
    CHECK_EQ(v.fs.lseek(fd, -1, vfat32::seek_set), -1L);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.lseek(fd, 0, 9), -1L);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.lseek(fd, 5000, vfat32::seek_set), 5000L);
    CHECK_EQ(v.fs.write(fd, "Z", 1), 1L);  // W3 gap
    CHECK_EQ(v.fs.lseek(fd, 1990, vfat32::seek_set), 1990L);
    static uint8_t rest[4000];
    CHECK_EQ(v.fs.read(fd, rest, sizeof rest), 3011L);
    CHECK(memcmp(rest, data + 1990, 10) == 0);
    bool zero = true;
    for (int i = 10; i < 3010; ++i)
        if (rest[i]) zero = false;
    CHECK(zero);
    CHECK_EQ(rest[3010], 'Z');
    CHECK_EQ(v.fs.write(fd, data, 0), 0L);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::check_clean(v.fs));
}

TEST(fd_ftruncate_grow_zeros_and_shrink_w3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    uint8_t junk[4096];
    memset(junk, 0xEE, sizeof junk);
    CHECK(test::write_file(v.fs, "/junk", junk, sizeof junk));
    CHECK_EQ(v.fs.unlink("/junk"), 0);
    int fd = v.fs.open("/t.bin", vfat32::o_rdwr | vfat32::o_creat);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, "abc", 3), 3L);
    CHECK_EQ(v.fs.ftruncate(fd, 2000), 0);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK_EQ(st.st_size, 2000u);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_set), 0L);
    static uint8_t back[2100];
    CHECK_EQ(v.fs.read(fd, back, sizeof back), 2000L);
    CHECK(memcmp(back, "abc", 3) == 0);
    bool zero = true;
    for (int i = 3; i < 2000; ++i)
        if (back[i]) zero = false;
    CHECK(zero);
    CHECK(test::check_clean(v.fs));
    uint32_t before = test::free_clusters(v.fs);
    CHECK_EQ(v.fs.ftruncate(fd, 1), 0);
    CHECK_EQ(test::free_clusters(v.fs), before + 3);  // 4 clusters -> 1
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK_EQ(st.st_size, 1u);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_set), 0L);
    CHECK_EQ(v.fs.read(fd, back, sizeof back), 1L);
    CHECK_EQ(back[0], 'a');
    CHECK_EQ(v.fs.ftruncate(fd, 0), 0);
    CHECK_EQ(test::free_clusters(v.fs), before + 4);  // W1: size 0 owns no cluster
    CHECK_EQ(v.fs.ftruncate(fd, -1), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.close(fd), 0);
    fd = v.fs.open("/t.bin", vfat32::o_rdonly);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.ftruncate(fd, 10), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::check_clean(v.fs));
}

TEST(fd_truncate_path_k7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/p.txt", "0123456789"));
    CHECK_EQ(v.fs.truncate("/p.txt", 4), 0);
    CHECK(test::text_equals(v.fs, "/p.txt", "0123"));
    CHECK_EQ(v.fs.truncate("/p.txt", 600), 0);
    CHECK_EQ(test::file_size(v.fs, "/p.txt"), 600);
    CHECK_EQ(v.fs.truncate("/missing", 4), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(test::check_clean(v.fs));
}

TEST(fd_close_and_bad_descriptors) {
    volume v(one_mib);
    REQUIRE(v.ok());
    int fd = v.fs.open("/c.bin", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK_EQ(v.fs.close(fd), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    const int bad[] = {-1, VFAT32_MAX_OPEN_FILES, 1000};
    char b;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK_EQ(v.fs.close(bad[i]), -1);
        CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
        CHECK_EQ(v.fs.read(bad[i], &b, 1), -1L);
        CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
        CHECK_EQ(v.fs.write(bad[i], &b, 1), -1L);
        CHECK_EQ(v.fs.lseek(bad[i], 0, vfat32::seek_set), -1L);
        CHECK_EQ(v.fs.fsync(bad[i]), -1);
        vfat32::stat st;
        CHECK_EQ(v.fs.fstat(bad[i], &st), -1);
        CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
        CHECK_EQ(v.fs.ftruncate(bad[i], 0), -1);
    }
}

TEST(fd_open_directory_is_directory_w6) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/d"), 0);
    CHECK_EQ(v.fs.open("/d", vfat32::o_rdonly), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
    CHECK_EQ(v.fs.open("/", vfat32::o_rdonly), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
    CHECK(v.fs.fopen("/d", "r") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::is_directory);
}

// fileno names the same row: a descriptor call on a stream's row, and close() frees it.
TEST(fd_fileno_and_close_interplay) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/fn.bin", "w+");
    REQUIRE(f);
    int fd = v.fs.fileno(f);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, "raw", 3), 3L);
    CHECK_EQ(test::file_size(v.fs, "/fn.bin"), 3);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK_EQ(v.fs.fclose(f), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK(test::text_equals(v.fs, "/fn.bin", "raw"));
}

#if VFAT32_BUFFER_SIZE > 0
// U4: a descriptor call flushes the row's stream write buffer and drops its read buffer and ungetc.
TEST(fd_mixing_with_stream_u4) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/mix.txt", "w+");
    REQUIRE(f);
    int fd = v.fs.fileno(f);
    REQUIRE(fd >= 0);
    CHECK(v.fs.fputs("abc", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/mix.txt"), 0);  // still buffered
    CHECK_EQ(v.fs.write(fd, "XYZ", 3), 3L);           // flushes "abc" first, then writes at 3
    CHECK_EQ(test::file_size(v.fs, "/mix.txt"), 6);
    CHECK_EQ(v.fs.lseek(fd, 0, vfat32::seek_set), 0L);
    char buf[8] = {0};
    CHECK_EQ(v.fs.read(fd, buf, sizeof buf), 6L);
    CHECK(memcmp(buf, "abcXYZ", 6) == 0);
    // read buffer and pushed-back character dropped
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_set), 0);
    CHECK_EQ(v.fs.fgetc(f), 'a');
    CHECK_EQ(v.fs.ungetc('#', f), '#');
    CHECK_EQ(v.fs.lseek(fd, 1, vfat32::seek_set), 1L);
    CHECK_EQ(v.fs.read(fd, buf, 2), 2L);
    CHECK(memcmp(buf, "bc", 2) == 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/mix.txt", "abcXYZ"));
}
#endif

TEST(fd_large_unbuffered_io) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const size_t n = 100 * 1024;
    uint8_t* data = static_cast<uint8_t*>(malloc(n));
    uint8_t* back = static_cast<uint8_t*>(malloc(n));
    REQUIRE(data && back);
    test::rng r(77);
    r.fill(data, n);
    int fd = v.fs.open("/big.bin", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(fd >= 0);
    CHECK_EQ(v.fs.write(fd, data, n), static_cast<long>(n));
    CHECK_EQ(v.fs.close(fd), 0);
    fd = v.fs.open("/big.bin", vfat32::o_rdonly);
    REQUIRE(fd >= 0);
    size_t off = 0;
    size_t step = 1;
    while (off < n) {
        long got = v.fs.read(fd, back + off, step);
        REQUIRE(got > 0);
        off += static_cast<size_t>(got);
        step = step * 3 + 1;
        if (step > 7000) step = 13;
    }
    CHECK(memcmp(data, back, n) == 0);
    CHECK_EQ(v.fs.close(fd), 0);
    free(data);
    free(back);
}

// D9 / F20: creation stamps at create; modification stamps on fsync / fclose of a modified handle.
TEST(fd_time_stamps_d9_f20) {
    volume v(one_mib);
    REQUIRE(v.ok());
    v.fs.set_clock(test_clock);
    set_time(2024, 3, 5, 10, 20, 30);
    vfat32::timestamp created = clock_now;
    int fd = v.fs.open("/ts.bin", vfat32::o_rdwr | vfat32::o_creat);
    REQUIRE(fd >= 0);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK(same_time(st.st_created, created));
    CHECK(same_time(st.st_modified, created));
    CHECK_EQ(st.st_accessed.year, 2024);
    CHECK_EQ(st.st_accessed.month, 3);
    CHECK_EQ(st.st_accessed.day, 5);
    CHECK_EQ(st.st_accessed.hour, 0);  // date only
    set_time(2025, 6, 7, 11, 22, 44);
    vfat32::timestamp modified = clock_now;
    CHECK_EQ(v.fs.write(fd, "data", 4), 4L);
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK(same_time(st.st_modified, created));  // not yet: D9
    CHECK_EQ(v.fs.fsync(fd), 0);
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK(same_time(st.st_modified, modified));
    CHECK(same_time(st.st_created, created));
    CHECK(st.st_attr & vfat32::attr_archive);
    CHECK_EQ(v.fs.close(fd), 0);
    // a handle that only reads changes no stamp
    set_time(2026, 1, 1, 1, 1, 2);
    fd = v.fs.open("/ts.bin", vfat32::o_rdonly);
    REQUIRE(fd >= 0);
    char b[4];
    CHECK_EQ(v.fs.read(fd, b, 4), 4L);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK_EQ(v.fs.stat("/ts.bin", &st), 0);
    CHECK(same_time(st.st_modified, modified));
    // fclose of a modified stream sets it
    vfat32::file* f = v.fs.fopen("/ts.bin", "a");
    REQUIRE(f);
    CHECK(v.fs.fputs("more", f) >= 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(v.fs.stat("/ts.bin", &st), 0);
    CHECK(same_time(st.st_modified, clock_now));
    // F19: an odd second survives in the creation stamp (CrtTimeTenth); the modification stamp has 2 s resolution
    set_time(2024, 3, 5, 10, 20, 31);
    CHECK(test::write_text(v.fs, "/odd.txt", "o"));
    CHECK_EQ(v.fs.stat("/odd.txt", &st), 0);
    CHECK_EQ(st.st_created.second, 31);
    CHECK_EQ(st.st_modified.second, 30);
    // years outside 1980..2107 are clamped (F19)
    set_time(1970, 1, 1, 0, 0, 0);
    CHECK(test::write_text(v.fs, "/old.txt", "o"));
    CHECK_EQ(v.fs.stat("/old.txt", &st), 0);
    CHECK_EQ(st.st_created.year, 1980);
}

TEST(fd_fstat_fields_k6) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/k6.txt", "12345"));
    int fd = v.fs.open("/k6.txt", vfat32::o_rdonly);
    REQUIRE(fd >= 0);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK(vfat32::s_isreg(st.st_mode));
    CHECK(!vfat32::s_isdir(st.st_mode));
    CHECK_EQ(st.st_size, 5u);
    CHECK_EQ(st.st_attr & vfat32::attr_directory, 0);
    CHECK_EQ(v.fs.fsync(fd), 0);
    CHECK_EQ(v.fs.close(fd), 0);
}
