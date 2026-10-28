// vfat32 tests — a full volume (spec W1, W2, U2, B6, F6) and space coming back.

#include "test_harness.hpp"

using test::volume;

namespace {

const size_t small_volume = 64 * 1024;

}  // namespace

// U2 / W2: write returns a short count when the volume fills, then fails with no_space.
TEST(space_fill_with_descriptor_u2_w2) {
    volume v(small_volume);
    REQUIRE(v.ok());
    vfat32::statvfs s;
    REQUIRE_EQ(v.fs.statvfs(&s), 0);
    const uint32_t capacity = s.f_bfree * s.f_bsize;
    int fd = v.fs.open("/fill.bin", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(fd >= 0);
    static uint8_t chunk[1000];
    test::rng r(3);
    uint32_t total = 0;
    bool short_seen = false;
    for (int i = 0; i < 1000; ++i) {
        r.fill(chunk, sizeof chunk);
        long w = v.fs.write(fd, chunk, sizeof chunk);
        if (w < static_cast<long>(sizeof chunk)) {
            CHECK(w >= 0);
            if (w > 0) total += static_cast<uint32_t>(w);
            short_seen = true;
            break;
        }
        total += static_cast<uint32_t>(w);
    }
    CHECK(short_seen);
    CHECK_EQ(total, capacity);
    CHECK_EQ(v.fs.write(fd, chunk, 1), -1L);
    CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    CHECK_EQ(v.fs.write(fd, chunk, 0), 0L);
    CHECK_EQ(test::free_clusters(v.fs), 0u);
    vfat32::stat st;
    CHECK_EQ(v.fs.fstat(fd, &st), 0);
    CHECK_EQ(st.st_size, capacity);  // W2: what was written stays
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::check_clean(v.fs));  // W1 holds on a full volume
    // the bytes written are all there
    test::rng r2(3);
    static uint8_t expect[1000];
    int fd2 = v.fs.open("/fill.bin", vfat32::o_rdonly);
    REQUIRE(fd2 >= 0);
    uint32_t off = 0;
    bool same = true;
    while (off < capacity) {
        r2.fill(expect, sizeof expect);
        uint32_t n = capacity - off < sizeof chunk ? capacity - off : static_cast<uint32_t>(sizeof chunk);
        long got = v.fs.read(fd2, chunk, n);
        if (got != static_cast<long>(n) || memcmp(chunk, expect, n) != 0) same = false;
        off += n;
    }
    CHECK(same);
    CHECK_EQ(v.fs.close(fd2), 0);
    // everything comes back after removing it
    CHECK(v.fs.mkdir("/cannot") == -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    CHECK_EQ(v.fs.remove("/fill.bin"), 0);
    CHECK_EQ(test::free_clusters(v.fs), s.f_bfree);
    CHECK_EQ(v.fs.mkdir("/can"), 0);
    CHECK(test::check_clean(v.fs));
}

// A buffered stream reports the shortage: a short fwrite or a failed flush with the error flag (B6).
TEST(space_fill_with_stream_b6) {
    volume v(small_volume);
    REQUIRE(v.ok());
    vfat32::statvfs s;
    REQUIRE_EQ(v.fs.statvfs(&s), 0);
    const size_t capacity = static_cast<size_t>(s.f_bfree) * s.f_bsize;
    uint8_t* data = static_cast<uint8_t*>(malloc(capacity + 4096));
    REQUIRE(data);
    memset(data, 'S', capacity + 4096);
    vfat32::file* f = v.fs.fopen("/s.bin", "w");
    REQUIRE(f);
    size_t w = v.fs.fwrite(data, 1, capacity + 4096, f);
    CHECK(w < capacity + 4096);
    CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    CHECK(v.fs.ferror(f) != 0);
    v.fs.fclose(f);
    CHECK_EQ(test::file_size(v.fs, "/s.bin"), static_cast<long>(capacity));
    CHECK(test::check_clean(v.fs));
#if VFAT32_BUFFER_SIZE >= 100
    // a small buffered write that only fails at flush
    CHECK_EQ(v.fs.truncate("/s.bin", static_cast<long>(capacity - 10)), 0);
    f = v.fs.fopen("/s.bin", "a");
    REQUIRE(f);
    CHECK_EQ(v.fs.fwrite(data, 1, 100, f), 100u);  // may sit in the buffer
    int rc = v.fs.fflush(f);
    if (rc == 0) {
        printf("    fflush succeeded on a full volume\n");
        CHECK(rc != 0);
    } else {
        CHECK_EQ(rc, vfat32::eof);
        CHECK(v.fs.ferror(f) != 0);
        CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    }
    v.fs.clearerr(f);
    CHECK_EQ(v.fs.ferror(f), 0);
    CHECK_EQ(v.fs.fclose(f), 0);  // B6: the bytes that did not fit were dropped
    CHECK_EQ(test::file_size(v.fs, "/s.bin"), static_cast<long>(capacity));
    CHECK(test::check_clean(v.fs));
#endif
    free(data);
}

// Many small files until no cluster is left: creation fails cleanly, the volume stays consistent.
TEST(space_many_files_until_full) {
    volume v(small_volume);
    REQUIRE(v.ok());
    char path[32];
    int created = 0;
    for (int i = 0; i < 10000; ++i) {
        snprintf(path, sizeof path, "/f%d.bin", i);
        vfat32::file* f = v.fs.fopen(path, "w");
        if (!f) {
            CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
            break;
        }
        size_t w = v.fs.fwrite("0123456789", 1, 10, f);
        int rc = v.fs.fclose(f);
        if (w != 10 || rc != 0) {
            CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
            break;
        }
        ++created;
    }
    CHECK(created > 10);
    CHECK(test::check_clean(v.fs));
    for (int i = 0; i < created; ++i) {
        snprintf(path, sizeof path, "/f%d.bin", i);
        CHECK(test::text_equals(v.fs, path, "0123456789"));
    }
    for (int i = 0; i < created; ++i) {
        snprintf(path, sizeof path, "/f%d.bin", i);
        CHECK_EQ(v.fs.remove(path), 0);
    }
    snprintf(path, sizeof path, "/f%d.bin", created);  // the one whose write ran out of space, if it was created
    if (test::exists(v.fs, path)) CHECK_EQ(v.fs.remove(path), 0);
    CHECK_EQ(test::count_entries(v.fs, "/"), 0);
    CHECK(test::check_clean(v.fs));
}

// A gap that cannot be zero-filled for lack of space does not break W1.
TEST(space_gap_fill_fails_cleanly_w3) {
    volume v(small_volume);
    REQUIRE(v.ok());
    vfat32::statvfs s;
    REQUIRE_EQ(v.fs.statvfs(&s), 0);
    int fd = v.fs.open("/gap.bin", vfat32::o_wronly | vfat32::o_creat);
    REQUIRE(fd >= 0);
    long far = static_cast<long>(s.f_bfree) * static_cast<long>(s.f_bsize) * 2;
    CHECK_EQ(v.fs.lseek(fd, far, vfat32::seek_set), far);
    long w = v.fs.write(fd, "x", 1);
    CHECK(w <= 0);
    CHECK_EQ(v.fs.last_error(), vfat32::error::no_space);
    CHECK_EQ(v.fs.close(fd), 0);
    CHECK(test::check_clean(v.fs));
    CHECK(test::file_size(v.fs, "/gap.bin") < far);
}
