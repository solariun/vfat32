// vfat32 — volume: probe, format, mount, umount, sync, FAT and cluster primitives, errors (spec §4, §5, §9).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32_internal.hpp"

#if VFAT32_USE_HEAP
#include <stdlib.h>
#endif

namespace vfat32 {

using namespace detail;

namespace {

const char default_label[label_length + 1] = "NO NAME    ";
const char oem_name[oem_length + 1] = "MSWIN4.1";
const char fs_type_name[fs_type_length + 1] = "FAT32   ";

bool valid_sector_size(uint32_t bps) {
    return bps >= min_sector_size && bps <= max_sector_size && is_power_of_two(bps);
}

// Upper-case, space-padded 11-byte volume label; characters not valid in an 8.3 name become '_'.
void make_label(const char* in, uint8_t out[label_length]) {
    memset(out, ' ', label_length);
    for (size_t i = 0; i < label_length && in[i] != '\0'; ++i) {
        uint8_t c = static_cast<uint8_t>(ascii_upper(static_cast<uint8_t>(in[i])));
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || strchr("!#$%&'()-@^_`{}~", c);
        out[i] = (ok && c < 0x80) ? c : '_';
    }
}

}  // namespace

// ── Small public types ──

format_options::format_options()
    : bytes_per_sector(min_sector_size), sectors_per_cluster(0), fat_copies(2), reserved_sectors(0), volume_id(0) {
    memcpy(volume_label, "NO NAME", sizeof("NO NAME"));
}

uint32_t check_report::errors() const {
    return reserved_bits + bad_chains + cross_links + size_errors + bad_entries + lost_clusters + fat_mismatches +
           fsinfo_errors;
}

file::file()
    : entry_(0),
      pos_(0),
      cache_index_(no_cache),
      cache_cluster_(0),
      buf_start_(0),
      buf_(nullptr),
      buf_size_(0),
      buf_len_(0),
      unget_(-1),
      buf_state_(st_idle),
      buf_mode_(io_nbf),
      flags_(0) {}

dir::dir() : first_(0), cluster_(0), index_(0), slot_(0), steps_(0) { memset(&entry_, 0, sizeof entry_); }

// ── Construction ──

vfs::vfs() : last_error_(error::none), lock_(nullptr), clock_(nullptr) { reset_state(); }

vfs::~vfs() {
    if (mounted_) umount();
}

void vfs::reset_state() {
    base_ = nullptr;
    size_ = 0;
    owned_ = false;
    mounted_ = false;
    was_dirty_ = false;
    bytes_per_sector_ = 0;
    cluster_size_ = 0;
    cluster_shift_ = 0;
    slots_per_cluster_ = 0;
    total_sectors_ = 0;
    fat_offset_ = 0;
    fat_bytes_ = 0;
    fat_copies_ = 0;
    active_fat_ = 0;
    mirror_ = true;
    data_offset_ = 0;
    cluster_count_ = 0;
    max_cluster_ = 0;
    root_cluster_ = 0;
    fsinfo_offset_ = 0;
    backup_offset_ = 0;
    free_count_ = 0;
    next_free_ = first_data_cluster;
    target_len_ = 0;
    name_len_ = 0;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) files_[i].entry_ = 0;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i) dirs_[i].first_ = 0;
}

int vfs::fail(error e) {
    last_error_ = e;
    return -1;
}

error vfs::last_error() const { return last_error_; }

const char* vfs::strerror(error e) {
    switch (e) {
        case error::none: return "no error";
        case error::not_found: return "no such file or directory";
        case error::exists: return "file exists";
        case error::no_space: return "no space left on volume";
        case error::bad_handle: return "bad file handle";
        case error::invalid: return "invalid argument";
        case error::is_directory: return "is a directory";
        case error::not_directory: return "not a directory";
        case error::not_empty: return "directory not empty";
        case error::busy: return "resource busy";
        case error::too_many_open: return "too many open files";
        case error::name_too_long: return "file name too long";
        case error::access: return "permission denied";
        case error::file_too_big: return "file too large";
        case error::overflow: return "value too large for type";
        case error::not_mounted: return "no volume mounted";
        case error::no_memory: return "out of memory";
        case error::no_filesystem: return "no FAT32 file system";
        case error::corrupt: return "file system corrupt";
    }
    return "unknown error";
}

void vfs::set_lock(lock_interface* lock) { lock_ = lock; }
void vfs::set_clock(clock_function clock) { clock_ = clock; }

void vfs::flockfile(file* stream) {
    (void)stream;  // the lock covers the whole volume (spec T3)
    if (lock_) lock_->lock();
}

void vfs::funlockfile(file* stream) {
    (void)stream;
    if (lock_) lock_->unlock();
}

bool vfs::mounted() const { return mounted_; }
bool vfs::was_dirty() const { return was_dirty_; }

bool vfs::any_open() const {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i)
        if (files_[i].entry_ != 0) return true;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i)
        if (dirs_[i].first_ != 0) return true;
    return false;
}

// ── Time (spec §4.8) ──

timestamp vfs::now() const {
    if (clock_) return clock_();
    timestamp t;
    t.year = fat_epoch_year;
    t.month = 1;
    t.day = 1;
    t.hour = 0;
    t.minute = 0;
    t.second = 0;
    return t;
}

void vfs::stamp_now(uint16_t& date, uint16_t& time, uint8_t& tenth) const {
    const uint8_t last_month = 12, last_day = 31, last_hour = 23, last_minute = 59, last_second = 59;
    timestamp t = now();
    uint16_t year = t.year < fat_epoch_year ? fat_epoch_year : (t.year > fat_last_year ? fat_last_year : t.year);
    uint8_t month = t.month < 1 ? 1 : (t.month > last_month ? last_month : t.month);
    uint8_t day = t.day < 1 ? 1 : (t.day > last_day ? last_day : t.day);
    uint8_t hour = t.hour > last_hour ? last_hour : t.hour;
    uint8_t minute = t.minute > last_minute ? last_minute : t.minute;
    uint8_t second = t.second > last_second ? last_second : t.second;
    date = static_cast<uint16_t>(((year - fat_epoch_year) << 9) | (month << 5) | day);
    time = static_cast<uint16_t>((hour << 11) | (minute << 5) | (second / 2));
    tenth = static_cast<uint8_t>((second % 2) * tenth_per_odd_second);
}

// ── FAT primitives (spec §4.4) ──

uint32_t vfs::fat_raw(uint32_t cluster) const {
    return ld32(base_ + fat_offset_ + active_fat_ * fat_bytes_ + static_cast<size_t>(cluster) * fat_entry_size);
}

void vfs::fat_raw_set(uint32_t cluster, uint32_t value) {
    st32(base_ + fat_offset_ + active_fat_ * fat_bytes_ + static_cast<size_t>(cluster) * fat_entry_size, value);
}

uint32_t vfs::fat_get(uint32_t cluster) const { return fat_raw(cluster) & fat_mask; }

void vfs::fat_set(uint32_t cluster, uint32_t value) {
    for (uint32_t k = 0; k < fat_copies_; ++k) {
        if (!mirror_ && k != active_fat_) continue;
        uint8_t* p = base_ + fat_offset_ + k * fat_bytes_ + static_cast<size_t>(cluster) * fat_entry_size;
        st32(p, (ld32(p) & fat_reserved_bits) | (value & fat_mask));
    }
}

bool vfs::valid_cluster(uint32_t cluster) const { return cluster >= first_data_cluster && cluster <= max_cluster_; }

bool vfs::is_eoc(uint32_t value) { return value >= fat_eoc_min; }

size_t vfs::cluster_offset(uint32_t cluster) const {
    return data_offset_ + (static_cast<size_t>(cluster - first_data_cluster) << cluster_shift_);
}

uint8_t* vfs::cluster_ptr(uint32_t cluster) const { return base_ + cluster_offset(cluster); }

int vfs::chain_next(uint32_t cluster, uint32_t& next) const {
    uint32_t v = fat_get(cluster);
    if (is_eoc(v)) {
        next = 0;
        return 0;
    }
    if (!valid_cluster(v)) return -1;
    next = v;
    return 1;
}

uint32_t vfs::alloc_cluster(uint32_t hint) {
    if (free_count_ == 0) return 0;
    uint32_t c = valid_cluster(hint) ? hint : (valid_cluster(next_free_) ? next_free_ : first_data_cluster);
    for (uint32_t i = 0; i < cluster_count_; ++i) {
        if (fat_get(c) == fat_free) {
            fat_set(c, fat_eoc);
            --free_count_;
            next_free_ = c == max_cluster_ ? first_data_cluster : c + 1;
            write_fsinfo();
            return c;
        }
        c = c == max_cluster_ ? first_data_cluster : c + 1;
    }
    free_count_ = 0;  // the count was wrong: nothing is free
    write_fsinfo();
    return 0;
}

uint32_t vfs::alloc_dir_cluster(uint32_t hint) {
    uint32_t c = alloc_cluster(hint);
    if (c) memset(cluster_ptr(c), 0, cluster_size_);
    return c;
}

void vfs::free_chain(uint32_t first) {
    uint32_t c = first;
    for (uint32_t steps = 0; valid_cluster(c) && steps < cluster_count_; ++steps) {
        uint32_t next = fat_get(c);
        if (next == fat_free) break;  // already free: a broken chain, stop here
        fat_set(c, fat_free);
        ++free_count_;
        if (c < next_free_) next_free_ = c;
        if (is_eoc(next)) break;
        c = next;
    }
    write_fsinfo();
}

void vfs::write_fsinfo() {
    if (!fsinfo_offset_) return;
    st32(base_ + fsinfo_offset_ + fsinfo_free, free_count_);
    st32(base_ + fsinfo_offset_ + fsinfo_next, next_free_);
}

void vfs::set_clean_bit(bool clean) {
    for (uint32_t k = 0; k < fat_copies_; ++k) {
        if (!mirror_ && k != active_fat_) continue;
        uint8_t* p = base_ + fat_offset_ + k * fat_bytes_ + fat_entry_size;  // FAT[1]
        uint32_t v = ld32(p);
        st32(p, clean ? (v | fat_clean_bit) : (v & ~fat_clean_bit));
    }
}

void vfs::write_root_cluster(uint32_t cluster) {
    root_cluster_ = cluster;
    st32(base_ + bpb_root_cluster, cluster);
    if (backup_offset_) st32(base_ + backup_offset_ + bpb_root_cluster, cluster);
}

// ── Probe (spec §5.1, rules P1–P12) ──

bool vfs::probe(const uint8_t* m, size_t size) {
    if (!m || size < min_sector_size) return false;                                                    // P1
    if (m[boot_signature_offset] != boot_signature_low || m[boot_signature_offset + 1] != boot_signature_high)
        return false;                                                                                   // P2
    if (!((m[bpb_jump] == jump_short && m[bpb_jump + 2] == jump_nop) || m[bpb_jump] == jump_near)) return false;  // P3
    uint32_t bps = ld16(m + bpb_bytes_per_sector);
    if (!valid_sector_size(bps)) return false;                                                          // P4
    uint32_t spc = m[bpb_sectors_per_cluster];
    if (!is_power_of_two(spc) || spc * bps > max_cluster_bytes_read) return false;                      // P5
    uint32_t rsvd = ld16(m + bpb_reserved_sectors);
    uint32_t nfats = m[bpb_fat_count];
    if (rsvd < 1 || nfats < 1) return false;                                                            // P6
    uint32_t fatsz = ld32(m + bpb_fat_size32);
    if (ld16(m + bpb_root_entries) != 0 || ld16(m + bpb_total_sectors16) != 0 || ld16(m + bpb_fat_size16) != 0 ||
        fatsz < 1 || ld16(m + bpb_fs_version) != 0)
        return false;                                                                                   // P7
    uint8_t media = m[bpb_media];
    if (media != media_removable && media < media_fixed) return false;                                  // P8
    uint32_t total = ld32(m + bpb_total_sectors32);
    if (static_cast<uint64_t>(total) * bps > size) return false;                                       // P9
    uint64_t data = rsvd + static_cast<uint64_t>(nfats) * fatsz;
    if (data >= total) return false;                                                                    // P10
    uint64_t clusters = (total - data) / spc;
    if (clusters < 1 || clusters > max_cluster_count) return false;
    if ((clusters + first_data_cluster) * fat_entry_size > static_cast<uint64_t>(fatsz) * bps) return false;
    uint16_t ext = ld16(m + bpb_ext_flags);
    uint32_t active = (ext & ext_flags_no_mirror) ? (ext & ext_flags_active_mask) : 0;
    if (active >= nfats) return false;
    const uint8_t* fat = m + static_cast<size_t>(rsvd) * bps + static_cast<size_t>(active) * fatsz * bps;
    uint32_t root = ld32(m + bpb_root_cluster);
    if (root < first_data_cluster || root > clusters + 1) return false;                                // P11
    uint32_t root_entry = ld32(fat + static_cast<size_t>(root) * fat_entry_size) & fat_mask;
    if (root_entry == fat_free || root_entry == fat_bad) return false;
    if ((ld32(fat) & fat_mask) != (fat_media_base | media)) return false;                              // P12
    return true;
}

bool vfs::needs_format(const void* memory, size_t size) { return !probe(static_cast<const uint8_t*>(memory), size); }

// ── Format (spec §4.1, F1–F7): all checks and the geometry come first, then the writes ──

int vfs::format_memory(uint8_t* m, size_t size, const format_options* options) {
    const format_options defaults;
    const format_options& o = options ? *options : defaults;

    uint32_t bps = o.bytes_per_sector;
    if (!valid_sector_size(bps)) return fail(error::invalid);
    if (o.fat_copies < 1 || o.fat_copies > 2) return fail(error::invalid);
    if (o.reserved_sectors != 0 && o.reserved_sectors < reserved_minimum) return fail(error::invalid);
    if (o.sectors_per_cluster != 0 &&
        (!is_power_of_two(o.sectors_per_cluster) || o.sectors_per_cluster * bps > max_cluster_bytes_format))
        return fail(error::invalid);

    uint64_t sectors = size / bps;
    uint32_t total = sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sectors);

    uint32_t spc = o.sectors_per_cluster;
    if (spc == 0) {
        uint64_t s512 = static_cast<uint64_t>(total) * (bps / min_sector_size);
        uint32_t cluster = s512 <= table_limit_260m ? cluster_512
                           : s512 <= table_limit_8g ? cluster_4k
                           : s512 <= table_limit_16g ? cluster_8k
                           : s512 <= table_limit_32g ? cluster_16k
                                                     : cluster_32k;
        spc = cluster <= bps ? 1 : cluster / bps;
    }

    uint32_t rsvd = o.reserved_sectors;
    if (rsvd == 0) rsvd = total < tiny_volume_sectors ? reserved_minimum
                          : total < small_volume_sectors ? reserved_small
                                                         : reserved_large;
    uint32_t nfats = o.fat_copies;
    if (rsvd >= total) return fail(error::no_space);

    // F5: smallest FAT that holds every cluster it leaves room for
    uint64_t fat = 0, clusters = 0;
    for (;;) {
        uint64_t meta = rsvd + nfats * fat;
        if (meta >= total) return fail(error::no_space);
        clusters = (total - meta) / spc;
        if (clusters > max_cluster_count) clusters = max_cluster_count;
        uint64_t need = ((clusters + first_data_cluster) * fat_entry_size + bps - 1) / bps;
        if (need <= fat) break;
        fat = need;
    }
    if (clusters < min_cluster_count) return fail(error::no_space);  // F6

    uint32_t fatsz = static_cast<uint32_t>(fat);
    uint32_t cluster_bytes = spc * bps;
    size_t meta_bytes = (static_cast<size_t>(rsvd) + static_cast<size_t>(nfats) * fatsz) * bps;

    // F7: reserved region and FATs zeroed, then the root cluster
    memset(m, 0, meta_bytes);
    memset(m + meta_bytes, 0, cluster_bytes);

    uint16_t date, time;
    uint8_t tenth;
    stamp_now(date, time, tenth);
    uint8_t label[label_length];
    make_label(o.volume_label, label);

    // Boot sector (§4.2)
    m[bpb_jump] = jump_short;
    m[bpb_jump + 1] = jump_target;
    m[bpb_jump + 2] = jump_nop;
    memcpy(m + bpb_oem, oem_name, oem_length);
    st16(m + bpb_bytes_per_sector, static_cast<uint16_t>(bps));
    m[bpb_sectors_per_cluster] = static_cast<uint8_t>(spc);
    st16(m + bpb_reserved_sectors, static_cast<uint16_t>(rsvd));
    m[bpb_fat_count] = static_cast<uint8_t>(nfats);
    m[bpb_media] = media_fixed;
    st16(m + bpb_sectors_per_track, geometry_sectors_per_track);
    st16(m + bpb_heads, geometry_heads);
    st32(m + bpb_total_sectors32, total);
    st32(m + bpb_fat_size32, fatsz);
    st32(m + bpb_root_cluster, first_data_cluster);
    st16(m + bpb_fsinfo_sector, static_cast<uint16_t>(fsinfo_sector));
    bool backup = rsvd >= reserved_for_backup;
    st16(m + bpb_backup_boot_sector, static_cast<uint16_t>(backup ? backup_boot_sector : 0));
    m[bpb_drive_number] = drive_number_fixed;
    m[bpb_boot_signature] = extended_boot_signature;
    st32(m + bpb_volume_id, o.volume_id ? o.volume_id : (static_cast<uint32_t>(date) << 16) | time);
    memcpy(m + bpb_volume_label, label, label_length);
    memcpy(m + bpb_fs_type, fs_type_name, fs_type_length);
    m[boot_signature_offset] = boot_signature_low;
    m[boot_signature_offset + 1] = boot_signature_high;

    // FSInfo (§4.3)
    uint8_t* fsi = m + fsinfo_sector * bps;
    st32(fsi + fsinfo_lead, fsinfo_lead_signature);
    st32(fsi + fsinfo_struct, fsinfo_struct_signature);
    st32(fsi + fsinfo_free, static_cast<uint32_t>(clusters) - 1);
    st32(fsi + fsinfo_next, first_data_cluster + 1);
    st32(fsi + fsinfo_trail, fsinfo_trail_signature);

    if (backup) {
        memcpy(m + backup_boot_sector * bps, m, bps);
        memcpy(m + (backup_boot_sector + 1) * bps, fsi, bps);
    }

    // FATs (§4.4, F10): entries 0, 1 and the root's end of chain
    for (uint32_t k = 0; k < nfats; ++k) {
        uint8_t* f = m + (static_cast<size_t>(rsvd) + static_cast<size_t>(k) * fatsz) * bps;
        st32(f, fat_media_base | media_fixed);
        st32(f + fat_entry_size, fat_eoc);
        st32(f + first_data_cluster * fat_entry_size, fat_eoc);
    }

    // Volume label entry in the root (F7)
    if (memcmp(label, default_label, label_length) != 0) {
        uint8_t* e = m + meta_bytes;
        memcpy(e + de_name, label, label_length);
        e[de_attr] = attr_volume_id;
        st16(e + de_write_time, time);
        st16(e + de_write_date, date);
    }
    return 0;
}

// ── Attach a probed or freshly formatted volume ──

void vfs::attach(uint8_t* m, size_t size, bool owned) {
    base_ = m;
    size_ = size;
    owned_ = owned;
    bytes_per_sector_ = ld16(m + bpb_bytes_per_sector);
    uint32_t spc = m[bpb_sectors_per_cluster];
    uint32_t rsvd = ld16(m + bpb_reserved_sectors);
    fat_copies_ = m[bpb_fat_count];
    uint32_t fatsz = ld32(m + bpb_fat_size32);
    total_sectors_ = ld32(m + bpb_total_sectors32);
    cluster_size_ = spc * bytes_per_sector_;
    cluster_shift_ = log2_of(cluster_size_);
    slots_per_cluster_ = cluster_size_ >> entry_shift;
    fat_offset_ = static_cast<size_t>(rsvd) * bytes_per_sector_;
    fat_bytes_ = static_cast<size_t>(fatsz) * bytes_per_sector_;
    uint16_t ext = ld16(m + bpb_ext_flags);
    mirror_ = (ext & ext_flags_no_mirror) == 0;
    active_fat_ = mirror_ ? 0 : (ext & ext_flags_active_mask);
    data_offset_ = fat_offset_ + fat_copies_ * fat_bytes_;
    cluster_count_ = static_cast<uint32_t>((total_sectors_ - (rsvd + static_cast<uint64_t>(fat_copies_) * fatsz)) / spc);
    max_cluster_ = cluster_count_ + 1;
    root_cluster_ = ld32(m + bpb_root_cluster);

    uint32_t fsi = ld16(m + bpb_fsinfo_sector);
    fsinfo_offset_ = 0;
    if (fsi >= 1 && fsi < rsvd) {
        const uint8_t* p = m + static_cast<size_t>(fsi) * bytes_per_sector_;
        if (ld32(p + fsinfo_lead) == fsinfo_lead_signature && ld32(p + fsinfo_struct) == fsinfo_struct_signature &&
            ld32(p + fsinfo_trail) == fsinfo_trail_signature)
            fsinfo_offset_ = static_cast<size_t>(fsi) * bytes_per_sector_;
    }
    uint32_t bk = ld16(m + bpb_backup_boot_sector);
    backup_offset_ = (bk >= 1 && bk < rsvd && bk != fsi) ? static_cast<size_t>(bk) * bytes_per_sector_ : 0;

    // F8: the free count is recounted, never trusted
    free_count_ = 0;
    next_free_ = 0;
    for (uint32_t c = first_data_cluster; c <= max_cluster_; ++c) {
        if (fat_get(c) == fat_free) {
            ++free_count_;
            if (!next_free_) next_free_ = c;
        }
    }
    if (!next_free_) next_free_ = first_data_cluster;
    write_fsinfo();

    // F10: the clean bit
    was_dirty_ = (fat_raw(1) & fat_clean_bit) == 0;
    set_clean_bit(false);

    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) files_[i].entry_ = 0;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i) dirs_[i].first_ = 0;
    mounted_ = true;
}

// ── Mount and friends (spec §5.2) ──

int vfs::mount(void* memory, size_t size, mount_mode mode, const format_options* options) {
    guard g(*this);
    if (mounted_) return fail(error::busy);
    if (!memory) return fail(error::invalid);
    uint8_t* m = static_cast<uint8_t*>(memory);
    switch (mode) {
        case mount_mode::reuse:
            if (!probe(m, size)) return fail(error::no_filesystem);
            break;
        case mount_mode::format:
            if (format_memory(m, size, options) < 0) return -1;
            break;
        case mount_mode::format_if_needed:
            if (!probe(m, size) && format_memory(m, size, options) < 0) return -1;
            break;
        default:
            return fail(error::invalid);
    }
    attach(m, size, false);
    return 0;
}

#if VFAT32_USE_HEAP
int vfs::mount(size_t size, const format_options* options) {
    guard g(*this);
    if (mounted_) return fail(error::busy);
    if (size == 0) return fail(error::invalid);
    uint8_t* m = static_cast<uint8_t*>(calloc(1, size));
    if (!m) return fail(error::no_memory);
    if (format_memory(m, size, options) < 0) {
        free(m);
        return -1;
    }
    attach(m, size, true);
    return 0;
}
#endif

int vfs::umount() {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    int rc = 0;
    error first = error::none;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) {
        if (files_[i].entry_ != 0 && release(files_[i]) < 0 && rc == 0) {
            rc = -1;
            first = last_error_;
        }
    }
    set_clean_bit(true);
    write_fsinfo();
#if VFAT32_USE_HEAP
    if (owned_) free(base_);
#endif
    reset_state();
    if (rc < 0) last_error_ = first;
    return rc;
}

int vfs::format(const format_options* options) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (any_open()) return fail(error::busy);
    if (format_memory(base_, size_, options) < 0) return -1;
    attach(base_, size_, owned_);
    return 0;
}

int vfs::sync() {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    int rc = 0;
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) {
        file& h = files_[i];
        if (h.entry_ == 0) continue;
        if (flush_stream(h) < 0) rc = -1;
        finalize(h);
    }
    write_fsinfo();
    return rc;
}

int vfs::statvfs(struct statvfs* buf) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (!buf) return fail(error::invalid);
    buf->f_bsize = cluster_size_;
    buf->f_frsize = cluster_size_;
    buf->f_blocks = cluster_count_;
    buf->f_bfree = free_count_;
    buf->f_bavail = free_count_;
    buf->f_namemax = VFAT32_NAME_MAX;
    return 0;
}

}  // namespace vfat32
