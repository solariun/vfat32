// vfat32 — directories: slots, lookup, paths, creating and deleting entries, the directory API, stat, rename,
// remove (spec §4.5–§4.7, §6, §8).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32_internal.hpp"

namespace vfat32 {

using namespace detail;

namespace {

constexpr uint32_t perm_all = 0777;
constexpr uint32_t perm_dir_read_only = 0555;
constexpr uint32_t perm_read_write = 0666;
constexpr uint32_t perm_read_only = 0444;
constexpr size_t alias_units = sfn_base_length + 1 + sfn_ext_length;
constexpr size_t tail_length = 2;  // "~N"
constexpr size_t hash_prefix = 2;
constexpr size_t hash_digits = 4;
constexpr uint32_t hex_shift = 4;
const char hex_digits[] = "0123456789ABCDEF";

bool is_dot_name(const char* name, size_t len) {
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

timestamp decode_stamp(uint16_t date, uint16_t time, uint8_t tenth) {
    timestamp t;
    memset(&t, 0, sizeof t);
    if (date == 0) return t;
    t.year = static_cast<uint16_t>(fat_epoch_year + (date >> 9));
    t.month = static_cast<uint8_t>((date >> 5) & 0x0F);
    t.day = static_cast<uint8_t>(date & 0x1F);
    t.hour = static_cast<uint8_t>(time >> 11);
    t.minute = static_cast<uint8_t>((time >> 5) & 0x3F);
    t.second = static_cast<uint8_t>((time & 0x1F) * 2 + tenth / tenth_per_odd_second);
    return t;
}

}  // namespace

// ── Entry fields ──

uint8_t* vfs::entry_ptr(size_t offset) const { return base_ + offset; }

uint32_t vfs::entry_cluster(const uint8_t* e) {
    return (static_cast<uint32_t>(ld16(e + de_cluster_high)) << 16) | ld16(e + de_cluster_low);
}

void vfs::set_entry_cluster(uint8_t* e, uint32_t cluster) {
    st16(e + de_cluster_high, static_cast<uint16_t>(cluster >> 16));
    st16(e + de_cluster_low, static_cast<uint16_t>(cluster & 0xFFFF));
}

bool vfs::is_dot_entry(const uint8_t* e) {
    return memcmp(e, dot_name, sfn_length) == 0 || memcmp(e, dotdot_name, sfn_length) == 0;
}

// ── Slot iteration ──

void vfs::iter_start(slot_iter& it, uint32_t first) const {
    it.first = first;
    it.cluster = first;
    it.index = 0;
    it.slot = 0;
    it.steps = 0;
}

uint8_t* vfs::iter_slot(const slot_iter& it) const {
    if (it.cluster == 0) return nullptr;
    return cluster_ptr(it.cluster) + (static_cast<size_t>(it.index) << entry_shift);
}

// 1: on a slot, 0: end of the directory, -1: broken chain.
int vfs::iter_advance(slot_iter& it) const {
    if (it.cluster == 0) return 0;
    ++it.slot;
    if (++it.index < slots_per_cluster_) return 1;
    uint32_t next;
    int r = chain_next(it.cluster, next);
    if (r < 0 || ++it.steps > cluster_count_) {
        it.cluster = 0;
        it.steps = corrupt_steps;
        return -1;
    }
    if (r == 0 || it.slot >= max_dir_slots) {
        it.cluster = 0;
        return 0;
    }
    it.cluster = next;
    it.index = 0;
    return 1;
}

int vfs::iter_seek(slot_iter& it, uint32_t first, uint32_t slot) const {
    iter_start(it, first);
    for (uint32_t skip = slot / slots_per_cluster_; skip > 0; --skip) {
        uint32_t next;
        if (chain_next(it.cluster, next) <= 0 || ++it.steps > cluster_count_) {
            it.cluster = 0;
            it.steps = corrupt_steps;
            return -1;
        }
        it.cluster = next;
    }
    it.index = slot % slots_per_cluster_;
    it.slot = slot;
    return 0;
}

// The next 8.3 entry (with its LFN, F18), skipping deleted slots, orphan LFN slots and volume labels.
// With want_name the name is assembled into name_ (the long name, or the 8.3 name, N7).
// 1: entry in fe (the iterator is past it), 0: end, -1: broken chain.
int vfs::dir_next(slot_iter& it, found_entry& fe, bool want_name) {
    const uint32_t cap = sizeof name_ / sizeof name_[0];
    uint8_t expect = 0;  // order byte the next LFN slot must have; 0 = no sequence
    uint8_t sum = 0;
    uint32_t lfn_start = 0;
    uint16_t lfn_len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t* p = iter_slot(it);
        if (!p) return it.steps == corrupt_steps ? -1 : 0;
        uint8_t b0 = p[de_name];
        if (b0 == slot_end) return 0;
        uint8_t attr = p[de_attr];
        if (b0 == slot_deleted) {
            expect = 0;
        } else if ((attr & attr_lfn_mask) == attr_lfn) {
            uint8_t ord = b0 & lfn_order_mask & static_cast<uint8_t>(~lfn_last);
            if (b0 & lfn_last) {
                expect = 0;
                if (ord >= 1 && ord <= lfn_max_slots) {
                    expect = ord;
                    sum = p[lfn_checksum];
                    lfn_start = it.slot;
                    uint32_t k = 0;
                    while (k < lfn_units_per_slot && ld16(p + lfn_unit_offset[k]) != 0) ++k;
                    uint32_t len = (ord - 1u) * lfn_units_per_slot + k;
                    overflow = len == 0 || len > cap || len > max_lfn_units;
                    lfn_len = overflow ? 0 : static_cast<uint16_t>(len);
                }
            } else if (expect >= 2 && ord == expect - 1 && p[lfn_checksum] == sum) {
                expect = ord;
            } else {
                expect = 0;
            }
            if (expect && want_name && !overflow) {
                uint32_t base = (expect - 1u) * lfn_units_per_slot;
                for (uint32_t k = 0; k < lfn_units_per_slot && base + k < lfn_len; ++k)
                    name_[base + k] = ld16(p + lfn_unit_offset[k]);
            }
        } else if (attr & attr_volume_id) {
            expect = 0;
        } else {
            bool lfn = expect == 1 && sfn_checksum(p) == sum;
            fe.dir = it.first;
            fe.slot = it.slot;
            fe.first_slot = lfn ? lfn_start : it.slot;
            fe.offset = cluster_offset(it.cluster) + (static_cast<size_t>(it.index) << entry_shift);
            if (want_name) name_len_ = (lfn && !overflow) ? lfn_len : sfn_display(p, name_);
            iter_advance(it);  // a broken chain shows on the next call
            return 1;
        }
        int r = iter_advance(it);
        if (r <= 0) return r;
    }
}

// N6: find target_ in a directory, by long name or by displayed 8.3 name. 1 found, 0 not found, -1 broken.
int vfs::find_in_dir(uint32_t dir, found_entry& fe) {
    slot_iter it;
    iter_start(it, dir);
    uint16_t alias[alias_units];
    for (;;) {
        int r = dir_next(it, fe, true);
        if (r <= 0) return r;
        if (names_equal(name_, name_len_, target_, target_len_)) return 1;
        if (fe.first_slot != fe.slot) {
            uint16_t n = sfn_display(entry_ptr(fe.offset), alias);
            if (names_equal(alias, n, target_, target_len_)) return 1;
        }
    }
}

int vfs::dotdot_of(uint32_t dir, uint32_t& parent) const {
    if (dir == root_cluster_) {
        parent = root_cluster_;
        return 0;
    }
    const uint8_t* p = cluster_ptr(dir) + entry_size;
    if (memcmp(p, dotdot_name, sfn_length) != 0 || !(p[de_attr] & attr_directory)) return -1;
    uint32_t c = entry_cluster(p);
    if (c == 0 || c == root_cluster_) {
        parent = root_cluster_;
        return 0;
    }
    if (!valid_cluster(c)) return -1;
    parent = c;
    return 0;
}

// The entry of directory `child` inside `parent`. 1 found, 0 not found, -1 broken.
int vfs::find_child_dir(uint32_t parent, uint32_t child, found_entry& fe) {
    slot_iter it;
    iter_start(it, parent);
    for (;;) {
        int r = dir_next(it, fe, false);
        if (r <= 0) return r;
        const uint8_t* e = entry_ptr(fe.offset);
        if ((e[de_attr] & attr_directory) && !is_dot_entry(e) && entry_cluster(e) == child) return 1;
    }
}

// ── Paths (spec §6, S1–S4) ──

int vfs::resolve(const char* path, lookup& r) {
    memset(&r, 0, sizeof r);
    r.parent = root_cluster_;
    if (!path) return fail(error::invalid);
    const char* p = path;
    while (*p == '/') ++p;
    if (*p == '\0') {
        r.is_root = true;
        r.found = true;
        r.name = p;
        return 0;
    }
    uint32_t dir = root_cluster_;
    found_entry cur;
    memset(&cur, 0, sizeof cur);
    for (;;) {
        const char* q = p;
        while (*q != '\0' && *q != '/') ++q;
        size_t len = static_cast<size_t>(q - p);
        const char* next = q;
        bool slash = false;
        while (*next == '/') {
            ++next;
            slash = true;
        }
        bool last = *next == '\0';
        if (last) {
            r.trailing_slash = slash;
            r.name = p;
            r.name_len = len;
        }
        if (is_dot_name(p, len)) {
            if (len == 2 && dir != root_cluster_) {
                uint32_t parent;
                if (dotdot_of(dir, parent) < 0) return fail(error::corrupt);
                if (parent != root_cluster_) {
                    uint32_t grand;
                    if (dotdot_of(parent, grand) < 0) return fail(error::corrupt);
                    if (find_child_dir(grand, parent, cur) <= 0) return fail(error::corrupt);
                }
                dir = parent;
            }
            if (last) {
                r.found = true;
                if (dir == root_cluster_) {
                    r.is_root = true;
                } else {
                    r.entry = cur;
                    r.parent = cur.dir;
                }
                return 0;
            }
            p = next;
            continue;
        }
        if (set_target(p, len) < 0) return -1;
        found_entry fe;
        int f = find_in_dir(dir, fe);
        if (f < 0) return fail(error::corrupt);
        if (last) {
            r.parent = dir;
            if (f == 1) {
                r.found = true;
                r.entry = fe;
                if (slash && !(entry_ptr(fe.offset)[de_attr] & attr_directory)) return fail(error::not_directory);
            }
            return 0;
        }
        if (f == 0) return fail(error::not_found);
        const uint8_t* e = entry_ptr(fe.offset);
        if (!(e[de_attr] & attr_directory)) return fail(error::not_directory);
        uint32_t c = entry_cluster(e);
        if (!valid_cluster(c)) return fail(error::corrupt);
        dir = c;
        cur = fe;
        p = next;
    }
}

// ── Creating and deleting entries (spec F15, N4, N5) ──

int vfs::sfn_in_use(uint32_t dir, const uint8_t* sfn, bool& used) {
    used = false;
    slot_iter it;
    iter_start(it, dir);
    for (;;) {
        const uint8_t* p = iter_slot(it);
        if (!p) return it.steps == corrupt_steps ? -1 : 0;
        if (p[de_name] == slot_end) return 0;
        if (p[de_name] != slot_deleted && (p[de_attr] & attr_lfn_mask) != attr_lfn && memcmp(p, sfn, sfn_length) == 0) {
            used = true;
            return 0;
        }
        int r = iter_advance(it);
        if (r <= 0) return r;
    }
}

int vfs::make_sfn(uint32_t dir, uint8_t sfn[11], uint8_t& nt_flags, bool& need_lfn) {
    bool used = false;
    nt_flags = 0;
    if (target_fits_83(sfn, nt_flags)) {
        need_lfn = false;
        if (sfn_in_use(dir, sfn, used) < 0) return fail(error::corrupt);
        if (!used) return 0;
    }
    need_lfn = true;
    nt_flags = 0;
    uint8_t basis[sfn_length];
    bool lossy;
    target_basis(basis, lossy);
    if (!lossy) {
        memcpy(sfn, basis, sfn_length);
        if (sfn_in_use(dir, sfn, used) < 0) return fail(error::corrupt);
        if (!used) return 0;
    }
    size_t base_len = 0;
    while (base_len < sfn_base_length && basis[base_len] != ' ') ++base_len;
    uint16_t hash = target_hash();
    for (uint32_t n = 1; n <= sfn_plain_tails + sfn_hash_attempts; ++n) {
        memcpy(sfn, basis, sfn_length);
        memset(sfn, ' ', sfn_base_length);
        size_t k;
        if (n <= sfn_plain_tails) {
            k = base_len < sfn_base_length - tail_length ? base_len : sfn_base_length - tail_length;
            memcpy(sfn, basis, k);
            sfn[k++] = '~';
            sfn[k++] = static_cast<uint8_t>('0' + n);
        } else {
            k = base_len < hash_prefix ? base_len : hash_prefix;
            memcpy(sfn, basis, k);
            uint16_t v = static_cast<uint16_t>(hash + (n - sfn_plain_tails - 1));
            for (size_t d = hash_digits; d > 0; --d) sfn[k++] = hex_digits[(v >> ((d - 1) * hex_shift)) & 0x0F];
            sfn[k++] = '~';
            sfn[k++] = '1';
        }
        if (sfn_in_use(dir, sfn, used) < 0) return fail(error::corrupt);
        if (!used) return 0;
    }
    return fail(error::exists);
}

int vfs::find_free_run(uint32_t dir, uint32_t count, uint32_t& first_slot) {
    slot_iter it;
    iter_start(it, dir);
    uint32_t run = 0, run_start = 0, last = dir;
    for (;;) {
        const uint8_t* p = iter_slot(it);
        if (!p) break;
        last = it.cluster;
        if (p[de_name] == slot_end || p[de_name] == slot_deleted) {
            if (run == 0) run_start = it.slot;
            if (++run == count) {
                first_slot = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
        int r = iter_advance(it);
        if (r < 0) return fail(error::corrupt);
        if (r == 0) break;
    }
    uint32_t total = it.slot;
    if (run == 0) run_start = total;
    uint32_t needed = count - run;
    uint32_t clusters = (needed + slots_per_cluster_ - 1) / slots_per_cluster_;
    if (static_cast<uint64_t>(total) + static_cast<uint64_t>(clusters) * slots_per_cluster_ > max_dir_slots)
        return fail(error::no_space);
    for (uint32_t i = 0; i < clusters; ++i) {
        uint32_t c = alloc_dir_cluster(last + 1);
        if (!c) return fail(error::no_space);
        fat_set(last, c);
        last = c;
    }
    first_slot = run_start;
    return 0;
}

// Creates an entry named target_ in `dir`. With copy_from, the 32 bytes of an existing 8.3 entry are kept
// (rename, K5); otherwise a new entry with `attr` and `first_cluster` is stamped now (F20).
int vfs::create_entry(uint32_t dir, uint8_t attr, uint32_t first_cluster, const uint8_t* copy_from,
                      found_entry& out) {
    uint8_t sfn[sfn_length];
    uint8_t nt;
    bool need_lfn;
    if (make_sfn(dir, sfn, nt, need_lfn) < 0) return -1;
    uint32_t lfn_slots = need_lfn ? (target_len_ + lfn_units_per_slot - 1) / lfn_units_per_slot : 0;
    uint32_t start;
    if (find_free_run(dir, lfn_slots + 1, start) < 0) return -1;
    slot_iter it;
    if (iter_seek(it, dir, start) < 0) return fail(error::corrupt);
    uint8_t sum = sfn_checksum(sfn);
    for (uint32_t k = 0; k < lfn_slots; ++k) {
        uint8_t* p = iter_slot(it);
        if (!p) return fail(error::corrupt);
        uint8_t ord = static_cast<uint8_t>(lfn_slots - k);
        memset(p, 0, entry_size);
        p[de_name] = static_cast<uint8_t>(ord | (k == 0 ? lfn_last : 0));
        p[de_attr] = attr_lfn;
        p[lfn_type] = 0;
        p[lfn_checksum] = sum;
        st16(p + lfn_cluster, 0);
        for (uint32_t j = 0; j < lfn_units_per_slot; ++j) {
            uint32_t idx = (ord - 1u) * lfn_units_per_slot + j;
            uint16_t u = idx < target_len_ ? target_[idx] : (idx == target_len_ ? 0 : lfn_pad);
            st16(p + lfn_unit_offset[j], u);
        }
        if (iter_advance(it) <= 0) return fail(error::corrupt);
    }
    uint8_t* e = iter_slot(it);
    if (!e) return fail(error::corrupt);
    if (copy_from) {
        memcpy(e, copy_from, entry_size);
    } else {
        uint16_t date, time;
        uint8_t tenth;
        stamp_now(date, time, tenth);
        memset(e, 0, entry_size);
        e[de_attr] = attr;
        e[de_create_tenth] = tenth;
        st16(e + de_create_time, time);
        st16(e + de_create_date, date);
        st16(e + de_access_date, date);
        st16(e + de_write_time, time);
        st16(e + de_write_date, date);
        set_entry_cluster(e, first_cluster);
    }
    memcpy(e + de_name, sfn, sfn_length);
    e[de_nt_flags] = static_cast<uint8_t>((e[de_nt_flags] & ~(nt_lower_base | nt_lower_ext)) | nt);
    out.dir = dir;
    out.first_slot = start;
    out.slot = start + lfn_slots;
    out.offset = cluster_offset(it.cluster) + (static_cast<size_t>(it.index) << entry_shift);
    return 0;
}

// Writes 0xE5 into every slot of an entry (saving the first bytes), or puts the saved bytes back.
int vfs::mark_entry_slots(const found_entry& fe, uint8_t* saved, bool restore) {
    slot_iter it;
    if (iter_seek(it, fe.dir, fe.first_slot) < 0) return fail(error::corrupt);
    uint32_t count = fe.slot - fe.first_slot + 1;
    for (uint32_t k = 0; k < count; ++k) {
        uint8_t* p = iter_slot(it);
        if (!p) return fail(error::corrupt);
        if (restore) {
            p[de_name] = saved[k];
        } else {
            if (saved) saved[k] = p[de_name];
            p[de_name] = slot_deleted;
        }
        if (k + 1 < count && iter_advance(it) <= 0) return fail(error::corrupt);
    }
    return 0;
}

int vfs::delete_entry(const found_entry& fe) { return mark_entry_slots(fe, nullptr, false); }

int vfs::dir_is_empty(uint32_t dir, bool& empty) {
    empty = true;
    slot_iter it;
    iter_start(it, dir);
    found_entry fe;
    for (;;) {
        int r = dir_next(it, fe, false);
        if (r < 0) return fail(error::corrupt);
        if (r == 0) return 0;
        if (!is_dot_entry(entry_ptr(fe.offset))) {
            empty = false;
            return 0;
        }
    }
}

bool vfs::file_open(size_t entry_offset) const {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i)
        if (files_[i].entry_ == entry_offset) return true;
    return false;
}

bool vfs::dir_open(uint32_t first) const {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i)
        if (dirs_[i].first_ == first) return true;
    return false;
}

void vfs::fill_stat(const uint8_t* e, struct stat* buf) const {
    memset(buf, 0, sizeof *buf);
    uint8_t attr = e[de_attr];
    bool ro = (attr & attr_read_only) != 0;
    if (attr & attr_directory) {
        buf->st_mode = s_ifdir | (ro ? perm_dir_read_only : perm_all);
    } else {
        buf->st_mode = s_ifreg | (ro ? perm_read_only : perm_read_write);
        buf->st_size = ld32(e + de_size);
    }
    buf->st_attr = attr;
    buf->st_modified = decode_stamp(ld16(e + de_write_date), ld16(e + de_write_time), 0);
    buf->st_created = decode_stamp(ld16(e + de_create_date), ld16(e + de_create_time), e[de_create_tenth]);
    buf->st_accessed = decode_stamp(ld16(e + de_access_date), 0, 0);
}

int vfs::is_ancestor(uint32_t ancestor, uint32_t dir, bool& result) {
    result = false;
    uint32_t c = dir;
    for (uint32_t steps = 0; steps <= cluster_count_; ++steps) {
        if (c == ancestor) {
            result = true;
            return 0;
        }
        if (c == root_cluster_) return 0;
        uint32_t parent;
        if (dotdot_of(c, parent) < 0) return fail(error::corrupt);
        c = parent;
    }
    return fail(error::corrupt);
}

// K2, K3, K4
int vfs::remove_entry(const lookup& r, bool allow_file, bool allow_dir) {
    if (!r.found) return fail(error::not_found);
    if (r.is_root) return fail(allow_dir ? error::busy : error::is_directory);
    if (is_dot_name(r.name, r.name_len)) return fail(error::invalid);
    const uint8_t* e = entry_ptr(r.entry.offset);
    uint32_t first = entry_cluster(e);
    if (e[de_attr] & attr_directory) {
        if (!allow_dir) return fail(error::is_directory);
        if (dir_open(first)) return fail(error::busy);
        if (valid_cluster(first)) {
            bool empty;
            if (dir_is_empty(first, empty) < 0) return -1;
            if (!empty) return fail(error::not_empty);
        }
    } else {
        if (!allow_file) return fail(error::not_directory);
        if (file_open(r.entry.offset)) return fail(error::busy);
    }
    if (delete_entry(r.entry) < 0) return -1;
    if (valid_cluster(first)) free_chain(first);
    return 0;
}

// ── Public directory and path API (spec §8) ──

int vfs::mkdir(const char* path) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    if (r.found) return fail(error::exists);
    if (!target_valid_for_create()) return fail(error::invalid);
    uint32_t c = alloc_dir_cluster(0);
    if (!c) return fail(error::no_space);
    uint16_t date, time;
    uint8_t tenth;
    stamp_now(date, time, tenth);
    uint8_t* d = cluster_ptr(c);
    for (int k = 0; k < 2; ++k) {
        uint8_t* e = d + k * entry_size;
        memcpy(e, k == 0 ? dot_name : dotdot_name, sfn_length);
        e[de_attr] = attr_directory;
        e[de_create_tenth] = tenth;
        st16(e + de_create_time, time);
        st16(e + de_create_date, date);
        st16(e + de_access_date, date);
        st16(e + de_write_time, time);
        st16(e + de_write_date, date);
        set_entry_cluster(e, k == 0 ? c : (r.parent == root_cluster_ ? 0 : r.parent));  // F13
    }
    found_entry fe;
    if (create_entry(r.parent, attr_directory, c, nullptr, fe) < 0) {
        error e = last_error_;
        free_chain(c);
        last_error_ = e;
        return -1;
    }
    return 0;
}

int vfs::rmdir(const char* path) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    if (r.found && !r.is_root && !(entry_ptr(r.entry.offset)[de_attr] & attr_directory))
        return fail(error::not_directory);
    return remove_entry(r, false, true);
}

int vfs::unlink(const char* path) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    return remove_entry(r, true, false);
}

int vfs::remove(const char* path) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    return remove_entry(r, true, true);
}

int vfs::stat(const char* path, struct stat* buf) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (!buf) return fail(error::invalid);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    if (!r.found) return fail(error::not_found);
    if (r.is_root) {
        memset(buf, 0, sizeof *buf);
        buf->st_mode = s_ifdir | perm_all;
        buf->st_attr = attr_directory;
        return 0;
    }
    fill_stat(entry_ptr(r.entry.offset), buf);
    return 0;
}

// K5, D7
int vfs::rename(const char* old_path, const char* new_path) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (!old_path || !new_path) return fail(error::invalid);
    lookup ro;
    if (resolve(old_path, ro) < 0) return -1;
    if (!ro.found) return fail(error::not_found);
    if (ro.is_root) return fail(error::busy);
    if (is_dot_name(ro.name, ro.name_len)) return fail(error::invalid);
    const found_entry old_fe = ro.entry;
    uint8_t old_copy[entry_size];
    memcpy(old_copy, entry_ptr(old_fe.offset), entry_size);
    const bool old_is_dir = (old_copy[de_attr] & attr_directory) != 0;
    const uint32_t old_cluster = entry_cluster(old_copy);

    lookup rn;
    if (resolve(new_path, rn) < 0) return -1;  // leaves the new name in target_
    if (rn.is_root) return fail(error::busy);
    if (is_dot_name(rn.name, rn.name_len)) return fail(error::invalid);
    if (rn.trailing_slash && !old_is_dir) return fail(error::not_directory);
    if (old_is_dir) {
        bool inside;
        if (is_ancestor(old_cluster, rn.parent, inside) < 0) return -1;
        if (inside) return fail(error::invalid);
    }
    const bool same = rn.found && rn.entry.offset == old_fe.offset;
    const bool replace = rn.found && !same;
    uint32_t replaced_cluster = 0;
    if (replace) {
        const uint8_t* t = entry_ptr(rn.entry.offset);
        bool new_is_dir = (t[de_attr] & attr_directory) != 0;
        if (old_is_dir && !new_is_dir) return fail(error::not_directory);
        if (!old_is_dir && new_is_dir) return fail(error::is_directory);
        replaced_cluster = entry_cluster(t);
        if (new_is_dir) {
            if (dir_open(replaced_cluster)) return fail(error::busy);
            bool empty = true;
            if (valid_cluster(replaced_cluster) && dir_is_empty(replaced_cluster, empty) < 0) return -1;
            if (!empty) return fail(error::not_empty);
        } else if (file_open(rn.entry.offset)) {
            return fail(error::busy);
        }
    }
    if (same && ro.parent == rn.parent && ro.name_len == rn.name_len && memcmp(ro.name, rn.name, ro.name_len) == 0)
        return 0;
    if (!target_valid_for_create()) return fail(error::invalid);

    uint8_t saved_target[max_entry_slots];
    uint8_t saved_old[max_entry_slots];
    if (replace && mark_entry_slots(rn.entry, saved_target, false) < 0) return -1;
    if (mark_entry_slots(old_fe, saved_old, false) < 0) return -1;
    found_entry new_fe;
    if (create_entry(rn.parent, old_copy[de_attr], old_cluster, old_copy, new_fe) < 0) {
        error e = last_error_;
        mark_entry_slots(old_fe, saved_old, true);
        if (replace) mark_entry_slots(rn.entry, saved_target, true);
        last_error_ = e;
        return -1;
    }
    if (replace && valid_cluster(replaced_cluster)) free_chain(replaced_cluster);
    if (old_is_dir && rn.parent != old_fe.dir && valid_cluster(old_cluster)) {
        uint8_t* dd = cluster_ptr(old_cluster) + entry_size;
        if (memcmp(dd, dotdot_name, sfn_length) == 0) set_entry_cluster(dd, rn.parent == root_cluster_ ? 0 : rn.parent);
    }
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i)
        if (files_[i].entry_ == old_fe.offset) files_[i].entry_ = new_fe.offset;  // H5
    return 0;
}

// ── Directory handles (spec K8) ──

dir* vfs::valid_dir(dir* d) const {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i)
        if (d == &dirs_[i]) return dirs_[i].first_ ? d : nullptr;
    return nullptr;
}

dir* vfs::opendir(const char* path) {
    guard g(*this);
    if (!mounted_) {
        fail(error::not_mounted);
        return nullptr;
    }
    lookup r;
    if (resolve(path, r) < 0) return nullptr;
    if (!r.found) {
        fail(error::not_found);
        return nullptr;
    }
    uint32_t first = root_cluster_;
    if (!r.is_root) {
        const uint8_t* e = entry_ptr(r.entry.offset);
        if (!(e[de_attr] & attr_directory)) {
            fail(error::not_directory);
            return nullptr;
        }
        first = entry_cluster(e);
        if (!valid_cluster(first)) {
            fail(error::corrupt);
            return nullptr;
        }
    }
    for (size_t i = 0; i < VFAT32_MAX_OPEN_DIRS; ++i) {
        dir& d = dirs_[i];
        if (d.first_ != 0) continue;
        d.first_ = first;
        d.cluster_ = first;
        d.index_ = 0;
        d.slot_ = 0;
        d.steps_ = 0;
        return &d;
    }
    fail(error::too_many_open);
    return nullptr;
}

dirent* vfs::readdir(dir* dirp) {
    guard g(*this);
    dir* d = valid_dir(dirp);
    if (!d) {
        fail(error::bad_handle);
        return nullptr;
    }
    slot_iter it;
    it.first = d->first_;
    it.cluster = d->cluster_;
    it.index = d->index_;
    it.slot = d->slot_;
    it.steps = d->steps_;
    dirent* result = nullptr;
    for (;;) {
        found_entry fe;
        int r = dir_next(it, fe, true);
        if (r < 0) {
            fail(error::corrupt);
            break;
        }
        if (r == 0) break;
        const uint8_t* e = entry_ptr(fe.offset);
        if (is_dot_entry(e)) continue;
        dirent& out = d->entry_;
        if (utf16_to_utf8(name_, name_len_, out.d_name, sizeof out.d_name) < 0) {  // N7
            uint16_t alias[alias_units];
            uint16_t n = sfn_display(e, alias);
            utf16_to_utf8(alias, n, out.d_name, sizeof out.d_name);
        }
        bool is_dir = (e[de_attr] & attr_directory) != 0;
        out.d_type = is_dir ? dt_dir : dt_reg;
        out.d_size = is_dir ? 0 : ld32(e + de_size);
        result = &out;
        break;
    }
    d->cluster_ = it.cluster;
    d->index_ = it.index;
    d->slot_ = it.slot;
    d->steps_ = it.steps;
    return result;
}

void vfs::rewinddir(dir* dirp) {
    guard g(*this);
    dir* d = valid_dir(dirp);
    if (!d) {
        fail(error::bad_handle);
        return;
    }
    d->cluster_ = d->first_;
    d->index_ = 0;
    d->slot_ = 0;
    d->steps_ = 0;
}

int vfs::closedir(dir* dirp) {
    guard g(*this);
    dir* d = valid_dir(dirp);
    if (!d) return fail(error::bad_handle);
    d->first_ = 0;
    return 0;
}

}  // namespace vfat32
