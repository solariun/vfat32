// vfat32 — an in-memory FAT32 file system with the C standard file API.
// C++11, no STL, no exceptions, no RTTI; runs on PCs and microcontrollers.
// Normative specification: spec.md. Manual: README.md.
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#ifndef VFAT32_HPP
#define VFAT32_HPP

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// avr-libc defines feof(s) and ferror(s) as function-like macros (it also has the real functions); they would
// rewrite the members of the same names (spec D16).
#ifdef feof
#undef feof
#endif
#ifdef ferror
#undef ferror
#endif

// ── Configuration (spec §13): override with -D on the compiler command line ──
#if defined(__AVR__)
#ifndef VFAT32_MAX_OPEN_FILES
#define VFAT32_MAX_OPEN_FILES 2
#endif
#ifndef VFAT32_MAX_OPEN_DIRS
#define VFAT32_MAX_OPEN_DIRS 1
#endif
#ifndef VFAT32_BUFFER_SIZE
#define VFAT32_BUFFER_SIZE 32
#endif
#ifndef VFAT32_NAME_MAX
#define VFAT32_NAME_MAX 32
#endif
#ifndef VFAT32_PRINTF_MAX
#define VFAT32_PRINTF_MAX 64
#endif
#endif

#ifndef VFAT32_MAX_OPEN_FILES
#define VFAT32_MAX_OPEN_FILES 8
#endif
#ifndef VFAT32_MAX_OPEN_DIRS
#define VFAT32_MAX_OPEN_DIRS 4
#endif
#ifndef VFAT32_BUFFER_SIZE
#define VFAT32_BUFFER_SIZE 512
#endif
#ifndef VFAT32_NAME_MAX
#define VFAT32_NAME_MAX 255
#endif
#ifndef VFAT32_PRINTF_MAX
#define VFAT32_PRINTF_MAX 256
#endif
#ifndef VFAT32_USE_HEAP
#define VFAT32_USE_HEAP 1
#endif

namespace vfat32 {

static_assert(VFAT32_MAX_OPEN_FILES >= 1 && VFAT32_MAX_OPEN_FILES <= 255, "VFAT32_MAX_OPEN_FILES must be 1..255");
static_assert(VFAT32_MAX_OPEN_DIRS >= 1 && VFAT32_MAX_OPEN_DIRS <= 255, "VFAT32_MAX_OPEN_DIRS must be 1..255");
static_assert(VFAT32_BUFFER_SIZE >= 0, "VFAT32_BUFFER_SIZE must not be negative");
static_assert(VFAT32_NAME_MAX >= 12 && VFAT32_NAME_MAX <= 1020, "VFAT32_NAME_MAX must be 12..1020");
static_assert(VFAT32_PRINTF_MAX >= 16, "VFAT32_PRINTF_MAX must be at least 16");

// ── Constants (spec D16: vfat32 names, never the host macros) ──
constexpr int eof = -1;

constexpr int seek_set = 0;
constexpr int seek_cur = 1;
constexpr int seek_end = 2;
#if defined(SEEK_SET) && defined(SEEK_CUR) && defined(SEEK_END)
static_assert(seek_set == SEEK_SET && seek_cur == SEEK_CUR && seek_end == SEEK_END, "host SEEK_* differ from 0/1/2");
#endif

constexpr int io_fbf = 0;
constexpr int io_lbf = 1;
constexpr int io_nbf = 2;
constexpr size_t bufsiz = VFAT32_BUFFER_SIZE > 0 ? VFAT32_BUFFER_SIZE : 512;

constexpr int o_rdonly = 0x0000;
constexpr int o_wronly = 0x0001;
constexpr int o_rdwr = 0x0002;
constexpr int o_accmode = 0x0003;
constexpr int o_creat = 0x0100;
constexpr int o_excl = 0x0200;
constexpr int o_trunc = 0x0400;
constexpr int o_append = 0x0800;

constexpr uint32_t s_ifmt = 0170000;
constexpr uint32_t s_ifdir = 0040000;
constexpr uint32_t s_ifreg = 0100000;
inline bool s_isdir(uint32_t mode) { return (mode & s_ifmt) == s_ifdir; }
inline bool s_isreg(uint32_t mode) { return (mode & s_ifmt) == s_ifreg; }

constexpr uint8_t dt_unknown = 0;
constexpr uint8_t dt_dir = 4;
constexpr uint8_t dt_reg = 8;

constexpr uint8_t attr_read_only = 0x01;
constexpr uint8_t attr_hidden = 0x02;
constexpr uint8_t attr_system = 0x04;
constexpr uint8_t attr_volume_id = 0x08;
constexpr uint8_t attr_directory = 0x10;
constexpr uint8_t attr_archive = 0x20;

// ── Errors (spec §9) ──
enum class error : uint8_t {
    none = 0,
    not_found,
    exists,
    no_space,
    bad_handle,
    invalid,
    is_directory,
    not_directory,
    not_empty,
    busy,
    too_many_open,
    name_too_long,
    access,
    file_too_big,
    overflow,
    not_mounted,
    no_memory,
    no_filesystem,
    corrupt
};

enum class mount_mode : uint8_t {
    reuse,            // the memory must already hold a complete FAT32 (spec §5.1)
    format,           // always format
    format_if_needed  // keep a complete FAT32, format anything else
};

struct timestamp {
    uint16_t year;   // 1980..2107
    uint8_t month;   // 1..12
    uint8_t day;     // 1..31
    uint8_t hour;    // 0..23
    uint8_t minute;  // 0..59
    uint8_t second;  // 0..59
};

// Returns the current local time; called with the volume lock held (spec T7).
typedef timestamp (*clock_function)();

struct format_options {
    uint16_t bytes_per_sector;    // 512, 1024, 2048 or 4096
    uint8_t sectors_per_cluster;  // 0 = automatic (spec F2), else a power of two, cluster <= 32 KiB
    uint8_t fat_copies;           // 1 or 2
    uint16_t reserved_sectors;    // 0 = automatic (spec F3), else >= 2
    uint32_t volume_id;           // 0 = derived from the clock
    char volume_label[12];        // up to 11 characters, NUL-terminated

    format_options();
};

struct stat {
    uint32_t st_mode;  // s_ifdir or s_ifreg plus permission bits
    uint32_t st_size;
    uint8_t st_attr;   // FAT attributes (attr_*)
    timestamp st_modified;
    timestamp st_created;
    timestamp st_accessed;  // date only
};

struct statvfs {
    uint32_t f_bsize;    // cluster size in bytes
    uint32_t f_frsize;   // cluster size in bytes
    uint32_t f_blocks;   // clusters in the data region
    uint32_t f_bfree;    // free clusters
    uint32_t f_bavail;   // free clusters
    uint32_t f_namemax;  // VFAT32_NAME_MAX
};

struct dirent {
    char d_name[VFAT32_NAME_MAX + 1];  // UTF-8 (spec N7)
    uint8_t d_type;                    // dt_dir or dt_reg
    uint32_t d_size;                   // bytes (0 for directories)
};

struct fpos_t {
    uint32_t position;
};

struct check_report {
    uint32_t files;
    uint32_t directories;
    uint32_t used_clusters;
    uint32_t free_clusters;
    uint32_t bad_clusters;
    uint32_t free_extents;  // runs of free clusters
    uint32_t fragmented;    // chains that are not one run of clusters
    // errors (spec §11.1)
    uint32_t reserved_bits;
    uint32_t bad_chains;
    uint32_t cross_links;
    uint32_t size_errors;
    uint32_t bad_entries;
    uint32_t lost_clusters;
    uint32_t fat_mismatches;
    uint32_t fsinfo_errors;
    uint32_t repaired;

    uint32_t errors() const;
};

// The developer's lock (spec §10): must be recursive.
class lock_interface {
public:
    virtual void lock() = 0;
    virtual void unlock() = 0;

protected:
    ~lock_interface() {}
};

class vfs;

// A handle row: `file*` for streams, its index for descriptors (spec §7.1). Opaque to the user.
class file {
public:
    file();

private:
    friend class vfs;
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    size_t entry_;           // byte offset of the 8.3 entry in the volume; 0 = row free
    uint32_t pos_;           // logical position
    uint32_t cache_index_;   // chain index of cache_cluster_ (no_cache = invalid)
    uint32_t cache_cluster_;
    uint32_t buf_start_;     // file offset of buf_[0]
    uint8_t* buf_;
    size_t buf_size_;
    size_t buf_len_;         // reading: valid bytes; writing: pending bytes
    int16_t unget_;          // pushed-back character or -1
    uint8_t buf_state_;      // idle, reading, writing
    uint8_t buf_mode_;       // io_fbf, io_lbf, io_nbf
    uint8_t flags_;          // access and status flags
#if VFAT32_BUFFER_SIZE > 0
    uint8_t storage_[VFAT32_BUFFER_SIZE];
#endif
};

// A directory row (spec K8). Opaque to the user.
class dir {
public:
    dir();

private:
    friend class vfs;
    dir(const dir&) = delete;
    dir& operator=(const dir&) = delete;

    uint32_t first_;    // first cluster of the directory; 0 = row free
    uint32_t cluster_;  // current cluster (0 = end)
    uint32_t index_;    // slot index inside cluster_
    uint32_t slot_;     // slot index inside the directory
    uint32_t steps_;    // clusters visited (loop guard)
    dirent entry_;
};

class vfs {
public:
    vfs();
    ~vfs();

    // ── Volume (spec §5) ──
    static bool needs_format(const void* memory, size_t size);
    int mount(void* memory, size_t size, mount_mode mode, const format_options* options = nullptr);
#if VFAT32_USE_HEAP
    int mount(size_t size, const format_options* options = nullptr);
#endif
    int umount();
    int format(const format_options* options = nullptr);
    bool mounted() const;
    bool was_dirty() const;
    int sync();
    int statvfs(struct statvfs* buf);
    int check(check_report* report, bool repair = false);
    int defrag(uint32_t max_moves = 0);

    // ── Errors and hooks (spec §9, §10, §4.8) ──
    error last_error() const;
    static const char* strerror(error e);
    void set_lock(lock_interface* lock);
    void set_clock(clock_function clock);
    void flockfile(file* stream);
    void funlockfile(file* stream);

    // ── Streams, buffered (spec §7.3) ──
    file* fopen(const char* path, const char* mode);
    int fclose(file* stream);
    size_t fread(void* ptr, size_t size, size_t nmemb, file* stream);
    size_t fwrite(const void* ptr, size_t size, size_t nmemb, file* stream);
    int fgetc(file* stream);
    int fputc(int c, file* stream);
    int ungetc(int c, file* stream);
    char* fgets(char* s, int n, file* stream);
    int fputs(const char* s, file* stream);
    int fprintf(file* stream, const char* format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    int vfprintf(file* stream, const char* format, va_list args);
    int fseek(file* stream, long offset, int whence);
    long ftell(file* stream);
    void rewind(file* stream);
    int fgetpos(file* stream, fpos_t* pos);
    int fsetpos(file* stream, const fpos_t* pos);
    int fflush(file* stream);
    int feof(file* stream);
    int ferror(file* stream);
    void clearerr(file* stream);
    int setvbuf(file* stream, char* buf, int mode, size_t size);
    void setbuf(file* stream, char* buf);
    int fileno(file* stream);

    // ── Descriptors, unbuffered (spec §7.4) ──
    int open(const char* path, int flags);
    int close(int fd);
    long read(int fd, void* buf, size_t count);
    long write(int fd, const void* buf, size_t count);
    long lseek(int fd, long offset, int whence);
    int fsync(int fd);
    int ftruncate(int fd, long length);
    int fstat(int fd, struct stat* buf);

    // ── Paths and directories (spec §8) ──
    int stat(const char* path, struct stat* buf);
    int remove(const char* path);
    int unlink(const char* path);
    int rename(const char* old_path, const char* new_path);
    int truncate(const char* path, long length);
    int mkdir(const char* path);
    int rmdir(const char* path);
    dir* opendir(const char* path);
    dirent* readdir(dir* dirp);
    void rewinddir(dir* dirp);
    int closedir(dir* dirp);

private:
    vfs(const vfs&) = delete;
    vfs& operator=(const vfs&) = delete;

    class guard;
    friend class guard;

    // Internal types (defined in the .cpp files)
    struct slot_iter {
        uint32_t first;    // first cluster of the directory
        uint32_t cluster;  // current cluster, 0 = end
        uint32_t index;    // slot index inside the cluster
        uint32_t slot;     // slot index inside the directory
        uint32_t steps;    // clusters visited (loop guard)
    };
    struct found_entry {
        uint32_t dir;         // first cluster of the containing directory
        uint32_t first_slot;  // first slot of the entry (LFN start or the 8.3 slot)
        uint32_t slot;        // the 8.3 slot
        size_t offset;        // byte offset of the 8.3 slot in the volume
    };
    struct lookup {
        uint32_t parent;       // first cluster of the last component's directory
        bool is_root;          // the path names the root
        bool found;            // the last component exists
        bool trailing_slash;
        found_entry entry;     // valid when found && !is_root
        const char* name;      // last component in the path
        size_t name_len;
    };
    struct referrer {
        uint8_t kind;  // ref_fat, ref_entry, ref_bpb
        size_t where;  // cluster number or entry offset
    };
    struct walk_state;

    // ── volume (vfat32_volume.cpp) ──
    int fail(error e);
    void reset_state();
    static bool probe(const uint8_t* m, size_t size);
    int format_memory(uint8_t* m, size_t size, const format_options* options);
    void attach(uint8_t* m, size_t size, bool owned);
    void write_fsinfo();
    void set_clean_bit(bool clean);
    void write_root_cluster(uint32_t cluster);
    timestamp now() const;
    void stamp_now(uint16_t& date, uint16_t& time, uint8_t& tenth) const;
    bool any_open() const;
    uint32_t fat_get(uint32_t cluster) const;
    void fat_set(uint32_t cluster, uint32_t value);
    uint32_t fat_raw(uint32_t cluster) const;
    void fat_raw_set(uint32_t cluster, uint32_t value);
    bool valid_cluster(uint32_t cluster) const;
    static bool is_eoc(uint32_t value);
    uint8_t* cluster_ptr(uint32_t cluster) const;
    size_t cluster_offset(uint32_t cluster) const;
    uint32_t alloc_cluster(uint32_t hint);
    uint32_t alloc_dir_cluster(uint32_t hint);
    void free_chain(uint32_t first);
    int chain_next(uint32_t cluster, uint32_t& next) const;

    // ── names (vfat32_name.cpp) ──
    int set_target(const char* name, size_t len);
    bool target_is_dot() const;
    bool target_valid_for_create() const;
    bool target_fits_83(uint8_t sfn[11], uint8_t& nt_flags) const;
    void target_basis(uint8_t sfn[11], bool& lossy) const;
    uint16_t target_hash() const;
    static uint8_t sfn_checksum(const uint8_t* sfn);
    static uint16_t sfn_display(const uint8_t* entry, uint16_t* out);
    bool names_equal(const uint16_t* a, uint16_t alen, const uint16_t* b, uint16_t blen) const;
    static int utf16_to_utf8(const uint16_t* in, uint16_t len, char* out, size_t cap);

    // ── directories (vfat32_dir.cpp) ──
    uint8_t* entry_ptr(size_t offset) const;
    static uint32_t entry_cluster(const uint8_t* e);
    static void set_entry_cluster(uint8_t* e, uint32_t cluster);
    static bool is_dot_entry(const uint8_t* e);
    void iter_start(slot_iter& it, uint32_t first) const;
    uint8_t* iter_slot(const slot_iter& it) const;
    int iter_advance(slot_iter& it) const;
    int iter_seek(slot_iter& it, uint32_t first, uint32_t slot) const;
    int dir_next(slot_iter& it, found_entry& fe, bool want_name);
    int find_in_dir(uint32_t dir, found_entry& fe);
    int dotdot_of(uint32_t dir, uint32_t& parent) const;
    int find_child_dir(uint32_t parent, uint32_t child, found_entry& fe);
    int resolve(const char* path, lookup& r);
    int sfn_in_use(uint32_t dir, const uint8_t* sfn, bool& used);
    int make_sfn(uint32_t dir, uint8_t sfn[11], uint8_t& nt_flags, bool& need_lfn);
    int find_free_run(uint32_t dir, uint32_t count, uint32_t& first_slot);
    int create_entry(uint32_t dir, uint8_t attr, uint32_t first_cluster, const uint8_t* copy_from,
                     found_entry& out);
    int mark_entry_slots(const found_entry& fe, uint8_t* saved, bool restore);
    int delete_entry(const found_entry& fe);
    int dir_is_empty(uint32_t dir, bool& empty);
    bool file_open(size_t entry_offset) const;
    bool dir_open(uint32_t first) const;
    void fill_stat(const uint8_t* e, struct stat* buf) const;
    int is_ancestor(uint32_t ancestor, uint32_t dir, bool& result);
    int remove_entry(const lookup& r, bool allow_file, bool allow_dir);
    dir* valid_dir(dir* d) const;

    // ── files (vfat32_file.cpp) ──
    file* valid_file(file* f) const;
    file* valid_fd(int fd);
    file* new_handle();
    file* open_common(const char* path, uint8_t access, bool create, bool excl, bool trunc, bool append,
                      bool stream);
    int cluster_for(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t index, bool allocate,
                    uint32_t& cluster);
    uint32_t raw_read(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t pos, void* dst,
                      uint32_t n, bool& failed);
    uint32_t raw_write(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t pos, const void* src,
                       uint32_t n, const file* writer, bool& failed);
    void invalidate_others(size_t entry, const file* except, bool caches);
    int truncate_entry(size_t entry, uint32_t length, file* writer);
    void drop_read_buffer(file& h);
    int flush_stream(file& h);
    int finalize(file& h);
    int release(file& h);
    void prepare_fd(file& h);
    int seek_to(file& h, int64_t offset, int whence, uint32_t& result);
    size_t stream_read(file& h, uint8_t* out, size_t total);
    size_t stream_write(file& h, const uint8_t* in, size_t total);
    int getc_locked(file& h);

    // ── maintenance (vfat32_check.cpp) ──
    int walk_next(walk_state& w, found_entry& fe);
    int walk_up(walk_state& w);
    void clear_marks();
    void clear_reserved(uint32_t cluster);
    void free_marked_chain(uint32_t first);
    void mark_chain(const found_entry* fe, uint32_t first, bool repair, check_report& r, uint32_t& length,
                    bool& broken);
    int check_pass(check_report& r, bool repair);
    bool ref_is_dir_head(const referrer& ref) const;
    void apply_ref(const referrer& ref, uint32_t x, uint32_t y, uint32_t value);
    void fix_dir_head(uint32_t head);
    int find_head_entry(uint32_t cluster, size_t& offset);
    int find_referrer(uint32_t cluster, referrer& ref);
    void swap_clusters(uint32_t x, uint32_t y);
    int move_cluster(uint32_t x, uint32_t y, const referrer& rx);
    int place_chain(referrer ref, uint32_t head, uint32_t& t, uint32_t& moves, uint32_t max_moves, bool& stopped);

    // ── state ──
    uint8_t* base_;
    size_t size_;
    bool owned_;
    bool mounted_;
    bool was_dirty_;
    error last_error_;
    lock_interface* lock_;
    clock_function clock_;

    uint32_t bytes_per_sector_;
    uint32_t cluster_size_;
    uint32_t cluster_shift_;
    uint32_t slots_per_cluster_;
    uint32_t total_sectors_;
    size_t fat_offset_;        // byte offset of the first FAT copy
    size_t fat_bytes_;         // bytes per FAT copy
    uint32_t fat_copies_;
    uint32_t active_fat_;      // ExtFlags: active copy when not mirroring
    bool mirror_;              // all copies written
    size_t data_offset_;       // byte offset of cluster 2
    uint32_t cluster_count_;   // clusters in the data region
    uint32_t max_cluster_;     // cluster_count_ + 1
    uint32_t root_cluster_;
    size_t fsinfo_offset_;     // 0 = no valid FSInfo
    size_t backup_offset_;     // 0 = no backup boot sector
    uint32_t free_count_;
    uint32_t next_free_;

    uint16_t target_[VFAT32_NAME_MAX < 255 ? VFAT32_NAME_MAX : 255];  // name being looked up / created (UTF-16)
    uint16_t target_len_;
    uint16_t name_[VFAT32_NAME_MAX < 255 ? VFAT32_NAME_MAX : 255];    // name assembled by dir_next (UTF-16)
    uint16_t name_len_;

    file files_[VFAT32_MAX_OPEN_FILES];
    dir dirs_[VFAT32_MAX_OPEN_DIRS];
};

}  // namespace vfat32

#endif  // VFAT32_HPP
