// vfat32 tests — independent validation (spec §15.3): the host's own FAT checker on images made by vfat32, and
// images made by the host's formatter mounted with reuse. Skipped (and passing) when no tool is installed.

#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.hpp"

namespace {

const size_t strict_size = 40 * test::one_mib;  // >= 65,525 clusters of 512 B: FAT32 by Microsoft's rule too

bool has_command(const char* name) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

const char* fat_checker() {
    if (access("/sbin/fsck_msdos", X_OK) == 0) return "/sbin/fsck_msdos";
    if (has_command("fsck.fat")) return "fsck.fat";
    if (has_command("fsck.vfat")) return "fsck.vfat";
    return nullptr;
}

void temp_path(char* out, size_t cap, const char* suffix) {
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t len = strlen(dir);
    snprintf(out, cap, "%s%svfat32_%d_%s", dir, (len && dir[len - 1] == '/') ? "" : "/", static_cast<int>(getpid()),
             suffix);
}

// Runs a shell command; returns its exit status (-1 if it did not exit) and its output.
int run(const char* cmd, char* out, size_t cap) {
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        size_t got = fread(out + n, 1, cap - 1 - n, p);
        if (got == 0) break;
        n += got;
    }
    out[n] = 0;
    char sink[256];
    while (fread(sink, 1, sizeof sink, p) > 0) {
    }
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool save(const char* path, const uint8_t* m, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(m, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

uint8_t* load(const char* path, size_t& n) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return nullptr;
    }
    n = static_cast<size_t>(size);
    uint8_t* m = static_cast<uint8_t*>(malloc(n));
    if (m && fread(m, 1, n, f) != n) {
        free(m);
        m = nullptr;
    }
    fclose(f);
    return m;
}

// fsck -n on an image file; prints the output when it fails.
int fsck_image(const char* tool, const char* image, bool quiet_ok) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s -n '%s' 2>&1", tool, image);
    static char out[16384];
    int rc = run(cmd, out, sizeof out);
    if (rc != 0 || !quiet_ok) printf("    $ %s -n <image> -> %d\n%s", tool, rc, out);
    return rc;
}

bool populate(vfat32::vfs& fs, const char* top) {
    char path[128];
    char base[64];
    snprintf(base, sizeof base, "%s", top);
    if (fs.mkdir(base) != 0) return false;
    snprintf(path, sizeof path, "%s/docs", base);
    if (fs.mkdir(path) != 0) return false;
    snprintf(path, sizeof path, "%s/docs/sub", base);
    if (fs.mkdir(path) != 0) return false;
    snprintf(path, sizeof path, "%s/many", base);
    if (fs.mkdir(path) != 0) return false;
    const char* texts[][2] = {
        {"readme.txt", "lower case 8.3 with NT case flags\n"},
        {"UPPER.TXT", "plain 8.3\n"},
        {"Long File Name Example.txt", "a long file name\n"},
        {"caf\xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87.txt", "unicode name\n"},
        {"empty.txt", ""},
    };
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", base, texts[i][0]);
        if (!test::write_text(fs, path, texts[i][1])) return false;
    }
    const size_t big = 1024 * 1024;
    uint8_t* data = static_cast<uint8_t*>(malloc(big));
    if (!data) return false;
    test::rng r(2026);
    r.fill(data, big);
    snprintf(path, sizeof path, "%s/docs/sub/deep.bin", base);
    bool ok = test::write_file(fs, path, data, big);
    free(data);
    if (!ok) return false;
    for (int i = 0; i < 50; ++i) {
        snprintf(path, sizeof path, "%s/many/entry with a long name %02d.dat", base, i);
        char content[32];
        int n = snprintf(content, sizeof content, "entry %d\n", i);
        if (!test::write_file(fs, path, content, static_cast<size_t>(n))) return false;
    }
    for (int i = 0; i < 50; i += 5) {
        snprintf(path, sizeof path, "%s/many/entry with a long name %02d.dat", base, i);
        if (fs.remove(path) != 0) return false;
    }
    char from[128];
    snprintf(from, sizeof from, "%s/UPPER.TXT", base);
    snprintf(path, sizeof path, "%s/docs/Renamed Upper.txt", base);
    if (fs.rename(from, path) != 0) return false;
    return test::check_clean(fs);
}

bool make_host_image(const char* image) {
    char cmd[2048];
    char out[4096];
    if (access("/usr/bin/hdiutil", X_OK) == 0) {
        char dmg[512];
        char base[512];
        if (snprintf(dmg, sizeof dmg, "%s.dmg", image) >= static_cast<int>(sizeof dmg) ||
            snprintf(base, sizeof base, "%s", image) >= static_cast<int>(sizeof base))
            return false;  // temp path too long
        size_t len = strlen(base);
        if (len > 4 && strcmp(base + len - 4, ".cdr") == 0) base[len - 4] = 0;
        ::remove(dmg);
        ::remove(image);
        if (snprintf(cmd, sizeof cmd,
                     "/usr/bin/hdiutil create -size 40m -fs 'MS-DOS FAT32' -layout NONE -volname HOSTVOL -o '%s' 2>&1 "
                     "&& /usr/bin/hdiutil convert '%s' -format UDTO -o '%s' 2>&1",
                     dmg, dmg, base) >= static_cast<int>(sizeof cmd))
            return false;
        int rc = run(cmd, out, sizeof out);
        ::remove(dmg);
        if (rc != 0) printf("    hdiutil failed (%d):\n%s", rc, out);
        return rc == 0;
    }
    if (has_command("mkfs.fat")) {
        ::remove(image);
        if (snprintf(cmd, sizeof cmd, "mkfs.fat -F 32 -C '%s' 40960 2>&1", image) >= static_cast<int>(sizeof cmd))
            return false;
        int rc = run(cmd, out, sizeof out);
        if (rc != 0) printf("    mkfs.fat failed (%d):\n%s", rc, out);
        return rc == 0;
    }
    return false;
}

}  // namespace

// §15.3: an image made by vfat32 passes the host's checker.
TEST(external_fsck_accepts_vfat32_image) {
    const char* tool = fat_checker();
    if (!tool) {
        printf("    SKIP: no fsck_msdos / fsck.fat on this system\n");
        return;
    }
    uint8_t* m = static_cast<uint8_t*>(calloc(1, strict_size));
    REQUIRE(m);
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, strict_size, vfat32::mount_mode::format), 0);
    test::geometry g = test::read_geometry(m);
    CHECK(g.clusters >= 65525u);
    CHECK(populate(fs, "/vfat32"));
    REQUIRE_EQ(fs.umount(), 0);
    char image[512];
    temp_path(image, sizeof image, "made.img");
    REQUIRE(save(image, m, strict_size));
    CHECK_EQ(fsck_image(tool, image, true), 0);
    // still ours afterwards
    vfat32::vfs again;
    CHECK_EQ(again.mount(m, strict_size, vfat32::mount_mode::reuse), 0);
    CHECK(test::text_equals(again, "/vfat32/docs/Renamed Upper.txt", "plain 8.3\n"));
    again.umount();
    ::remove(image);
    free(m);
}

// §15.3: an image made by the host's formatter is complete for vfat32, usable, and still passes the host checker.
TEST(external_host_formatted_image_reused) {
    const char* tool = fat_checker();
    char image[512];
    temp_path(image, sizeof image, "host.cdr");
    if (!tool || !make_host_image(image)) {
        printf("    SKIP: no host FAT32 formatter (hdiutil / mkfs.fat) or checker\n");
        return;
    }
    size_t n = 0;
    uint8_t* m = load(image, n);
    REQUIRE(m);
    CHECK(!vfat32::vfs::needs_format(m, n));
    vfat32::vfs fs;
    REQUIRE_EQ(fs.mount(m, n, vfat32::mount_mode::reuse), 0);
    vfat32::check_report r;
    test::clear_report(r);
    int rc = fs.check(&r);
    if (rc != 0) test::print_report(r);
    CHECK_EQ(rc, 0);
    CHECK(populate(fs, "/from vfat32"));
    CHECK(test::text_equals(fs, "/from vfat32/readme.txt", "lower case 8.3 with NT case flags\n"));
    REQUIRE_EQ(fs.umount(), 0);
    REQUIRE(save(image, m, n));
    CHECK_EQ(fsck_image(tool, image, true), 0);
    ::remove(image);
    free(m);
}

// L1 (information only): a small, relaxed FAT32 image (fewer than 65,525 clusters) through the host checker.
TEST(external_small_relaxed_image_info) {
    const char* tool = fat_checker();
    if (!tool) {
        printf("    SKIP: no fsck_msdos / fsck.fat on this system\n");
        return;
    }
    test::volume v(test::one_mib);
    REQUIRE(v.ok());
    CHECK(test::write_text(v.fs, "/small.txt", "relaxed layout"));
    CHECK_EQ(v.fs.mkdir("/dir"), 0);
    REQUIRE_EQ(v.fs.umount(), 0);
    char image[512];
    temp_path(image, sizeof image, "small.img");
    REQUIRE(save(image, v.mem, v.size));
    int rc = fsck_image(tool, image, false);
    printf("    (information, spec L1) small relaxed image: checker exit %d\n", rc);
    ::remove(image);
}
