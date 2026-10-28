// vfat32 — files: handle rows, the file I/O core, buffered streams, unbuffered descriptors, truncate
// (spec §7, D1, D5, D9, D14).
//
// MIT License — Copyright (c) 2026 Gustavo Campos

#include "vfat32_internal.hpp"

#include <limits.h>

namespace vfat32 {

using namespace detail;

namespace {

constexpr int64_t max_position = 0xFFFFFFFFll;
constexpr uint32_t max_chunk = 0x7FFFFFFFu;  // keeps byte counts representable as long

uint32_t clamp_count(size_t n) {
    uint64_t v = n;  // size_t is 16 bits on AVR
    return v > max_chunk ? max_chunk : static_cast<uint32_t>(v);
}

}  // namespace

// ── Handle rows (spec H1–H3) ──

file* vfs::valid_file(file* f) const {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i)
        if (f == &files_[i]) return files_[i].entry_ ? f : nullptr;
    return nullptr;
}

file* vfs::valid_fd(int fd) {
    if (fd < 0 || fd >= VFAT32_MAX_OPEN_FILES) return nullptr;
    return files_[fd].entry_ ? &files_[fd] : nullptr;
}

file* vfs::new_handle() {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i)
        if (files_[i].entry_ == 0) return &files_[i];
    return nullptr;
}

file* vfs::open_common(const char* path, uint8_t access, bool create, bool excl, bool trunc, bool append,
                       bool stream) {
    if (!mounted_) {
        fail(error::not_mounted);
        return nullptr;
    }
    if (!path) {
        fail(error::invalid);
        return nullptr;
    }
    file* h = new_handle();
    if (!h) {
        fail(error::too_many_open);
        return nullptr;
    }
    lookup r;
    if (resolve(path, r) < 0) return nullptr;
    size_t offset;
    bool truncated = false;
    if (r.found) {
        if (r.is_root || (entry_ptr(r.entry.offset)[de_attr] & attr_directory)) {
            fail(error::is_directory);
            return nullptr;
        }
        if (create && excl) {
            fail(error::exists);
            return nullptr;
        }
        if ((access & fl_write) && (entry_ptr(r.entry.offset)[de_attr] & attr_read_only)) {  // D10
            fail(error::access);
            return nullptr;
        }
        offset = r.entry.offset;
        if (trunc && (access & fl_write) && ld32(entry_ptr(offset) + de_size) != 0) {
            if (truncate_entry(offset, 0, nullptr) < 0) return nullptr;
            truncated = true;
        }
    } else {
        if (!create) {
            fail(error::not_found);
            return nullptr;
        }
        if (r.trailing_slash) {
            fail(error::is_directory);
            return nullptr;
        }
        if (!target_valid_for_create()) {
            fail(error::invalid);
            return nullptr;
        }
        found_entry fe;
        if (create_entry(r.parent, attr_archive, 0, nullptr, fe) < 0) return nullptr;
        offset = fe.offset;
    }
    h->entry_ = offset;
    h->pos_ = 0;
    h->cache_index_ = no_cache;
    h->cache_cluster_ = 0;
    h->buf_start_ = 0;
    h->buf_len_ = 0;
    h->unget_ = -1;
    h->buf_state_ = st_idle;
    h->flags_ = static_cast<uint8_t>(access | (append ? fl_append : 0) | (truncated ? fl_modified : 0));
#if VFAT32_BUFFER_SIZE > 0
    if (stream) {
        h->buf_ = h->storage_;
        h->buf_size_ = VFAT32_BUFFER_SIZE;
        h->buf_mode_ = io_fbf;
    } else
#endif
    {
        (void)stream;
        h->buf_ = nullptr;
        h->buf_size_ = 0;
        h->buf_mode_ = io_nbf;
    }
    return h;
}

// ── File I/O core (spec §7.2, W1–W5) ──

// The cluster at chain index `index`, through the handle's cache; with `allocate` the chain grows.
int vfs::cluster_for(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t index, bool allocate,
                     uint32_t& cluster) {
    uint8_t* e = entry_ptr(entry);
    uint32_t first = entry_cluster(e);
    if (first == 0) {
        if (!allocate) return fail(error::corrupt);
        first = alloc_cluster(next_free_);
        if (!first) return fail(error::no_space);
        set_entry_cluster(e, first);
        cache_index = 0;
        cache_cluster = first;
    }
    uint32_t cur = first, idx = 0;
    if (cache_index != no_cache && cache_index <= index && valid_cluster(cache_cluster)) {
        cur = cache_cluster;
        idx = cache_index;
    }
    if (!valid_cluster(cur)) return fail(error::corrupt);
    while (idx < index) {
        uint32_t v = fat_get(cur);
        if (is_eoc(v)) {
            if (!allocate) return fail(error::corrupt);
            v = alloc_cluster(cur + 1);  // F11: contiguity
            if (!v) return fail(error::no_space);
            fat_set(cur, v);
        } else if (!valid_cluster(v)) {
            return fail(error::corrupt);
        }
        cur = v;
        ++idx;
    }
    cache_index = idx;
    cache_cluster = cur;
    cluster = cur;
    return 0;
}

uint32_t vfs::raw_read(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t pos, void* dst,
                       uint32_t n, bool& failed) {
    failed = false;
    uint32_t size = ld32(entry_ptr(entry) + de_size);
    if (pos >= size || n == 0) return 0;
    if (n > size - pos) n = size - pos;
    uint8_t* out = static_cast<uint8_t*>(dst);
    const uint32_t mask = cluster_size_ - 1;
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done;
        uint32_t c;
        if (cluster_for(entry, cache_index, cache_cluster, p >> cluster_shift_, false, c) < 0) {
            failed = true;
            break;
        }
        uint32_t off = p & mask;
        uint32_t chunk = cluster_size_ - off;
        if (chunk > n - done) chunk = n - done;
        memcpy(out + done, cluster_ptr(c) + off, chunk);
        done += chunk;
    }
    return done;
}

// src == nullptr writes zeros (W3). The size is stored after every cluster's worth of data (W2).
uint32_t vfs::raw_write(size_t entry, uint32_t& cache_index, uint32_t& cache_cluster, uint32_t pos, const void* src,
                        uint32_t n, const file* writer, bool& failed) {
    failed = false;
    if (n == 0) return 0;
    if (pos >= max_file_size) {  // W4
        failed = true;
        fail(error::file_too_big);
        return 0;
    }
    if (n > max_file_size - pos) n = max_file_size - pos;
    uint8_t* e = entry_ptr(entry);
    uint32_t size = ld32(e + de_size);
    if (pos > size) {
        raw_write(entry, cache_index, cache_cluster, size, nullptr, pos - size, writer, failed);
        if (failed) return 0;
        size = ld32(e + de_size);
    }
    const uint8_t* in = static_cast<const uint8_t*>(src);
    const uint32_t mask = cluster_size_ - 1;
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done;
        uint32_t c;
        if (cluster_for(entry, cache_index, cache_cluster, p >> cluster_shift_, true, c) < 0) {
            failed = true;
            break;
        }
        uint32_t off = p & mask;
        uint32_t chunk = cluster_size_ - off;
        if (chunk > n - done) chunk = n - done;
        uint8_t* d = cluster_ptr(c) + off;
        if (in)
            memcpy(d, in + done, chunk);
        else
            memset(d, 0, chunk);
        done += chunk;
        if (p + chunk > size) {
            size = p + chunk;
            st32(e + de_size, size);
        }
    }
    if (done) invalidate_others(entry, writer, false);  // D5
    return done;
}

void vfs::invalidate_others(size_t entry, const file* except, bool caches) {
    for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) {
        file& h = files_[i];
        if (h.entry_ != entry || &h == except) continue;
        if (caches) h.cache_index_ = no_cache;
        if (h.buf_state_ == st_reading) {
            h.buf_state_ = st_idle;
            h.buf_len_ = 0;
        }
    }
}

// W3, H4
int vfs::truncate_entry(size_t entry, uint32_t length, file* writer) {
    uint8_t* e = entry_ptr(entry);
    uint32_t size = ld32(e + de_size);
    if (length == size) return 0;
    if (length < size) {
        uint32_t first = entry_cluster(e);
        uint32_t keep = (length >> cluster_shift_) + ((length & (cluster_size_ - 1)) ? 1 : 0);
        if (keep == 0) {
            set_entry_cluster(e, 0);
            st32(e + de_size, 0);
            if (valid_cluster(first)) free_chain(first);
        } else {
            uint32_t c = first;
            if (!valid_cluster(c)) return fail(error::corrupt);
            for (uint32_t i = 1; i < keep; ++i) {
                uint32_t next;
                if (chain_next(c, next) <= 0) return fail(error::corrupt);
                c = next;
            }
            uint32_t tail = fat_get(c);
            fat_set(c, fat_eoc);
            st32(e + de_size, length);
            if (valid_cluster(tail)) free_chain(tail);
        }
        invalidate_others(entry, nullptr, true);
    } else {
        uint32_t ci = no_cache, cc = 0;
        bool failed;
        raw_write(entry, ci, cc, size, nullptr, length - size, writer, failed);
        if (failed) return -1;
    }
    if (writer) writer->flags_ |= fl_modified;
    return 0;
}

// ── Stream state (spec §7.3) ──

void vfs::drop_read_buffer(file& h) {
    if (h.buf_state_ == st_reading) {
        h.buf_state_ = st_idle;
        h.buf_len_ = 0;
    }
}

// B6
int vfs::flush_stream(file& h) {
    if (h.buf_state_ == st_reading) {
        drop_read_buffer(h);
        return 0;
    }
    if (h.buf_state_ != st_writing) return 0;
    h.buf_state_ = st_idle;
    if (h.buf_len_ == 0) return 0;
    uint32_t start = (h.flags_ & fl_append) ? ld32(entry_ptr(h.entry_) + de_size) : h.buf_start_;
    uint32_t len = static_cast<uint32_t>(h.buf_len_);
    h.buf_len_ = 0;
    bool failed;
    uint32_t w = raw_write(h.entry_, h.cache_index_, h.cache_cluster_, start, h.buf_, len, &h, failed);
    h.pos_ = start + w;
    if (w) h.flags_ |= fl_modified;
    if (w < len) {
        h.flags_ |= fl_error;
        return -1;
    }
    return 0;
}

// D9, F20
int vfs::finalize(file& h) {
    if (!(h.flags_ & fl_modified)) return 0;
    uint16_t date, time;
    uint8_t tenth;
    stamp_now(date, time, tenth);
    uint8_t* e = entry_ptr(h.entry_);
    st16(e + de_write_time, time);
    st16(e + de_write_date, date);
    st16(e + de_access_date, date);
    e[de_attr] |= attr_archive;
    h.flags_ &= static_cast<uint8_t>(~fl_modified);
    return 0;
}

int vfs::release(file& h) {
    int rc = flush_stream(h);
    finalize(h);
    h.entry_ = 0;
    h.buf_state_ = st_idle;
    h.buf_len_ = 0;
    h.unget_ = -1;
    h.flags_ = 0;
    return rc;
}

void vfs::prepare_fd(file& h) {
    if (h.buf_state_ == st_writing) flush_stream(h);  // U4
    drop_read_buffer(h);
    h.unget_ = -1;
}

int vfs::seek_to(file& h, int64_t offset, int whence, uint32_t& result) {
    if (h.buf_state_ == st_writing && flush_stream(h) < 0) return -1;
    int64_t base;
    switch (whence) {
        case seek_set: base = 0; break;
        case seek_cur: base = static_cast<int64_t>(h.pos_) - ((h.unget_ >= 0 && h.pos_ > 0) ? 1 : 0); break;
        case seek_end: base = ld32(entry_ptr(h.entry_) + de_size); break;
        default: return fail(error::invalid);
    }
    int64_t target = base + offset;
    if (target < 0 || target > max_position) return fail(error::invalid);
    uint32_t t = static_cast<uint32_t>(target);
    if (h.buf_state_ == st_reading && (t < h.buf_start_ || t > h.buf_start_ + h.buf_len_)) drop_read_buffer(h);
    h.unget_ = -1;
    h.flags_ &= static_cast<uint8_t>(~fl_eof);
    h.pos_ = t;
    result = t;
    return 0;
}

size_t vfs::stream_read(file& h, uint8_t* out, size_t total) {
    if (!(h.flags_ & fl_read)) {
        h.flags_ |= fl_error;
        fail(error::bad_handle);
        return 0;
    }
    if (h.buf_state_ == st_writing && flush_stream(h) < 0) return 0;
    size_t done = 0;
    if (h.unget_ >= 0 && total > 0) {
        out[done++] = static_cast<uint8_t>(h.unget_);
        h.unget_ = -1;
    }
    bool buffered = h.buf_ && h.buf_mode_ != io_nbf && h.buf_size_ > 0;
    while (done < total) {
        if (h.buf_state_ == st_reading && h.pos_ >= h.buf_start_ && h.pos_ < h.buf_start_ + h.buf_len_) {
            size_t avail = h.buf_start_ + h.buf_len_ - h.pos_;
            size_t k = total - done < avail ? total - done : avail;
            memcpy(out + done, h.buf_ + (h.pos_ - h.buf_start_), k);
            h.pos_ += static_cast<uint32_t>(k);
            done += k;
            continue;
        }
        size_t remaining = total - done;
        bool failed;
        if (buffered && remaining < h.buf_size_) {
            uint32_t n = raw_read(h.entry_, h.cache_index_, h.cache_cluster_, h.pos_, h.buf_, clamp_count(h.buf_size_),
                                  failed);
            if (failed) {
                h.flags_ |= fl_error;
                break;
            }
            if (n == 0) {
                h.flags_ |= fl_eof;
                break;
            }
            h.buf_start_ = h.pos_;
            h.buf_len_ = n;
            h.buf_state_ = st_reading;
        } else {  // B3: large requests bypass the buffer
            drop_read_buffer(h);
            uint32_t want = clamp_count(remaining);
            uint32_t n = raw_read(h.entry_, h.cache_index_, h.cache_cluster_, h.pos_, out + done, want, failed);
            h.pos_ += n;
            done += n;
            if (failed) {
                h.flags_ |= fl_error;
                break;
            }
            if (n < want) {
                h.flags_ |= fl_eof;
                break;
            }
        }
    }
    return done;
}

size_t vfs::stream_write(file& h, const uint8_t* in, size_t total) {
    if (!(h.flags_ & fl_write)) {
        h.flags_ |= fl_error;
        fail(error::bad_handle);
        return 0;
    }
    h.unget_ = -1;
    drop_read_buffer(h);
    bool append = (h.flags_ & fl_append) != 0;
    bool buffered = h.buf_ && h.buf_mode_ != io_nbf && h.buf_size_ > 0;
    size_t done = 0;
    while (done < total) {
        size_t remaining = total - done;
        if (!buffered || (h.buf_state_ != st_writing && remaining >= h.buf_size_)) {  // unbuffered or B3
            if (h.buf_state_ == st_writing && flush_stream(h) < 0) return done;
            uint32_t start = append ? ld32(entry_ptr(h.entry_) + de_size) : h.pos_;
            uint32_t want = clamp_count(remaining);
            bool failed;
            uint32_t n = raw_write(h.entry_, h.cache_index_, h.cache_cluster_, start, in + done, want, &h, failed);
            h.pos_ = start + n;
            done += n;
            if (n) h.flags_ |= fl_modified;
            if (failed || n < want) {
                h.flags_ |= fl_error;
                return done;
            }
            continue;
        }
        if (h.buf_state_ != st_writing) {
            h.buf_start_ = append ? ld32(entry_ptr(h.entry_) + de_size) : h.pos_;
            h.buf_len_ = 0;
            h.buf_state_ = st_writing;
        }
        if (h.buf_len_ == h.buf_size_) {
            if (flush_stream(h) < 0) return done;
            continue;
        }
        uint64_t room = static_cast<uint64_t>(max_position) - (static_cast<uint64_t>(h.buf_start_) + h.buf_len_);
        if (room == 0) {  // W4
            h.flags_ |= fl_error;
            fail(error::file_too_big);
            return done;
        }
        size_t k = h.buf_size_ - h.buf_len_;
        if (k > remaining) k = remaining;
        if (k > room) k = static_cast<size_t>(room);
        memcpy(h.buf_ + h.buf_len_, in + done, k);
        h.buf_len_ += k;
        done += k;
        h.pos_ = h.buf_start_ + static_cast<uint32_t>(h.buf_len_);
    }
    if (h.buf_mode_ == io_lbf && h.buf_state_ == st_writing && memchr(in, '\n', total)) flush_stream(h);
    return done;
}

// ── Streams (spec §7.3) ──

file* vfs::fopen(const char* path, const char* mode) {
    guard g(*this);
    if (!mode) {
        fail(error::invalid);
        return nullptr;
    }
    uint8_t access;
    bool create = false, trunc = false, append = false, excl = false;
    switch (mode[0]) {  // B1
        case 'r': access = fl_read; break;
        case 'w': access = fl_write; create = trunc = true; break;
        case 'a': access = fl_write; create = append = true; break;
        default: fail(error::invalid); return nullptr;
    }
    for (const char* m = mode + 1; *m; ++m) {
        if (*m == '+') {
            access = fl_read | fl_write;
        } else if (*m == 'b' || *m == 't') {
        } else if (*m == 'x' && mode[0] == 'w') {
            excl = true;
        } else {
            fail(error::invalid);
            return nullptr;
        }
    }
    return open_common(path, access, create, excl, trunc, append, true);
}

int vfs::fclose(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    return release(*h) < 0 ? eof : 0;  // B7
}

size_t vfs::fread(void* ptr, size_t size, size_t nmemb, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return 0;
    }
    if (size == 0 || nmemb == 0) return 0;
    if (!ptr || nmemb > static_cast<size_t>(-1) / size) {
        fail(error::invalid);
        return 0;
    }
    return stream_read(*h, static_cast<uint8_t*>(ptr), size * nmemb) / size;
}

size_t vfs::fwrite(const void* ptr, size_t size, size_t nmemb, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return 0;
    }
    if (size == 0 || nmemb == 0) return 0;
    if (!ptr || nmemb > static_cast<size_t>(-1) / size) {
        fail(error::invalid);
        return 0;
    }
    return stream_write(*h, static_cast<const uint8_t*>(ptr), size * nmemb) / size;
}

int vfs::getc_locked(file& h) {
    if (h.unget_ >= 0 && (h.flags_ & fl_read)) {
        int c = h.unget_;
        h.unget_ = -1;
        return c;
    }
    if (h.buf_state_ == st_reading && h.pos_ >= h.buf_start_ && h.pos_ < h.buf_start_ + h.buf_len_)
        return h.buf_[h.pos_++ - h.buf_start_];
    uint8_t c;
    return stream_read(h, &c, 1) == 1 ? c : eof;
}

int vfs::fgetc(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    return getc_locked(*h);
}

int vfs::fputc(int c, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    uint8_t b = static_cast<uint8_t>(c);
    // Fast path: room in a write buffer that needs no flush for this byte.
    if (h->buf_state_ == st_writing && h->buf_len_ < h->buf_size_ && (h->buf_mode_ == io_fbf || b != '\n') &&
        static_cast<uint64_t>(h->buf_start_) + h->buf_len_ < static_cast<uint64_t>(max_position)) {
        h->buf_[h->buf_len_++] = b;
        h->pos_ = h->buf_start_ + static_cast<uint32_t>(h->buf_len_);
        return b;
    }
    return stream_write(*h, &b, 1) == 1 ? b : eof;
}

// B5
int vfs::ungetc(int c, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    if (c == eof || h->unget_ >= 0) return eof;
    if (!(h->flags_ & fl_read)) {
        fail(error::bad_handle);
        return eof;
    }
    if (h->buf_state_ == st_writing && flush_stream(*h) < 0) return eof;
    h->unget_ = static_cast<uint8_t>(c);
    h->flags_ &= static_cast<uint8_t>(~fl_eof);
    return static_cast<uint8_t>(c);
}

char* vfs::fgets(char* s, int n, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return nullptr;
    }
    if (!s || n <= 0) {
        fail(error::invalid);
        return nullptr;
    }
    int i = 0;
    bool error_before = (h->flags_ & fl_error) != 0;
    while (i < n - 1) {
        int c = getc_locked(*h);
        if (c == eof) break;
        s[i++] = static_cast<char>(c);
        if (c == '\n') break;
    }
    if ((i == 0 && n > 1) || (!error_before && (h->flags_ & fl_error))) return nullptr;
    s[i] = '\0';
    return s;
}

int vfs::fputs(const char* s, file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    if (!s) {
        fail(error::invalid);
        return eof;
    }
    size_t len = strlen(s);
    return stream_write(*h, reinterpret_cast<const uint8_t*>(s), len) == len ? 0 : eof;
}

int vfs::fprintf(file* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = vfprintf(stream, format, args);
    va_end(args);
    return n;
}

// D14
int vfs::vfprintf(file* stream, const char* format, va_list args) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return -1;
    }
    if (!format) {
        fail(error::invalid);
        return -1;
    }
    char buf[VFAT32_PRINTF_MAX];
    int n = vsnprintf(buf, sizeof buf, format, args);
    if (n < 0 || static_cast<size_t>(n) >= sizeof buf) {
        fail(error::invalid);
        return -1;
    }
    return stream_write(*h, reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n)) ==
                   static_cast<size_t>(n)
               ? n
               : -1;
}

int vfs::fseek(file* stream, long offset, int whence) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    uint32_t pos;
    return seek_to(*h, offset, whence, pos);
}

long vfs::ftell(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    uint32_t p = h->pos_ - ((h->unget_ >= 0 && h->pos_ > 0) ? 1 : 0);
    if (static_cast<unsigned long>(p) > static_cast<unsigned long>(LONG_MAX)) return fail(error::overflow);  // B8
    return static_cast<long>(p);
}

void vfs::rewind(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return;
    }
    uint32_t pos;
    seek_to(*h, 0, seek_set, pos);
    h->flags_ &= static_cast<uint8_t>(~fl_error);
}

int vfs::fgetpos(file* stream, fpos_t* pos) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    if (!pos) return fail(error::invalid);
    pos->position = h->pos_ - ((h->unget_ >= 0 && h->pos_ > 0) ? 1 : 0);
    return 0;
}

int vfs::fsetpos(file* stream, const fpos_t* pos) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    if (!pos) return fail(error::invalid);
    uint32_t p;
    return seek_to(*h, pos->position, seek_set, p);
}

int vfs::fflush(file* stream) {
    guard g(*this);
    if (!stream) {  // B7: every stream
        if (!mounted_) return fail(error::not_mounted);
        int rc = 0;
        for (size_t i = 0; i < VFAT32_MAX_OPEN_FILES; ++i) {
            file& h = files_[i];
            if (h.entry_ == 0) continue;
            if (flush_stream(h) < 0) rc = eof;
            finalize(h);
        }
        return rc;
    }
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return eof;
    }
    int rc = flush_stream(*h) < 0 ? eof : 0;
    finalize(*h);
    return rc;
}

int vfs::feof(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return 0;
    }
    return (h->flags_ & fl_eof) ? 1 : 0;
}

int vfs::ferror(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return 0;
    }
    return (h->flags_ & fl_error) ? 1 : 0;
}

void vfs::clearerr(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) {
        fail(error::bad_handle);
        return;
    }
    h->flags_ &= static_cast<uint8_t>(~(fl_eof | fl_error));
}

// B2
int vfs::setvbuf(file* stream, char* buf, int mode, size_t size) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    if (mode != io_fbf && mode != io_lbf && mode != io_nbf) return fail(error::invalid);
    if (mode != io_nbf && buf && size == 0) return fail(error::invalid);
    if (mode != io_nbf && !buf && VFAT32_BUFFER_SIZE == 0) return fail(error::no_memory);
    if (flush_stream(*h) < 0) return -1;
    if (mode == io_nbf) {
        h->buf_ = nullptr;
        h->buf_size_ = 0;
    } else if (buf) {
        h->buf_ = reinterpret_cast<uint8_t*>(buf);
        h->buf_size_ = size;
    }
#if VFAT32_BUFFER_SIZE > 0
    else {
        h->buf_ = h->storage_;
        h->buf_size_ = VFAT32_BUFFER_SIZE;
    }
#endif
    h->buf_mode_ = static_cast<uint8_t>(mode);
    return 0;
}

void vfs::setbuf(file* stream, char* buf) { setvbuf(stream, buf, buf ? io_fbf : io_nbf, bufsiz); }

int vfs::fileno(file* stream) {
    guard g(*this);
    file* h = valid_file(stream);
    if (!h) return fail(error::bad_handle);
    return static_cast<int>(h - files_);
}

// ── Descriptors (spec §7.4) ──

int vfs::open(const char* path, int flags) {
    guard g(*this);
    uint8_t access;
    switch (flags & o_accmode) {
        case o_rdonly: access = fl_read; break;
        case o_wronly: access = fl_write; break;
        case o_rdwr: access = fl_read | fl_write; break;
        default: return fail(error::invalid);
    }
    file* h = open_common(path, access, (flags & o_creat) != 0, (flags & o_excl) != 0, (flags & o_trunc) != 0,
                          (flags & o_append) != 0, false);
    return h ? static_cast<int>(h - files_) : -1;
}

int vfs::close(int fd) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h) return fail(error::bad_handle);
    return release(*h);
}

long vfs::read(int fd, void* buf, size_t count) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h || !(h->flags_ & fl_read)) return fail(error::bad_handle);
    if (count == 0) return 0;
    if (!buf) return fail(error::invalid);
    prepare_fd(*h);
    bool failed;
    uint32_t n = raw_read(h->entry_, h->cache_index_, h->cache_cluster_, h->pos_, buf, clamp_count(count), failed);
    h->pos_ += n;
    if (failed && n == 0) return -1;
    return static_cast<long>(n);
}

long vfs::write(int fd, const void* buf, size_t count) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h || !(h->flags_ & fl_write)) return fail(error::bad_handle);
    if (count == 0) return 0;
    if (!buf) return fail(error::invalid);
    prepare_fd(*h);
    uint32_t start = (h->flags_ & fl_append) ? ld32(entry_ptr(h->entry_) + de_size) : h->pos_;
    bool failed;
    uint32_t n = raw_write(h->entry_, h->cache_index_, h->cache_cluster_, start, buf, clamp_count(count), h, failed);
    h->pos_ = start + n;
    if (n) h->flags_ |= fl_modified;
    if (n == 0) return -1;  // U2
    return static_cast<long>(n);
}

long vfs::lseek(int fd, long offset, int whence) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h) return fail(error::bad_handle);
    prepare_fd(*h);
    uint32_t pos;
    if (seek_to(*h, offset, whence, pos) < 0) return -1;
    if (static_cast<unsigned long>(pos) > static_cast<unsigned long>(LONG_MAX)) return fail(error::overflow);
    return static_cast<long>(pos);
}

int vfs::fsync(int fd) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h) return fail(error::bad_handle);
    int rc = flush_stream(*h);
    finalize(*h);
    return rc;
}

int vfs::ftruncate(int fd, long length) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h || !(h->flags_ & fl_write)) return fail(error::bad_handle);
    if (length < 0 || static_cast<unsigned long>(length) > max_file_size) return fail(error::invalid);
    prepare_fd(*h);
    return truncate_entry(h->entry_, static_cast<uint32_t>(length), h);
}

int vfs::fstat(int fd, struct stat* buf) {
    guard g(*this);
    file* h = valid_fd(fd);
    if (!h) return fail(error::bad_handle);
    if (!buf) return fail(error::invalid);
    fill_stat(entry_ptr(h->entry_), buf);
    return 0;
}

// K7
int vfs::truncate(const char* path, long length) {
    guard g(*this);
    if (!mounted_) return fail(error::not_mounted);
    if (length < 0 || static_cast<unsigned long>(length) > max_file_size) return fail(error::invalid);
    lookup r;
    if (resolve(path, r) < 0) return -1;
    if (!r.found) return fail(error::not_found);
    if (r.is_root || (entry_ptr(r.entry.offset)[de_attr] & attr_directory)) return fail(error::is_directory);
    if (entry_ptr(r.entry.offset)[de_attr] & attr_read_only) return fail(error::access);
    uint32_t before = ld32(entry_ptr(r.entry.offset) + de_size);
    if (truncate_entry(r.entry.offset, static_cast<uint32_t>(length), nullptr) < 0) return -1;
    if (before != static_cast<uint32_t>(length)) {
        uint16_t date, time;
        uint8_t tenth;
        stamp_now(date, time, tenth);
        uint8_t* e = entry_ptr(r.entry.offset);
        st16(e + de_write_time, time);
        st16(e + de_write_date, date);
        e[de_attr] |= attr_archive;
    }
    return 0;
}

}  // namespace vfat32
