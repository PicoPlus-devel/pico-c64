/*
 * Fast load (pico-c64): LOAD from drive 8 straight out of the disk image,
 * instead of byte by byte over the serial bus. The KERNAL and the 1541 DOS
 * move about 400 bytes a second; a 150-block file takes some 95 seconds.
 *
 * The KERNAL's LOAD ($FFD5) goes through the vector at $0330 to $F4A5; the
 * CPU stops there (stop_pc, cbm.c). When the KERNAL ROM is in and has its
 * own code there, the device is 8 with a disk in and it is a LOAD (not a
 * VERIFY) of a program file, the file is read from the disk as the drive's
 * head would read it (gcr_read_sector(): writes not yet flushed included),
 * stored in RAM, and the routine returns as the KERNAL's does. Anything else
 * - "$", a file that isn't there or isn't a program, a sector that can't be
 * read, a LOAD vector a cartridge or a loader changed - goes on into the ROM,
 * and the emulated 1541 answers as it would.
 */
#include <string.h>
#include "cbm_internal.h"
#include "gcr.h"

#define LOAD_PC         0xF4A5          /* the KERNAL's LOAD, through ($0330) */

static bool s_on;

void cbm_fast_load(bool on)
{
    s_on = on;
}

uint32_t fastload_pc(void)
{
    return s_on && cbm.cfg.model == CBM_MODEL_C64 ? LOAD_PC : 0x10000;
}

/* A file name pattern as the DOS matches it: '*' the rest, '?' any one */
static bool name_matches(const uint8_t *pattern, int n, const uint8_t *name)
{
    for (int i = 0; i < 16; i++) {
        if (i == n) {
            return name[i] == 0xA0;     /* the name ends here too */
        }
        if (pattern[i] == '*') {
            return true;
        }
        if (pattern[i] != '?' && pattern[i] != name[i]) {
            return false;
        }
    }
    return n == 16;
}

/* The directory entry of the first file the pattern matches: its first
 * track and sector; false if there is none or it isn't a program file
 */
static bool find_file(gcr_disk_t *disk, const uint8_t *pattern, int n, uint8_t *track, uint8_t *sector)
{
    uint8_t s[256];
    int t = 18, k = 1;
    for (int count = 0; t && count < 40; count++) {
        if (!gcr_read_sector(disk, t, k, s)) {
            return false;
        }
        for (int e = 0; e < 8; e++) {
            const uint8_t *entry = s + e * 32;
            if (!(entry[2] & 0x80) || (entry[2] & 7) == 0) {
                continue;               /* deleted, not closed, or DEL (directory art): the DOS skips them */
            }
            if (name_matches(pattern, n, entry + 5)) {
                if ((entry[2] & 7) != 2) {
                    return false;       /* not PRG: the DOS's answer */
                }
                *track = entry[3];
                *sector = entry[4];
                return true;
            }
        }
        t = s[0];
        k = s[1];
    }
    return false;
}

bool fastload_trap(m6502_t *c)
{
    const uint8_t *ram = cbm.cfg.ram;
    static const uint8_t code[6] = { 0x85, 0x93, 0xA9, 0x00, 0x85, 0x90 };    /* STA $93: LDA #0: STA $90 */
    if (c->rd[LOAD_PC >> 8] != cbm.kernal + ((LOAD_PC >> 8) - 0xE0) * 256 ||
        memcmp(cbm.kernal + (LOAD_PC - 0xE000), code, sizeof(code)) != 0) {
        return false;                   /* RAM there, or another KERNAL */
    }
    gcr_disk_t *disk = drive_disk(0);
    int n = ram[0xB7];
    if (c->a != 0 || ram[0xBA] != 8 || disk == NULL || n == 0 || n > 18) {
        return false;
    }
    uint8_t pattern[18];
    uint16_t at = ram[0xBB] | (ram[0xBC] << 8);
    for (int i = 0; i < n; i++) {
        pattern[i] = cbm_peek(at + i);
    }
    /* "0:NAME" and ":NAME" are NAME on drive 0 */
    const uint8_t *p = pattern;
    for (int i = 0; i < n && i < 2; i++) {
        if (pattern[i] == ':') {
            p = pattern + i + 1;
            n -= i + 1;
            break;
        }
    }
    uint8_t t, s;
    if (n == 0 || p[0] == '$' || !find_file(disk, p, n, &t, &s)) {
        return false;
    }
    /* the file, sector by sector: 254 bytes each, the last one's count in byte 1 */
    uint8_t block[256];
    uint32_t addr = 0, start = 0;
    bool first = true;
    for (int count = 0; t && count < 800; count++) {
        if (!gcr_read_sector(disk, t, s, block)) {
            return false;               /* the drive tries (and reports) it */
        }
        int last = block[0] ? 255 : block[1];
        int from = 2;
        if (first) {
            if (last < 3) {
                return false;
            }
            uint16_t own = block[2] | (block[3] << 8);
            addr = start = ram[0xB9] ? own : ram[0xC3] | (ram[0xC4] << 8);   /* SA 0: the address asked for */
            from = 4;
            first = false;
        }
        for (int i = from; i <= last && addr < 0x10000; i++) {
            cbm.cfg.ram[addr++] = block[i];
        }
        t = block[0];
        s = block[1];
    }
    if (first) {
        return false;
    }
    /* as the KERNAL's LOAD returns: the end address, status EOI, carry clear */
    uint16_t end = addr > 0xFFFF ? 0xFFFF : addr;
    cbm.cfg.ram[0x93] = 0;
    cbm.cfg.ram[0x90] = 0x40;
    cbm.cfg.ram[0xAE] = end & 0xFF;
    cbm.cfg.ram[0xAF] = end >> 8;
    c->x = end & 0xFF;
    c->y = end >> 8;
    c->p &= ~M6502_C;
    /* RTS */
    uint8_t lo = ram[0x100 | (uint8_t)(c->s + 1)], hi = ram[0x100 | (uint8_t)(c->s + 2)];
    c->s += 2;
    c->pc = ((hi << 8) | lo) + 1;
    CBM_LOG("fast load: $%04X-$%04X", (unsigned)start, (unsigned)end);
    return true;
}
