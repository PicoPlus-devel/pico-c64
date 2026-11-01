/*
 * Disk images as GCR tracks (see gcr.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define GCR_HALF_TRACKS     84          /* per side: tracks 1-42 and the halves between */
#define GCR_SLOTS_MAX       4           /* tracks a cache holds at most */

typedef struct gcr_disk {
    char path[256];
    bool wp, g64;
    int sides, tracks;
    /* [side * GCR_HALF_TRACKS + half track]; track t is half track (t - 1) * 2 */
    uint8_t *track[GCR_HALF_TRACKS * 2];
    uint16_t len[GCR_HALF_TRACKS * 2];
    bool dirty[GCR_HALF_TRACKS * 2];
    uint32_t file_off[GCR_HALF_TRACKS * 2];     /* G64 */
    uint8_t *image;                     /* D64/D71: the sectors */
    uint32_t image_size;
    /* With a track cache (gcr_cache_tracks > 0) the image stays in its file:
     * a track is read in (and a D64's encoded) when the head gets to it, into
     * one of `slots` buffers; a dirty one is written back before its buffer
     * is used again. track[] is NULL for the tracks not in memory.
     */
    int slots;                          /* 0: the whole disk is in memory */
    FILE *file;                         /* the image, open while the disk is in */
    uint8_t *slot[GCR_SLOTS_MAX];
    int slot_track[GCR_SLOTS_MAX];      /* half track index in it, -1 none */
    uint32_t slot_used[GCR_SLOTS_MAX];  /* last use, for the least recently used */
    uint32_t use;
    uint8_t *sectors;                   /* D64/D71: one track's sectors */
    int sectors_track;                  /* the track they are, as in the file (0: none) */
    uint8_t *errors;                    /* D64/D71: the error bytes, NULL if none */
    uint8_t id[2];                      /* D64/D71: the disk ID, from the BAM */
} gcr_disk_t;

/* Tracks kept in memory per disk: 0 (the default) the whole disk, read in at
 * gcr_load(); n (1 - GCR_SLOTS_MAX) at most n, read from the file as needed
 */
extern int gcr_cache_tracks;

bool gcr_load(gcr_disk_t *d, const char *path);
/* Track `index` (track[] / len[]) in memory: read in if need be (a cache);
 * NULL if the disk has nothing there or it can't be read
 */
uint8_t *gcr_track(gcr_disk_t *d, int index);
void gcr_free(gcr_disk_t *d);
bool gcr_dirty(const gcr_disk_t *d);
bool gcr_flush(gcr_disk_t *d, uint32_t *sectors_written);
bool gcr_create(const char *path, bool d71, const char *name);
/* A sector's 256 bytes as the head reads them from the GCR now (tracks
 * 1-70: a D71's side 2 is 36-70; a G64's 1-42); false if its header or
 * data block isn't found or is bad
 */
bool gcr_read_sector(gcr_disk_t *d, int track, int sector, uint8_t *out);
/* A track's sectors and speed zone (3: 21 sectors ... 0: 17): a D71's
 * tracks 36-70 (sides 2) are its second side's 1-35, a single-sided disk's
 * 36-42 are zone 0's
 */
int gcr_sectors(int track, int sides);
int gcr_zone(int track, int sides);
