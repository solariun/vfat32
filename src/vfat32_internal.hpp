// vfat32 — internal definitions shared by the implementation files (not part of the API).
// On-disk constants follow spec §4; every field is accessed byte by byte, little-endian (spec X2).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#ifndef VFAT32_INTERNAL_HPP
#define VFAT32_INTERNAL_HPP

#include "vfat32.hpp"

#include <string.h>

namespace vfat32 {
namespace detail {

// ── Sectors and the boot sector (spec §4.2) ──
constexpr uint32_t min_sector_size = 512;
constexpr uint32_t max_sector_size = 4096;
constexpr uint32_t max_cluster_bytes_read = 65536;   // accepted when reusing (P5)
constexpr uint32_t max_cluster_bytes_format = 32768; // written by format (F2)
constexpr size_t boot_signature_offset = 510;
constexpr uint8_t boot_signature_low = 0x55;
constexpr uint8_t boot_signature_high = 0xAA;
constexpr uint8_t jump_short = 0xEB;
constexpr uint8_t jump_near = 0xE9;
constexpr uint8_t jump_nop = 0x90;
constexpr uint8_t jump_target = 0x58;  // start of the boot code area of a FAT32 boot sector

constexpr size_t bpb_jump = 0;
constexpr size_t bpb_oem = 3;
constexpr size_t bpb_bytes_per_sector = 11;
constexpr size_t bpb_sectors_per_cluster = 13;
constexpr size_t bpb_reserved_sectors = 14;
constexpr size_t bpb_fat_count = 16;
constexpr size_t bpb_root_entries = 17;
constexpr size_t bpb_total_sectors16 = 19;
constexpr size_t bpb_media = 21;
constexpr size_t bpb_fat_size16 = 22;
constexpr size_t bpb_sectors_per_track = 24;
constexpr size_t bpb_heads = 26;
constexpr size_t bpb_hidden_sectors = 28;
constexpr size_t bpb_total_sectors32 = 32;
constexpr size_t bpb_fat_size32 = 36;
constexpr size_t bpb_ext_flags = 40;
constexpr size_t bpb_fs_version = 42;
constexpr size_t bpb_root_cluster = 44;
constexpr size_t bpb_fsinfo_sector = 48;
constexpr size_t bpb_backup_boot_sector = 50;
constexpr size_t bpb_drive_number = 64;
constexpr size_t bpb_boot_signature = 66;
constexpr size_t bpb_volume_id = 67;
constexpr size_t bpb_volume_label = 71;
constexpr size_t bpb_fs_type = 82;

constexpr uint8_t media_fixed = 0xF8;
constexpr uint8_t media_removable = 0xF0;
constexpr uint8_t drive_number_fixed = 0x80;
constexpr uint8_t extended_boot_signature = 0x29;
constexpr uint16_t geometry_sectors_per_track = 63;
constexpr uint16_t geometry_heads = 255;
constexpr uint16_t ext_flags_no_mirror = 0x0080;
constexpr uint16_t ext_flags_active_mask = 0x000F;
constexpr size_t oem_length = 8;
constexpr size_t label_length = 11;
constexpr size_t fs_type_length = 8;

constexpr uint32_t fsinfo_sector = 1;
constexpr uint32_t backup_boot_sector = 6;
constexpr uint32_t reserved_for_backup = 8;  // sectors needed for the backup at 6 and 7 (F3)
constexpr uint32_t reserved_minimum = 2;     // boot sector + FSInfo
constexpr uint32_t reserved_small = 8;
constexpr uint32_t reserved_large = 32;
constexpr uint32_t tiny_volume_sectors = 128;
constexpr uint32_t small_volume_sectors = 4096;

// Microsoft's FAT32 cluster-size table, in 512-byte sectors (F2)
constexpr uint64_t table_limit_260m = 532480;
constexpr uint64_t table_limit_8g = 16777216;
constexpr uint64_t table_limit_16g = 33554432;
constexpr uint64_t table_limit_32g = 67108864;
constexpr uint32_t cluster_512 = 512;
constexpr uint32_t cluster_4k = 4096;
constexpr uint32_t cluster_8k = 8192;
constexpr uint32_t cluster_16k = 16384;
constexpr uint32_t cluster_32k = 32768;

// ── FSInfo (spec §4.3) ──
constexpr uint32_t fsinfo_lead_signature = 0x41615252;
constexpr uint32_t fsinfo_struct_signature = 0x61417272;
constexpr uint32_t fsinfo_trail_signature = 0xAA550000;
constexpr size_t fsinfo_lead = 0;
constexpr size_t fsinfo_struct = 484;
constexpr size_t fsinfo_free = 488;
constexpr size_t fsinfo_next = 492;
constexpr size_t fsinfo_trail = 508;

// ── FAT entries (spec §4.4) ──
constexpr uint32_t fat_entry_size = 4;
constexpr uint32_t fat_mask = 0x0FFFFFFF;
constexpr uint32_t fat_reserved_bits = 0xF0000000;
constexpr uint32_t fat_mark = 0x80000000;  // check/defrag "reached" mark (spec D3)
constexpr uint32_t fat_free = 0;
constexpr uint32_t fat_bad = 0x0FFFFFF7;
constexpr uint32_t fat_eoc_min = 0x0FFFFFF8;
constexpr uint32_t fat_eoc = 0x0FFFFFFF;
constexpr uint32_t fat_media_base = 0x0FFFFF00;
constexpr uint32_t fat_clean_bit = 0x08000000;
constexpr uint32_t first_data_cluster = 2;
constexpr uint32_t max_cluster_count = 0x0FFFFFF5;
constexpr uint32_t min_cluster_count = 2;  // root + one (F6)

// ── Directory entries (spec §4.5, §4.6) ──
constexpr uint32_t entry_size = 32;
constexpr uint32_t entry_shift = 5;
constexpr uint32_t max_dir_slots = 65536;
constexpr size_t de_name = 0;
constexpr size_t de_attr = 11;
constexpr size_t de_nt_flags = 12;
constexpr size_t de_create_tenth = 13;
constexpr size_t de_create_time = 14;
constexpr size_t de_create_date = 16;
constexpr size_t de_access_date = 18;
constexpr size_t de_cluster_high = 20;
constexpr size_t de_write_time = 22;
constexpr size_t de_write_date = 24;
constexpr size_t de_cluster_low = 26;
constexpr size_t de_size = 28;
constexpr size_t sfn_length = 11;
constexpr size_t sfn_base_length = 8;
constexpr size_t sfn_ext_length = 3;

constexpr uint8_t slot_end = 0x00;
constexpr uint8_t slot_deleted = 0xE5;
constexpr uint8_t slot_kanji_e5 = 0x05;
constexpr uint8_t attr_lfn = 0x0F;
constexpr uint8_t attr_lfn_mask = 0x3F;
constexpr uint8_t nt_lower_base = 0x08;
constexpr uint8_t nt_lower_ext = 0x10;

constexpr uint8_t lfn_last = 0x40;
constexpr uint8_t lfn_order_mask = 0x3F;
constexpr uint8_t lfn_max_slots = 20;
constexpr uint32_t lfn_units_per_slot = 13;
constexpr size_t lfn_type = 12;
constexpr size_t lfn_checksum = 13;
constexpr size_t lfn_cluster = 26;
constexpr uint16_t lfn_pad = 0xFFFF;
constexpr uint32_t max_lfn_units = 255;
constexpr uint32_t max_entry_slots = lfn_max_slots + 1;
// byte offsets of the 13 UTF-16 units inside an LFN slot
constexpr uint8_t lfn_unit_offset[lfn_units_per_slot] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

constexpr uint8_t dot_name[sfn_length] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
constexpr uint8_t dotdot_name[sfn_length] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

constexpr uint32_t sfn_hash_attempts = 65536;
constexpr uint32_t sfn_plain_tails = 4;

// ── Time stamps (spec §4.8) ──
constexpr uint16_t fat_epoch_year = 1980;
constexpr uint16_t fat_last_year = 2107;
constexpr uint8_t tenth_per_odd_second = 100;

// ── Handles ──
constexpr uint32_t no_cache = 0xFFFFFFFF;
constexpr uint32_t max_file_size = 0xFFFFFFFF;
constexpr uint32_t corrupt_steps = 0xFFFFFFFF;  // slot_iter::steps after a broken chain

constexpr uint8_t fl_read = 0x01;
constexpr uint8_t fl_write = 0x02;
constexpr uint8_t fl_append = 0x04;
constexpr uint8_t fl_eof = 0x08;
constexpr uint8_t fl_error = 0x10;
constexpr uint8_t fl_modified = 0x20;

constexpr uint8_t st_idle = 0;
constexpr uint8_t st_reading = 1;
constexpr uint8_t st_writing = 2;

constexpr uint8_t ref_fat = 0;
constexpr uint8_t ref_entry = 1;
constexpr uint8_t ref_bpb = 2;

constexpr size_t swap_chunk = 32;

// ── Little-endian access (spec X2) ──
inline uint16_t ld16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t ld32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline void st16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void st32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline bool is_power_of_two(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }
inline uint32_t log2_of(uint32_t v) {
    uint32_t s = 0;
    while ((1u << s) < v) ++s;
    return s;
}
inline uint16_t ascii_upper(uint16_t u) { return (u >= 'a' && u <= 'z') ? static_cast<uint16_t>(u - ('a' - 'A')) : u; }
inline uint16_t ascii_lower(uint16_t u) { return (u >= 'A' && u <= 'Z') ? static_cast<uint16_t>(u + ('a' - 'A')) : u; }

}  // namespace detail

// RAII lock guard used by every public member (spec T2).
class vfs::guard {
public:
    explicit guard(const vfs& v) : lock_(v.lock_) {
        if (lock_) lock_->lock();
    }
    ~guard() {
        if (lock_) lock_->unlock();
    }

private:
    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;
    lock_interface* lock_;
};

struct vfs::walk_state {
    slot_iter it;
};

}  // namespace vfat32

#endif  // VFAT32_INTERNAL_HPP
