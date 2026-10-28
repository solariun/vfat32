// vfat32 — names: UTF-8 ↔ UTF-16, name rules, 8.3 fit and alias basis, LFN checksum (spec §4.6, §4.7).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32_internal.hpp"

namespace vfat32 {

using namespace detail;

namespace {

constexpr uint32_t max_code_point = 0x10FFFF;
constexpr uint32_t surrogate_first = 0xD800;
constexpr uint32_t surrogate_last = 0xDFFF;
constexpr uint32_t high_surrogate_last = 0xDBFF;
constexpr uint32_t supplementary_base = 0x10000;
constexpr uint32_t two_byte_min = 0x80;
constexpr uint32_t three_byte_min = 0x800;
constexpr uint8_t last_lead_byte = 0xF4;
constexpr uint16_t first_printable = 0x20;
constexpr uint16_t ascii_limit = 0x80;
constexpr uint16_t fnv_offset = 0x811C;
constexpr uint16_t fnv_prime = 0x0193;

const char lfn_forbidden[] = "\"*/:<>?\\|";
const char sfn_specials[] = "!#$%&'()-@^_`{}~";

// Valid in an 8.3 name as stored (upper case, ASCII OEM, spec D13).
bool sfn_char(uint16_t u) {
    if (u >= ascii_limit || u <= ' ') return false;
    if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) return true;
    return strchr(sfn_specials, static_cast<char>(u)) != nullptr;
}

}  // namespace

// N1, N2: decode one UTF-8 component into target_.
int vfs::set_target(const char* name, size_t len) {
    const size_t cap = sizeof target_ / sizeof target_[0];
    if (len > VFAT32_NAME_MAX) return fail(error::name_too_long);
    const uint8_t* s = reinterpret_cast<const uint8_t*>(name);
    size_t units = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t b = s[i];
        uint32_t cp, minimum;
        size_t n;
        if (b < two_byte_min) {
            cp = b;
            n = 1;
            minimum = 0;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            n = 2;
            minimum = two_byte_min;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            n = 3;
            minimum = three_byte_min;
        } else if ((b & 0xF8) == 0xF0 && b <= last_lead_byte) {
            cp = b & 0x07;
            n = 4;
            minimum = supplementary_base;
        } else {
            return fail(error::invalid);
        }
        if (i + n > len) return fail(error::invalid);
        for (size_t k = 1; k < n; ++k) {
            uint8_t c = s[i + k];
            if ((c & 0xC0) != 0x80) return fail(error::invalid);
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < minimum || cp > max_code_point || (cp >= surrogate_first && cp <= surrogate_last) || cp == 0)
            return fail(error::invalid);
        size_t need = cp >= supplementary_base ? 2 : 1;
        if (units + need > cap || units + need > max_lfn_units) return fail(error::name_too_long);
        if (need == 2) {
            uint32_t v = cp - supplementary_base;
            target_[units++] = static_cast<uint16_t>(surrogate_first + (v >> 10));
            target_[units++] = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
        } else {
            target_[units++] = static_cast<uint16_t>(cp);
        }
        i += n;
    }
    if (units == 0) return fail(error::invalid);
    target_len_ = static_cast<uint16_t>(units);
    return 0;
}

bool vfs::target_is_dot() const {
    return (target_len_ == 1 && target_[0] == '.') || (target_len_ == 2 && target_[0] == '.' && target_[1] == '.');
}

// N3
bool vfs::target_valid_for_create() const {
    if (target_len_ == 0 || target_is_dot()) return false;
    for (uint16_t i = 0; i < target_len_; ++i) {
        uint16_t u = target_[i];
        if (u < first_printable) return false;
        if (u < ascii_limit && strchr(lfn_forbidden, static_cast<char>(u))) return false;
    }
    uint16_t last = target_[target_len_ - 1];
    return last != '.' && last != ' ';
}

// N4: the name fits 8.3 exactly (with the NT case flags).
bool vfs::target_fits_83(uint8_t sfn[11], uint8_t& nt_flags) const {
    int dot = -1;
    for (uint16_t i = 0; i < target_len_; ++i) {
        if (target_[i] == '.') {
            if (dot >= 0) return false;
            dot = i;
        }
    }
    if (dot == 0) return false;
    uint16_t base_len = dot < 0 ? target_len_ : static_cast<uint16_t>(dot);
    uint16_t ext_len = dot < 0 ? 0 : static_cast<uint16_t>(target_len_ - dot - 1);
    if (base_len < 1 || base_len > sfn_base_length || ext_len > sfn_ext_length) return false;
    if (dot >= 0 && ext_len == 0) return false;
    bool base_upper = false, base_lower = false, ext_upper = false, ext_lower = false;
    memset(sfn, ' ', sfn_length);
    for (uint16_t i = 0; i < target_len_; ++i) {
        if (static_cast<int>(i) == dot) continue;
        uint16_t u = target_[i];
        bool in_ext = dot >= 0 && static_cast<int>(i) > dot;
        if (u >= 'a' && u <= 'z') (in_ext ? ext_lower : base_lower) = true;
        if (u >= 'A' && u <= 'Z') (in_ext ? ext_upper : base_upper) = true;
        uint16_t up = ascii_upper(u);
        if (!sfn_char(up)) return false;
        if (in_ext)
            sfn[sfn_base_length + (i - dot - 1)] = static_cast<uint8_t>(up);
        else
            sfn[i] = static_cast<uint8_t>(up);
    }
    if ((base_upper && base_lower) || (ext_upper && ext_lower)) return false;
    nt_flags = static_cast<uint8_t>((base_lower ? nt_lower_base : 0) | (ext_lower ? nt_lower_ext : 0));
    return true;
}

// N5: the basis name of the 8.3 alias; lossy when anything was dropped, replaced or cut.
void vfs::target_basis(uint8_t sfn[11], bool& lossy) const {
    memset(sfn, ' ', sfn_length);
    lossy = false;
    uint16_t start = 0;
    while (start < target_len_ && (target_[start] == '.' || target_[start] == ' ')) {
        ++start;
        lossy = true;
    }
    int last_dot = -1;
    for (uint16_t i = start; i < target_len_; ++i)
        if (target_[i] == '.') last_dot = i;
    uint16_t base_end = last_dot < 0 ? target_len_ : static_cast<uint16_t>(last_dot);

    size_t n = 0;
    for (uint16_t i = start; i < base_end; ++i) {
        uint16_t u = target_[i];
        if (u == ' ' || u == '.') {
            lossy = true;
            continue;
        }
        if (n == sfn_base_length) {
            lossy = true;
            break;
        }
        uint16_t up = ascii_upper(u);
        if (!sfn_char(up)) {
            up = '_';
            lossy = true;
        }
        sfn[n++] = static_cast<uint8_t>(up);
    }
    if (n == 0) {
        sfn[0] = '_';
        lossy = true;
    }
    if (last_dot >= 0) {
        n = 0;
        for (uint16_t i = static_cast<uint16_t>(last_dot + 1); i < target_len_; ++i) {
            uint16_t u = target_[i];
            if (u == ' ') {
                lossy = true;
                continue;
            }
            if (n == sfn_ext_length) {
                lossy = true;
                break;
            }
            uint16_t up = ascii_upper(u);
            if (!sfn_char(up)) {
                up = '_';
                lossy = true;
            }
            sfn[sfn_base_length + n++] = static_cast<uint8_t>(up);
        }
    }
}

uint16_t vfs::target_hash() const {
    uint16_t h = fnv_offset;
    for (uint16_t i = 0; i < target_len_; ++i) h = static_cast<uint16_t>((h ^ ascii_upper(target_[i])) * fnv_prime);
    return h;
}

// F17
uint8_t vfs::sfn_checksum(const uint8_t* sfn) {
    uint8_t sum = 0;
    for (size_t i = 0; i < sfn_length; ++i) sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

// N6: the 8.3 name as displayed (case flags applied); returns its length in units (at most 12).
uint16_t vfs::sfn_display(const uint8_t* e, uint16_t* out) {
    uint16_t n = 0;
    size_t base_len = sfn_base_length;
    while (base_len > 0 && e[base_len - 1] == ' ') --base_len;
    size_t ext_len = sfn_ext_length;
    while (ext_len > 0 && e[sfn_base_length + ext_len - 1] == ' ') --ext_len;
    for (size_t i = 0; i < base_len; ++i) {
        uint8_t c = e[i];
        if (i == 0 && c == slot_kanji_e5) c = slot_deleted;
        uint16_t u = c >= ascii_limit ? '_' : c;
        out[n++] = (e[de_nt_flags] & nt_lower_base) ? ascii_lower(u) : u;
    }
    if (ext_len) {
        out[n++] = '.';
        for (size_t i = 0; i < ext_len; ++i) {
            uint8_t c = e[sfn_base_length + i];
            uint16_t u = c >= ascii_limit ? '_' : c;
            out[n++] = (e[de_nt_flags] & nt_lower_ext) ? ascii_lower(u) : u;
        }
    }
    return n;
}

bool vfs::names_equal(const uint16_t* a, uint16_t alen, const uint16_t* b, uint16_t blen) const {
    if (alen != blen) return false;
    for (uint16_t i = 0; i < alen; ++i)
        if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
    return true;
}

// Returns the UTF-8 length written (NUL-terminated), or -1 if invalid UTF-16 or longer than cap - 1.
int vfs::utf16_to_utf8(const uint16_t* in, uint16_t len, char* out, size_t cap) {
    size_t n = 0;
    for (uint16_t i = 0; i < len; ++i) {
        uint32_t cp = in[i];
        if (cp >= surrogate_first && cp <= surrogate_last) {
            if (cp > high_surrogate_last || i + 1 >= len) return -1;
            uint32_t lo = in[i + 1];
            if (lo < 0xDC00 || lo > surrogate_last) return -1;
            cp = supplementary_base + ((cp - surrogate_first) << 10) + (lo - 0xDC00);
            ++i;
        }
        if (cp == 0) return -1;
        size_t need = cp < two_byte_min ? 1 : cp < three_byte_min ? 2 : cp < supplementary_base ? 3 : 4;
        if (n + need + 1 > cap) return -1;
        if (need == 1) {
            out[n++] = static_cast<char>(cp);
        } else if (need == 2) {
            out[n++] = static_cast<char>(0xC0 | (cp >> 6));
            out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (need == 3) {
            out[n++] = static_cast<char>(0xE0 | (cp >> 12));
            out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out[n++] = static_cast<char>(0xF0 | (cp >> 18));
            out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace vfat32
