// vfat32 tests — model-based random stress test: random operations on the volume and on a simple model of it,
// compared after every operation; check() clean at intervals; remounts (reuse) and incremental defrag in between.
// VFAT32_LONG=1 runs many more operations and seeds.

#include "test_harness.hpp"

using test::volume;

namespace {

const int max_files = 40;
const size_t max_file_size = 24 * 1024;
const int dir_count = 7;
const char* const dir_paths[dir_count] = {"", "/d0", "/d1", "/d0/e0", "/d0/e1", "/d1/Long Dir Name", "/d0/e0/z"};
const int dir_parent[dir_count] = {-1, 0, 0, 1, 1, 2, 3};

// VFAT32_TRACE=1 prints every operation and compares every file's size with the model after each one.
bool trace_enabled() {
    const char* v = getenv("VFAT32_TRACE");
    return v && v[0] && strcmp(v, "0") != 0;
}

struct model_file {
    bool used;
    int dir;
    char name[48];
    uint8_t* data;
    size_t size;
};

struct model {
    bool dir_exists[dir_count];
    model_file files[max_files];
    model() {
        memset(dir_exists, 0, sizeof dir_exists);
        dir_exists[0] = true;
        memset(files, 0, sizeof files);
        for (int i = 0; i < max_files; ++i) files[i].data = static_cast<uint8_t*>(malloc(max_file_size));
    }
    ~model() {
        for (int i = 0; i < max_files; ++i) free(files[i].data);
    }
    int count() const {
        int n = 0;
        for (int i = 0; i < max_files; ++i) n += files[i].used ? 1 : 0;
        return n;
    }
    int find(int dir, const char* name) const {
        for (int i = 0; i < max_files; ++i)
            if (files[i].used && files[i].dir == dir && strcmp(files[i].name, name) == 0) return i;
        return -1;
    }
    int free_slot() const {
        for (int i = 0; i < max_files; ++i)
            if (!files[i].used) return i;
        return -1;
    }
    bool dir_empty(int d) const {
        for (int i = 0; i < max_files; ++i)
            if (files[i].used && files[i].dir == d) return false;
        for (int k = 0; k < dir_count; ++k)
            if (dir_exists[k] && dir_parent[k] == d) return false;
        return true;
    }
};

void make_name(test::rng& r, char* out, size_t cap) {
    unsigned n = r.below(30);
    switch (r.below(4)) {
        case 0: snprintf(out, cap, "n%u.bin", n); break;
        case 1: snprintf(out, cap, "Long Name %u.txt", n); break;
        case 2: snprintf(out, cap, "\xC3\xBCn\xC3\xAF %u.dat", n); break;  // "ünï %u.dat"
        default: snprintf(out, cap, "UP%u.TXT", n); break;
    }
}

void path_of(const model_file& f, char* out, size_t cap) { snprintf(out, cap, "%s/%s", dir_paths[f.dir], f.name); }

int random_existing_dir(test::rng& r, const model& m) {
    for (;;) {
        int d = static_cast<int>(r.below(dir_count));
        if (m.dir_exists[d]) return d;
    }
}

int random_file(test::rng& r, const model& m) {
    if (m.count() == 0) return -1;
    for (;;) {
        int i = static_cast<int>(r.below(max_files));
        if (m.files[i].used) return i;
    }
}

bool verify_file(vfat32::vfs& fs, const model_file& f, test::rng& r) {
    char path[128];
    path_of(f, path, sizeof path);
    static uint8_t back[max_file_size + 4096];
    if (r.below(2)) {
        vfat32::file* s = fs.fopen(path, "r");
        if (!s) {
            printf("    cannot open %s: %s\n", path, vfat32::vfs::strerror(fs.last_error()));
            return false;
        }
        if (r.below(3) == 0) fs.setvbuf(s, nullptr, vfat32::io_nbf, 0);
        size_t got = 0;
        for (;;) {
            size_t want = 1 + r.below(3000);
            size_t n = fs.fread(back + got, 1, want, s);
            got += n;
            if (n < want || got > max_file_size) break;
        }
        bool eof_ok = fs.feof(s) != 0;
        fs.fclose(s);
        bool same = got == f.size && (f.size == 0 || memcmp(back, f.data, f.size) == 0);
        if (!eof_ok || !same) {
            size_t at = 0;
            while (at < got && at < f.size && back[at] == f.data[at]) ++at;
            printf("    stream read of %s: eof %d, %lu bytes, model %lu, first difference at %lu\n", path, eof_ok,
                   static_cast<unsigned long>(got), static_cast<unsigned long>(f.size), static_cast<unsigned long>(at));
        }
        return eof_ok && same;
    }
    int fd = fs.open(path, vfat32::o_rdonly);
    if (fd < 0) {
        printf("    cannot open %s: %s\n", path, vfat32::vfs::strerror(fs.last_error()));
        return false;
    }
    vfat32::stat st;
    if (fs.fstat(fd, &st) != 0 || st.st_size != f.size) {
        printf("    %s: size %lu, model %lu\n", path, static_cast<unsigned long>(st.st_size),
               static_cast<unsigned long>(f.size));
        fs.close(fd);
        return false;
    }
    bool ok = true;
    if (f.size > 0) {  // a random window
        uint32_t off = r.below(static_cast<uint32_t>(f.size));
        uint32_t len = 1 + r.below(static_cast<uint32_t>(f.size - off));
        if (fs.lseek(fd, static_cast<long>(off), vfat32::seek_set) != static_cast<long>(off)) ok = false;
        if (ok && fs.read(fd, back, len) != static_cast<long>(len)) ok = false;
        if (ok && memcmp(back, f.data + off, len) != 0) ok = false;
    }
    fs.close(fd);
    if (!ok) printf("    descriptor read of %s (size %lu) differs\n", path, static_cast<unsigned long>(f.size));
    return ok;
}

bool verify_all(vfat32::vfs& fs, model& m, test::rng& r) {
    for (int i = 0; i < max_files; ++i)
        if (m.files[i].used && !verify_file(fs, m.files[i], r)) {
            char path[128];
            path_of(m.files[i], path, sizeof path);
            printf("    content mismatch: %s\n", path);
            return false;
        }
    for (int d = 0; d < dir_count; ++d) {
        const char* p = d == 0 ? "/" : dir_paths[d];
        if (!m.dir_exists[d]) {
            if (test::exists(fs, p)) {
                printf("    directory should not exist: %s\n", p);
                return false;
            }
            continue;
        }
        int expected = 0;
        for (int i = 0; i < max_files; ++i) expected += (m.files[i].used && m.files[i].dir == d) ? 1 : 0;
        for (int k = 0; k < dir_count; ++k) expected += (m.dir_exists[k] && dir_parent[k] == d) ? 1 : 0;
        vfat32::dir* dh = fs.opendir(p);
        if (!dh) return false;
        int seen = 0;
        bool known = true;
        while (vfat32::dirent* e = fs.readdir(dh)) {
            ++seen;
            bool hit = m.find(d, e->d_name) >= 0;
            for (int k = 0; k < dir_count && !hit; ++k) {
                if (!m.dir_exists[k] || dir_parent[k] != d) continue;
                const char* base = strrchr(dir_paths[k], '/') + 1;
                if (strcmp(base, e->d_name) == 0) hit = true;
            }
            if (!hit) {
                printf("    unexpected entry \"%s\" in %s\n", e->d_name, p);
                known = false;
            }
        }
        fs.closedir(dh);
        if (seen != expected || !known) {
            printf("    %s: %d entries, model has %d\n", p, seen, expected);
            return false;
        }
    }
    return true;
}

// Writes `len` bytes at `off` through a random API and buffering, and applies it to the model.
bool do_write(vfat32::vfs& fs, model_file& f, size_t off, size_t len, bool append_mode, test::rng& r) {
    static uint8_t buf[max_file_size];
    r.fill(buf, len);
    char path[128];
    path_of(f, path, sizeof path);
    bool ok = true;
    if (r.below(2)) {
        vfat32::file* s = fs.fopen(path, append_mode ? "a" : "r+");
        if (!s) return false;
        uint32_t mode = r.below(4);
        switch (mode) {
            case 0: fs.setvbuf(s, nullptr, vfat32::io_nbf, 0); break;
            case 1: fs.setvbuf(s, nullptr, vfat32::io_lbf, 0); break;
            default: break;
        }
        if (trace_enabled())
            printf("      stream %s %s off %lu len %lu (size %lu) buffering %s\n", append_mode ? "a" : "r+", path,
                   static_cast<unsigned long>(off), static_cast<unsigned long>(len),
                   static_cast<unsigned long>(f.size), mode == 0 ? "none" : mode == 1 ? "line" : "full");
        if (!append_mode && fs.fseek(s, static_cast<long>(off), vfat32::seek_set) != 0) ok = false;
        size_t done = 0;
        while (ok && done < len) {
            size_t c = 1 + r.below(static_cast<uint32_t>(len - done < 2000 ? len - done : 2000));
            if (fs.fwrite(buf + done, 1, c, s) != c) ok = false;
            done += c;
        }
        if (fs.fclose(s) != 0) ok = false;
    } else {
        int fd = fs.open(path, append_mode ? (vfat32::o_wronly | vfat32::o_append) : vfat32::o_rdwr);
        if (fd < 0) return false;
        if (trace_enabled())
            printf("      fd %s %s off %lu len %lu (size %lu)\n", append_mode ? "append" : "rdwr", path,
                   static_cast<unsigned long>(off), static_cast<unsigned long>(len),
                   static_cast<unsigned long>(f.size));
        if (!append_mode && fs.lseek(fd, static_cast<long>(off), vfat32::seek_set) != static_cast<long>(off)) ok = false;
        if (ok && len && fs.write(fd, buf, len) != static_cast<long>(len)) ok = false;
        if (fs.close(fd) != 0) ok = false;
    }
    if (len == 0) return ok;  // a zero-length write changes nothing, even past the end
    if (append_mode) off = f.size;
    if (off > f.size) memset(f.data + f.size, 0, off - f.size);
    memcpy(f.data + off, buf, len);
    if (off + len > f.size) f.size = off + len;
    return ok;
}

bool run_seed(uint64_t seed, int ops) {
    volume v(4 * test::one_mib);
    if (!v.ok()) return false;
    vfat32::vfs& fs = v.fs;
    model m;
    test::rng r(seed);
    char path[128];
    char path2[128];
    for (int op = 0; op < ops; ++op) {
        uint32_t kind = r.below(100);
        bool ok = true;
        const char* what = "";
        if (kind < 14) {  // create
            what = "create";
            int slot = m.free_slot();
            if (slot < 0) continue;
            int d = random_existing_dir(r, m);
            char name[48];
            make_name(r, name, sizeof name);
            if (m.find(d, name) >= 0) continue;
            model_file& f = m.files[slot];
            f.dir = d;
            snprintf(f.name, sizeof f.name, "%s", name);
            f.size = 0;
            path_of(f, path, sizeof path);
            if (r.below(2)) {
                vfat32::file* s = fs.fopen(path, "wx");
                ok = s != nullptr && fs.fclose(s) == 0;
            } else {
                int fd = fs.open(path, vfat32::o_rdwr | vfat32::o_creat | vfat32::o_excl);
                ok = fd >= 0 && fs.close(fd) == 0;
            }
            f.used = true;
            size_t len = r.below(4000);
            if (ok && len) ok = do_write(fs, f, 0, len, false, r);
        } else if (kind < 34) {  // write at a random offset (possibly past the end)
            what = "write";
            int i = random_file(r, m);
            if (i < 0) continue;
            model_file& f = m.files[i];
            size_t off = r.below(static_cast<uint32_t>(f.size + 2000));
            if (off >= max_file_size) continue;
            size_t len = r.below(static_cast<uint32_t>(max_file_size - off < 3000 ? max_file_size - off : 3000));
            ok = do_write(fs, f, off, len, false, r);
        } else if (kind < 44) {  // append
            what = "append";
            int i = random_file(r, m);
            if (i < 0) continue;
            model_file& f = m.files[i];
            if (f.size >= max_file_size) continue;
            size_t room = max_file_size - f.size;
            size_t len = r.below(static_cast<uint32_t>(room < 2500 ? room : 2500));
            ok = do_write(fs, f, 0, len, true, r);
        } else if (kind < 52) {  // truncate
            what = "truncate";
            int i = random_file(r, m);
            if (i < 0) continue;
            model_file& f = m.files[i];
            size_t len = r.below(static_cast<uint32_t>(max_file_size / 2));
            path_of(f, path, sizeof path);
            if (r.below(2)) {
                ok = fs.truncate(path, static_cast<long>(len)) == 0;
            } else {
                int fd = fs.open(path, vfat32::o_wronly);
                ok = fd >= 0 && fs.ftruncate(fd, static_cast<long>(len)) == 0;
                if (fd >= 0 && fs.close(fd) != 0) ok = false;
            }
            if (trace_enabled())
                printf("      truncate %s to %lu (size %lu)\n", path, static_cast<unsigned long>(len),
                       static_cast<unsigned long>(f.size));
            if (len > f.size) memset(f.data + f.size, 0, len - f.size);
            f.size = len;
        } else if (kind < 66) {  // read and verify
            what = "read";
            int i = random_file(r, m);
            if (i < 0) continue;
            ok = verify_file(fs, m.files[i], r);
        } else if (kind < 73) {  // rename (sometimes over an existing file)
            what = "rename";
            int i = random_file(r, m);
            if (i < 0) continue;
            model_file& f = m.files[i];
            int d = random_existing_dir(r, m);
            char name[48];
            make_name(r, name, sizeof name);
            int target = m.find(d, name);
            if (target == i) continue;
            path_of(f, path, sizeof path);
            snprintf(path2, sizeof path2, "%s/%s", dir_paths[d], name);
            ok = fs.rename(path, path2) == 0;
            if (trace_enabled()) printf("      rename %s -> %s%s\n", path, path2, target >= 0 ? " (replace)" : "");
            if (target >= 0) m.files[target].used = false;
            f.dir = d;
            snprintf(f.name, sizeof f.name, "%s", name);
        } else if (kind < 79) {  // remove
            what = "remove";
            int i = random_file(r, m);
            if (i < 0) continue;
            path_of(m.files[i], path, sizeof path);
            ok = fs.remove(path) == 0;
            m.files[i].used = false;
        } else if (kind < 85) {  // mkdir
            what = "mkdir";
            int d = 1 + static_cast<int>(r.below(dir_count - 1));
            if (m.dir_exists[d]) {
                ok = fs.mkdir(dir_paths[d]) == -1 && fs.last_error() == vfat32::error::exists;
            } else if (!m.dir_exists[dir_parent[d]]) {
                ok = fs.mkdir(dir_paths[d]) == -1 && fs.last_error() == vfat32::error::not_found;
            } else {
                ok = fs.mkdir(dir_paths[d]) == 0;
                m.dir_exists[d] = true;
            }
        } else if (kind < 90) {  // rmdir
            what = "rmdir";
            int d = 1 + static_cast<int>(r.below(dir_count - 1));
            if (!m.dir_exists[d]) continue;
            if (m.dir_empty(d)) {
                ok = fs.rmdir(dir_paths[d]) == 0;
                m.dir_exists[d] = false;
            } else {
                ok = fs.rmdir(dir_paths[d]) == -1 && fs.last_error() == vfat32::error::not_empty;
            }
        } else if (kind < 95) {  // a few defrag steps
            what = "defrag";
            int rc = fs.defrag(1 + r.below(64));
            ok = rc == 0 || rc == 1;
        } else if (kind < 97) {  // remount (reuse)
            what = "remount";
            ok = fs.umount() == 0 && fs.mount(v.mem, v.size, vfat32::mount_mode::reuse) == 0 && !fs.was_dirty();
        } else {
            what = "verify_all";
            ok = verify_all(fs, m, r);
        }
        if (trace_enabled()) {
            printf("    op %d %s\n", op, what);
            for (int i = 0; ok && i < max_files; ++i) {
                if (!m.files[i].used) continue;
                path_of(m.files[i], path, sizeof path);
                long sz = test::file_size(fs, path);
                if (sz != static_cast<long>(m.files[i].size)) {
                    printf("    after op %d: %s size %ld, model %lu\n", op, path, sz,
                           static_cast<unsigned long>(m.files[i].size));
                    ok = false;
                }
            }
        }
        if (ok && op % 50 == 49) {
            what = "check";
            ok = test::check_clean(fs);
        }
        if (!ok) {
            printf("    seed %llu op %d (%s) failed: %s\n", static_cast<unsigned long long>(seed), op, what,
                   vfat32::vfs::strerror(fs.last_error()));
            return false;
        }
    }
    if (!verify_all(fs, m, r) || !test::check_clean(fs)) {
        printf("    seed %llu: final verification failed\n", static_cast<unsigned long long>(seed));
        return false;
    }
    if (fs.defrag() != 0 || !verify_all(fs, m, r)) {
        printf("    seed %llu: final defrag failed\n", static_cast<unsigned long long>(seed));
        return false;
    }
    vfat32::check_report rep;
    test::clear_report(rep);
    if (fs.check(&rep) != 0 || rep.fragmented != 0 || rep.free_extents > 1) {
        test::print_report(rep);
        return false;
    }
    return true;
}

}  // namespace

TEST(random_model_stress) {
    const int seeds = test::long_mode() ? 64 : 4;
    const int ops = test::long_mode() ? 20000 : 2500;
    for (int s = 0; s < seeds; ++s) {
        uint64_t seed = 0xC0FFEE00ULL + static_cast<uint64_t>(s) * 7919;
        if (!run_seed(seed, ops)) {
            CHECK(false);
            return;
        }
    }
}
