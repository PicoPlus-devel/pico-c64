/*
 * The C128's VDC (8563): 80 columns from its own 16 or 64 KB of RAM,
 * reached through two registers:
 *
 *   $D600 write: register number; read: status (bit 7 ready, 5 vertical
 *         blank, 0-2 version)
 *   $D601 the register's value; R31 reads or writes the RAM at the update
 *         address (R18/R19) and moves it on
 *
 * Text (R25 bit 7 clear): the screen at R12/R13, R1 characters a row, R6
 * rows of R9 + 1 lines; with attributes (R25 bit 6) one byte per character
 * at R20/R21: bits 0-3 colour (RGBI), 4 blink, 5 underline, 6 reverse,
 * 7 the second 256 characters. Characters are 16 bytes each in the set at
 * R28 bits 5-7 (8 KB steps). Bitmap (R25 bit 7): 80 bytes a line, the
 * attributes' colours per character cell. R30 starts a block fill (R31's
 * value) or copy (R24 bit 7, from R32/R33) of that many bytes.
 *
 * Drawn once a frame into an 800 x 200 picture: each character cell of 8
 * pixels 10 wide (two of its pixels doubled), so the panel's 800 pixels
 * show the 80 columns evenly; the board triples the lines. Or (pico-c64,
 * no cfg.vdc_frame) the board asks for each line as it scans it out,
 * 640 pixels: cbm_vdc_line().
 */
#include <string.h>
#include "cbm_internal.h"

#define VRAM_SIZE   0x10000

static uint8_t *s_vram;
static uint8_t *s_own;                  /* 64 KB of its own (no cfg.vdc_ram) */
static uint16_t s_mask = 0xFFFF;        /* the RAM's size - 1 */
static uint8_t r[38];
static uint8_t s_reg;                   /* selected register */
static uint32_t s_frames;
static bool s_dirty = true;             /* RAM or registers changed since the last picture */
static uint32_t s_signature;

/* RGBI */
static const uint16_t s_rgbi[16] = {
#define RGB(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
    RGB(0x00, 0x00, 0x00), RGB(0x55, 0x55, 0x55), RGB(0x00, 0x00, 0xAA), RGB(0x55, 0x55, 0xFF),
    RGB(0x00, 0xAA, 0x00), RGB(0x55, 0xFF, 0x55), RGB(0x00, 0xAA, 0xAA), RGB(0x55, 0xFF, 0xFF),
    RGB(0xAA, 0x00, 0x00), RGB(0xFF, 0x55, 0x55), RGB(0xAA, 0x00, 0xAA), RGB(0xFF, 0x55, 0xFF),
    RGB(0xAA, 0x55, 0x00), RGB(0xFF, 0xFF, 0x55), RGB(0xAA, 0xAA, 0xAA), RGB(0xFF, 0xFF, 0xFF),
#undef RGB
};

/* The 4416 chips (16 KB) see 14 address bits, the 4164s (64 KB) 16 */
static inline uint16_t vaddr(uint16_t a)
{
    return (r[28] & 0x10) ? (a & s_mask) : (a & 0x3FFF);
}

static inline uint16_t reg16(int hi)
{
    return (r[hi] << 8) | r[hi + 1];
}

static inline void set_reg16(int hi, uint16_t v)
{
    r[hi] = v >> 8;
    r[hi + 1] = v & 0xFF;
}

void vdc_init(void)
{
    if (cbm.cfg.vdc_ram) {
        s_vram = cbm.cfg.vdc_ram;
        s_mask = 0x3FFF;
        return;
    }
    if (s_own == NULL) {
        s_own = cbm_calloc(1, VRAM_SIZE);
    }
    s_vram = s_own;
    s_mask = 0xFFFF;
}

void vdc_reset(void)
{
    if (s_vram) {
        memset(s_vram, 0, s_mask + 1);
    }
    memset(r, 0, sizeof(r));
    /* what the KERNAL sets up anyway: 80 x 25 */
    r[0] = 126; r[1] = 80; r[4] = 39; r[6] = 25; r[9] = 7; r[22] = 0x78; r[23] = 8; r[26] = 0xF0;
    r[28] = 0x20;
    s_reg = 0;
    s_dirty = true;
}

/* ---- The CPU's side ---- */

uint8_t CBM_HOT vdc_read(uint16_t a)
{
    if (!(a & 1)) {
        /* ready; vertical blank over the VIC's last lines (the VDC keeps
         * its own time, but about the same frame rate)
         */
        int line = cbm.line;
        bool vblank = line < 16 || line >= 300;
        return 0x80 | (vblank ? 0x20 : 0) | 0x01;
    }
    switch (s_reg) {
    case 31: {
        uint16_t u = reg16(18);
        uint8_t v = s_vram[vaddr(u)];
        set_reg16(18, u + 1);
        return v;
    }
    case 16: case 17:
        return 0;                       /* light pen */
    default:
        return s_reg < sizeof(r) ? r[s_reg] : 0xFF;
    }
}

/* R30: a block of `count` bytes from the update address on */
static void CBM_HOT block(uint8_t count)
{
    int n = count ? count : 256;
    uint16_t u = reg16(18);
    if (r[24] & 0x80) {
        uint16_t src = reg16(32);
        for (int i = 0; i < n; i++) {
            s_vram[vaddr(u++)] = s_vram[vaddr(src++)];
        }
        set_reg16(32, src);
    } else {
        for (int i = 0; i < n; i++) {
            s_vram[vaddr(u++)] = r[31];
        }
    }
    set_reg16(18, u);
}

void CBM_HOT vdc_write(uint16_t a, uint8_t v)
{
    if (!(a & 1)) {
        s_reg = v & 0x3F;
        return;
    }
    s_dirty = true;
    switch (s_reg) {
    case 31: {
        uint16_t u = reg16(18);
        s_vram[vaddr(u)] = v;
        r[31] = v;
        set_reg16(18, u + 1);
        break;
    }
    case 30:
        r[30] = v;
#ifdef CBM_VDC_LOG
        CBM_LOG("VDC block %s %u at $%04X (R31 $%02X, attr base $%04X)", (r[24] & 0x80) ? "copy" : "fill", v ? v : 256,
                reg16(18), r[31], reg16(20));
#endif
        block(v);
        break;
    default:
        if (s_reg < sizeof(r)) {
            r[s_reg] = v;
        }
        break;
    }
}

/* ---- The picture ---- */

/* 8 pixels into 10: pixels 1 and 5 twice */
static inline void put_cell(uint16_t *dst, uint8_t bits, uint16_t fg, uint16_t bg)
{
    static const uint8_t src_bit[10] = { 7, 6, 6, 5, 4, 3, 2, 2, 1, 0 };
    for (int i = 0; i < 10; i++) {
        dst[i] = (bits >> src_bit[i]) & 1 ? fg : bg;
    }
}

static void draw(void)
{
    uint16_t *frame = cbm.cfg.vdc_frame;
    const int cols = r[1] > 80 ? 80 : r[1];
    const int rows = r[6];
    const int char_h = (r[9] & 0x1F) + 1;
    const int stride = r[1] + r[27];
    const bool bitmap = r[25] & 0x80, attrs = r[25] & 0x40;
    const bool reverse_all = r[24] & 0x40;
    const uint16_t screen = reg16(12), attr_base = reg16(20), cursor = reg16(14);
    const uint16_t charset = (r[28] & 0xE0) << 8;
    const int underline = r[29] & 0x1F;
    /* blink: 1/16 or 1/32 of the frame rate; the cursor (R10 bits 5-6):
     * steady, off, 1/16, 1/32
     */
    const bool blink_on = (s_frames >> ((r[24] & 0x20) ? 4 : 3)) & 1;
    const int cursor_mode = (r[10] >> 5) & 3;
    const bool cursor_on = cursor_mode == 0 || (cursor_mode >= 2 && ((s_frames >> (cursor_mode == 2 ? 3 : 4)) & 1));
    const int cursor_top = r[10] & 0x1F, cursor_bottom = r[11] & 0x1F;
    const uint16_t bg = s_rgbi[r[26] & 0x0F], fg_global = s_rgbi[r[26] >> 4];

    for (int y = 0; y < CBM_VDC_HEIGHT; y++) {
        uint16_t *dst = frame + y * CBM_VDC_WIDTH;
        int row = y / char_h, line = y % char_h;
        if (row >= rows || char_h <= 0) {
            for (int x = 0; x < CBM_VDC_WIDTH; x++) {
                dst[x] = bg;
            }
            continue;
        }
        for (int col = 0; col < 80; col++, dst += 10) {
            if (col >= cols) {
                put_cell(dst, 0, bg, bg);
                continue;
            }
            uint16_t at = row * stride + col;
            uint8_t attr = attrs ? s_vram[vaddr(attr_base + at)] : 0;
            uint16_t fg = attrs ? s_rgbi[attr & 0x0F] : fg_global;
            uint8_t bits;
            if (bitmap) {
                bits = s_vram[vaddr(screen + y * stride + col)];
            } else {
                uint8_t code = s_vram[vaddr(screen + at)];
                int ch = code | ((attr & 0x80) ? 0x100 : 0);
                bits = line < 16 ? s_vram[vaddr(charset + ch * 16 + line)] : 0;
                if ((attr & 0x20) && line == underline) {
                    bits = 0xFF;
                }
                if ((attr & 0x10) && blink_on) {
                    bits = 0;
                }
                if (attr & 0x40) {
                    bits = ~bits;
                }
                if (cursor_on && (uint16_t)(screen + at) == cursor && line >= cursor_top && line <= cursor_bottom) {
                    bits = ~bits;
                }
            }
            if (reverse_all) {
                bits = ~bits;
            }
            put_cell(dst, bits, fg, bg);
        }
    }
}

/* Drawn only while shown (`shown`: the 80-column screen is on) */
void vdc_frame_end(bool shown)
{
    s_frames++;
    if (!shown) {
        s_dirty = true;                 /* (all of it next time it's shown) */
        return;
    }
    /* blinking and the cursor change the picture too */
    bool blinking = (s_frames & 7) == 0;
    if (s_dirty || blinking) {
        s_dirty = false;
        if (cbm.cfg.vdc_frame) {
            draw();
        }
        s_signature++;                  /* (a board drawing it itself: cbm_vdc_line()) */
    }
}

uint32_t vdc_signature(void)
{
    return s_signature;
}

/* ---- Line by line, for the board (pico-c64) ---- */

int cbm_vdc_lines(void)
{
    return r[6] * ((r[9] & 0x1F) + 1);
}

/* As draw(), one line, 8 pixels a cell, as colour numbers two to a byte.
 * A character row's codes and attributes are read once, at its first line
 * (the board asks for y = 0, 1, 2, ... in order); each cell's 8 pixels are
 * then one lookup: s_expand[bits] has a nibble of 1s for each pixel set.
 */
static uint32_t s_expand[256];
static struct {
    int row;                            /* the character row decoded; -1 none */
    uint16_t glyph[80];                 /* its character's first line in the set */
    uint8_t fg[80];
    uint8_t mode[80];                   /* CELL_* */
} s_cells = { .row = -1 };

enum { CELL_UNDERLINE = 1, CELL_BLANK = 2, CELL_REVERSE = 4, CELL_CURSOR = 8, CELL_EMPTY = 16 };

static void CBM_HOT decode_row(int row)
{
    const int cols = r[1] > 80 ? 80 : r[1];
    const int stride = r[1] + r[27];
    const bool attrs = r[25] & 0x40;
    const uint16_t screen = reg16(12), attr_base = reg16(20), cursor = reg16(14);
    const uint16_t charset = (r[28] & 0xE0) << 8;
    const bool blink_on = (s_frames >> ((r[24] & 0x20) ? 4 : 3)) & 1;
    const int cursor_mode = (r[10] >> 5) & 3;
    const bool cursor_on = cursor_mode == 0 || (cursor_mode >= 2 && ((s_frames >> (cursor_mode == 2 ? 3 : 4)) & 1));
    for (int col = 0; col < 80; col++) {
        if (col >= cols) {
            s_cells.mode[col] = CELL_EMPTY;
            continue;
        }
        uint16_t at = row * stride + col;
        uint8_t attr = attrs ? s_vram[vaddr(attr_base + at)] : 0;
        uint8_t code = s_vram[vaddr(screen + at)];
        s_cells.glyph[col] = charset + (code | ((attr & 0x80) ? 0x100 : 0)) * 16;
        s_cells.fg[col] = attrs ? attr & 0x0F : r[26] >> 4;
        s_cells.mode[col] = ((attr & 0x20) ? CELL_UNDERLINE : 0) | ((attr & 0x10) && blink_on ? CELL_BLANK : 0) |
                            ((attr & 0x40) ? CELL_REVERSE : 0) |
                            (cursor_on && (uint16_t)(screen + at) == cursor ? CELL_CURSOR : 0);
    }
    s_cells.row = row;
}

void CBM_HOT cbm_vdc_line(int y, uint8_t *dst)
{
    uint32_t *d = (uint32_t *)dst;
    const int char_h = (r[9] & 0x1F) + 1;
    const int row = y / char_h, line = y % char_h;
    const uint8_t bg = r[26] & 0x0F;
    const uint32_t bg8 = bg * 0x11111111u;
    if (y < 0 || row >= r[6] || s_vram == NULL) {
        for (int i = 0; i < 80; i++) {
            d[i] = bg8;
        }
        return;
    }
    if (s_expand[1] == 0) {
        for (int b = 0; b < 256; b++) {
            uint32_t m = 0;
            for (int p = 0; p < 8; p++) {
                if (b & (0x80 >> p)) {
                    m |= 0xFu << (4 * p);   /* pixel p: byte p / 2, the low nibble first */
                }
            }
            s_expand[b] = m;
        }
    }
    const uint8_t reverse_all = (r[24] & 0x40) ? 0xFF : 0;
    if (r[25] & 0x80) {
        /* bitmap: a byte a cell from the screen address, the colours from the attributes */
        const int stride = r[1] + r[27];
        const uint16_t screen = reg16(12);
        if (line == 0 || s_cells.row != row) {
            decode_row(row);
        }
        for (int col = 0; col < 80; col++) {
            if (s_cells.mode[col] & CELL_EMPTY) {
                d[col] = bg8;
                continue;
            }
            uint8_t bits = s_vram[vaddr(screen + y * stride + col)] ^ reverse_all;
            uint32_t m = s_expand[bits];
            d[col] = ((s_cells.fg[col] * 0x11111111u) & m) | (bg8 & ~m);
        }
        return;
    }
    if (line == 0 || s_cells.row != row) {
        decode_row(row);
    }
    const int underline = r[29] & 0x1F;
    const bool cursor_line = line >= (r[10] & 0x1F) && line <= (r[11] & 0x1F);
    for (int col = 0; col < 80; col++) {
        uint8_t mode = s_cells.mode[col];
        if (mode & CELL_EMPTY) {
            d[col] = bg8;
            continue;
        }
        uint8_t bits = line < 16 ? s_vram[vaddr(s_cells.glyph[col] + line)] : 0;
        if (mode) {
            if ((mode & CELL_UNDERLINE) && line == underline) {
                bits = 0xFF;
            }
            if (mode & CELL_BLANK) {
                bits = 0;
            }
            if (mode & CELL_REVERSE) {
                bits = ~bits;
            }
            if ((mode & CELL_CURSOR) && cursor_line) {
                bits = ~bits;
            }
        }
        uint32_t m = s_expand[bits ^ reverse_all];
        d[col] = ((s_cells.fg[col] * 0x11111111u) & m) | (bg8 & ~m);
    }
}

/* The 80 columns as text (cbm_screen_text()): attribute bit 7 is the second set */
int vdc_text(char *out, int size, char (*conv)(uint8_t code, bool lower))
{
    int n = 0;
    if (s_vram == NULL || size <= 0 || (r[25] & 0x80)) {
        return 0;                       /* (bitmap mode: no text) */
    }
    const int cols = r[1] > 80 ? 80 : r[1], rows = r[6] > 50 ? 50 : r[6];
    const int stride = r[1] + r[27];
    const bool attrs = r[25] & 0x40;
    const uint16_t screen = reg16(12), attr_base = reg16(20);
    for (int row = 0; row < rows; row++) {
        int start = n, end = n;
        for (int col = 0; col < cols && n < size - 2; col++) {
            uint16_t at = row * stride + col;
            uint8_t attr = attrs ? s_vram[vaddr(attr_base + at)] : 0;
            char c = conv(s_vram[vaddr(screen + at)], attr & 0x80);
            out[n++] = c;
            if (c != ' ') {
                end = n;
            }
        }
        n = end > start ? end : start;  /* without the spaces at the end */
        if (n < size - 1) {
            out[n++] = '\n';
        }
    }
    out[n < size ? n : size - 1] = 0;
    return n;
}
