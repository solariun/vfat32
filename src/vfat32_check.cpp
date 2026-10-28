// vfat32 — maintenance: the stackless tree walk, the integrity check with repair, in-place defragmentation
// (spec §11, §12, D3, D4).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32_internal.hpp"

namespace vfat32 {

using namespace detail;

// ── Stackless pre-order walk (spec C2) ──

// The next entry of the current directory, without "." and "..". 1 entry, 0 end of directory, -1 broken.
int vfs::walk_next(walk_state& w, found_entry& fe) {
    for (;;) {
        int r = dir_next(w.it, fe, false);
        if (r <= 0) return r;
        if (!is_dot_entry(entry_ptr(fe.offset))) return 1;
    }
}

// Back to the parent through "..", positioned after the entry of the directory just finished.
// 1 moved up, 0 the root is finished, -1 broken.
int vfs::walk_up(walk_state& w) {
    uint32_t child = w.it.first;
    if (child == root_cluster_) return 0;
    uint32_t parent;
    if (dotdot_of(child, parent) < 0) return -1;
    iter_start(w.it, parent);
    for (;;) {
        found_entry fe;
        if (walk_next(w, fe) <= 0) return -1;
        const uint8_t* e = entry_ptr(fe.offset);
        if ((e[de_attr] & attr_directory) && entry_cluster(e) == child) return 1;
    }
}

// ── Marks (spec D3) ──

void vfs::clear_marks() {
    for (uint32_t c = first_data_cluster; c <= max_cluster_; ++c) {
        uint32_t raw = fat_raw(c);
        if (raw & fat_mark) fat_raw_set(c, raw & ~fat_mark);
    }
}

void vfs::clear_reserved(uint32_t cluster) {
    for (uint32_t k = 0; k < fat_copies_; ++k) {
        uint8_t* p = base_ + fat_offset_ + k * fat_bytes_ + static_cast<size_t>(cluster) * fat_entry_size;
        st32(p, ld32(p) & fat_mask);
    }
}

// Frees a chain whose clusters carry marks (repair of extra clusters).
void vfs::free_marked_chain(uint32_t c) {
    for (uint32_t steps = 0; valid_cluster(c) && steps < cluster_count_; ++steps) {
        uint32_t v = fat_get(c);
        fat_set(c, fat_free);
        fat_raw_set(c, fat_free);
        if (is_eoc(v) || !valid_cluster(v)) break;
        c = v;
    }
}

// Walks and marks one chain. A link out of range, into a free or bad cluster (bad chain) or into a marked cluster
// (cross-link or loop) ends it; with repair the chain is cut before that link.
void vfs::mark_chain(const found_entry* fe, uint32_t first, bool repair, check_report& r, uint32_t& length,
                     bool& broken) {
    length = 0;
    broken = false;
    bool fragmented = false;
    uint32_t prev = 0, c = first;
    for (;;) {
        bool bad = !valid_cluster(c);
        bool cross = false;
        uint32_t raw = 0;
        if (!bad) {
            raw = fat_raw(c);
            cross = (raw & fat_mark) != 0;
            uint32_t v = raw & fat_mask;
            bad = !cross && (v == fat_free || v == fat_bad);
        }
        if (bad || cross) {
            if (bad)
                ++r.bad_chains;
            else
                ++r.cross_links;
            broken = true;
            if (repair) {
                if (prev)
                    fat_set(prev, fat_eoc);
                else if (fe)
                    set_entry_cluster(entry_ptr(fe->offset), 0);
                ++r.repaired;
            }
            break;
        }
        fat_raw_set(c, raw | fat_mark);
        ++length;
        if (prev && c != prev + 1) fragmented = true;
        uint32_t v = raw & fat_mask;
        if (is_eoc(v)) break;
        prev = c;
        c = v;
    }
    if (fragmented) ++r.fragmented;
}

// ── Integrity check (spec §11) ──

int vfs::check_pass(check_report& r, bool repair) {
    memset(&r, 0, sizeof r);
    const uint32_t mask = cluster_size_ - 1;

    // C1: reserved bits; the free count FSInfo should hold
    uint32_t initial_free = 0;
    for (uint32_t c = first_data_cluster; c <= max_cluster_; ++c) {
        uint32_t raw = fat_raw(c);
        if ((raw & fat_mask) == fat_free) ++initial_free;
        if (raw & fat_reserved_bits) {
            ++r.reserved_bits;
            if (repair) {
                clear_reserved(c);
                ++r.repaired;
            }
        }
    }
    if (fsinfo_offset_ && ld32(base_ + fsinfo_offset_ + fsinfo_free) != initial_free) ++r.fsinfo_errors;

    const bool walk = repair || r.reserved_bits == 0;
    bool complete = walk;
    if (walk) {
        uint32_t len;
        bool broken;
        mark_chain(nullptr, root_cluster_, repair, r, len, broken);
        bool walking = !(broken && !repair);  // a broken root chain cannot be read safely
        if (!walking) complete = false;
        walk_state w;
        iter_start(w.it, root_cluster_);
        while (walking) {  // a subtree that cannot be entered clears `complete` but the walk goes on
            found_entry fe;
            int res = walk_next(w, fe);
            if (res < 0) {
                complete = false;
                break;
            }
            if (res == 0) {
                int up = walk_up(w);
                if (up < 0) complete = false;
                if (up <= 0) break;
                continue;
            }
            uint8_t* e = entry_ptr(fe.offset);
            uint32_t first = entry_cluster(e);
            uint32_t size = ld32(e + de_size);
            if (e[de_attr] & attr_directory) {
                ++r.directories;
                if (size != 0) {
                    ++r.size_errors;
                    if (repair) {
                        st32(e + de_size, 0);
                        ++r.repaired;
                    }
                }
                if (!valid_cluster(first) || (fat_raw(first) & fat_mark)) {  // no cluster, or another's
                    if (!valid_cluster(first))
                        ++r.bad_entries;
                    else
                        ++r.cross_links;
                    if (repair) {
                        delete_entry(fe);
                        ++r.repaired;
                    }
                    continue;
                }
                mark_chain(&fe, first, repair, r, len, broken);
                if (broken && !repair) {
                    complete = false;
                    continue;
                }
                if (len == 0) {  // head unusable: the entry was cut to nothing
                    if (repair) delete_entry(fe);
                    continue;
                }
                uint8_t* d = cluster_ptr(first);
                if (memcmp(d, dot_name, sfn_length) != 0 || memcmp(d + entry_size, dotdot_name, sfn_length) != 0) {
                    ++r.bad_entries;  // no "." / "..": the walk could not come back up
                    complete = false;
                    continue;
                }
                if (entry_cluster(d) != first) {
                    ++r.bad_entries;
                    if (repair) {
                        set_entry_cluster(d, first);
                        ++r.repaired;
                    }
                }
                uint32_t want = w.it.first == root_cluster_ ? 0 : w.it.first;
                uint32_t dd = entry_cluster(d + entry_size);
                if (dd != want && !(want == 0 && dd == root_cluster_)) {
                    ++r.bad_entries;
                    if (!repair) {
                        complete = false;
                        continue;
                    }
                    set_entry_cluster(d + entry_size, want);
                    ++r.repaired;
                }
                iter_start(w.it, first);  // descend
            } else {
                ++r.files;
                if (first == 0) {
                    if (size != 0) {
                        ++r.size_errors;
                        if (repair) {
                            st32(e + de_size, 0);
                            ++r.repaired;
                        }
                    }
                    continue;
                }
                mark_chain(&fe, first, repair, r, len, broken);
                uint32_t expected = (size >> cluster_shift_) + ((size & mask) ? 1 : 0);
                if (len == expected) continue;
                if (!broken) ++r.size_errors;
                if (!repair) continue;
                if (len < expected) {
                    uint64_t s = static_cast<uint64_t>(len) << cluster_shift_;
                    st32(e + de_size, s > max_file_size ? max_file_size : static_cast<uint32_t>(s));
                } else if (expected == 0) {
                    set_entry_cluster(e, 0);
                    free_marked_chain(first);
                } else {
                    uint32_t c = first;
                    for (uint32_t i = 1; i < expected; ++i) c = fat_get(c);
                    uint32_t tail = fat_get(c);
                    fat_set(c, fat_eoc);
                    free_marked_chain(tail);
                }
                ++r.repaired;
            }
        }
    }

    // Lost clusters and the information counters
    bool prev_free = false;
    for (uint32_t c = first_data_cluster; c <= max_cluster_; ++c) {
        uint32_t raw = fat_raw(c);
        uint32_t v = raw & fat_mask;
        bool now_free = v == fat_free;
        if (!now_free) {
            if (v == fat_bad) {
                ++r.bad_clusters;
            } else if (!walk || (raw & fat_mark)) {
                ++r.used_clusters;
            } else {
                ++r.lost_clusters;
                if (repair && complete) {
                    fat_set(c, fat_free);
                    ++r.repaired;
                    now_free = true;
                }
            }
        }
        if (now_free) {
            ++r.free_clusters;
            if (!prev_free) ++r.free_extents;
        }
        prev_free = now_free;
    }
    if (walk) clear_marks();  // without a walk nothing was marked: bit 31 belongs to the image (C1, C4)

    // FAT copies
    if (mirror_ && fat_copies_ > 1) {
        const uint8_t* first_copy = base_ + fat_offset_;
        for (uint32_t k = 1; k < fat_copies_; ++k) {
            uint8_t* copy = base_ + fat_offset_ + k * fat_bytes_;
            uint32_t mismatches = 0;
            for (uint32_t c = 0; c <= max_cluster_; ++c)
                if (ld32(first_copy + c * fat_entry_size) != ld32(copy + c * fat_entry_size)) ++mismatches;
            r.fat_mismatches += mismatches;
            if (repair && mismatches) {
                memcpy(copy, first_copy, fat_bytes_);
                ++r.repaired;
            }
        }
    }

    if (repair) {
        free_count_ = r.free_clusters;
        if (r.fsinfo_errors) ++r.repaired;
        write_fsinfo();
    }
    return 0;
}

int vfs::check(check_report* report, bool repair) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (!report) return fail(error::invalid);
    if (repair && any_open()) return fail(error::busy);  // C4
    check_pass(*report, repair);
    if (repair) {  // C3: a second pass decides
        check_report after;
        check_pass(after, false);
        report->files = after.files;
        report->directories = after.directories;
        report->used_clusters = after.used_clusters;
        report->free_clusters = after.free_clusters;
        report->bad_clusters = after.bad_clusters;
        report->free_extents = after.free_extents;
        report->fragmented = after.fragmented;
        return after.errors() ? fail(error::corrupt) : 0;
    }
    return report->errors() ? fail(error::corrupt) : 0;
}

// ── Defragmentation (spec §12) ──

bool vfs::ref_is_dir_head(const referrer& ref) const {
    if (ref.kind == ref_bpb) return true;
    if (ref.kind == ref_entry) return (entry_ptr(ref.where)[de_attr] & attr_directory) != 0;
    return false;
}

// D-4: the referrer now stores `value`; a referrer stored inside cluster x or y moved with its cluster.
void vfs::apply_ref(const referrer& ref, uint32_t x, uint32_t y, uint32_t value) {
    if (ref.kind == ref_fat) {
        uint32_t p = static_cast<uint32_t>(ref.where);
        if (p != x && p != y) fat_set(p, value);  // a referrer in x or y was relabelled with the FAT entries
    } else if (ref.kind == ref_entry) {
        size_t off = ref.where;
        size_t ox = cluster_offset(x), oy = cluster_offset(y);
        if (off >= ox && off < ox + cluster_size_)
            off = off - ox + oy;
        else if (off >= oy && off < oy + cluster_size_)
            off = off - oy + ox;
        set_entry_cluster(entry_ptr(off), value);
    } else {
        write_root_cluster(value);
    }
}

// A directory's first cluster is now `head`: rewrite its "." and its children's "..".
void vfs::fix_dir_head(uint32_t head) {
    uint8_t* d = cluster_ptr(head);
    if (head != root_cluster_ && memcmp(d, dot_name, sfn_length) == 0) set_entry_cluster(d, head);
    uint32_t self = head == root_cluster_ ? 0 : head;
    slot_iter it;
    iter_start(it, head);
    found_entry fe;
    while (dir_next(it, fe, false) == 1) {
        const uint8_t* e = entry_ptr(fe.offset);
        if (!(e[de_attr] & attr_directory) || is_dot_entry(e)) continue;
        uint32_t child = entry_cluster(e);
        if (!valid_cluster(child)) continue;
        uint8_t* dd = cluster_ptr(child) + entry_size;
        if (memcmp(dd, dotdot_name, sfn_length) == 0) set_entry_cluster(dd, self);
    }
}

int vfs::find_head_entry(uint32_t cluster, size_t& offset) {
    walk_state w;
    iter_start(w.it, root_cluster_);
    for (;;) {
        found_entry fe;
        int res = walk_next(w, fe);
        if (res < 0) return -1;
        if (res == 0) {
            int up = walk_up(w);
            if (up <= 0) return up;
            continue;
        }
        const uint8_t* e = entry_ptr(fe.offset);
        uint32_t c = entry_cluster(e);
        if (c == cluster) {
            offset = fe.offset;
            return 1;
        }
        if ((e[de_attr] & attr_directory) && valid_cluster(c)) iter_start(w.it, c);
    }
}

// D-3
int vfs::find_referrer(uint32_t cluster, referrer& ref) {
    for (uint32_t p = first_data_cluster; p <= max_cluster_; ++p) {
        if (fat_get(p) == cluster) {
            ref.kind = ref_fat;
            ref.where = p;
            return 0;
        }
    }
    if (cluster == root_cluster_) {
        ref.kind = ref_bpb;
        ref.where = 0;
        return 0;
    }
    size_t offset;
    if (find_head_entry(cluster, offset) == 1) {
        ref.kind = ref_entry;
        ref.where = offset;
        return 0;
    }
    return fail(error::corrupt);
}

void vfs::swap_clusters(uint32_t x, uint32_t y) {
    uint8_t tmp[swap_chunk];
    uint8_t* a = cluster_ptr(x);
    uint8_t* b = cluster_ptr(y);
    for (size_t i = 0; i < cluster_size_; i += swap_chunk) {
        memcpy(tmp, a + i, swap_chunk);
        memcpy(a + i, b + i, swap_chunk);
        memcpy(b + i, tmp, swap_chunk);
    }
}

// Puts the data of cluster x into cluster y (moving into free space, or swapping with y's owner).
int vfs::move_cluster(uint32_t x, uint32_t y, const referrer& rx) {
    bool x_dir = ref_is_dir_head(rx);
    if (fat_get(y) == fat_free) {
        memcpy(cluster_ptr(y), cluster_ptr(x), cluster_size_);
        fat_set(y, fat_get(x));
        fat_set(x, fat_free);
        apply_ref(rx, x, y, y);
        if (x_dir) fix_dir_head(y);
        return 0;
    }
    referrer ry;
    if (find_referrer(y, ry) < 0) return -1;
    bool y_dir = ref_is_dir_head(ry);
    uint32_t fx = fat_get(x), fy = fat_get(y);
    swap_clusters(x, y);
    fat_set(x, fy == x ? y : (fy == y ? x : fy));  // D-4: FAT[x] <- sigma(FAT[y])
    fat_set(y, fx == x ? y : (fx == y ? x : fx));  //      FAT[y] <- sigma(FAT[x])
    apply_ref(rx, x, y, y);
    apply_ref(ry, x, y, x);
    if (x_dir) fix_dir_head(y);
    if (y_dir) fix_dir_head(x);
    return 0;
}

// Places one chain at t, t+1, … (D-2, D-5).
int vfs::place_chain(referrer ref, uint32_t head, uint32_t& t, uint32_t& moves, uint32_t max_moves, bool& stopped) {
    uint32_t c = head;
    for (uint32_t steps = 0; steps <= cluster_count_; ++steps) {
        while (t <= max_cluster_ && fat_get(t) == fat_bad) ++t;
        if (t > max_cluster_) return fail(error::corrupt);
        if (c != t) {
            if (max_moves && moves >= max_moves) {
                stopped = true;
                return 0;
            }
            if (move_cluster(c, t, ref) < 0) return -1;
            ++moves;
            c = t;
        }
        uint32_t v = fat_get(c);
        ++t;
        if (is_eoc(v)) return 0;
        if (!valid_cluster(v)) return fail(error::corrupt);
        ref.kind = ref_fat;
        ref.where = c;
        c = v;
    }
    return fail(error::corrupt);
}

int vfs::defrag(uint32_t max_moves) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (any_open()) return fail(error::busy);  // D-1
    check_report r;
    check_pass(r, false);
    if (r.errors()) return fail(error::corrupt);

    uint32_t t = first_data_cluster, moves = 0;
    bool stopped = false;
    referrer root_ref;
    root_ref.kind = ref_bpb;
    root_ref.where = 0;
    if (place_chain(root_ref, root_cluster_, t, moves, max_moves, stopped) < 0) return -1;
    if (stopped) return 1;

    walk_state w;
    iter_start(w.it, root_cluster_);
    for (;;) {
        found_entry fe;
        int res = walk_next(w, fe);
        if (res < 0) return fail(error::corrupt);
        if (res == 0) {
            int up = walk_up(w);
            if (up < 0) return fail(error::corrupt);
            if (up == 0) break;
            continue;
        }
        const uint8_t* e = entry_ptr(fe.offset);
        uint32_t first = entry_cluster(e);
        if (first != 0) {
            referrer ref;
            ref.kind = ref_entry;
            ref.where = fe.offset;
            if (place_chain(ref, first, t, moves, max_moves, stopped) < 0) return -1;
            if (stopped) return 1;
        }
        if (e[de_attr] & attr_directory) iter_start(w.it, entry_cluster(e));
    }
    next_free_ = valid_cluster(t) ? t : first_data_cluster;
    write_fsinfo();
    return 0;
}

}  // namespace vfat32
