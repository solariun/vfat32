// vfat32 tests — thread safety (spec §10): a recursive pthread mutex as vfat32::lock_interface, many threads on one
// volume, atomic groups with flockfile/funlockfile (T3), last_error read inside the group (T4), lock balance (T2, T7).

#include <pthread.h>

#include "test_harness.hpp"

using test::volume;

namespace {

class recursive_lock : public vfat32::lock_interface {
public:
    recursive_lock() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    ~recursive_lock() { pthread_mutex_destroy(&mutex_); }
    void lock() override { pthread_mutex_lock(&mutex_); }
    void unlock() override { pthread_mutex_unlock(&mutex_); }

private:
    pthread_mutex_t mutex_;
};

// Single-threaded probe: counts calls and nesting depth.
class counting_lock : public vfat32::lock_interface {
public:
    counting_lock() : locks(0), unlocks(0), depth(0), max_depth(0) {}
    void lock() override {
        ++locks;
        ++depth;
        if (depth > max_depth) max_depth = depth;
    }
    void unlock() override {
        ++unlocks;
        --depth;
    }
    long locks;
    long unlocks;
    int depth;
    int max_depth;
};

counting_lock* clock_probe = nullptr;
bool clock_called_unlocked = false;
vfat32::timestamp probing_clock() {
    if (clock_probe && clock_probe->depth <= 0) clock_called_unlocked = true;
    vfat32::timestamp t;
    t.year = 2026;
    t.month = 10;
    t.day = 28;
    t.hour = 12;
    t.minute = 0;
    t.second = 0;
    return t;
}

// Each worker holds at most one row at a time, plus the shared log's row.
const int thread_count = VFAT32_MAX_OPEN_FILES - 1 < 4 ? VFAT32_MAX_OPEN_FILES - 1 : 4;
const int thread_slots = thread_count > 0 ? thread_count : 1;
const int iterations = 120;
const int files_per_thread = 5;

struct worker_state {
    vfat32::vfs* fs;
    vfat32::file* log;
    int id;
    int failures;
    size_t final_size[files_per_thread];
    uint8_t final_data[files_per_thread][1500];
    bool final_exists[files_per_thread];
};

void* worker(void* arg) {
    worker_state* s = static_cast<worker_state*>(arg);
    vfat32::vfs& fs = *s->fs;
    test::rng r(static_cast<uint64_t>(s->id) * 1000003 + 17);
    char path[64];
    uint8_t data[1500];
    uint8_t back[1600];
    for (int i = 0; i < iterations; ++i) {
        int k = i % files_per_thread;
        snprintf(path, sizeof path, "/t%d/file number %d.bin", s->id, k);
        size_t len = 1 + r.below(sizeof data);
        r.fill(data, len);
        vfat32::file* f = fs.fopen(path, "w");
        if (!f) {
            ++s->failures;
            continue;
        }
        if (fs.fwrite(data, 1, len, f) != len) ++s->failures;
        if (fs.fclose(f) != 0) ++s->failures;
        int fd = fs.open(path, vfat32::o_rdonly);
        if (fd < 0) {
            ++s->failures;
            continue;
        }
        long got = fs.read(fd, back, sizeof back);
        if (got != static_cast<long>(len) || memcmp(back, data, len) != 0) ++s->failures;
        fs.close(fd);
        memcpy(s->final_data[k], data, len);
        s->final_size[k] = len;
        s->final_exists[k] = true;
        if (i % 7 == 6) {
            if (fs.remove(path) != 0) ++s->failures;
            s->final_exists[k] = false;
        }
        // T3: two calls that must not be interleaved with other threads' lines
        fs.flockfile(s->log);
        fs.fprintf(s->log, "T%d-", s->id);
        fs.fprintf(s->log, "%04d\n", i);
        // T4: the error of a failing call, read inside the same group
        if (fs.fopen("/no/such/file", "r") != nullptr || fs.last_error() != vfat32::error::not_found) ++s->failures;
        fs.funlockfile(s->log);
        vfat32::stat st;
        if (fs.stat("/", &st) != 0) ++s->failures;
        if (i % 20 == 0 && VFAT32_MAX_OPEN_DIRS >= thread_count) {  // each thread may hold a dir row
            snprintf(path, sizeof path, "/t%d", s->id);
            vfat32::dir* d = fs.opendir(path);
            if (d) {
                while (fs.readdir(d)) {
                }
                fs.closedir(d);
            } else {
                ++s->failures;
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST(threads_shared_volume_with_recursive_lock) {
    if (thread_count < 1) {
        printf("    SKIP: needs VFAT32_MAX_OPEN_FILES >= 2\n");
        return;
    }
    recursive_lock lk;  // outlives the volume
    volume v(4 * test::one_mib);
    REQUIRE(v.ok());
    v.fs.set_lock(&lk);
    char path[32];
    for (int t = 0; t < thread_count; ++t) {
        snprintf(path, sizeof path, "/t%d", t);
        REQUIRE_EQ(v.fs.mkdir(path), 0);
    }
    vfat32::file* log = v.fs.fopen("/shared.log", "a");
    REQUIRE(log);
    static worker_state states[thread_slots];
    pthread_t threads[thread_slots];
    for (int t = 0; t < thread_count; ++t) {
        memset(&states[t], 0, sizeof states[t]);
        states[t].fs = &v.fs;
        states[t].log = log;
        states[t].id = t;
        REQUIRE_EQ(pthread_create(&threads[t], nullptr, worker, &states[t]), 0);
    }
    for (int t = 0; t < thread_count; ++t) pthread_join(threads[t], nullptr);
    CHECK_EQ(v.fs.fclose(log), 0);
    for (int t = 0; t < thread_count; ++t) CHECK_EQ(states[t].failures, 0);
    CHECK(test::check_clean(v.fs));
    // every line intact, every thread's sequence complete and in order
    vfat32::file* f = v.fs.fopen("/shared.log", "r");
    REQUIRE(f);
    int next[thread_slots] = {0};
    int lines = 0;
    int bad = 0;
    char line[64];
    while (v.fs.fgets(line, sizeof line, f)) {
        int id = -1;
        int seq = -1;
        char tail = 0;
        if (sscanf(line, "T%d-%4d%c", &id, &seq, &tail) != 3 || tail != '\n' || id < 0 || id >= thread_count ||
            seq != next[id]) {
            ++bad;
        } else {
            ++next[id];
        }
        ++lines;
    }
    v.fs.fclose(f);
    CHECK_EQ(bad, 0);
    CHECK_EQ(lines, thread_count * iterations);
    for (int t = 0; t < thread_count; ++t) CHECK_EQ(next[t], iterations);
    for (int t = 0; t < thread_count; ++t)
        for (int k = 0; k < files_per_thread; ++k) {
            if (snprintf(path, sizeof path, "/t%d/file number %d.bin", t, k) >= static_cast<int>(sizeof path)) continue;
            if (states[t].final_exists[k])
                CHECK(test::file_equals(v.fs, path, states[t].final_data[k], states[t].final_size[k]));
            else
                CHECK(!test::exists(v.fs, path));
        }
    v.fs.set_lock(nullptr);
}

// T2 / T3 / T7: every call is balanced, flockfile nests, the clock runs under the lock.
TEST(threads_lock_balance_t2_t7) {
    counting_lock lk;  // outlives the volume
    volume v(test::one_mib);
    REQUIRE(v.ok());
    clock_probe = &lk;
    clock_called_unlocked = false;
    v.fs.set_lock(&lk);
    v.fs.set_clock(probing_clock);
    CHECK(test::write_text(v.fs, "/a.txt", "a"));
    CHECK(lk.locks > 0);
    CHECK_EQ(lk.locks, lk.unlocks);
    CHECK_EQ(lk.depth, 0);
    long before = lk.locks;
    CHECK(v.fs.fopen("/missing", "r") == nullptr);  // error path
    CHECK_EQ(v.fs.mkdir("/a.txt/x"), -1);
    CHECK_EQ(v.fs.close(-5), -1);
    CHECK(v.fs.readdir(nullptr) == nullptr);
    CHECK(lk.locks > before);
    CHECK_EQ(lk.locks, lk.unlocks);
    CHECK_EQ(lk.depth, 0);
    vfat32::file* f = v.fs.fopen("/a.txt", "r+");
    REQUIRE(f);
    v.fs.flockfile(f);
    CHECK_EQ(lk.depth, 1);
    CHECK(v.fs.fputs("b", f) >= 0);
    CHECK(lk.max_depth >= 2);  // the call nested inside the group
    v.fs.flockfile(f);
    CHECK_EQ(lk.depth, 2);
    v.fs.funlockfile(f);
    v.fs.funlockfile(f);
    CHECK_EQ(lk.depth, 0);
    CHECK_EQ(v.fs.fclose(f), 0);
    vfat32::check_report r;
    test::clear_report(r);
    CHECK_EQ(v.fs.check(&r), 0);
    CHECK_EQ(v.fs.defrag(), 0);
    vfat32::statvfs s;
    CHECK_EQ(v.fs.statvfs(&s), 0);
    CHECK_EQ(lk.locks, lk.unlocks);
    CHECK_EQ(lk.depth, 0);
    CHECK(!clock_called_unlocked);  // T7
    long quiet = lk.locks;
    CHECK(vfat32::vfs::strerror(vfat32::error::busy) != nullptr);  // T2: no lock
    CHECK(!vfat32::vfs::needs_format(v.mem, v.size));
    v.fs.set_clock(nullptr);
    CHECK_EQ(lk.locks, quiet);
    CHECK_EQ(v.fs.umount(), 0);
    CHECK_EQ(lk.locks, lk.unlocks);
    v.fs.set_lock(nullptr);
    clock_probe = nullptr;
}
