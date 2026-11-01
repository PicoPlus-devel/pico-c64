/*
 * Keyboard and joysticks, from other tasks (Bluetooth, console).
 *
 * The C64's keyboard is an 8 x 8 matrix: CIA 1 port A selects rows (low),
 * port B reads the columns (low = pressed); scanning the other way round
 * works too. The C128 adds rows K0-K2 (selected through VIC $D02F) and
 * three keys outside the matrix (RESTORE, CAPS LOCK, 40/80 DISPLAY).
 *
 *        PB0   PB1    PB2    PB3   PB4    PB5  PB6   PB7
 *   PA0  DEL   RETURN CRSR-> F7    F1     F3   F5    CRSR-DOWN
 *   PA1  3     W      A      4     Z      S    E     LSHIFT
 *   PA2  5     R      D      6     C      F    T     X
 *   PA3  7     Y      G      8     B      H    U     V
 *   PA4  9     I      J      0     M      K    O     N
 *   PA5  +     P      L      -     .      :    @     ,
 *   PA6  POUND *      ;      HOME  RSHIFT =    ^     /
 *   PA7  1     <-     CTRL   2     SPACE  C=   Q     RUN/STOP
 */
#include <string.h>
#include "cbm_internal.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()      portENTER_CRITICAL(&s_lock)
#define UNLOCK()    portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QUEUE_SIZE  64

enum { EV_KEY, EV_JOY, EV_KEYS_UP };

typedef struct {
    uint8_t type, code, value;
} input_event_t;

static input_event_t s_queue[QUEUE_SIZE];
static volatile uint32_t s_head, s_tail;

static struct {
    uint8_t row[11];            /* pressed columns per row (bit set = pressed); 8-10: K0-K2 */
    bool restore, caps, d40_80;
    uint8_t joy[2];             /* active-high CBM_JOY_* */
} in;

static void push(const input_event_t *ev)
{
    LOCK();
    if (s_head - s_tail < QUEUE_SIZE) {
        s_queue[s_head % QUEUE_SIZE] = *ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    UNLOCK();
}

void cbm_key(uint8_t key, bool down)
{
    input_event_t ev = { .type = EV_KEY, .code = key, .value = down };
    push(&ev);
}

void cbm_joystick(int port, uint8_t bits)
{
    input_event_t ev = { .type = EV_JOY, .code = port, .value = bits };
    push(&ev);
}

void cbm_keys_up(void)
{
    input_event_t ev = { .type = EV_KEYS_UP };
    push(&ev);
}

static void set_key(int key, bool down)
{
    if (key < 0) {
        return;
    }
    if (key < 88) {
        uint8_t bit = 1u << (key & 7);
        in.row[key >> 3] = down ? in.row[key >> 3] | bit : in.row[key >> 3] & ~bit;
    } else if (key == CBM_KEY_RESTORE) {
        in.restore = down;
    } else if (key == CBM_KEY_CAPS_LOCK) {
        if (down) {
            in.caps = !in.caps;         /* a locking key */
        }
    } else if (key == CBM_KEY_40_80) {
        if (down) {
            in.d40_80 = !in.d40_80;
        }
    }
}

/* ---- Typing ---- */

#define TYPE_MAX    256
#define KEY_LSHIFT  15

static char s_type[TYPE_MAX];
static volatile int s_type_len;
static int s_type_pos, s_type_frame;

int cbm_ascii_key(char c, bool *shift)
{
    static const char plain[64] = {
        0, '\n', 0, 0, 0, 0, 0, 0,          '3', 'w', 'a', '4', 'z', 's', 'e', 0,
        '5', 'r', 'd', '6', 'c', 'f', 't', 'x', '7', 'y', 'g', '8', 'b', 'h', 'u', 'v',
        '9', 'i', 'j', '0', 'm', 'k', 'o', 'n', '+', 'p', 'l', '-', '.', ':', '@', ',',
        0, '*', ';', 0, 0, '=', '^', '/',   '1', 0, 0, '2', ' ', 0, 'q', 0,
    };
    static const char shifted[64] = {
        0, 0, 0, 0, 0, 0, 0, 0,             '#', 0, 0, '$', 0, 0, 0, 0,
        '%', 0, 0, '&', 0, 0, 0, 0,         '\'', 0, 0, '(', 0, 0, 0, 0,
        ')', 0, 0, 0, 0, 0, 0, 0,           0, 0, 0, 0, '>', '[', 0, '<',
        0, 0, ']', 0, 0, 0, 0, '?',         '!', 0, 0, '"', 0, 0, 0, 0,
    };
    char lower = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    *shift = lower != c;
    for (int k = 0; k < 64; k++) {
        if (plain[k] && plain[k] == lower) {
            return k;
        }
    }
    for (int k = 0; k < 64; k++) {
        if (shifted[k] && shifted[k] == c) {
            *shift = true;
            return k;
        }
    }
    return -1;
}

void cbm_type(const char *text)
{
    LOCK();
    size_t n = strlen(text);
    if (n >= TYPE_MAX) {
        n = TYPE_MAX - 1;
    }
    memcpy(s_type, text, n);
    s_type[n] = 0;
    s_type_len = n;
    s_type_pos = 0;
    s_type_frame = 0;
    UNLOCK();
}

/* Each character: down for 2 frames, up for 2 (the KERNAL scans 50 times
 * a second and wants a key let go before the same one counts again); 8
 * and 8 under CP/M
 */
static void type_frame(void)
{
    if (s_type_pos >= s_type_len) {
        return;
    }
    bool shift;
    int key = cbm_ascii_key(s_type[s_type_pos], &shift);
    int phase = s_type_frame++;
    if (key < 0) {
        s_type_pos++;
        s_type_frame = 0;
        return;
    }
    /* CP/M on the C128 scans one row an interrupt: longer there (chosen
     * once a key: the CPUs take turns)
     */
    static int hold;
    if (phase == 0) {
        hold = c128_z80_active() ? 8 : 2;
    }
    if (phase == 0) {
        if (shift) {
            set_key(KEY_LSHIFT, true);
        }
        set_key(key, true);
    } else if (phase == hold) {
        set_key(key, false);
        set_key(KEY_LSHIFT, false);
    } else if (phase >= hold * 2) {
        s_type_pos++;
        s_type_frame = 0;
    }
}

/* ---- A board's keyboard over a serial line (pico-c64) ----
 *
 * One key at a time, held until the machine has read its row (and its
 * modifiers' rows) on its own - a keyboard scan selects one row at a time:
 * the KERNAL's, CP/M's on the Z80, a program's - and then let go until a
 * read sees it up. Not for a fixed time, so nothing is lost while BASIC
 * works through a line or the drive is busy. With the KERNAL running the
 * next key also waits for room in its keyboard buffer (C64 $C6 of $0289,
 * C128 $D0 of $0A20), which it would otherwise drop keys from.
 */
/* Frames to wait for a read: only for a program that never reads the row. Long, as
 * CP/M doesn't scan the keyboard while it reloads its CCP from disk after a command
 */
#define FEED_TIMEOUT    500
#define FEED_START      750             /* frames after a reset at most, see feed_frame() */
#define KEY_CTRL        58
#define KEY_CBM         61
#define KEY_RUN_STOP    63
#define KEY_C128_TAB    67
#define KEY_C128_ESC    72

static uint16_t s_read_one, s_read_any; /* rows read since the last frame (8-10: K0-K2) */
static int s_feed_wait;                 /* frames before the first key after a reset */
/* As a typist: the modifiers first, until a scan has seen them (CP/M scans
 * a row an interrupt, and a key whose row comes before SHIFT's in the scan
 * would otherwise count unshifted), then the key down until seen and up
 * until seen, then the modifiers up
 */
enum { FEED_IDLE, FEED_PRESS, FEED_MODS_DOWN, FEED_KEY_DOWN, FEED_KEY_UP, FEED_MODS_UP };
static struct {
    uint8_t phase;
    uint8_t key, mods;
    uint16_t need;                      /* rows still to be read */
    int frames;
} s_feed;

void input_rows_read(uint16_t rows)
{
    s_read_any |= rows;
    if (rows && !(rows & (rows - 1))) {
        s_read_one |= rows;             /* one row on its own: a scan looked at it */
    }
}

static const uint8_t s_mod_keys[3] = { KEY_LSHIFT, KEY_CTRL, KEY_CBM };

static void feed_mods(bool down)
{
    for (int m = 0; m < 3; m++) {
        if (s_feed.mods & (1u << m)) {
            set_key(s_mod_keys[m], down);
        }
    }
}

static uint16_t mod_rows(void)
{
    uint16_t rows = 0;
    for (int m = 0; m < 3; m++) {
        if (s_feed.mods & (1u << m)) {
            rows |= 1u << (s_mod_keys[m] >> 3);
        }
    }
    return rows;
}

/* RESTORE and 40/80 are outside the matrix: no row to wait for */
static uint16_t key_row(void)
{
    return s_feed.key < 88 ? 1u << (s_feed.key >> 3) : 0;
}

static void feed_next(int phase)
{
    s_feed.phase = phase;
    s_feed.frames = 0;
}

static void feed_frame(void)
{
    uint16_t one = s_read_one, any = s_read_any;
    s_read_one = s_read_any = 0;
    /* After a reset the KERNAL clears its keyboard buffer while it starts (and
     * a C128 first looks for a disk to boot): the first key waits until the
     * editor waits for one (C64 $E5CD-$E5D6, C128 $C25E-$C263), or 15 s
     */
    if (s_feed_wait > 0) {
        uint16_t pc = cbm.cpu.pc;
        bool editor = !c128_z80_active() &&
                      (c128_mode() ? pc >= 0xC25E && pc <= 0xC263 : pc >= 0xE5CD && pc <= 0xE5D6);
        s_feed_wait = editor ? 0 : s_feed_wait - 1;
    }
    bool timeout = s_feed.phase != FEED_IDLE && ++s_feed.frames >= FEED_TIMEOUT;
    switch (s_feed.phase) {
    case FEED_PRESS:                    /* reads from now on count */
        if (s_feed.mods) {
            feed_mods(true);
            s_feed.need = mod_rows();
            feed_next(FEED_MODS_DOWN);
            break;
        }
        /* fall through */
    case FEED_MODS_DOWN:
        s_feed.need &= ~one;
        if (s_feed.phase == FEED_PRESS || s_feed.need == 0 || timeout) {
            set_key(s_feed.key, true);
            s_feed.need = key_row();
            feed_next(FEED_KEY_DOWN);
        }
        break;
    case FEED_KEY_DOWN:
        s_feed.need &= ~one;
        if (s_feed.need == 0 || timeout) {
#ifdef CBM_FEED_LOG
            CBM_LOG("key %d (mods %d) down %d frames%s", s_feed.key, s_feed.mods, s_feed.frames, timeout ? " (timeout)" : "");
#endif
            set_key(s_feed.key, false);
            feed_next(FEED_KEY_UP);
        }
        break;
    case FEED_KEY_UP:
        if (!key_row() || (any & key_row()) || timeout) {
            feed_mods(false);
            feed_next(s_feed.mods ? FEED_MODS_UP : FEED_IDLE);
        }
        break;
    case FEED_MODS_UP:
        if ((any & mod_rows()) || timeout) {
            feed_next(FEED_IDLE);
        }
        break;
    default:
        break;
    }
}

/* With the KERNAL running: its keyboard buffer is full */
static bool kernal_buffer_full(void)
{
    if (c128_z80_active()) {
        return false;
    }
    const uint8_t *ram = cbm.cfg.ram;
    int n = c128_mode() ? ram[0xD0] : ram[0xC6];
    int max = c128_mode() ? ram[0x0A20] : ram[0x0289];
    return max >= 1 && max <= 10 && n >= max;  /* (other values: not the KERNAL's) */
}

/* The key for a character, as cbm_type_serial() types it; -1 none */
static int serial_key(char c, uint8_t *mods)
{
    bool c128 = c128_mode() || c128_z80_active();
    *mods = 0;
    switch (c) {
    case '\r': case '\n': return 1;     /* RETURN */
    case 0x08: case 0x7F: return 0;     /* DEL */
    case '\t': return c128 ? KEY_C128_TAB : 60;
    case 0x1B: return c128 ? KEY_C128_ESC : KEY_RUN_STOP;
    case 0x1D: return CBM_KEY_40_80;    /* Ctrl-] */
    case 0x1E: return CBM_KEY_RESTORE;  /* Ctrl-^ */
    case '_': return 57;                /* left arrow */
    case '\\': return 48;               /* pound sign */
    default: break;
    }
    if (c >= 1 && c <= 26) {            /* Ctrl-A to Ctrl-Z */
        *mods = CBM_MOD_CTRL;
        bool shift;
        return cbm_ascii_key('a' + c - 1, &shift);
    }
    if (c >= 'A' && c <= 'Z') {
        c += 32;                        /* letters without SHIFT, whatever their case */
    }
    bool shift;
    int key = cbm_ascii_key(c, &shift);
    *mods = shift ? CBM_MOD_SHIFT : 0;
    return key;
}

/* A drive's motor runs: what is typed now may be thrown away (CP/M reloading
 * its CCP after a command) or answer a question asked later (a LOAD)
 */
static bool drive_busy(void)
{
    cbm_status_t st;
    drives_status(&st);
    for (int i = 0; i < CBM_DRIVES; i++) {
        if (st.drive_motor[i]) {
            return true;
        }
    }
    return false;
}

bool cbm_type_serial(char c)
{
    if (s_feed.phase != FEED_IDLE || s_type_pos < s_type_len) {
        return false;
    }
    uint8_t mods;
    int key = serial_key(c, &mods);
    if (key < 0) {
        return true;                    /* no key for it: skipped */
    }
    /* RESTORE and 40/80 type nothing anyone could miss: at once (40/80 while
     * CP/M is still loading picks its screen)
     */
    if (key < 88 && (s_feed_wait > 0 || drive_busy() || kernal_buffer_full())) {
        return false;
    }
    s_feed.key = key;
    s_feed.mods = mods;
    feed_next(FEED_PRESS);
    return true;
}

bool cbm_typing(void)
{
    return s_feed.phase != FEED_IDLE || s_type_pos < s_type_len;
}

void input_frame(void)
{
    type_frame();
    feed_frame();
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    while (s_tail != head) {
        input_event_t ev = s_queue[s_tail % QUEUE_SIZE];
        __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
        switch (ev.type) {
        case EV_KEY:
            set_key(ev.code, ev.value);
            break;
        case EV_JOY:
            if (ev.code >= 1 && ev.code <= 2) {
                in.joy[ev.code - 1] = ev.value;
            }
            break;
        default:
            memset(in.row, 0, sizeof(in.row));
            in.restore = false;
            in.joy[0] = in.joy[1] = 0;
            break;
        }
    }
}

/* ---- As the CIA sees it ---- */

uint8_t input_columns(uint8_t rows)
{
    uint8_t pressed = 0;
    for (int r = 0; r < 8; r++) {
        if (!(rows & (1u << r))) {
            pressed |= in.row[r];
        }
    }
    return ~pressed;
}

uint8_t input_k_columns(uint8_t k_rows)
{
    uint8_t pressed = 0;
    for (int r = 0; r < 3; r++) {
        if (!(k_rows & (1u << r))) {
            pressed |= in.row[8 + r];
        }
    }
    return ~pressed;
}

uint8_t input_rows(uint8_t columns)
{
    uint8_t pulled = 0;
    for (int r = 0; r < 8; r++) {
        if (in.row[r] & ~columns) {
            pulled |= 1u << r;
        }
    }
    return ~pulled;
}

uint8_t input_joystick(int port)
{
    return ~(in.joy[(port - 1) & 1] & 0x1F);
}

bool input_restore(void)
{
    return in.restore;
}

bool input_caps_lock(void)
{
    return in.caps;
}

bool input_40_80(void)
{
    return in.d40_80;
}

void input_reset(void)
{
    memset(in.row, 0, sizeof(in.row));
    in.restore = false;
    s_feed.phase = FEED_IDLE;
    s_feed_wait = FEED_START;
}
