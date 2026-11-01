/*
 * Disk images as GCR: what the drive's head reads.
 *
 * .D64 (35 or 40 tracks, with or without the error bytes) and .D71 (two
 * sides of 35 tracks) are encoded into GCR tracks as a 1541 formats them: per sector a sync, the header block, a gap, a
 * sync, the data block and a gap, the gaps sized to fill the track at its
 * zone's speed. The error bytes become the errors the DOS sees (missing
 * header or data block, bad checksums, no sync, wrong ID). .G64 holds the
 * GCR itself, half tracks and protections included.
 *
 * Writes are kept in the GCR; flushing decodes the changed tracks back to
 * sectors (D64, D71), or writes the GCR as it is (G64, track lengths
 * unchanged).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cbm_internal.h"
#include "gcr.h"

static const uint8_t gcr_code[16] = {
    0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17, 0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15,
};
static int8_t gcr_decode_table[32];

int gcr_cache_tracks;

int gcr_sectors(int track, int sides)
{
    if (sides == 2) {
        track = (track - 1) % 35 + 1;   /* side 2 of a D71 counts again */
    }
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

int gcr_zone(int track, int sides)
{
    if (sides == 2) {
        track = (track - 1) % 35 + 1;
    }
    return track <= 17 ? 3 : track <= 24 ? 2 : track <= 30 ? 1 : 0;
}

/* Where track t lies in gcr_disk_t.track[]: a D71's 36-70 on its second side */
static int track_index(int track, int sides)
{
    return sides == 2 && track > 35 ? GCR_HALF_TRACKS + (track - 36) * 2 : (track - 1) * 2;
}

static const uint16_t zone_bytes[4] = { 6250, 6666, 7142, 7692 };

/* 4 bytes -> 5 GCR bytes */
static void encode4(const uint8_t *in, uint8_t *out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 4; i++) {
        bits = (bits << 10) | (gcr_code[in[i] >> 4] << 5) | gcr_code[in[i] & 15];
    }
    for (int i = 4; i >= 0; i--) {
        out[i] = bits & 0xFF;
        bits >>= 8;
    }
}

/* 5 GCR bytes -> 4 bytes; false if a code isn't valid */
static bool decode5(const uint8_t *in, uint8_t *out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 5; i++) {
        bits = (bits << 8) | in[i];
    }
    bool ok = true;
    for (int i = 3; i >= 0; i--) {
        int lo = gcr_decode_table[bits & 31], hi = gcr_decode_table[(bits >> 5) & 31];
        bits >>= 10;
        if (lo < 0 || hi < 0) {
            ok = false;
        }
        out[i] = ((hi & 15) << 4) | (lo & 15);
    }
    return ok;
}

static void init_tables(void)
{
    memset(gcr_decode_table, -1, sizeof(gcr_decode_table));
    for (int i = 0; i < 16; i++) {
        gcr_decode_table[gcr_code[i]] = i;
    }
}

/* ---- Encoding a D64 track ---- */

static uint32_t sector_offset(int track, int sector, int sides)
{
    uint32_t off = 0;
    for (int t = 1; t < track; t++) {
        off += gcr_sectors(t, sides);
    }
    return (off + sector) * 256;
}

/* Track `track` (1-70) into GCR, from its sectors (the first at `sectors`);
 * errors: its sectors' error bytes or NULL. Into `t`, or a new buffer if NULL
 */
static uint8_t *encode_track(const uint8_t *sectors, int track, int sides, const uint8_t *errors, const uint8_t id[2],
                             uint8_t *t, uint16_t *len)
{
    int zone = gcr_zone(track, sides), n = gcr_sectors(track, sides);
    uint16_t total = zone_bytes[zone];
    if (t == NULL) {
        t = cbm_malloc(total);
        if (t == NULL) {
            return NULL;
        }
    }
    memset(t, 0x55, total);
    int per_sector = 5 + 10 + 9 + 5 + 325;
    int gap = (total - n * per_sector) / n;
    uint8_t *p = t;
    for (int s = 0; s < n; s++) {
        const uint8_t *data = sectors + s * 256;
        uint8_t err = errors ? errors[s] : 1;
        uint8_t id1 = id[0], id2 = id[1];
        if (err == 0x0B || err == 29) {
            id1 ^= 0xFF;                /* disk ID mismatch */
        }
        /* header */
        /* (side 2 of a D71: tracks 36-70 in the headers, as a 1571 writes them) */
        uint8_t h[8] = { 0x08, 0, (uint8_t)s, (uint8_t)track, id2, id1, 0x0F, 0x0F };
        h[1] = h[2] ^ h[3] ^ h[4] ^ h[5];
        if (err == 0x09 || err == 27) {
            h[1] ^= 0xFF;
        }
        if (err == 0x02 || err == 20) {
            h[0] = 0x00;                /* header block not found */
        }
        bool sync = !(err == 0x03 || err == 21);
        memset(p, sync ? 0xFF : 0x55, 5);
        p += 5;
        encode4(h, p);
        encode4(h + 4, p + 5);
        p += 10;
        p += 9;                         /* gap (0x55) */
        memset(p, sync ? 0xFF : 0x55, 5);
        p += 5;
        uint8_t block[260];
        block[0] = (err == 0x04 || err == 22) ? 0x00 : 0x07;
        memcpy(block + 1, data, 256);
        uint8_t ck = 0;
        for (int i = 0; i < 256; i++) {
            ck ^= data[i];
        }
        block[257] = (err == 0x05 || err == 23) ? ck ^ 0xFF : ck;
        block[258] = block[259] = 0;
        for (int i = 0; i < 260; i += 4) {
            encode4(block + i, p);
            p += 5;
        }
        p += gap;
    }
    *len = total;
    return t;
}

/* ---- Decoding a track back into its sectors (writes) ---- */

/* The sectors found in the GCR into the track's sectors (the first at `sectors`) */
static void decode_track(const uint8_t *t, uint16_t len, int track, int sides, uint8_t *sectors, uint32_t *written)
{
    int n = gcr_sectors(track, sides);
    int sector = -1;
    for (uint32_t i = 0; i < (uint32_t)len * 2; i++) {
        /* a sync: two 0xFF, then the block after the last of them */
        if (t[i % len] != 0xFF || t[(i + 1) % len] != 0xFF) {
            continue;
        }
        uint32_t j = i;
        while (t[j % len] == 0xFF && j < i + len) {
            j++;
        }
        uint8_t gcr[325], b[260];
        for (int k = 0; k < 325; k++) {
            gcr[k] = t[(j + k) % len];
        }
        decode5(gcr, b);
        if (b[0] == 0x08) {
            decode5(gcr + 5, b + 4);
            sector = b[2] < n && b[3] == track ? b[2] : -1;
        } else if (b[0] == 0x07 && sector >= 0) {
            for (int k = 1; k < 65; k++) {
                decode5(gcr + k * 5, b + k * 4);
            }
            uint8_t *dst = sectors + sector * 256;
            if (memcmp(dst, b + 1, 256) != 0) {
                memcpy(dst, b + 1, 256);
                (*written)++;
            }
            sector = -1;
        }
        i = j + 9;
        if (i >= len) {
            break;
        }
    }
}

/* A D64/D71 sector as the image has it: when its track holds no writes not
 * yet flushed and it has no error byte, it reads the same as from the GCR,
 * without decoding a track (and with a cache, without reading one in)
 */
static bool image_sector(gcr_disk_t *d, int track, int sector, uint8_t *out)
{
    if (d->g64 || track > d->tracks || sector >= gcr_sectors(track, d->sides) ||
        d->dirty[track_index(track, d->sides)]) {
        return false;
    }
    uint32_t off = sector_offset(track, sector, d->sides);
    if (d->errors && d->errors[off / 256] > 1) {
        return false;                   /* (0 and 1: no error) */
    }
    if (d->image) {
        memcpy(out, d->image + off, 256);
        return true;
    }
    /* with a cache: the track's sectors in one read, kept for the next sector */
    if (d->sectors_track != track) {
        uint32_t first = sector_offset(track, 0, d->sides), n = gcr_sectors(track, d->sides) * 256;
        d->sectors_track = 0;
        if (d->file == NULL || fseek(d->file, first, SEEK_SET) != 0 || fread(d->sectors, 1, n, d->file) != n) {
            return false;
        }
        d->sectors_track = track;
    }
    memcpy(out, d->sectors + sector * 256, 256);
    return true;
}

bool gcr_read_sector(gcr_disk_t *d, int track, int sector, uint8_t *out)
{
    if (track >= 1 && image_sector(d, track, sector, out)) {
        return true;
    }
    int i = d->g64 ? (track - 1) * 2 : track_index(track, d->sides);
    if (track < 1 || i >= GCR_HALF_TRACKS * 2 || d->len[i] == 0 || gcr_track(d, i) == NULL) {
        return false;
    }
    const uint8_t *t = d->track[i];
    uint32_t len = d->len[i];
    bool header = false;
    /* (as decode_track(), over two turns: a sector may wrap round) */
    for (uint32_t p = 0; p < len * 2; p++) {
        if (t[p % len] != 0xFF || t[(p + 1) % len] != 0xFF) {
            continue;
        }
        uint32_t j = p;
        while (t[j % len] == 0xFF && j < p + len) {
            j++;
        }
        uint8_t gcr[325], b[260];
        for (int k = 0; k < 325; k++) {
            gcr[k] = t[(j + k) % len];
        }
        decode5(gcr, b);
        if (b[0] == 0x08) {
            header = b[2] == sector && b[3] == track;
        } else if (b[0] == 0x07 && header) {
            bool ok = true;
            uint8_t ck = 0;
            for (int k = 1; k < 65; k++) {
                ok = decode5(gcr + k * 5, b + k * 4) && ok;
            }
            for (int k = 1; k <= 256; k++) {
                ck ^= b[k];
            }
            if (!ok || ck != b[257]) {
                return false;
            }
            memcpy(out, b + 1, 256);
            return true;
        }
        p = j + 9;
    }
    return false;
}

/* ---- Images ---- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

void gcr_free(gcr_disk_t *d)
{
    if (d->slots) {
        for (int s = 0; s < d->slots; s++) {
            cbm_free(d->slot[s]);       /* (the tracks in memory are in these) */
        }
    } else {
        for (int i = 0; i < GCR_HALF_TRACKS * 2; i++) {
            cbm_free(d->track[i]);
        }
    }
    cbm_free(d->image);
    cbm_free(d->sectors);
    cbm_free(d->errors);
    if (d->file) {
        fclose(d->file);
    }
    memset(d, 0, sizeof(*d));
}

/* Half track index `i` of a D64/D71 as its track number (0: a half track) */
static int index_track(const gcr_disk_t *d, int i)
{
    int side = i >= GCR_HALF_TRACKS, half = i % GCR_HALF_TRACKS;
    if (half & 1) {
        return 0;
    }
    int track = half / 2 + 1 + side * 35;
    return track <= d->tracks ? track : 0;
}

/* A track from the file into buf (a cache's slot) */
static bool read_track(gcr_disk_t *d, int i, uint8_t *buf)
{
    if (d->g64) {
        return fseek(d->file, d->file_off[i], SEEK_SET) == 0 && fread(buf, 1, d->len[i], d->file) == d->len[i];
    }
    int track = index_track(d, i);
    if (track == 0) {
        return false;
    }
    uint32_t off = sector_offset(track, 0, d->sides), n = gcr_sectors(track, d->sides) * 256;
    d->sectors_track = 0;
    if (fseek(d->file, off, SEEK_SET) != 0 || fread(d->sectors, 1, n, d->file) != n) {
        return false;
    }
    d->sectors_track = track;           /* (encoding leaves them as they are) */
    uint16_t len;
    encode_track(d->sectors, track, d->sides, d->errors ? d->errors + off / 256 : NULL, d->id, buf, &len);
    return true;
}

/* A track's changes into the file (fp: the image, open for writing) */
static bool write_track(gcr_disk_t *d, int i, FILE *fp, uint32_t *sectors_written)
{
    if (d->g64) {
        return fseek(fp, d->file_off[i], SEEK_SET) == 0 && fwrite(d->track[i], 1, d->len[i], fp) == d->len[i];
    }
    int track = index_track(d, i);
    if (track == 0) {
        return true;
    }
    uint32_t off = sector_offset(track, 0, d->sides), n = gcr_sectors(track, d->sides) * 256;
    uint8_t *sectors = d->image ? d->image + off : d->sectors;
    if (d->image == NULL) {
        d->sectors_track = 0;           /* (decoded into: not the file's any more until written) */
        if (fseek(fp, off, SEEK_SET) != 0 || fread(sectors, 1, n, fp) != n) {
            return false;
        }
    }
    uint32_t written = 0;
    decode_track(d->track[i], d->len[i], track, d->sides, sectors, &written);
    if (written == 0) {
        return true;
    }
    if (sectors_written) {
        *sectors_written += written;
    }
    return fseek(fp, off, SEEK_SET) == 0 && fwrite(sectors, 1, n, fp) == n;
}

uint8_t *gcr_track(gcr_disk_t *d, int i)
{
    if (d->slots == 0 || d->len[i] == 0) {
        return d->track[i];
    }
    int s = 0;
    for (int k = 0; k < d->slots; k++) {
        if (d->slot_track[k] == i) {
            d->slot_used[k] = ++d->use;
            return d->track[i];
        }
        if (d->slot_track[k] < 0 || (d->slot_track[s] >= 0 && d->slot_used[k] < d->slot_used[s])) {
            s = k;                      /* a free one, or the least recently used */
        }
    }
    int old = d->slot_track[s];
    if (old >= 0) {
        if (d->dirty[old] && !d->wp) {
            if (!write_track(d, old, d->file, NULL)) {
                CBM_LOG("can't write %s", d->path);
            }
            d->dirty[old] = false;
        }
        d->track[old] = NULL;
        d->slot_track[s] = -1;
    }
    if (!read_track(d, i, d->slot[s])) {
        return NULL;
    }
    d->track[i] = d->slot[s];
    d->slot_track[s] = i;
    d->slot_used[s] = ++d->use;
    return d->track[i];
}

/* f: the whole file, or (with a cache) its first `got` bytes and the rest in d->file */
static bool load_g64(gcr_disk_t *d, const uint8_t *f, long got, long size)
{
    if (got < 12 || memcmp(f, "GCR-1541", 8) != 0) {
        return false;
    }
    int tracks = f[9];
    if (tracks > GCR_HALF_TRACKS) {
        tracks = GCR_HALF_TRACKS;
    }
    if (12 + tracks * 4 > got) {
        return false;
    }
    for (int i = 0; i < tracks; i++) {
        uint32_t off = f[12 + i * 4] | (f[13 + i * 4] << 8) | (f[14 + i * 4] << 16) | ((uint32_t)f[15 + i * 4] << 24);
        if (off == 0 || off + 2 > (uint32_t)size) {
            continue;
        }
        uint8_t l[2];
        if (d->file) {
            if (fseek(d->file, off, SEEK_SET) != 0 || fread(l, 1, 2, d->file) != 2) {
                return false;
            }
        } else {
            memcpy(l, f + off, 2);
        }
        uint16_t len = l[0] | (l[1] << 8);
        if (len == 0 || off + 2 + len > (uint32_t)size) {
            continue;
        }
        d->len[i] = len;
        d->file_off[i] = off + 2;
        if (d->file == NULL) {
            d->track[i] = cbm_malloc(len);
            if (d->track[i] == NULL) {
                return false;
            }
            memcpy(d->track[i], f + off + 2, len);
        }
    }
    d->g64 = true;
    return true;
}

/* f: the whole file, or NULL with a cache (read from d->file as needed) */
static bool load_d64(gcr_disk_t *d, const uint8_t *f, long size, bool d71)
{
    int tracks, sides = d71 ? 2 : 1;
    bool errors = false;
    if (d71) {
        if (size != 349696 && size != 351062) {
            return false;
        }
        tracks = 70;
        errors = size == 351062;
    } else {
        switch (size) {
        case 174848: tracks = 35; break;
        case 175531: tracks = 35; errors = true; break;
        case 196608: tracks = 40; break;
        case 197376: tracks = 40; errors = true; break;
        default: return false;
        }
    }
    uint32_t blocks = 0;
    for (int t = 1; t <= tracks; t++) {
        blocks += gcr_sectors(t, sides);
    }
    d->tracks = tracks;
    d->sides = sides;
    d->image_size = blocks * 256;
    uint32_t bam_off = sector_offset(18, 0, sides);
    if (f == NULL) {
        /* a cache: the error bytes and the ID now, the tracks when needed */
        d->sectors = cbm_malloc(21 * 256);
        if (d->sectors == NULL) {
            return false;
        }
        if (errors) {
            d->errors = cbm_malloc(blocks);
            if (d->errors == NULL || fseek(d->file, blocks * 256, SEEK_SET) != 0 ||
                fread(d->errors, 1, blocks, d->file) != blocks) {
                return false;
            }
        }
        if (fseek(d->file, bam_off + 0xA2, SEEK_SET) != 0 || fread(d->id, 1, 2, d->file) != 2) {
            return false;
        }
        for (int t = 1; t <= tracks; t++) {
            d->len[track_index(t, sides)] = zone_bytes[gcr_zone(t, sides)];
        }
        return true;
    }
    d->image = cbm_malloc(blocks * 256);
    if (d->image == NULL) {
        return false;
    }
    memcpy(d->image, f, blocks * 256);
    if (errors) {
        d->errors = cbm_malloc(blocks);
        if (d->errors == NULL) {
            return false;
        }
        memcpy(d->errors, f + blocks * 256, blocks);
    }
    const uint8_t *err = d->errors;
    d->id[0] = d->image[bam_off + 0xA2];
    d->id[1] = d->image[bam_off + 0xA3];
    for (int t = 1; t <= tracks; t++) {
        int i = track_index(t, sides);
        uint32_t off = sector_offset(t, 0, sides);
        d->track[i] = encode_track(d->image + off, t, sides, err ? err + off / 256 : NULL, d->id, NULL, &d->len[i]);
        if (d->track[i] == NULL) {
            return false;
        }
    }
    return true;
}

/* With a cache: the file stays open, tracks are read in by gcr_track() */
static bool load_cached(gcr_disk_t *d, const char *path)
{
    d->file = fopen(path, "r+b");
    d->wp = d->file == NULL;
    if (d->file == NULL) {
        d->file = fopen(path, "rb");
    }
    if (d->file == NULL) {
        return false;
    }
    fseek(d->file, 0, SEEK_END);
    long size = ftell(d->file);
    bool ok;
    if (has_ext(path, ".g64")) {
        uint8_t head[12 + GCR_HALF_TRACKS * 4];
        long got = size < (long)sizeof(head) ? size : (long)sizeof(head);
        ok = fseek(d->file, 0, SEEK_SET) == 0 && fread(head, 1, got, d->file) == (size_t)got &&
             load_g64(d, head, got, size);
        d->sides = 1;
    } else {
        ok = load_d64(d, NULL, size, has_ext(path, ".d71"));
    }
    /* the slots, as long as the longest track */
    uint16_t longest = 0;
    for (int i = 0; i < GCR_HALF_TRACKS * 2; i++) {
        longest = d->len[i] > longest ? d->len[i] : longest;
    }
    d->slots = gcr_cache_tracks < GCR_SLOTS_MAX ? gcr_cache_tracks : GCR_SLOTS_MAX;
    for (int s = 0; s < d->slots && ok; s++) {
        d->slot[s] = cbm_malloc(longest ? longest : 1);
        d->slot_track[s] = -1;
        ok = d->slot[s] != NULL;
    }
    if (!ok) {
        gcr_free(d);
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    return true;
}

bool gcr_load(gcr_disk_t *d, const char *path)
{
    static bool tables;
    if (!tables) {
        init_tables();
        tables = true;
    }
    memset(d, 0, sizeof(*d));
    if (gcr_cache_tracks > 0) {
        return load_cached(d, path);
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *f = size > 0 && size <= 1024 * 1024 ? cbm_malloc(size) : NULL;
    bool ok = f && fread(f, 1, size, fp) == (size_t)size;
    fclose(fp);
    if (ok) {
        if (has_ext(path, ".g64")) {
            ok = load_g64(d, f, size, size);
            d->sides = 1;
        } else {
            ok = load_d64(d, f, size, has_ext(path, ".d71"));
        }
    }
    cbm_free(f);
    if (!ok) {
        gcr_free(d);
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    FILE *rw = fopen(path, "r+b");
    d->wp = rw == NULL;
    if (rw) {
        fclose(rw);
    }
    return true;
}

bool gcr_dirty(const gcr_disk_t *d)
{
    for (int i = 0; i < GCR_HALF_TRACKS * 2; i++) {
        if (d->dirty[i]) {
            return true;
        }
    }
    return false;
}

bool gcr_flush(gcr_disk_t *d, uint32_t *sectors_written)
{
    if (d->wp || !gcr_dirty(d)) {
        return true;
    }
    FILE *fp = d->file ? d->file : fopen(d->path, "r+b");
    if (fp == NULL) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < GCR_HALF_TRACKS * 2 && ok; i++) {
        if (!d->dirty[i] || d->track[i] == NULL) {
            continue;
        }
        d->dirty[i] = false;
        ok = write_track(d, i, fp, sectors_written);
    }
    if (fp != d->file) {
        fclose(fp);
    }
    return ok;
}

/* A sector of a blank disk: the BAM (18/0), the first directory sector
 * (18/1), on a D71 side 2's BAM (53/0); all else is zeros. False: zeros
 */
static bool blank_sector(int track, int sector, bool d71, const char *name, uint8_t *b)
{
    int sides = d71 ? 2 : 1;
    memset(b, 0, 256);
    if (track == 18 && sector == 0) {
        b[0] = 18;
        b[1] = 1;
        b[2] = 0x41;                    /* 'A': 1541 format */
        b[3] = d71 ? 0x80 : 0x00;       /* double-sided flag */
        for (int t = 1; t <= 35; t++) {
            int n = gcr_sectors(t, sides);
            uint8_t *e = b + 4 * t;
            uint32_t bits = (1u << n) - 1;
            if (t == 18) {
                bits &= ~3u;            /* BAM and the first directory sector */
                n -= 2;
            }
            e[0] = n;
            e[1] = bits;
            e[2] = bits >> 8;
            e[3] = bits >> 16;
        }
        memset(b + 0x90, 0xA0, 0x1B);
        memcpy(b + 0x90, name, strlen(name) < 16 ? strlen(name) : 16);
        b[0xA2] = '0';
        b[0xA3] = '1';
        b[0xA5] = '2';
        b[0xA6] = 'A';
        if (d71) {
            for (int t = 36; t <= 70; t++) {
                b[0xDD + t - 36] = t == 53 ? 0 : gcr_sectors(t, sides);     /* side 2's free counts */
            }
        }
        return true;
    }
    if (track == 18 && sector == 1) {
        b[1] = 0xFF;
        return true;
    }
    if (d71 && track == 53 && sector == 0) {
        for (int t = 36; t <= 70; t++) {    /* side 2's bitmap; track 53 reserved on a 1571 */
            uint32_t bits = t == 53 ? 0 : (1u << gcr_sectors(t, sides)) - 1;
            uint8_t *e = b + (t - 36) * 3;
            e[0] = bits;
            e[1] = bits >> 8;
            e[2] = bits >> 16;
        }
        return true;
    }
    return false;
}

/* Written as it is made, a few KB at a time (a board without PSRAM has no
 * room for a whole image)
 */
bool gcr_create(const char *path, bool d71, const char *name)
{
    int sides = d71 ? 2 : 1, tracks = d71 ? 70 : 35;
    uint8_t *buf = cbm_malloc(4096);
    FILE *fp = buf ? fopen(path, "wb") : NULL;
    bool ok = fp != NULL;
    int used = 0;
    for (int t = 1; t <= tracks && ok; t++) {
        for (int k = 0; k < gcr_sectors(t, sides) && ok; k++) {
            blank_sector(t, k, d71, name, buf + used);
            used += 256;
            if (used == 4096) {
                ok = fwrite(buf, 1, used, fp) == (size_t)used;
                used = 0;
            }
        }
    }
    if (ok && used) {
        ok = fwrite(buf, 1, used, fp) == (size_t)used;
    }
    if (fp) {
        fclose(fp);
    }
    cbm_free(buf);
    return ok;
}
