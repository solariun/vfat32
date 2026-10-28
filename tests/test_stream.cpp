// vfat32 tests — streams (spec §7.3): modes, positioning, character I/O, buffering and its visibility rules (D5).

#include "test_harness.hpp"

using test::one_mib;
using test::volume;

namespace {

void pattern(uint8_t* p, size_t n, uint32_t seed) {
    test::rng r(seed);
    r.fill(p, n);
}

}  // namespace

TEST(stream_mode_r_missing_not_found) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(v.fs.fopen("/nope.txt", "r") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(v.fs.fopen("/nope.txt", "r+") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(!test::exists(v.fs, "/nope.txt"));
}

TEST(stream_mode_w_creates_and_truncates) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/w.txt", "hello world"));
    CHECK_EQ(test::file_size(v.fs, "/w.txt"), 11);
    vfat32::file* f = v.fs.fopen("/w.txt", "w");
    REQUIRE(f);
    CHECK_EQ(test::file_size(v.fs, "/w.txt"), 0);
    char c;
    CHECK_EQ(v.fs.fread(&c, 1, 1, f), 0u);  // "w" cannot read
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::check_clean(v.fs));
}

TEST(stream_mode_r_cannot_write) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/r.txt", "abc"));
    vfat32::file* f = v.fs.fopen("/r.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.fwrite("x", 1, 1, f), 0u);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fputc('x', f), vfat32::eof);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/r.txt", "abc"));
}

TEST(stream_mode_a_always_appends) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/a.txt", "a");
    REQUIRE(f);
    CHECK(v.fs.fputs("abc", f) >= 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    f = v.fs.fopen("/a.txt", "a");
    REQUIRE(f);
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_set), 0);
    CHECK(v.fs.fputs("def", f) >= 0);  // still at the end
    CHECK_EQ(v.fs.fseek(f, 1, vfat32::seek_set), 0);
    CHECK(v.fs.fputs("ghi", f) >= 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/a.txt", "abcdefghi"));
}

TEST(stream_mode_r_plus_reads_and_writes_in_place) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/rp.txt", "abcdef"));
    vfat32::file* f = v.fs.fopen("/rp.txt", "r+");
    REQUIRE(f);
    char buf[8] = {0};
    CHECK_EQ(v.fs.fread(buf, 1, 2, f), 2u);
    CHECK(memcmp(buf, "ab", 2) == 0);
    CHECK_EQ(v.fs.fwrite("XY", 1, 2, f), 2u);  // B4: no fseek needed between read and write
    CHECK_EQ(v.fs.fread(buf, 1, 2, f), 2u);     // and back to reading
    CHECK(memcmp(buf, "ef", 2) == 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/rp.txt", "abXYef"));
}

TEST(stream_mode_w_plus_truncates_and_reads_back) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/wp.txt", "old contents"));
    vfat32::file* f = v.fs.fopen("/wp.txt", "w+");
    REQUIRE(f);
    CHECK(v.fs.fputs("hello", f) >= 0);
    char buf[16] = {0};
    CHECK_EQ(v.fs.fread(buf, 1, sizeof buf, f), 0u);  // at the end
    CHECK(v.fs.feof(f) != 0);
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_set), 0);
    CHECK(v.fs.feof(f) == 0);
    CHECK_EQ(v.fs.fread(buf, 1, sizeof buf, f), 5u);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK_EQ(v.fs.fclose(f), 0);
}

TEST(stream_mode_a_plus_reads_from_start_writes_at_end) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/ap.txt", "12345"));
    vfat32::file* f = v.fs.fopen("/ap.txt", "a+");
    REQUIRE(f);
    char buf[16] = {0};
    CHECK_EQ(v.fs.fread(buf, 1, 3, f), 3u);
    CHECK(memcmp(buf, "123", 3) == 0);
    CHECK(v.fs.fputs("67", f) >= 0);
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_set), 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ(v.fs.fread(buf, 1, sizeof buf, f), 7u);
    CHECK(memcmp(buf, "1234567", 7) == 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    // a+ creates
    f = v.fs.fopen("/ap_new.txt", "a+");
    CHECK(f != nullptr);
    if (f) CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::exists(v.fs, "/ap_new.txt"));
}

TEST(stream_mode_x_exclusive) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/x.txt", "wx");
    REQUIRE(f);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(v.fs.fopen("/x.txt", "wx") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    CHECK(v.fs.fopen("/x.txt", "w+x") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    f = v.fs.fopen("/x2.txt", "wbx");
    CHECK(f != nullptr);
    if (f) v.fs.fclose(f);
    f = v.fs.fopen("/x3.txt", "w+bx");
    CHECK(f != nullptr);
    if (f) v.fs.fclose(f);
}

TEST(stream_mode_strings_b1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/m.txt", "m"));
    const char* bad[] = {"", "z", "rw", "r+q", "+r", "rx", "ax", "a+x", "wq", "R"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        vfat32::file* f = v.fs.fopen("/m.txt", bad[i]);
        if (f) {
            printf("    mode \"%s\" accepted\n", bad[i]);
            v.fs.fclose(f);
        }
        CHECK(f == nullptr);
        CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    }
    const char* good[] = {"r", "rb", "rt", "r+", "r+b", "rb+", "w", "wb", "w+", "wb+", "a", "ab", "a+", "a+b", "wt"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; ++i) {
        vfat32::file* f = v.fs.fopen("/m.txt", good[i]);
        if (!f) printf("    mode \"%s\" rejected\n", good[i]);
        CHECK(f != nullptr);
        if (f) CHECK_EQ(v.fs.fclose(f), 0);
    }
    CHECK(v.fs.fopen("/m.txt", nullptr) == nullptr);
    CHECK(v.fs.fopen(nullptr, "r") == nullptr);
}

// Data crossing 512-byte clusters with many chunk sizes, written and read back with other chunk sizes.
TEST(stream_read_write_across_clusters) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const size_t n = 9000;
    uint8_t* data = static_cast<uint8_t*>(malloc(n));
    uint8_t* back = static_cast<uint8_t*>(malloc(n));
    REQUIRE(data && back);
    pattern(data, n, 7);
    const size_t chunks[] = {1, 7, 511, 512, 513, 1500, 4096};
    for (size_t wi = 0; wi < sizeof chunks / sizeof chunks[0]; ++wi) {
        vfat32::file* f = v.fs.fopen("/x.bin", "w");
        REQUIRE(f);
        for (size_t off = 0; off < n;) {
            size_t c = chunks[wi] < n - off ? chunks[wi] : n - off;
            CHECK_EQ(v.fs.fwrite(data + off, 1, c, f), c);
            off += c;
        }
        CHECK_EQ(v.fs.fclose(f), 0);
        CHECK_EQ(test::file_size(v.fs, "/x.bin"), static_cast<long>(n));
        size_t ri = (wi + 3) % (sizeof chunks / sizeof chunks[0]);
        f = v.fs.fopen("/x.bin", "r");
        REQUIRE(f);
        memset(back, 0, n);
        for (size_t off = 0; off < n;) {
            size_t c = chunks[ri] < n - off ? chunks[ri] : n - off;
            CHECK_EQ(v.fs.fread(back + off, 1, c, f), c);
            off += c;
        }
        CHECK_EQ(v.fs.fread(back, 1, 1, f), 0u);
        CHECK(v.fs.feof(f) != 0);
        CHECK_EQ(v.fs.fclose(f), 0);
        CHECK(memcmp(data, back, n) == 0);
    }
    CHECK(test::check_clean(v.fs));
    free(data);
    free(back);
}

TEST(stream_fread_counts_members) {
    volume v(one_mib);
    REQUIRE(v.ok());
    char text[26];
    memcpy(text, "abcdefghijklmnopqrstuvwxy", 25);
    CHECK(test::write_file(v.fs, "/m.bin", text, 25));
    vfat32::file* f = v.fs.fopen("/m.bin", "r");
    REQUIRE(f);
    char buf[64];
    CHECK_EQ(v.fs.fread(buf, 10, 5, f), 2u);  // 25 bytes = 2 whole members of 10
    CHECK(v.fs.feof(f) != 0);
    CHECK_EQ(v.fs.fread(buf, 0, 5, f), 0u);
    CHECK_EQ(v.fs.fclose(f), 0);
    f = v.fs.fopen("/m2.bin", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fwrite(text, 5, 3, f), 3u);
    CHECK_EQ(v.fs.fwrite(text, 0, 3, f), 0u);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(test::file_size(v.fs, "/m2.bin"), 15);
}

TEST(stream_seek_tell_rewind_getpos_setpos) {
    volume v(one_mib);
    REQUIRE(v.ok());
    uint8_t data[3000];
    pattern(data, sizeof data, 3);
    CHECK(test::write_file(v.fs, "/s.bin", data, sizeof data));
    vfat32::file* f = v.fs.fopen("/s.bin", "r+");
    REQUIRE(f);
    CHECK_EQ(v.fs.ftell(f), 0L);
    CHECK_EQ(v.fs.fseek(f, 1000, vfat32::seek_set), 0);
    CHECK_EQ(v.fs.ftell(f), 1000L);
    CHECK_EQ(v.fs.fgetc(f), static_cast<int>(data[1000]));
    CHECK_EQ(v.fs.ftell(f), 1001L);
    CHECK_EQ(v.fs.fseek(f, -501, vfat32::seek_cur), 0);
    CHECK_EQ(v.fs.ftell(f), 500L);
    CHECK_EQ(v.fs.fgetc(f), static_cast<int>(data[500]));
    CHECK_EQ(v.fs.fseek(f, -10, vfat32::seek_end), 0);
    CHECK_EQ(v.fs.ftell(f), 2990L);
    CHECK_EQ(v.fs.fgetc(f), static_cast<int>(data[2990]));
    CHECK_EQ(v.fs.fseek(f, -1, vfat32::seek_set), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.fseek(f, -4000, vfat32::seek_end), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.fseek(f, 0, 7), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.ftell(f), 2991L);  // failed seeks do not move
    vfat32::fpos_t pos;
    CHECK_EQ(v.fs.fseek(f, 1234, vfat32::seek_set), 0);
    CHECK_EQ(v.fs.fgetpos(f, &pos), 0);
    CHECK_EQ(pos.position, 1234u);
    v.fs.rewind(f);
    CHECK_EQ(v.fs.ftell(f), 0L);
    CHECK_EQ(v.fs.fsetpos(f, &pos), 0);
    CHECK_EQ(v.fs.ftell(f), 1234L);
    CHECK_EQ(v.fs.fgetc(f), static_cast<int>(data[1234]));
    // seeking past the end alone does not grow the file
    CHECK_EQ(v.fs.fseek(f, 10000, vfat32::seek_set), 0);
    CHECK_EQ(v.fs.ftell(f), 10000L);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(test::file_size(v.fs, "/s.bin"), 3000);
}

// W3: writing past the end zero-fills the gap, even over clusters that held old data.
TEST(stream_seek_past_end_zero_fills_w3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    uint8_t junk[8192];
    memset(junk, 0xFF, sizeof junk);
    CHECK(test::write_file(v.fs, "/junk.bin", junk, sizeof junk));
    CHECK_EQ(v.fs.remove("/junk.bin"), 0);  // its clusters keep 0xFF
    vfat32::file* f = v.fs.fopen("/gap.bin", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fputc('A', f), 'A');
    CHECK_EQ(v.fs.fseek(f, 3000, vfat32::seek_set), 0);
    CHECK_EQ(v.fs.fputc('B', f), 'B');
    CHECK_EQ(v.fs.fclose(f), 0);
    static uint8_t back[4000];
    CHECK_EQ(test::read_file(v.fs, "/gap.bin", back, sizeof back), 3001L);
    CHECK_EQ(back[0], 'A');
    CHECK_EQ(back[3000], 'B');
    bool zero = true;
    for (int i = 1; i < 3000; ++i)
        if (back[i] != 0) zero = false;
    CHECK(zero);
    CHECK(test::check_clean(v.fs));
}

TEST(stream_eof_error_clearerr) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/e.txt", "ab"));
    vfat32::file* f = v.fs.fopen("/e.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.feof(f), 0);
    CHECK_EQ(v.fs.ferror(f), 0);
    CHECK_EQ(v.fs.fgetc(f), 'a');
    CHECK_EQ(v.fs.fgetc(f), 'b');
    CHECK_EQ(v.fs.feof(f), 0);
    CHECK_EQ(v.fs.fgetc(f), vfat32::eof);
    CHECK(v.fs.feof(f) != 0);
    CHECK_EQ(v.fs.ferror(f), 0);
    v.fs.clearerr(f);
    CHECK_EQ(v.fs.feof(f), 0);
    CHECK_EQ(v.fs.fgetc(f), vfat32::eof);
    CHECK(v.fs.feof(f) != 0);
    v.fs.rewind(f);
    CHECK_EQ(v.fs.feof(f), 0);
    CHECK_EQ(v.fs.fgetc(f), 'a');
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(v.fs.feof(nullptr), 0);
    CHECK_EQ(v.fs.fclose(nullptr), vfat32::eof);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
}

// B5: one pushed-back character.
TEST(stream_ungetc_b5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/u.txt", "abcdef"));
    vfat32::file* f = v.fs.fopen("/u.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.fgetc(f), 'a');
    CHECK_EQ(v.fs.fgetc(f), 'b');
    CHECK_EQ(v.fs.fgetc(f), 'c');
    CHECK_EQ(v.fs.ungetc('z', f), 'z');
    CHECK_EQ(v.fs.ftell(f), 2L);              // ftell counts it
    CHECK_EQ(v.fs.ungetc('y', f), vfat32::eof);  // only one is kept
    CHECK_EQ(v.fs.fgetc(f), 'z');
    CHECK_EQ(v.fs.fgetc(f), 'd');
    CHECK_EQ(v.fs.ungetc(vfat32::eof, f), vfat32::eof);
    CHECK_EQ(v.fs.ungetc('q', f), 'q');
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_set), 0);  // dropped by fseek
    CHECK_EQ(v.fs.fgetc(f), 'a');
    // ungetc clears end of file; fread consumes the pushed-back character first
    CHECK_EQ(v.fs.fseek(f, 0, vfat32::seek_end), 0);
    CHECK_EQ(v.fs.fgetc(f), vfat32::eof);
    CHECK(v.fs.feof(f) != 0);
    CHECK_EQ(v.fs.ungetc('!', f), '!');
    CHECK_EQ(v.fs.feof(f), 0);
    char buf[4] = {0};
    CHECK_EQ(v.fs.fread(buf, 1, 4, f), 1u);
    CHECK_EQ(buf[0], '!');
    CHECK_EQ(v.fs.ungetc(0xFF, f), 0xFF);
    CHECK_EQ(v.fs.fgetc(f), 0xFF);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/u.txt", "abcdef"));
}

TEST(stream_fgetc_fputc_fgets_fputs) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/t.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fputc(0xFF, f), 0xFF);
    CHECK_EQ(v.fs.fputc('\n', f), '\n');
    CHECK(v.fs.fputs("line one\nline two\nlast", f) >= 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    f = v.fs.fopen("/t.txt", "r");
    REQUIRE(f);
    CHECK_EQ(v.fs.fgetc(f), 0xFF);  // unsigned char value, not EOF
    CHECK_EQ(v.fs.fgetc(f), '\n');
    char line[64];
    CHECK(v.fs.fgets(line, sizeof line, f) == line);
    CHECK_STR(line, "line one\n");
    CHECK(v.fs.fgets(line, 4, f) == line);  // n - 1 characters
    CHECK_STR(line, "lin");
    CHECK(v.fs.fgets(line, sizeof line, f) == line);
    CHECK_STR(line, "e two\n");
    CHECK(v.fs.fgets(line, sizeof line, f) == line);
    CHECK_STR(line, "last");
    CHECK(v.fs.fgets(line, sizeof line, f) == nullptr);
    CHECK(v.fs.feof(f) != 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    // many characters through fputc/fgetc across clusters
    f = v.fs.fopen("/c.bin", "w");
    REQUIRE(f);
    for (int i = 0; i < 5000; ++i) CHECK_EQ(v.fs.fputc(i & 0xFF, f), i & 0xFF);
    CHECK_EQ(v.fs.fclose(f), 0);
    f = v.fs.fopen("/c.bin", "r");
    REQUIRE(f);
    bool same = true;
    for (int i = 0; i < 5000; ++i)
        if (v.fs.fgetc(f) != (i & 0xFF)) same = false;
    CHECK(same);
    CHECK_EQ(v.fs.fgetc(f), vfat32::eof);
    CHECK_EQ(v.fs.fclose(f), 0);
}

// D14: fprintf output limited to VFAT32_PRINTF_MAX - 1 bytes per call.
TEST(stream_fprintf_d14) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/p.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fprintf(f, "%d-%s-%c", 42, "xy", 'z'), 7);
    static char big[VFAT32_PRINTF_MAX + 8];
    memset(big, 'k', sizeof big);
    big[VFAT32_PRINTF_MAX - 1] = 0;  // exactly fits with its NUL
    CHECK_EQ(v.fs.fprintf(f, "%s", big), VFAT32_PRINTF_MAX - 1);
    big[VFAT32_PRINTF_MAX - 1] = 'k';
    big[VFAT32_PRINTF_MAX] = 0;  // one byte too many
    CHECK(v.fs.fprintf(f, "%s", big) < 0);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(test::file_size(v.fs, "/p.txt"), 7L + VFAT32_PRINTF_MAX - 1);  // nothing of the failed call
    uint8_t head[8] = {0};
    CHECK_EQ(test::read_file(v.fs, "/p.txt", head, 7), 7L);
    CHECK(memcmp(head, "42-xy-z", 7) == 0);
}

#if VFAT32_BUFFER_SIZE > 0
// D5: a full-buffered stream's writes reach the file only when flushed.
TEST(stream_full_buffering_private_until_flush) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* a = v.fs.fopen("/b.txt", "w");
    REQUIRE(a);
    CHECK_EQ(v.fs.fwrite("0123456789", 1, 10, a), 10u);
    CHECK_EQ(test::file_size(v.fs, "/b.txt"), 0);
    vfat32::file* b = v.fs.fopen("/b.txt", "r");
    REQUIRE(b);
    char buf[16];
    CHECK_EQ(v.fs.fread(buf, 1, sizeof buf, b), 0u);
    CHECK_EQ(v.fs.fflush(a), 0);
    CHECK_EQ(test::file_size(v.fs, "/b.txt"), 10);
    v.fs.rewind(b);
    CHECK_EQ(v.fs.fread(buf, 1, sizeof buf, b), 10u);
    CHECK(memcmp(buf, "0123456789", 10) == 0);
    CHECK_EQ(v.fs.fclose(a), 0);
    CHECK_EQ(v.fs.fclose(b), 0);
}

TEST(stream_line_buffering_flushes_at_newline) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/l.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.setvbuf(f, nullptr, vfat32::io_lbf, 0), 0);
    CHECK(v.fs.fputs("abc", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/l.txt"), 0);
    CHECK_EQ(v.fs.fputc('\n', f), '\n');
    CHECK_EQ(test::file_size(v.fs, "/l.txt"), 4);
    CHECK(v.fs.fputs("de\nf", f) >= 0);
    CHECK(test::file_size(v.fs, "/l.txt") >= 7);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/l.txt", "abc\nde\nf"));
}

// B2: setvbuf at any time flushes pending data first.
TEST(stream_setvbuf_midstream_flushes) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/mid.txt", "w");
    REQUIRE(f);
    CHECK(v.fs.fputs("pending", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/mid.txt"), 0);
    CHECK_EQ(v.fs.setvbuf(f, nullptr, vfat32::io_nbf, 0), 0);
    CHECK_EQ(test::file_size(v.fs, "/mid.txt"), 7);
    CHECK_EQ(v.fs.fclose(f), 0);
}

TEST(stream_setvbuf_internal_buffer_and_bad_mode) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/i.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.setvbuf(f, nullptr, vfat32::io_fbf, 0), 0);
    CHECK(v.fs.setvbuf(f, nullptr, 7, 0) != 0);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.fclose(f), 0);
}

// D5: a write through one handle discards the read buffers of the others.
TEST(stream_read_buffers_coherent_d5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    char text[101];
    memset(text, 'A', 100);
    text[100] = 0;
    CHECK(test::write_text(v.fs, "/coh.txt", text));
    vfat32::file* b = v.fs.fopen("/coh.txt", "r");
    REQUIRE(b);
    CHECK_EQ(v.fs.fgetc(b), 'A');  // b's buffer now holds old data
    vfat32::file* a = v.fs.fopen("/coh.txt", "r+");
    REQUIRE(a);
    CHECK_EQ(v.fs.fwrite("BBBBBBBBBB", 1, 10, a), 10u);
    CHECK_EQ(v.fs.fgetc(b), 'A');  // a's write is still in a's private buffer
    CHECK_EQ(v.fs.fflush(a), 0);
    CHECK_EQ(v.fs.fgetc(b), 'B');  // coherent once written
    CHECK_EQ(v.fs.fclose(a), 0);
    CHECK_EQ(v.fs.fclose(b), 0);
}

// B3: a request at least as large as the buffer goes straight to the file.
TEST(stream_large_requests_bypass_buffer_b3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const size_t n = 4 * VFAT32_BUFFER_SIZE;
    uint8_t* data = static_cast<uint8_t*>(malloc(n));
    REQUIRE(data);
    pattern(data, n, 11);
    vfat32::file* f = v.fs.fopen("/big.bin", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.fwrite(data, 1, n, f), n);
    CHECK_EQ(test::file_size(v.fs, "/big.bin"), static_cast<long>(n));  // visible without fflush
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::file_equals(v.fs, "/big.bin", data, n));
    free(data);
}
#endif

TEST(stream_unbuffered_visible_at_once) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/n.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.setvbuf(f, nullptr, vfat32::io_nbf, 0), 0);
    CHECK_EQ(v.fs.fputc('q', f), 'q');
    CHECK_EQ(test::file_size(v.fs, "/n.txt"), 1);
    CHECK(v.fs.fputs("rs", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/n.txt"), 3);
    v.fs.setbuf(f, nullptr);
    CHECK_EQ(v.fs.fputc('t', f), 't');
    CHECK_EQ(test::file_size(v.fs, "/n.txt"), 4);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/n.txt", "qrst"));
}

TEST(stream_user_buffer_and_setbuf) {
    volume v(one_mib);
    REQUIRE(v.ok());
    static char user[64];
    vfat32::file* f = v.fs.fopen("/ub.txt", "w");
    REQUIRE(f);
    CHECK_EQ(v.fs.setvbuf(f, user, vfat32::io_fbf, sizeof user), 0);
    char data[200];
    for (int i = 0; i < 200; ++i) data[i] = static_cast<char>('a' + i % 26);
    CHECK_EQ(v.fs.fwrite(data, 1, 63, f), 63u);
    CHECK_EQ(test::file_size(v.fs, "/ub.txt"), 0);
    CHECK_EQ(v.fs.fwrite(data + 63, 1, 137, f), 137u);
    CHECK(test::file_size(v.fs, "/ub.txt") >= 64);  // the 64-byte buffer filled at least once
    CHECK_EQ(v.fs.fflush(f), 0);
    CHECK_EQ(test::file_size(v.fs, "/ub.txt"), 200);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::file_equals(v.fs, "/ub.txt", data, 200));

    static char sb[vfat32::bufsiz];
    f = v.fs.fopen("/sb.txt", "w");
    REQUIRE(f);
    v.fs.setbuf(f, sb);
    CHECK(v.fs.fputs("set", f) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/sb.txt"), 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK(test::text_equals(v.fs, "/sb.txt", "set"));
}

TEST(stream_fflush_null_flushes_all) {
    volume v(one_mib);
    REQUIRE(v.ok());
    static char b1[64];
    static char b2[64];
    vfat32::file* f1 = v.fs.fopen("/f1.txt", "w");
    vfat32::file* f2 = v.fs.fopen("/f2.txt", "w");
    REQUIRE(f1 && f2);
    CHECK_EQ(v.fs.setvbuf(f1, b1, vfat32::io_fbf, sizeof b1), 0);
    CHECK_EQ(v.fs.setvbuf(f2, b2, vfat32::io_fbf, sizeof b2), 0);
    CHECK(v.fs.fputs("one", f1) >= 0);
    CHECK(v.fs.fputs("two!", f2) >= 0);
    CHECK_EQ(test::file_size(v.fs, "/f1.txt"), 0);
    CHECK_EQ(test::file_size(v.fs, "/f2.txt"), 0);
    CHECK_EQ(v.fs.fflush(nullptr), 0);
    CHECK_EQ(test::file_size(v.fs, "/f1.txt"), 3);
    CHECK_EQ(test::file_size(v.fs, "/f2.txt"), 4);
    CHECK_EQ(v.fs.fclose(f1), 0);
    CHECK_EQ(v.fs.fclose(f2), 0);
}

TEST(stream_fileno_range) {
    volume v(one_mib);
    REQUIRE(v.ok());
    vfat32::file* f = v.fs.fopen("/fn.txt", "w");
    REQUIRE(f);
    int fd = v.fs.fileno(f);
    CHECK(fd >= 0 && fd < VFAT32_MAX_OPEN_FILES);
    CHECK_EQ(v.fs.fileno(nullptr), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::bad_handle);
    CHECK_EQ(v.fs.fclose(f), 0);
    CHECK_EQ(v.fs.fileno(f), -1);
}
