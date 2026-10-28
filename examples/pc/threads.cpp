// threads — sharing one volume between threads (spec §10, README "Thread safety").
//
// 1. A recursive pthread mutex is wrapped in vfat32::lock_interface and installed
//    with set_lock() before any thread starts: every vfat32 call is now atomic.
// 2. Four workers each write their own file, and all of them append two-line
//    records to one shared log. flockfile()/funlockfile() keep each record's
//    two fputs calls together (the lock is recursive, so the calls inside the
//    group lock it again without deadlock).
// 3. last_error() belongs to the volume, so a worker reads it inside the same
//    locked group as the call that failed.
// 4. At the end check() proves the volume is consistent and the log is verified.
//
// Build: c++ -std=c++11 -Isrc examples/pc/threads.cpp src/*.cpp -lpthread -o threads

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "vfat32.hpp"

namespace {

const size_t volume_size = 4 * 1024 * 1024;
uint8_t volume[volume_size];

const int worker_count = 4;
const int records_per_worker = 500;
const int line_size = 96;

class pthread_recursive_lock : public vfat32::lock_interface {
public:
    pthread_recursive_lock() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);  // required: spec T3
        pthread_mutex_init(&mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    ~pthread_recursive_lock() { pthread_mutex_destroy(&mutex_); }
    void lock() override { pthread_mutex_lock(&mutex_); }
    void unlock() override { pthread_mutex_unlock(&mutex_); }

private:
    pthread_mutex_t mutex_;
};

vfat32::vfs fs;
pthread_recursive_lock volume_lock;

struct worker_args {
    int id;
    int failures;
};

void* worker(void* arg) {
    worker_args* w = static_cast<worker_args*>(arg);
    char path[32];
    snprintf(path, sizeof path, "/worker_%d.txt", w->id);

    vfat32::file* own = fs.fopen(path, "w");  // one handle per thread: no sharing of positions
    vfat32::file* shared = fs.fopen("/shared.log", "a");
    if (!own || !shared) {
        w->failures++;
        return nullptr;
    }

    char line[line_size];
    for (int i = 0; i < records_per_worker; ++i) {
        fs.fprintf(own, "worker %d line %d\n", w->id, i);

        // A two-line record: without the group, another worker's line could land between them.
        fs.flockfile(shared);
        snprintf(line, sizeof line, "begin %d %d\n", w->id, i);
        int a = fs.fputs(line, shared);
        snprintf(line, sizeof line, "end   %d %d\n", w->id, i);
        int b = fs.fputs(line, shared);
        if (a == vfat32::eof || b == vfat32::eof) {
            // Read the error while still holding the lock: no other thread can overwrite it (spec T4).
            printf("worker %d: fputs failed: %s\n", w->id, vfat32::vfs::strerror(fs.last_error()));
            w->failures++;
        }
        fs.fflush(shared);  // the record reaches the file before the group ends
        fs.funlockfile(shared);
    }
    fs.fclose(shared);
    fs.fclose(own);
    return nullptr;
}

// Every "begin w i" must be followed directly by "end   w i".
bool verify_log() {
    vfat32::file* f = fs.fopen("/shared.log", "r");
    if (!f) return false;
    char a[line_size];
    char b[line_size];
    int records = 0;
    bool ok = true;
    while (fs.fgets(a, line_size, f)) {
        if (!fs.fgets(b, line_size, f)) {
            ok = false;
            break;
        }
        int wa, ia, wb, ib;
        if (sscanf(a, "begin %d %d", &wa, &ia) != 2 || sscanf(b, "end %d %d", &wb, &ib) != 2 || wa != wb
            || ia != ib) {
            ok = false;
            break;
        }
        ++records;
    }
    fs.fclose(f);
    printf("shared log: %d records, %s\n", records, ok ? "every record intact" : "INTERLEAVED");
    return ok && records == worker_count * records_per_worker;
}

}  // namespace

int main() {
    if (fs.mount(volume, volume_size, vfat32::mount_mode::format) != 0) return 1;
    fs.set_lock(&volume_lock);  // before the volume is shared (spec T1)

    pthread_t threads[worker_count];
    worker_args args[worker_count];
    for (int i = 0; i < worker_count; ++i) {
        args[i].id = i;
        args[i].failures = 0;
        pthread_create(&threads[i], nullptr, worker, &args[i]);
    }
    int failures = 0;
    for (int i = 0; i < worker_count; ++i) {
        pthread_join(threads[i], nullptr);
        failures += args[i].failures;
    }
    printf("%d workers x %d records done, %d failures\n", worker_count, records_per_worker, failures);

    bool ok = verify_log() && failures == 0;

    vfat32::check_report report;
    int clean = fs.check(&report);
    printf("check: %u files, %u errors -> %s\n", static_cast<unsigned>(report.files),
           static_cast<unsigned>(report.errors()), clean == 0 ? "clean" : "NOT clean");

    fs.set_lock(nullptr);
    fs.umount();
    return ok && clean == 0 ? 0 : 1;
}
