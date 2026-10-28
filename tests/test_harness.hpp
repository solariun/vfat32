// vfat32 tests — a tiny test framework without the STL, and helpers shared by every test file.
//
// TEST(name) registers a test; CHECK/CHECK_EQ/CHECK_STR report a failure and go on; REQUIRE/REQUIRE_EQ report
// and leave the test. Raw helpers read the FAT32 image bytes directly (spec §4) so tests can verify and damage
// the on-disk structures without going through the library.

#ifndef VFAT32_TEST_HARNESS_HPP
#define VFAT32_TEST_HARNESS_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfat32.hpp"

namespace test {

typedef void (*test_function)();

struct test_case {
    const char* name;
    const char* file;
    test_function function;
    test_case* next;
    test_case(const char* test_name, const char* file_name, test_function fn);
};

test_case* first_test();
void report_failure(const char* file, int line, const char* message);
int current_failures();
bool long_mode();

// ── values printed by CHECK_EQ ──
struct value {
    bool negative;
    unsigned long long magnitude;
    const char* text;
};

inline value make_signed(long long v) {
    value r;
    r.negative = v < 0;
    r.magnitude = v < 0 ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    r.text = nullptr;
    return r;
}
inline value make_unsigned(unsigned long long v) {
    value r;
    r.negative = false;
    r.magnitude = v;
    r.text = nullptr;
    return r;
}
inline value make_value(char v) { return make_signed(v); }
inline value make_value(signed char v) { return make_signed(v); }
inline value make_value(short v) { return make_signed(v); }
inline value make_value(int v) { return make_signed(v); }
inline value make_value(long v) { return make_signed(v); }
inline value make_value(long long v) { return make_signed(v); }
inline value make_value(unsigned char v) { return make_unsigned(v); }
inline value make_value(unsigned short v) { return make_unsigned(v); }
inline value make_value(unsigned int v) { return make_unsigned(v); }
inline value make_value(unsigned long v) { return make_unsigned(v); }
inline value make_value(unsigned long long v) { return make_unsigned(v); }
inline value make_value(bool v) { return make_unsigned(v ? 1 : 0); }
inline value make_value(vfat32::error e) {
    value r = make_unsigned(static_cast<unsigned>(e));
    r.text = vfat32::vfs::strerror(e);
    return r;
}

bool check_eq(const value& a, const value& b, const char* ea, const char* eb, const char* file, int line);
bool check_str(const char* a, const char* b, const char* ea, const char* eb, const char* file, int line);

// ── deterministic PRNG: xorshift64, the same numbers on every system ──
struct rng {
    uint64_t state;
    explicit rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x;
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
    void fill(uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(next() >> 56);
    }
};

// ── a formatted volume on calloc'd memory ──
struct volume {
    uint8_t* mem;
    size_t size;
    vfat32::vfs fs;
    int rc;
    explicit volume(size_t bytes, const vfat32::format_options* options = nullptr) : mem(nullptr), size(bytes), rc(-1) {
        mem = static_cast<uint8_t*>(calloc(1, bytes));
        if (!mem) {
            fprintf(stderr, "out of memory allocating %lu bytes\n", static_cast<unsigned long>(bytes));
            exit(2);
        }
        rc = fs.mount(mem, bytes, vfat32::mount_mode::format, options);
    }
    ~volume() {
        if (fs.mounted()) fs.umount();
        free(mem);
    }
    bool ok() const { return rc == 0; }

private:
    volume(const volume&);
    volume& operator=(const volume&);
};

const size_t one_mib = 1024 * 1024;

// ── raw little-endian access ──
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

const uint32_t fat_mask = 0x0FFFFFFF;
const uint32_t fat_eoc_min = 0x0FFFFFF8;
const uint32_t fat_eoc = 0x0FFFFFFF;
const uint32_t fat_bad = 0x0FFFFFF7;
const uint32_t slot_bytes = 32;

// The geometry of an image, read from its BPB (spec §4.2).
struct geometry {
    uint32_t bps;
    uint32_t spc;
    uint32_t rsvd;
    uint32_t nfats;
    uint32_t fat_sectors;
    uint32_t total;
    uint32_t root;
    uint32_t fsinfo;
    uint32_t backup;
    uint32_t clusters;
    uint32_t cluster_bytes;
    size_t fat_offset;
    size_t data_offset;
};

inline geometry read_geometry(const uint8_t* m) {
    geometry g;
    g.bps = le16(m + 11);
    g.spc = m[13];
    g.rsvd = le16(m + 14);
    g.nfats = m[16];
    g.total = le32(m + 32);
    g.fat_sectors = le32(m + 36);
    g.root = le32(m + 44);
    g.fsinfo = le16(m + 48);
    g.backup = le16(m + 50);
    g.cluster_bytes = g.bps * g.spc;
    g.fat_offset = static_cast<size_t>(g.rsvd) * g.bps;
    uint32_t data_sectors = g.rsvd + g.nfats * g.fat_sectors;
    g.data_offset = static_cast<size_t>(data_sectors) * g.bps;
    g.clusters = g.spc ? (g.total - data_sectors) / g.spc : 0;
    return g;
}

inline uint32_t fat_raw(const uint8_t* m, const geometry& g, uint32_t cluster, uint32_t copy = 0) {
    return le32(m + g.fat_offset + static_cast<size_t>(copy) * g.fat_sectors * g.bps + static_cast<size_t>(cluster) * 4);
}
inline uint32_t fat_get(const uint8_t* m, const geometry& g, uint32_t cluster) {
    return fat_raw(m, g, cluster) & fat_mask;
}
// Writes the raw 32-bit value in one copy, or in every copy when copy < 0.
inline void fat_put(uint8_t* m, const geometry& g, uint32_t cluster, uint32_t value, int copy = -1) {
    for (uint32_t c = 0; c < g.nfats; ++c) {
        if (copy >= 0 && static_cast<uint32_t>(copy) != c) continue;
        put32(m + g.fat_offset + static_cast<size_t>(c) * g.fat_sectors * g.bps + static_cast<size_t>(cluster) * 4,
              value);
    }
}
inline uint8_t* cluster_data(uint8_t* m, const geometry& g, uint32_t cluster) {
    return m + g.data_offset + static_cast<size_t>(cluster - 2) * g.cluster_bytes;
}
inline uint32_t entry_cluster(const uint8_t* e) {
    return (static_cast<uint32_t>(le16(e + 20)) << 16) | le16(e + 26);
}
inline void set_entry_cluster(uint8_t* e, uint32_t c) {
    put16(e + 20, static_cast<uint16_t>(c >> 16));
    put16(e + 26, static_cast<uint16_t>(c));
}
inline uint32_t chain_length(const uint8_t* m, const geometry& g, uint32_t first) {
    uint32_t n = 0;
    uint32_t c = first;
    while (c >= 2 && c <= g.clusters + 1 && n <= g.clusters) {
        ++n;
        uint32_t v = fat_get(m, g, c);
        if (v >= fat_eoc_min) break;
        c = v;
    }
    return n;
}
// The index-th 32-byte slot of the directory whose chain starts at `first`, or nullptr past its end.
inline uint8_t* dir_slot(uint8_t* m, const geometry& g, uint32_t first, uint32_t index) {
    uint32_t per_cluster = g.cluster_bytes / slot_bytes;
    uint32_t c = first;
    for (uint32_t skip = index / per_cluster; skip > 0; --skip) {
        uint32_t v = fat_get(m, g, c);
        if (v < 2 || v >= fat_eoc_min) return nullptr;
        c = v;
    }
    if (c < 2 || c > g.clusters + 1) return nullptr;
    return cluster_data(m, g, c) + (index % per_cluster) * slot_bytes;
}
// The 8.3 slot named exactly `sfn11` (11 bytes, space padded) in a directory, or nullptr.
inline uint8_t* find_sfn(uint8_t* m, const geometry& g, uint32_t dir_first, const char* sfn11) {
    for (uint32_t i = 0;; ++i) {
        uint8_t* s = dir_slot(m, g, dir_first, i);
        if (!s || s[0] == 0x00) return nullptr;
        if (s[0] == 0xE5 || s[11] == 0x0F) continue;
        if (memcmp(s, sfn11, 11) == 0) return s;
    }
}
inline uint8_t lfn_checksum(const uint8_t* sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}
// True when no FAT entry of the first copy carries the check/defrag mark (spec D3, C1).
inline bool no_marks(const uint8_t* m, const geometry& g) {
    for (uint32_t c = 2; c <= g.clusters + 1; ++c)
        if (fat_raw(m, g, c) & 0x80000000u) return false;
    return true;
}

// ── file helpers through the API ──
inline bool write_file(vfat32::vfs& fs, const char* path, const void* data, size_t n) {
    vfat32::file* f = fs.fopen(path, "w");
    if (!f) return false;
    size_t w = n ? fs.fwrite(data, 1, n, f) : 0;
    int rc = fs.fclose(f);
    return w == n && rc == 0;
}
inline bool write_text(vfat32::vfs& fs, const char* path, const char* text) {
    return write_file(fs, path, text, strlen(text));
}
// Reads a whole file; returns its size or -1.
inline long read_file(vfat32::vfs& fs, const char* path, uint8_t* buf, size_t cap) {
    vfat32::file* f = fs.fopen(path, "r");
    if (!f) return -1;
    size_t n = fs.fread(buf, 1, cap, f);
    fs.fclose(f);
    return static_cast<long>(n);
}
inline bool file_equals(vfat32::vfs& fs, const char* path, const void* data, size_t n) {
    uint8_t* buf = static_cast<uint8_t*>(malloc(n + 16));
    long got = read_file(fs, path, buf, n + 16);
    bool same = got == static_cast<long>(n) && (n == 0 || memcmp(buf, data, n) == 0);
    free(buf);
    return same;
}
inline bool text_equals(vfat32::vfs& fs, const char* path, const char* text) {
    return file_equals(fs, path, text, strlen(text));
}
inline bool exists(vfat32::vfs& fs, const char* path) {
    vfat32::stat st;
    return fs.stat(path, &st) == 0;
}
inline long file_size(vfat32::vfs& fs, const char* path) {
    vfat32::stat st;
    if (fs.stat(path, &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}
inline int count_entries(vfat32::vfs& fs, const char* path) {
    vfat32::dir* d = fs.opendir(path);
    if (!d) return -1;
    int n = 0;
    while (fs.readdir(d)) ++n;
    fs.closedir(d);
    return n;
}
inline bool dir_contains(vfat32::vfs& fs, const char* path, const char* name) {
    vfat32::dir* d = fs.opendir(path);
    if (!d) return false;
    bool found = false;
    while (vfat32::dirent* e = fs.readdir(d))
        if (strcmp(e->d_name, name) == 0) found = true;
    fs.closedir(d);
    return found;
}
inline uint32_t free_clusters(vfat32::vfs& fs) {
    vfat32::statvfs s;
    if (fs.statvfs(&s) != 0) return 0xFFFFFFFFu;
    return s.f_bfree;
}

void print_report(const vfat32::check_report& r);

inline void clear_report(vfat32::check_report& r) { memset(&r, 0, sizeof r); }

// check() without repair must return 0 with no error counted.
inline bool check_clean(vfat32::vfs& fs) {
    vfat32::check_report r;
    clear_report(r);
    int rc = fs.check(&r, false);
    if (rc != 0 || r.errors() != 0) {
        print_report(r);
        return false;
    }
    return true;
}

}  // namespace test

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::test::test_case name##_registration(#name, __FILE__, name);  \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) ::test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define REQUIRE(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ::test::report_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")");        \
            return;                                                                  \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b) ::test::check_eq(::test::make_value(a), ::test::make_value(b), #a, #b, __FILE__, __LINE__)

#define REQUIRE_EQ(a, b)                                                                                     \
    do {                                                                                                     \
        if (!::test::check_eq(::test::make_value(a), ::test::make_value(b), #a, #b, __FILE__, __LINE__)) return; \
    } while (0)

#define CHECK_STR(a, b) ::test::check_str((a), (b), #a, #b, __FILE__, __LINE__)

#endif  // VFAT32_TEST_HARNESS_HPP
