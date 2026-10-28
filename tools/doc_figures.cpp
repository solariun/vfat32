// doc_figures — draws the documentation figures from real vfat32 volumes.
//
//   layout.svg         the on-disk layout of a 1 MiB volume formatted with the defaults,
//                      read back from the boot sector bytes
//   defrag_before.svg  the cluster map of a deliberately fragmented 128 KiB volume
//   defrag_after.svg   the same volume after fs.defrag()
//
// The maps are drawn from the raw memory, parsing the BPB, the FAT and the
// directory entries here (not through vfat32's internals), so they are also an
// independent reading of what vfat32 wrote. Output is deterministic.
//
// Build: c++ -std=c++11 -Isrc tools/doc_figures.cpp src/*.cpp -o doc_figures
// Run:   ./doc_figures [output directory]      (default: docs/figures)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfat32.hpp"

// After vfat32.hpp: the host's <sys/stat.h> defines st_mtime/st_atime/st_birthtime as macros.
#include <sys/stat.h>

namespace {

// ── raw little-endian reads and FAT32 constants (spec §4) ──
const uint32_t fat_entry_mask = 0x0FFFFFFF;
const uint32_t fat_eoc_min = 0x0FFFFFF8;
const uint32_t fat_bad = 0x0FFFFFF7;
const uint32_t first_data_cluster = 2;
const size_t dir_entry_size = 32;
const uint8_t attr_lfn = 0x0F;
const uint8_t entry_end = 0x00;
const uint8_t entry_deleted = 0xE5;
const uint8_t nt_lower_base = 0x08;
const uint8_t nt_lower_ext = 0x10;
const uint32_t fsinfo_lead_sig = 0x41615252;
const int max_dir_depth = 8;

uint32_t ld16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint32_t ld32(const uint8_t* p) { return ld16(p) | ld16(p + 2) << 16; }

struct geometry {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved;
    uint32_t fats;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t backup_sector;
    uint32_t data_sector;
    uint32_t clusters;
    uint32_t max_cluster;
};

geometry read_geometry(const uint8_t* m) {
    geometry g;
    g.bytes_per_sector = ld16(m + 11);
    g.sectors_per_cluster = m[13];
    g.reserved = ld16(m + 14);
    g.fats = m[16];
    g.total_sectors = ld32(m + 32);
    g.fat_sectors = ld32(m + 36);
    g.root_cluster = ld32(m + 44);
    g.fsinfo_sector = ld16(m + 48);
    g.backup_sector = ld16(m + 50);
    g.data_sector = g.reserved + g.fats * g.fat_sectors;
    g.clusters = (g.total_sectors - g.data_sector) / g.sectors_per_cluster;
    g.max_cluster = g.clusters + 1;
    return g;
}

uint32_t fat_at(const uint8_t* m, const geometry& g, uint32_t c) {
    return ld32(m + static_cast<size_t>(g.reserved) * g.bytes_per_sector + static_cast<size_t>(c) * 4) & fat_entry_mask;
}

const uint8_t* cluster_at(const uint8_t* m, const geometry& g, uint32_t c) {
    size_t cluster_bytes = static_cast<size_t>(g.bytes_per_sector) * g.sectors_per_cluster;
    return m + static_cast<size_t>(g.data_sector) * g.bytes_per_sector + (c - first_data_cluster) * cluster_bytes;
}

// ── output helpers ──
bool make_dirs(const char* path) {
    char buf[512];
    size_t n = strlen(path);
    if (n >= sizeof buf) return false;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == 0) {
            char saved = buf[i];
            buf[i] = 0;
            if (::mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
            buf[i] = saved;
        }
    }
    return true;
}

FILE* open_svg(const char* dir, const char* name, int width, int height) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* out = ::fopen(path, "w");
    if (!out) {
        printf("cannot write %s\n", path);
        return nullptr;
    }
    ::fprintf(out,
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
              "font-family=\"Helvetica, Arial, sans-serif\">\n",
              width, height, width, height);
    ::fprintf(out, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"#ffffff\"/>\n", width, height);
    printf("writing %s\n", path);
    return out;
}

void close_svg(FILE* out) {
    ::fprintf(out, "</svg>\n");
    ::fclose(out);
}

void text(FILE* out, double x, double y, int size, const char* anchor, const char* fill, const char* s) {
    ::fprintf(out, "<text x=\"%.1f\" y=\"%.1f\" font-size=\"%d\" text-anchor=\"%s\" fill=\"%s\">", x, y, size, anchor,
              fill);
    for (const char* p = s; *p; ++p) {
        if (*p == '&') ::fputs("&amp;", out);
        else if (*p == '<') ::fputs("&lt;", out);
        else if (*p == '>') ::fputs("&gt;", out);
        else ::fputc(*p, out);
    }
    ::fputs("</text>\n", out);
}

void rect(FILE* out, double x, double y, double w, double h, const char* fill, const char* stroke) {
    ::fprintf(out, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\" stroke=\"%s\"/>\n", x, y, w,
              h, fill, stroke);
}

const char* const ink = "#222222";
const char* const faint = "#666666";

// ── figure 1: layout ──
const size_t layout_volume = 1024 * 1024;
const int layout_width = 920;
const int layout_margin = 20;
const int layout_data_cells = 8;  // data clusters shown in the zoomed strip

enum region { r_boot, r_fsinfo, r_backup, r_backup_fsinfo, r_reserved, r_fat1, r_fat2, r_root, r_data, region_count };
const char* const region_fill[region_count] = {"#0072B2", "#56B4E9", "#7a9cc6", "#a9c4e0", "#dde6f0",
                                               "#E69F00", "#f3c96b", "#009E73", "#bfe3d6"};
region region_of_sector(const uint8_t* m, const geometry& g, uint32_t s) {
    if (s == 0) return r_boot;
    if (s < g.reserved) {
        if (s == g.fsinfo_sector && ld32(m + static_cast<size_t>(s) * g.bytes_per_sector) == fsinfo_lead_sig)
            return r_fsinfo;
        if (g.backup_sector && s == g.backup_sector) return r_backup;
        if (g.backup_sector && s == g.backup_sector + 1) return r_backup_fsinfo;
        return r_reserved;
    }
    if (s < g.reserved + g.fat_sectors) return r_fat1;
    if (s < g.data_sector) return r_fat2;
    uint32_t c = first_data_cluster + (s - g.data_sector) / g.sectors_per_cluster;
    return c == g.root_cluster ? r_root : r_data;
}

bool layout_figure(const char* dir) {
    static uint8_t m[layout_volume];
    vfat32::vfs fs;
    if (fs.mount(m, layout_volume, vfat32::mount_mode::format) != 0) return false;
    fs.umount();
    geometry g = read_geometry(m);

    const int height = 330;
    FILE* out = open_svg(dir, "layout.svg", layout_width, height);
    if (!out) return false;
    char s[256];
    snprintf(s, sizeof s, "On-disk layout of a %u KiB vfat32 volume (default format options, read back from the BPB)",
             static_cast<unsigned>(layout_volume / 1024));
    text(out, layout_margin, 28, 16, "start", ink, s);

    // Whole volume, proportional.
    const double bar_y = 60;
    const double bar_h = 34;
    const double bar_w = layout_width - 2 * layout_margin;
    double meta_w = bar_w * g.data_sector / g.total_sectors;
    const double min_visible = 3;
    if (meta_w < min_visible) meta_w = min_visible;
    rect(out, layout_margin, bar_y, meta_w, bar_h, region_fill[r_fat1], ink);
    rect(out, layout_margin + meta_w, bar_y, bar_w - meta_w, bar_h, region_fill[r_data], ink);
    snprintf(s, sizeof s, "data region: %u clusters of %u bytes (%.1f %% of the volume)",
             static_cast<unsigned>(g.clusters),
             static_cast<unsigned>(g.bytes_per_sector * g.sectors_per_cluster),
             100.0 * (g.total_sectors - g.data_sector) / g.total_sectors);
    text(out, layout_margin + bar_w / 2, bar_y + 22, 13, "middle", ink, s);
    snprintf(s, sizeof s, "whole volume: %u sectors of %u bytes — metadata (reserved + FATs) is the thin left part: %u sectors",
             static_cast<unsigned>(g.total_sectors), static_cast<unsigned>(g.bytes_per_sector),
             static_cast<unsigned>(g.data_sector));
    text(out, layout_margin, bar_y + bar_h + 18, 12, "start", faint, s);

    // Zoomed strip: one cell per sector, the metadata and the first data clusters.
    uint32_t shown = g.data_sector + layout_data_cells * g.sectors_per_cluster;
    const double zoom_y = 150;
    const double zoom_h = 40;
    double cell = bar_w / shown;
    for (uint32_t sec = 0; sec < shown; ++sec) {
        region r = region_of_sector(m, g, sec);
        rect(out, layout_margin + sec * cell, zoom_y, cell, zoom_h, region_fill[r], "#ffffff");
    }
    // Sector numbers at region starts.
    region prev = region_count;
    for (uint32_t sec = 0; sec < shown; ++sec) {
        region r = region_of_sector(m, g, sec);
        if (r != prev && r != r_data) {
            snprintf(s, sizeof s, "%u", static_cast<unsigned>(sec));
            text(out, layout_margin + sec * cell + 1, zoom_y - 6, 11, "start", ink, s);
        }
        prev = r;
    }
    snprintf(s, sizeof s, "zoom: sectors 0 … %u, one cell per sector (numbers mark where each part starts)",
             static_cast<unsigned>(shown - 1));
    text(out, layout_margin, zoom_y + zoom_h + 18, 12, "start", faint, s);

    // Legend with the real sizes.
    const double legend_y = 240;
    const double col_w = bar_w / 3;
    uint32_t sector = g.bytes_per_sector;
    char lines[region_count][96];
    snprintf(lines[r_boot], 96, "boot sector (BPB): sector 0");
    snprintf(lines[r_fsinfo], 96, "FSInfo: sector %u (free count)", static_cast<unsigned>(g.fsinfo_sector));
    if (g.backup_sector) {
        snprintf(lines[r_backup], 96, "backup boot: sector %u", static_cast<unsigned>(g.backup_sector));
        snprintf(lines[r_backup_fsinfo], 96, "backup FSInfo: sector %u", static_cast<unsigned>(g.backup_sector + 1));
    } else {
        snprintf(lines[r_backup], 96, "backup boot: none");
        snprintf(lines[r_backup_fsinfo], 96, "backup FSInfo: none");
    }
    snprintf(lines[r_reserved], 96, "reserved region: %u sectors (%u bytes)", static_cast<unsigned>(g.reserved),
             static_cast<unsigned>(g.reserved * sector));
    snprintf(lines[r_fat1], 96, "FAT copy 1: sectors %u–%u (%u bytes)", static_cast<unsigned>(g.reserved),
             static_cast<unsigned>(g.reserved + g.fat_sectors - 1), static_cast<unsigned>(g.fat_sectors * sector));
    if (g.fats > 1) {
        snprintf(lines[r_fat2], 96, "FAT copy 2: sectors %u–%u (mirror)", static_cast<unsigned>(g.reserved + g.fat_sectors),
                 static_cast<unsigned>(g.data_sector - 1));
    } else {
        snprintf(lines[r_fat2], 96, "FAT copy 2: none");
    }
    snprintf(lines[r_root], 96, "root directory: cluster %u (sector %u)", static_cast<unsigned>(g.root_cluster),
             static_cast<unsigned>(g.data_sector + (g.root_cluster - first_data_cluster) * g.sectors_per_cluster));
    snprintf(lines[r_data], 96, "data clusters: 3 … %u", static_cast<unsigned>(g.max_cluster));
    for (int i = 0; i < region_count; ++i) {
        double x = layout_margin + (i % 3) * col_w;
        double y = legend_y + (i / 3) * 24;
        rect(out, x, y - 11, 14, 14, region_fill[i], ink);
        text(out, x + 20, y, 12, "start", ink, lines[i]);
    }
    close_svg(out);
    printf("  layout: %u sectors, %u reserved, %u FATs x %u sectors, %u clusters, root %u\n",
           static_cast<unsigned>(g.total_sectors), static_cast<unsigned>(g.reserved), static_cast<unsigned>(g.fats),
           static_cast<unsigned>(g.fat_sectors), static_cast<unsigned>(g.clusters),
           static_cast<unsigned>(g.root_cluster));
    return true;
}

// ── figures 2 and 3: cluster maps ──
const size_t map_volume = 128 * 1024;
const int map_columns = 32;
const double map_cell = 18;
const double map_gap = 2;
const double map_left = 50;
const double map_top = 64;
const int max_chains = 32;
const size_t name_size = 64;
const char* const root_fill = "#333333";
const char* const free_fill = "#e8e8e8";
const char* const lost_fill = "#ff0000";
const char* const palette[] = {"#0072B2", "#E69F00", "#009E73", "#CC79A7", "#56B4E9", "#D55E00",
                               "#999933", "#882255", "#44AA99", "#117733", "#AA4499", "#DDCC77"};
const int palette_size = sizeof palette / sizeof palette[0];

struct chain_info {
    uint32_t head;
    char path[name_size];
    bool is_dir;
    uint32_t clusters;
    uint32_t runs;
};

struct map_model {
    geometry g;
    chain_info chains[max_chains];
    int chain_count;
    int* owner;  // per cluster: chain index, -1 free, -2 used but unowned
    uint32_t free_extents;
    uint32_t fragmented;
};

void sfn_name(const uint8_t* e, char* out) {
    size_t n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) {
        char c = static_cast<char>(e[i]);
        if ((e[12] & nt_lower_base) && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; ++i) {
            char c = static_cast<char>(e[i]);
            if ((e[12] & nt_lower_ext) && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            out[n++] = c;
        }
    }
    out[n] = 0;
}

void add_chain(map_model& mm, uint32_t head, const char* path, bool is_dir) {
    if (mm.chain_count >= max_chains || head < first_data_cluster || head > mm.g.max_cluster) return;
    chain_info& ci = mm.chains[mm.chain_count++];
    ci.head = head;
    snprintf(ci.path, name_size, "%s", path);
    ci.is_dir = is_dir;
}

void walk_dir(const uint8_t* m, map_model& mm, uint32_t first, const char* prefix, int depth) {
    size_t cluster_bytes = static_cast<size_t>(mm.g.bytes_per_sector) * mm.g.sectors_per_cluster;
    uint32_t c = first;
    for (uint32_t steps = 0; steps <= mm.g.clusters && c >= first_data_cluster && c <= mm.g.max_cluster; ++steps) {
        const uint8_t* data = cluster_at(m, mm.g, c);
        for (size_t off = 0; off < cluster_bytes; off += dir_entry_size) {
            const uint8_t* e = data + off;
            if (e[0] == entry_end) return;
            if (e[0] == entry_deleted || e[11] == attr_lfn || (e[11] & vfat32::attr_volume_id) || e[0] == '.') continue;
            char name[16];
            sfn_name(e, name);
            char path[name_size];
            if (snprintf(path, sizeof path, "%s/%s", prefix, name) >= static_cast<int>(sizeof path)) continue;
            uint32_t head = ld16(e + 20) << 16 | ld16(e + 26);
            bool is_dir = (e[11] & vfat32::attr_directory) != 0;
            add_chain(mm, head, path, is_dir);
            if (is_dir && depth < max_dir_depth) walk_dir(m, mm, head, path, depth + 1);
        }
        uint32_t next = fat_at(m, mm.g, c);
        if (next >= fat_eoc_min) return;
        c = next;
    }
}

void build_model(const uint8_t* m, map_model& mm) {
    mm.g = read_geometry(m);
    mm.chain_count = 0;
    add_chain(mm, mm.g.root_cluster, "/ (root)", true);
    walk_dir(m, mm, mm.g.root_cluster, "", 0);

    mm.owner = static_cast<int*>(malloc(sizeof(int) * (mm.g.max_cluster + 1)));
    for (uint32_t c = 0; c <= mm.g.max_cluster; ++c) {
        uint32_t v = fat_at(m, mm.g, c);
        mm.owner[c] = (c < first_data_cluster || v == 0) ? -1 : -2;
    }
    mm.fragmented = 0;
    for (int i = 0; i < mm.chain_count; ++i) {
        chain_info& ci = mm.chains[i];
        ci.clusters = 0;
        ci.runs = 0;
        uint32_t c = ci.head;
        uint32_t prev = 0;
        for (uint32_t steps = 0; steps <= mm.g.clusters && c >= first_data_cluster && c <= mm.g.max_cluster; ++steps) {
            mm.owner[c] = i;
            ci.clusters++;
            if (prev == 0 || c != prev + 1) ci.runs++;
            uint32_t next = fat_at(m, mm.g, c);
            if (next >= fat_eoc_min || next == fat_bad) break;
            prev = c;
            c = next;
        }
        if (ci.runs > 1) mm.fragmented++;
    }
    mm.free_extents = 0;
    bool in_free = false;
    for (uint32_t c = first_data_cluster; c <= mm.g.max_cluster; ++c) {
        bool is_free = mm.owner[c] == -1;
        if (is_free && !in_free) mm.free_extents++;
        in_free = is_free;
    }
}

// Colour by path order so a file keeps its colour in both maps.
const char* chain_fill(const map_model& mm, int index) {
    if (index == 0) return root_fill;
    int rank = 0;
    for (int i = 1; i < mm.chain_count; ++i) {
        if (strcmp(mm.chains[i].path, mm.chains[index].path) < 0) ++rank;
    }
    return palette[rank % palette_size];
}

bool map_figure(const char* dir, const char* file_name, const char* title, const uint8_t* m, map_model& mm) {
    build_model(m, mm);
    uint32_t cells = mm.g.clusters;
    int rows = static_cast<int>((cells + map_columns - 1) / map_columns);
    double pitch = map_cell + map_gap;
    double grid_bottom = map_top + rows * pitch;
    const double legend_row = 22;
    const int legend_columns = 2;
    int legend_items = mm.chain_count + 1;  // + free
    int legend_rows = (legend_items + legend_columns - 1) / legend_columns;
    int width = static_cast<int>(map_left + map_columns * pitch + 20);
    int height = static_cast<int>(grid_bottom + 30 + legend_rows * legend_row + 20);

    FILE* out = open_svg(dir, file_name, width, height);
    if (!out) return false;
    char s[256];
    text(out, 20, 26, 16, "start", ink, title);
    snprintf(s, sizeof s,
             "%u clusters of %u bytes · %u chains, %u fragmented · %u free extent%s · one square per cluster, row "
             "starts numbered",
             static_cast<unsigned>(cells), static_cast<unsigned>(mm.g.bytes_per_sector * mm.g.sectors_per_cluster),
             static_cast<unsigned>(mm.chain_count), static_cast<unsigned>(mm.fragmented),
             static_cast<unsigned>(mm.free_extents), mm.free_extents == 1 ? "" : "s");
    text(out, 20, 46, 12, "start", faint, s);

    for (uint32_t i = 0; i < cells; ++i) {
        uint32_t c = first_data_cluster + i;
        int col = static_cast<int>(i % map_columns);
        int row = static_cast<int>(i / map_columns);
        if (col == 0) {
            snprintf(s, sizeof s, "%u", static_cast<unsigned>(c));
            text(out, map_left - 6, map_top + row * pitch + map_cell - 5, 10, "end", faint, s);
        }
        int o = mm.owner[c];
        const char* fill = o >= 0 ? chain_fill(mm, o) : (o == -1 ? free_fill : lost_fill);
        rect(out, map_left + col * pitch, map_top + row * pitch, map_cell, map_cell, fill,
             o == -1 ? "#cccccc" : "none");
    }

    double legend_top = grid_bottom + 24;
    double col_w = (width - 40) / legend_columns;
    for (int i = 0; i < legend_items; ++i) {
        double x = 20 + (i % legend_columns) * col_w;
        double y = legend_top + (i / legend_columns) * legend_row;
        if (i < mm.chain_count) {
            const chain_info& ci = mm.chains[i];
            rect(out, x, y - 11, 14, 14, chain_fill(mm, i), "none");
            snprintf(s, sizeof s, "%s%s — %u cluster%s, %u run%s", ci.path, ci.is_dir && i ? "/" : "",
                     static_cast<unsigned>(ci.clusters), ci.clusters == 1 ? "" : "s", static_cast<unsigned>(ci.runs),
                     ci.runs == 1 ? "" : "s");
        } else {
            rect(out, x, y - 11, 14, 14, free_fill, "#cccccc");
            uint32_t free_clusters = 0;
            for (uint32_t c = first_data_cluster; c <= mm.g.max_cluster; ++c) free_clusters += mm.owner[c] == -1;
            snprintf(s, sizeof s, "free — %u clusters in %u extent%s", static_cast<unsigned>(free_clusters),
                     static_cast<unsigned>(mm.free_extents), mm.free_extents == 1 ? "" : "s");
        }
        text(out, x + 20, y, 12, "start", ink, s);
    }
    close_svg(out);
    free(mm.owner);
    mm.owner = nullptr;
    return true;
}

const size_t chunk = 700;
const int rounds = 10;
const size_t big_file = 6 * 1024;  // fills only part of the holes, so free space stays split

bool write_pattern(vfat32::vfs& fs, const char* path, const char* mode, size_t bytes, uint8_t seed) {
    vfat32::file* f = fs.fopen(path, mode);
    if (!f) return false;
    for (size_t i = 0; i < bytes; ++i) {
        if (fs.fputc(static_cast<uint8_t>(seed + i), f) == vfat32::eof) {
            fs.fclose(f);
            return false;
        }
    }
    return fs.fclose(f) == 0;
}

void print_check(vfat32::vfs& fs, const map_model& mm, const char* when) {
    vfat32::check_report r;
    int clean = fs.check(&r);
    printf("  %s: own parse %u fragmented / %u free extents; check() %u fragmented / %u free extents, %s\n", when,
           static_cast<unsigned>(mm.fragmented), static_cast<unsigned>(mm.free_extents),
           static_cast<unsigned>(r.fragmented), static_cast<unsigned>(r.free_extents),
           clean == 0 ? "clean" : "NOT clean");
    if (r.fragmented != mm.fragmented || r.free_extents != mm.free_extents)
        printf("  note: the two readings differ\n");
}

bool defrag_figures(const char* dir) {
    static uint8_t m[map_volume];
    vfat32::vfs fs;
    if (fs.mount(m, map_volume, vfat32::mount_mode::format) != 0) return false;

    // Interleaved growth fragments every file; removing two leaves holes; a last
    // file fills some of them, the others stay free (free space in many extents).
    if (fs.mkdir("/logs") != 0) return false;
    const char* names[] = {"/a.bin", "/b.bin", "/c.bin", "/d.bin", "/logs/e.log", "/logs/f.log"};
    const int name_count = sizeof names / sizeof names[0];
    for (int round = 0; round < rounds; ++round) {
        for (int i = 0; i < name_count; ++i) {
            if (!write_pattern(fs, names[i], "a", chunk, static_cast<uint8_t>(i))) return false;
        }
    }
    if (fs.remove("/b.bin") != 0 || fs.remove("/logs/e.log") != 0) return false;
    if (!write_pattern(fs, "/g.bin", "w", big_file, 0x5A)) return false;

    map_model mm;
    mm.owner = nullptr;
    if (!map_figure(dir, "defrag_before.svg", "Cluster map before defrag()", m, mm)) return false;
    print_check(fs, mm, "before");

    if (fs.defrag() != 0) {
        printf("defrag failed: %s\n", vfat32::vfs::strerror(fs.last_error()));
        return false;
    }
    if (!map_figure(dir, "defrag_after.svg", "Cluster map after defrag(): one run per chain, free space at the end", m,
                    mm))
        return false;
    print_check(fs, mm, "after");
    fs.umount();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "docs/figures";
    if (!make_dirs(dir)) {
        printf("cannot create %s\n", dir);
        return 1;
    }
    bool ok = layout_figure(dir) && defrag_figures(dir);
    if (!ok) printf("figure generation failed\n");
    return ok ? 0 : 1;
}
