/*
 * The datasette (C2N / 1530) with a .TAP image (pico-c64).
 *
 * A TAP file holds the time from one falling edge of the read signal to the
 * next: a byte of 8-cycle units, or (version 1) a 0 and the cycles in the
 * next three bytes (version 0: a 0 is an overflow, 256 units). The tape
 * turns while PLAY is down and the 6510 runs the motor (port bit 5 low);
 * each edge reaches CIA 1's FLAG at its own cycle (cbm.c schedules it like
 * a CIA event), so the KERNAL's loader and turbo loaders see the timing they
 * expect. PLAY is the sense line, port bit 4 (low: down).
 *
 * Recording (RECORD and PLAY down): the time between rising edges of the
 * write line, port bit 3, while the motor runs, into a new TAP version 1.
 *
 * The image is read 2 KB at a time, from inside the emulation: a turbo
 * loader takes about 2 KB a second.
 */
#include <string.h>
#include "cbm_internal.h"

#define TAP_HEADER      20
#define BUF_SIZE        2048

static struct {
    FILE *file;
    char path[256];
    uint8_t version;
    uint32_t end;                       /* file offset after the last pulse */
    uint32_t pos;                       /* file offset of the next pulse */
    uint8_t buf[BUF_SIZE];
    uint32_t buf_off, buf_len;          /* buf holds the file from buf_off */
    bool play;                          /* PLAY (with RECORD when recording) down */
    bool motor;
    uint64_t next_edge;                 /* the next falling edge (UINT64_MAX: none) */
    uint64_t left;                      /* motor stopped: cycles to the next edge */
    /* recording */
    bool recording;
    bool write_level;
    uint64_t write_edge;                /* the last rising edge, 0: none yet */
    uint32_t written;                   /* bytes of pulses in the file */
} tape = { .next_edge = UINT64_MAX };

/* ---- Playing ---- */

static int next_byte(void)
{
    if (tape.pos >= tape.end) {
        return -1;
    }
    if (tape.pos < tape.buf_off || tape.pos >= tape.buf_off + tape.buf_len) {
        uint32_t n = tape.end - tape.pos < BUF_SIZE ? tape.end - tape.pos : BUF_SIZE;
        if (fseek(tape.file, tape.pos, SEEK_SET) != 0 || fread(tape.buf, 1, n, tape.file) != n) {
            return -1;
        }
        tape.buf_off = tape.pos;
        tape.buf_len = n;
    }
    return tape.buf[tape.pos++ - tape.buf_off];
}

/* The next pulse's length in cycles; 0 at the end of the tape */
static uint32_t next_pulse(void)
{
    int b = next_byte();
    if (b < 0) {
        return 0;
    }
    if (b > 0) {
        return b * 8;
    }
    if (tape.version == 0) {
        return 256 * 8;
    }
    uint32_t v = 0;
    for (int i = 0; i < 3; i++) {
        int c = next_byte();
        if (c < 0) {
            return 0;
        }
        v |= (uint32_t)c << (8 * i);
    }
    return v ? v : 256 * 8;
}

uint64_t tape_next_edge(void)
{
    return tape.next_edge;
}

void tape_run(uint64_t now)
{
    while (tape.next_edge <= now) {
        cia_flag(&cbm.cia1, tape.next_edge);
        uint32_t d = next_pulse();
        tape.next_edge = d ? tape.next_edge + d : UINT64_MAX;
    }
}

/* The motor or PLAY changed: the tape starts or stops turning */
static void transport(uint64_t now)
{
    bool turning = tape.file && tape.play && tape.motor && !tape.recording;
    if (turning && tape.next_edge == UINT64_MAX) {
        uint64_t d = tape.left ? tape.left : next_pulse();
        tape.left = 0;
        tape.next_edge = d ? now + d : UINT64_MAX;
    } else if (!turning && tape.next_edge != UINT64_MAX) {
        tape.left = tape.next_edge > now ? tape.next_edge - now : 1;
        tape.next_edge = UINT64_MAX;
    }
    cbm_events_changed();
}

/* ---- Recording ---- */

static void put_byte(uint8_t b)
{
    if (tape.buf_len == BUF_SIZE) {
        fwrite(tape.buf, 1, tape.buf_len, tape.file);
        tape.buf_len = 0;
    }
    tape.buf[tape.buf_len++] = b;
    tape.written++;
}

/* What is recorded so far into the file, and its length into the header */
static void record_flush(void)
{
    if (!tape.recording || tape.file == NULL) {
        return;
    }
    if (tape.buf_len) {
        fwrite(tape.buf, 1, tape.buf_len, tape.file);
        tape.buf_len = 0;
    }
    uint8_t size[4] = { tape.written, tape.written >> 8, tape.written >> 16, tape.written >> 24 };
    long here = ftell(tape.file);
    fseek(tape.file, 16, SEEK_SET);
    fwrite(size, 1, 4, tape.file);
    fseek(tape.file, here, SEEK_SET);
}

void tape_write_line(bool level, uint64_t now)
{
    if (level == tape.write_level) {
        return;
    }
    tape.write_level = level;
    if (!level || !tape.recording || !tape.play || !tape.motor) {
        return;
    }
    if (tape.write_edge) {
        uint64_t d = now - tape.write_edge;
        if (d / 8 >= 1 && d / 8 <= 255) {
            put_byte((uint8_t)(d / 8));
        } else {
            put_byte(0);
            put_byte(d & 0xFF);
            put_byte((d >> 8) & 0xFF);
            put_byte(d > 0xFFFFFF ? 0xFF : (d >> 16) & 0xFF);
        }
    }
    tape.write_edge = now;
}

/* ---- The 6510's port ---- */

bool tape_sense(void)
{
    return tape.file && tape.play;
}

void tape_motor_line(bool on, uint64_t now)
{
    if (on == tape.motor) {
        return;
    }
    tape.motor = on;
    if (!on) {
        tape.write_edge = 0;            /* (a recording resumes with a new pulse) */
        record_flush();
    }
    transport(now);
}

/* ---- The board's side ---- */

void cbm_tape_eject(void)
{
    if (tape.file) {
        record_flush();
        fclose(tape.file);
    }
    bool motor = tape.motor;
    memset(&tape, 0, sizeof(tape));
    tape.motor = motor;
    tape.next_edge = UINT64_MAX;
    cbm_port_refresh();
}

bool cbm_tape_insert(const char *path)
{
    cbm_tape_eject();
    FILE *f = fopen(path, "rb");
    uint8_t h[TAP_HEADER];
    if (f == NULL) {
        return false;
    }
    if (fread(h, 1, TAP_HEADER, f) != TAP_HEADER || memcmp(h, "C64-TAPE-RAW", 12) != 0 || h[12] > 1) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    uint32_t len = h[16] | (h[17] << 8) | (h[18] << 16) | ((uint32_t)h[19] << 24);
    tape.file = f;
    tape.version = h[12];
    tape.end = TAP_HEADER + len < (uint32_t)size ? TAP_HEADER + len : (uint32_t)size;
    tape.pos = TAP_HEADER;
    snprintf(tape.path, sizeof(tape.path), "%s", path);
    cbm_port_refresh();
    return true;
}

bool cbm_tape_record(const char *path)
{
    cbm_tape_eject();
    FILE *f = fopen(path, "w+b");
    static const uint8_t h[TAP_HEADER] = { 'C', '6', '4', '-', 'T', 'A', 'P', 'E', '-', 'R', 'A', 'W', 1 };
    if (f == NULL) {
        return false;
    }
    if (fwrite(h, 1, TAP_HEADER, f) != TAP_HEADER) {
        fclose(f);
        return false;
    }
    tape.file = f;
    tape.version = 1;
    tape.recording = true;
    tape.play = true;                   /* RECORD and PLAY */
    snprintf(tape.path, sizeof(tape.path), "%s", path);
    cbm_port_refresh();
    return true;
}

void cbm_tape_play(bool down)
{
    if (tape.file && !tape.recording && down != tape.play) {
        tape.play = down;
        transport(cbm_now());
        cbm_port_refresh();
    }
}

void cbm_tape_rewind(void)
{
    if (tape.file && !tape.recording) {
        tape.pos = TAP_HEADER;
        tape.left = 0;
        bool turning = tape.next_edge != UINT64_MAX;
        tape.next_edge = UINT64_MAX;
        if (turning) {
            transport(cbm_now());
        }
    }
}

const char *cbm_tape_path(void)
{
    return tape.file ? tape.path : NULL;
}

void cbm_tape_status(cbm_tape_status_t *s)
{
    s->play = tape.file && tape.play;
    s->recording = tape.recording;
    s->motor = tape.motor && tape.play && tape.file;
    s->position = tape.recording ? tape.written : tape.pos > TAP_HEADER ? tape.pos - TAP_HEADER : 0;
    s->length = tape.recording ? tape.written : tape.end > TAP_HEADER ? tape.end - TAP_HEADER : 0;
}
