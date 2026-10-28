// vfat32 tests — names (spec §4.6, §4.7): N1–N7, LFN layout F16–F18, D13 case folding.

#include "test_harness.hpp"

using test::geometry;
using test::one_mib;
using test::volume;

namespace {

const int lfn_offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// Slots of the root directory up to the first never-used slot.
int root_slots(uint8_t* m, const geometry& g, uint8_t** out, int cap) {
    int n = 0;
    for (uint32_t i = 0; n < cap; ++i) {
        uint8_t* s = test::dir_slot(m, g, g.root, i);
        if (!s || s[0] == 0x00) break;
        out[n++] = s;
    }
    return n;
}

}  // namespace

TEST(names_long_ascii_limit_n2) {
    volume v(one_mib);
    REQUIRE(v.ok());
    char name[VFAT32_NAME_MAX + 8];
    char path[VFAT32_NAME_MAX + 16];
    const int max = VFAT32_NAME_MAX < 255 ? VFAT32_NAME_MAX : 255;
    memset(name, 'a', sizeof name);
    memcpy(name + max - 4, ".txt", 4);
    name[max] = 0;
    snprintf(path, sizeof path, "/%s", name);
    CHECK(test::write_text(v.fs, path, "longest"));
    CHECK(test::text_equals(v.fs, path, "longest"));
    CHECK(test::dir_contains(v.fs, "/", name));
    memset(name, 'b', sizeof name);
    name[max + 1] = 0;
    snprintf(path, sizeof path, "/%s", name);
    CHECK(v.fs.fopen(path, "w") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::name_too_long);
    CHECK_EQ(v.fs.mkdir(path), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::name_too_long);
    CHECK(test::check_clean(v.fs));
}

TEST(names_utf8_round_trip_n1_n7) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/u"), 0);
    const char* names[] = {
        "caf\xC3\xA9.txt",                                      // café
        "\xE4\xB8\xAD\xE6\x96\x87\xE5\x90\x8D\xE5\xAD\x97.txt",  // 中文名字
        "smile_\xF0\x9F\x98\x80.txt",                           // U+1F600, a surrogate pair
        "\xC3\x9F\xC3\xB6\xE2\x82\xAC mixed \xF0\x9F\x8E\xB5",   // ßö€ mixed 🎵
    };
    const int n = sizeof names / sizeof names[0];
    char path[128];
    for (int i = 0; i < n; ++i) {
        snprintf(path, sizeof path, "/u/%s", names[i]);
        CHECK(test::write_file(v.fs, path, &i, sizeof i));
        CHECK(test::file_equals(v.fs, path, &i, sizeof i));
    }
    int found[n];
    memset(found, 0, sizeof found);
    vfat32::dir* d = v.fs.opendir("/u");
    REQUIRE(d);
    while (vfat32::dirent* e = v.fs.readdir(d)) {
        bool known = false;
        for (int i = 0; i < n; ++i)
            if (strcmp(e->d_name, names[i]) == 0) {
                ++found[i];
                known = true;
            }
        if (!known) printf("    unexpected name in readdir\n");
        CHECK(known);
    }
    v.fs.closedir(d);
    for (int i = 0; i < n; ++i) CHECK_EQ(found[i], 1);
    // 85 three-byte characters = 255 bytes fit; 86 do not (N2 counts UTF-8 bytes)
    char name[300];
    size_t len = 0;
    for (int i = 0; i < 85; ++i) {
        memcpy(name + len, "\xE4\xB8\xAD", 3);
        len += 3;
    }
    name[len] = 0;
    char full[320];
    snprintf(full, sizeof full, "/%s", name);
    if (VFAT32_NAME_MAX >= 255) {
        CHECK(test::write_text(v.fs, full, "cjk"));
        CHECK(test::dir_contains(v.fs, "/", name));
    }
    memcpy(name + len, "\xE4\xB8\xAD", 3);
    name[len + 3] = 0;
    snprintf(full, sizeof full, "/%s", name);
    CHECK(v.fs.fopen(full, "w") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::name_too_long);
}

TEST(names_invalid_utf8_n1) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const char* bad[] = {
        "/a\xC3\x28.txt",          // bad continuation
        "/a\xC0\xAF.txt",          // overlong '/'
        "/a\xE0\x80\xAF.txt",      // overlong
        "/a\xED\xA0\x80.txt",      // encoded surrogate
        "/a\xF5\x80\x80\x80.txt",  // > U+10FFFF lead byte
        "/a\xF4\x90\x80\x80.txt",  // > U+10FFFF
        "/a\xE4\xB8",              // truncated at end
        "/a\x80.txt",              // stray continuation
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        vfat32::file* f = v.fs.fopen(bad[i], "w");
        if (f) {
            printf("    accepted invalid UTF-8 #%lu\n", static_cast<unsigned long>(i));
            v.fs.fclose(f);
        }
        CHECK(f == nullptr);
        CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    }
    CHECK_EQ(test::count_entries(v.fs, "/"), 0);
}

TEST(names_forbidden_characters_n3) {
    volume v(one_mib);
    REQUIRE(v.ok());
    const char forbidden[] = "\"*:<>?\\|";
    char path[32];
    for (size_t i = 0; forbidden[i]; ++i) {
        snprintf(path, sizeof path, "/a%cb.txt", forbidden[i]);
        CHECK(v.fs.fopen(path, "w") == nullptr);
        CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
        CHECK_EQ(v.fs.mkdir(path), -1);
        CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    }
    for (int c = 1; c < 0x20; ++c) {
        snprintf(path, sizeof path, "/ctl%cx", c);
        CHECK(v.fs.fopen(path, "w") == nullptr);
        CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    }
    const char* bad[] = {"/trailing.", "/trailing ", "/dir.", "/x/..", "/."};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        vfat32::file* f = v.fs.fopen(bad[i], "w");
        CHECK(f == nullptr);
        if (f) v.fs.fclose(f);
    }
    CHECK(v.fs.fopen("/trailing.", "w") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK(v.fs.fopen("/trailing ", "w") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    CHECK_EQ(v.fs.mkdir("/end."), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::invalid);
    // a forbidden name is simply not found by lookups (N3)
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/a*b", &st), -1);
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    // leading spaces, inner dots and spaces are allowed
    CHECK(test::write_text(v.fs, "/ lead.txt", "1"));
    CHECK(test::write_text(v.fs, "/a.b.c", "2"));
    CHECK(test::write_text(v.fs, "/.hidden", "3"));
    CHECK(test::write_text(v.fs, "/with  two  spaces", "4"));
    CHECK(test::dir_contains(v.fs, "/", " lead.txt"));
    CHECK(test::dir_contains(v.fs, "/", "a.b.c"));
    CHECK(test::dir_contains(v.fs, "/", ".hidden"));
    CHECK(test::dir_contains(v.fs, "/", "with  two  spaces"));
    CHECK_EQ(test::count_entries(v.fs, "/"), 4);
}

// N6 / D13: ASCII case folding only.
TEST(names_case_insensitive_ascii_only_n6_d13) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/MixedCase.Txt", "m"));
    CHECK(test::text_equals(v.fs, "/mixedcase.txt", "m"));
    CHECK(test::text_equals(v.fs, "/MIXEDCASE.TXT", "m"));
    CHECK(v.fs.fopen("/mixedcase.txt", "wx") == nullptr);
    CHECK_EQ(v.fs.last_error(), vfat32::error::exists);
    CHECK(test::dir_contains(v.fs, "/", "MixedCase.Txt"));  // the stored case is kept
    CHECK(test::write_text(v.fs, "/\xC3\xA9.txt", "lower e acute"));
    vfat32::stat st;
    CHECK_EQ(v.fs.stat("/\xC3\x89.txt", &st), -1);  // É is not folded to é
    CHECK_EQ(v.fs.last_error(), vfat32::error::not_found);
    CHECK(test::write_text(v.fs, "/\xC3\x89.txt", "upper"));  // so it is a different file
    CHECK_EQ(test::count_entries(v.fs, "/"), 3);
    CHECK_EQ(v.fs.mkdir("/Dir"), 0);
    CHECK(test::write_text(v.fs, "/DIR/inside", "i"));
    CHECK(test::text_equals(v.fs, "/dir/INSIDE", "i"));
}

// N4: exact 8.3 fits use one slot; lower case goes into the NTRes flags.
TEST(names_exact_83_fit_n4) {
    struct row {
        const char* name;
        const char* sfn;
        int flags;  // -1 = needs an LFN
    };
    const row rows[] = {
        {"readme.txt", "README  TXT", 0x18}, {"README.TXT", "README  TXT", 0x00}, {"abc.TXT", "ABC     TXT", 0x08},
        {"ABC.txt", "ABC     TXT", 0x10},    {"12345678.123", "12345678123", 0x00}, {"noext", "NOEXT      ", 0x08},
        {"A~1.B", "A~1     B  ", 0x00},       {"ReadMe.txt", nullptr, -1},           {"aBc.txt", nullptr, -1},
        {"123456789.txt", nullptr, -1},      {"a.b.c", nullptr, -1},                 {".hidden", nullptr, -1},
        {"file+1.txt", nullptr, -1},          {"a b.txt", nullptr, -1},              {"x.abcd", nullptr, -1},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        volume v(one_mib);
        REQUIRE(v.ok());
        char path[64];
        snprintf(path, sizeof path, "/%s", rows[i].name);
        REQUIRE(test::write_text(v.fs, path, "x"));
        geometry g = test::read_geometry(v.mem);
        uint8_t* slots[8];
        int n = root_slots(v.mem, g, slots, 8);
        if (rows[i].flags >= 0) {
            if (n != 1) printf("    %s: %d slots\n", rows[i].name, n);
            CHECK_EQ(n, 1);
            if (n >= 1) {
                CHECK(memcmp(slots[0], rows[i].sfn, 11) == 0);
                CHECK_EQ(slots[0][12] & 0x18, rows[i].flags);
            }
        } else {
            if (n < 2) printf("    %s: no LFN\n", rows[i].name);
            CHECK(n >= 2);
            if (n >= 2) {
                CHECK_EQ(slots[0][11], 0x0F);
                CHECK(slots[n - 1][11] != 0x0F);
            }
        }
        CHECK(test::dir_contains(v.fs, "/", rows[i].name));  // the name comes back as given
        CHECK(test::text_equals(v.fs, path, "x"));
    }
}

// N5 / N6 / F16 / F17: the 8.3 alias of a long name, its checksum and the LFN slot layout.
TEST(names_alias_lfn_layout_checksum_n5_f16_f17) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/Long file name.txt", "alias one"));
    CHECK(test::write_text(v.fs, "/Long file name 2.txt", "alias two"));
    CHECK(test::text_equals(v.fs, "/LONGFI~1.TXT", "alias one"));  // N6: open by alias
    CHECK(test::text_equals(v.fs, "/longfi~1.txt", "alias one"));
    CHECK(test::text_equals(v.fs, "/LONGFI~2.TXT", "alias two"));
    geometry g = test::read_geometry(v.mem);
    uint8_t* slots[8];
    int n = root_slots(v.mem, g, slots, 8);
    REQUIRE(n >= 3);
    // "Long file name.txt" = 18 units -> 2 LFN slots, last part first
    CHECK_EQ(slots[0][0], 0x42);
    CHECK_EQ(slots[1][0], 0x01);
    CHECK(memcmp(slots[2], "LONGFI~1TXT", 11) == 0);
    uint8_t sum = test::lfn_checksum(slots[2]);
    for (int s = 0; s < 2; ++s) {
        CHECK_EQ(slots[s][11], 0x0F);
        CHECK_EQ(slots[s][12], 0);
        CHECK_EQ(slots[s][13], sum);
        CHECK_EQ(test::le16(slots[s] + 26), 0);
    }
    const char* name = "Long file name.txt";
    for (int k = 0; k < 13; ++k) CHECK_EQ(test::le16(slots[1] + lfn_offsets[k]), static_cast<uint16_t>(name[k]));
    for (int k = 0; k < 5; ++k) CHECK_EQ(test::le16(slots[0] + lfn_offsets[k]), static_cast<uint16_t>(name[13 + k]));
    CHECK_EQ(test::le16(slots[0] + lfn_offsets[5]), 0x0000);
    for (int k = 6; k < 13; ++k) CHECK_EQ(test::le16(slots[0] + lfn_offsets[k]), 0xFFFF);
    // invalid characters become '_' (N5)
    CHECK(test::write_text(v.fs, "/file+1.txt", "plus"));
    CHECK(test::text_equals(v.fs, "/FILE_1~1.TXT", "plus"));
    // readdir returns long names, never aliases
    CHECK(test::dir_contains(v.fs, "/", "Long file name.txt"));
    CHECK(!test::dir_contains(v.fs, "/", "LONGFI~1.TXT"));
}

// N5: hundreds of similar long names all get distinct aliases (tails, then the hash form).
TEST(names_many_similar_long_names_n5) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK_EQ(v.fs.mkdir("/many"), 0);
    const int n = 300;
    char path[96];
    for (int i = 0; i < n; ++i) {
        snprintf(path, sizeof path, "/many/long name number %d.txt", i);
        if (!test::write_file(v.fs, path, &i, sizeof i)) {
            printf("    create failed at %d: %s\n", i, vfat32::vfs::strerror(v.fs.last_error()));
            CHECK(false);
            return;
        }
    }
    CHECK_EQ(test::count_entries(v.fs, "/many"), n);
    for (int i = 0; i < n; ++i) {
        snprintf(path, sizeof path, "/many/LONG NAME NUMBER %d.TXT", i);
        CHECK(test::file_equals(v.fs, path, &i, sizeof i));
    }
    geometry g = test::read_geometry(v.mem);
    uint8_t* de = test::find_sfn(v.mem, g, g.root, "MANY       ");
    REQUIRE(de);
    uint32_t dir = test::entry_cluster(de);
    static uint8_t sfns[n][11];
    int count = 0;
    for (uint32_t i = 0;; ++i) {
        uint8_t* s = test::dir_slot(v.mem, g, dir, i);
        if (!s || s[0] == 0) break;
        if (s[0] == 0xE5 || s[11] == 0x0F || s[0] == '.') continue;
        REQUIRE(count < n);
        memcpy(sfns[count++], s, 11);
    }
    CHECK_EQ(count, n);
    int duplicates = 0;
    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j)
            if (memcmp(sfns[i], sfns[j], 11) == 0) ++duplicates;
    CHECK_EQ(duplicates, 0);
    CHECK(test::check_clean(v.fs));
}

// F18: an LFN whose checksum does not match is an orphan: the 8.3 name is used.
TEST(names_orphan_lfn_ignored_f18) {
    volume v(one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/Orphaned long name.txt", "orphan"));
    geometry g = test::read_geometry(v.mem);
    uint8_t* slots[8];
    int n = root_slots(v.mem, g, slots, 8);
    REQUIRE(n >= 2);
    char alias[16];
    const uint8_t* sfn = slots[n - 1];
    int k = 0;
    for (int i = 0; i < 8 && sfn[i] != ' '; ++i) alias[k++] = static_cast<char>(sfn[i]);
    alias[k++] = '.';
    for (int i = 8; i < 11 && sfn[i] != ' '; ++i) alias[k++] = static_cast<char>(sfn[i]);
    alias[k] = 0;
    for (int s = 0; s < n - 1; ++s) slots[s][13] ^= 0x5A;  // break the checksum
    CHECK(test::dir_contains(v.fs, "/", alias));
    CHECK(!test::dir_contains(v.fs, "/", "Orphaned long name.txt"));
    char path[32];
    snprintf(path, sizeof path, "/%s", alias);
    CHECK(test::text_equals(v.fs, path, "orphan"));
}
