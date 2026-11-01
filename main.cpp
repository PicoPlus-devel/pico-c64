// =====================================================================================
// pico-c64 - Commodore 64 emulator for RP2350
//
// Front end: video, audio, input, menu and the emulation loop, on top of pico_shared.
// The emulated machine lives in cbm/ (esp32cbm's core - see cbm/UPSTREAM.md).
//
// Part of the same family as pico-infonesPlus, pico-smsplus, pico-pacPlus,
// pico-pcePlus, pico-genesisPlus, pico-peanutGB, pico_snesPlus and pico-994A.
// =====================================================================================
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "ff.h"
#include "tusb.h"

#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "menu_settings.h"

#if HSTX
#include "video_output.h"
#include "hstx_data_island_queue.h"
#endif

extern "C"
{
#include "cbm.h"
}

// -------------------------------------------------------------------------------------
// Clocks. 252 MHz at 1.20 V is enough on both video back ends. Measured on a Pico 2 per
// 20 ms frame: gameplay 9 ms, the 1541 loading flat out 16.7 ms (HSTX, the SID on core
// 1). It is also the only clock that gives PicoDVI an exact 60 Hz, and a multiple of
// the 126 MHz HSTX needs for HDMI audio.
//
// HSTX boards offer 384 MHz at 1.50 V as the Overclock setting (ROM browser; it reboots),
// as pico-genesisPlus runs there (8 x 48 MHz: the built-in USB port keeps working): headroom for demos and heavy fast loaders.
// Both can be given at build time (-DC64_CLOCK_KHZ=... -DC64_VOLTAGE=..., CMakeLists.txt).
// -------------------------------------------------------------------------------------
#ifndef EMULATOR_CLOCKFREQ_KHZ
#define EMULATOR_CLOCKFREQ_KHZ 252000
#define VOLTAGE VREG_VOLTAGE_1_20
#endif
#if HSTX
#define EMULATOR_MAX_CLOCKFREQ_KHZ 384000
#define MAX_VOLTAGE VREG_VOLTAGE_1_50
#define OVERCLOCK_SETTING 1
#else
#define OVERCLOCK_SETTING 0
#endif

// The serial keyboard reads the stdio UART's RX pin, so only where a board has a UART console
// and does not use that pin for something else (see "Serial keyboard" below)
#if defined(LIB_PICO_STDIO_UART) && defined(PICO_DEFAULT_UART_RX_PIN)
#define SERIAL_RX_PIN_CLAIMED (                                                  \
       PICO_DEFAULT_UART_RX_PIN == NES_PIN_CLK                                   \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_DATA                                  \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_LAT                                   \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_CLK_1                                 \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_DATA_1                                \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_LAT_1                                 \
    || PICO_DEFAULT_UART_RX_PIN == WII_PIN_SDA                                   \
    || PICO_DEFAULT_UART_RX_PIN == WII_PIN_SCL                                   \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_CS                                 \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_SCK                                \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_MOSI                               \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_MISO)
#define SERIAL_KEYBOARD_AVAILABLE (!SERIAL_RX_PIN_CLAIMED)
#else
#define SERIAL_KEYBOARD_AVAILABLE 0
#endif

// PicoDVI's audio ring takes a whole frame (882 samples) with room to spare
#define AUDIOBUFFERSIZE 2048

// The C64 runs at PAL speed: 50 frames a second, 882 samples of 44.1 kHz audio each.
#define FRAME_US        20000
#define FRAME_SAMPLES   (CBM_AUDIO_HZ / CBM_SCREEN_HZ)

#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 1024
#endif

static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

bool isFatalError = false;
static bool showSettings = false;
static bool key_done = false;
static uint32_t fps = 0;

// -------------------------------------------------------------------------------------
// Settings menu: which options the C64 shows (see menu_settings.h). Filled in at start-up
// by option name, so the list cannot drift out of step with pico_shared's enum.
// -------------------------------------------------------------------------------------
static int8_t g_settings_visibility_c64[MOPT_COUNT];

static void init_settings_visibility(void)
{
    int8_t *v = g_settings_visibility_c64;
    v[MOPT_EXIT_GAME] = 0;                          // always shown in-game
    v[MOPT_RESET_GAME] = 1;
    v[MOPT_REBOOT_TO_LOADER] = BOOTLOADER_BUILD;
    v[MOPT_SCREENMODE] = 1;
    v[MOPT_SCANLINE_TYPE] = HSTX;
    v[MOPT_FPS_OVERLAY] = 1;
    v[MOPT_DISPLAY_MODE] = HSTX && ENABLEDVI;
    v[MOPT_EXTERNAL_AUDIO] = EXT_AUDIO_IS_ENABLED;
    v[MOPT_FONT_COLOR] = 1;
    v[MOPT_FONT_BACK_COLOR] = 1;
    v[MOPT_FRUITJAM_VUMETER] = ENABLE_VU_METER;
    v[MOPT_FRUITJAM_VOLUME_CONTROL] = (HW_CONFIG == 8);
    v[MOPT_OVERCLOCK] = OVERCLOCK_SETTING;     // ROM browser only: it reboots
    v[MOPT_DISK] = 1;                           // in-game only (menu.cpp); N/A without a 1541 ROM
    v[MOPT_CASSETTE] = 1;
    v[MOPT_ENTER_BOOTSEL_MODE] = 1;
    v[MOPT_CONTROLLER_TEST] = 1;
    v[MOPT_SERIAL_KEYBOARD] = SERIAL_KEYBOARD_AVAILABLE;   // hidden where the board has no UART RX
    g_settings_visibility = g_settings_visibility_c64;
}

const uint8_t g_available_screen_modes_c64[] = {
    0, // SCANLINE_8_7
    0, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1  // NOSCANLINE_1_1
};

// =====================================================================================
// Memory for the core
//
// The SDK's malloc panics rather than returning NULL, so a request is checked against
// what is left before it is made. Big blocks (disk tracks as GCR, cartridges, the SID's
// event queue) go to PSRAM when the board has it; the core asks for nothing it touches
// per cycle - RAM and ROMs come from load_machine().
// =====================================================================================
#define CBM_SRAM_BIG    (4 * 1024)      // from this size on, PSRAM if there is any
#define CBM_SRAM_RESERVE (24 * 1024)    // left for the menu, FatFs and the stack

static size_t sram_free(void)
{
    extern char end;            // first byte of the heap (newlib)
    extern char __StackLimit;   // heap ceiling (pico-sdk _sbrk)
    struct mallinfo mi = mallinfo();
    size_t capacity = (size_t)(&__StackLimit - &end);
    size_t claimed = (size_t)mi.arena;
    return (capacity > claimed ? capacity - claimed : 0) + (size_t)mi.fordblks;
}

static bool is_psram(const void *p)
{
    return (uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x12000000u;
}

extern "C" void *cbm_port_malloc(size_t n)
{
    // Small blocks in SRAM while it has room; with PSRAM, what does not fit goes there too
    if (Frens::isPsramEnabled() && (n >= CBM_SRAM_BIG || n + CBM_SRAM_RESERVE > sram_free()))
    {
        return Frens::f_malloc(n);
    }
    if (n + CBM_SRAM_RESERVE > sram_free())
    {
        printf("cbm: no room for %u bytes (%u free)\n", (unsigned)n, (unsigned)sram_free());
        return nullptr;
    }
    return malloc(n);
}

extern "C" void *cbm_port_calloc(size_t n, size_t size)
{
    void *p = cbm_port_malloc(n * size);
    if (p)
    {
        memset(p, 0, n * size);
    }
    return p;
}

extern "C" void cbm_port_free(void *p)
{
    if (p == nullptr)
    {
        return;
    }
    if (is_psram(p))
    {
        Frens::f_free(p);
    }
    else
    {
        free(p);
    }
}

// Where a block for the machine goes. SRAM is the fastest, but how much is left depends
// on the board (PIO USB, for one, takes about 30 KB): blocks are asked for most used
// first, and what no longer fits in SRAM with CBM_SRAM_RESERVE left goes to PSRAM.
enum class Where
{
    SramFirst,  // SRAM, or PSRAM if the board has it and SRAM is full
    Psram,      // PSRAM if the board has it (else SRAM)
};

static uint8_t *alloc_block(size_t size, Where where)
{
    bool sramFits = size + CBM_SRAM_RESERVE <= sram_free();
    if (Frens::isPsramEnabled() && (where == Where::Psram || (where == Where::SramFirst && !sramFits)))
    {
        return (uint8_t *)Frens::f_malloc(size);
    }
    return sramFits ? (uint8_t *)malloc(size) : nullptr;
}

// The machine's RAM and ROMs, allocated per game and released for the menu, most used
// first: RAM (the C128's bank 0, then the VDC's), the KERNAL, BASIC, the character ROM,
// then the drive's DOS. The C128's BASIC 7, its C64 mode's ROM, its 1571 DOS and RAM bank
// 1 always go to PSRAM.
static struct
{
    uint8_t *ram;       // 64 KB (the C128's bank 0)
    uint8_t *ram1;      // C128: RAM bank 1, 64 KB
    uint8_t *basic;     // 8 KB; C128 32 KB
    uint8_t *kernal;    // 8 KB; C128 16 KB
    uint8_t *chargen;   // 4 KB; C128 8 KB
    uint8_t *c64rom;    // C128: its C64 mode's BASIC and KERNAL, 16 KB
    uint8_t *vdcram;    // C128: the VDC's 16 KB
    uint8_t *drive;     // 1541-II DOS 16 KB, 1571 32 KB (optional: no disk drive without it)
} mem;

static cbm_model_t model = CBM_MODEL_C64;  // the machine running

static void vdc_hide(void);

static void free_machine(void)
{
    cbm_disk_eject(0);
    cbm_tape_eject();
    // Ejecting resets the machine, so only while its RAM is still there
    if (cbm_cart_path())
    {
        cbm_cart_eject();
    }
    vdc_hide();
    for (uint8_t *p : { mem.ram, mem.ram1, mem.basic, mem.kernal, mem.chargen, mem.c64rom, mem.vdcram, mem.drive })
    {
        cbm_port_free(p);
    }
    memset(&mem, 0, sizeof(mem));
}

// =====================================================================================
// ROMs
//
// Commodore's ROMs are not distributed with the emulator; they are read from the SD
// card. Both the names esp32cbm uses and VICE's are accepted, in any of these folders.
// =====================================================================================
static const char *const rom_dirs[] = { "/roms/C64/bios", "/bios/c64", "/bios", "/roms/bios" };

static const char *const basic_names[] = { "c64basic.rom", "basic", "basic.901226-01.bin", "basic-901226-01.bin", nullptr };
static const char *const kernal_names[] = { "c64kernal.rom", "kernal", "kernal.901227-03.bin", "kernal-901227-03.bin", nullptr };
static const char *const chargen_names[] = { "c64chargen.rom", "chargen", "characters.901225-01.bin", "chargen-901225-01.bin", nullptr };
static const char *const drive_names[] = { "1541.rom", "dos1541", "1541-II.251968-03.bin", "dos1541-II", nullptr };
// The C128 (the names zimmers.net uses too); BASIC 7.0 is one 32 KB image, low and high
static const char *const c128_basic_names[] = { "c128basic.rom", "basic.318022-02.bin", nullptr };
static const char *const c128_kernal_names[] = { "c128kernal.rom", "kernal.318020-05.bin", nullptr };
static const char *const c128_chargen_names[] = { "c128chargen.rom", "characters.390059-01.bin", nullptr };
static const char *const c128_c64_names[] = { "c128c64.rom", "64c.251913-01.bin", nullptr };
static const char *const c128_drive_names[] = { "1571.rom", "dos1571", "1571-rom.310654-05.bin", nullptr };

static bool romNoRoom;                  // a ROM was found, but there was no memory for it

// One ROM image of exactly `size` bytes, from the first folder that has it (placed as
// alloc_block does). NULL if none, or no room for it (romNoRoom).
static uint8_t *load_rom(const char *const *names, size_t size, Where where = Where::SramFirst)
{
    char path[FF_MAX_LFN];
    for (const char *dir : rom_dirs)
    {
        for (int i = 0; names[i]; i++)
        {
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
            FIL fil;
            if (f_open(&fil, path, FA_READ) != FR_OK)
            {
                continue;
            }
            uint8_t *buf = nullptr;
            UINT n = 0;
            if (f_size(&fil) == size)
            {
                buf = alloc_block(size, where);
                romNoRoom |= buf == nullptr;
                if (buf && (f_read(&fil, buf, size, &n) != FR_OK || n != size))
                {
                    cbm_port_free(buf);
                    buf = nullptr;
                }
            }
            f_close(&fil);
            if (buf)
            {
                printf("ROM %s%s\n", path, is_psram(buf) ? " (PSRAM)" : "");
                return buf;
            }
        }
    }
    return nullptr;
}

static bool load_machine(char *err, size_t errSize)
{
    romNoRoom = false;
    bool c128 = model == CBM_MODEL_C128;
    mem.ram = alloc_block(65536, Where::SramFirst);
    if (c128)
    {
        mem.vdcram = alloc_block(16384, Where::SramFirst);
        mem.kernal = load_rom(c128_kernal_names, 16384);
        mem.chargen = load_rom(c128_chargen_names, 8192);
        mem.basic = load_rom(c128_basic_names, 32768, Where::Psram);
        mem.c64rom = load_rom(c128_c64_names, 16384, Where::Psram);
        mem.drive = load_rom(c128_drive_names, 32768, Where::Psram);
        mem.ram1 = alloc_block(65536, Where::Psram);
    }
    else
    {
        mem.kernal = load_rom(kernal_names, 8192);
        mem.basic = load_rom(basic_names, 8192);
        mem.chargen = load_rom(chargen_names, 4096);
        mem.drive = load_rom(drive_names, 16384);
    }
    if (!mem.ram || (c128 && (!mem.vdcram || !mem.ram1)) || romNoRoom)
    {
        snprintf(err, errSize, "Out of memory");
        return false;
    }
    if (!mem.kernal || !mem.basic || !mem.chargen || (c128 && !mem.c64rom))
    {
        snprintf(err, errSize, c128 ? "C128 ROMs missing in /bios" : "C64 ROMs missing in /bios");
        return false;
    }
    if (!mem.drive)
    {
        printf("No %s ROM: no disk drive\n", c128 ? "1571" : "1541");
    }
    printf("RAM in %s, %u bytes of SRAM left\n", is_psram(mem.ram) ? "PSRAM" : "SRAM", (unsigned)sram_free());
    return true;
}

// =====================================================================================
// Video
//
// The VIC-II hands over each line that changed (cbm_config_t.line) as 400 colour
// numbers: the 320 pixels of the display window with 40 pixels of border either side,
// for raster lines 8-307. The framebuffer is 320x240, so the side borders are left
// out and the 200 display lines get 20 lines of border above and below.
// =====================================================================================
#define WINDOW_LEFT 40      // the display window's first pixel in the VIC's line
#define WINDOW_TOP  23      // the VIC's line shown as framebuffer line 0 (raster 31)

// Pepto's PAL palette, as the core uses it
static const uint8_t c64_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, {0x68, 0x37, 0x2B}, {0x70, 0xA4, 0xB2},
    {0x6F, 0x3D, 0x86}, {0x58, 0x8D, 0x43}, {0x35, 0x28, 0x79}, {0xB8, 0xC7, 0x6F},
    {0x6F, 0x4F, 0x25}, {0x43, 0x39, 0x00}, {0x9A, 0x67, 0x59}, {0x44, 0x44, 0x44},
    {0x6C, 0x6C, 0x6C}, {0x9A, 0xD2, 0x84}, {0x6C, 0x5E, 0xB5}, {0x95, 0x95, 0x95},
};

static WORD c64_palette[16];
// Two pixels at a time: index (left | right << 4), both display pixels in one word
static uint32_t pixel_pairs[256];

static void build_palette(void)
{
    for (int i = 0; i < 16; i++)
    {
        uint8_t r = c64_rgb[i][0], g = c64_rgb[i][1], b = c64_rgb[i][2];
#if HSTX
        c64_palette[i] = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);     // RGB555
#else
        c64_palette[i] = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);      // RGB444
#endif
    }
    for (int i = 0; i < 256; i++)
    {
        pixel_pairs[i] = c64_palette[i & 15] | ((uint32_t)c64_palette[i >> 4] << 16);
    }
}

static inline WORD *get_display_line(int y)
{
#if HSTX
    return hstx_getlineFromFramebuffer(y);
#else
    return &Frens::framebuffer[y * 320];
#endif
}

// The C128's 80 columns are in the framebuffer while they are shown (see below)
static volatile bool vdcShown = false;

static void __not_in_flash_func(c64_line)(int y, const uint8_t *pixels)
{
    int dy = y - WINDOW_TOP;
    if ((unsigned)dy >= 240 || vdcShown)
    {
        return;
    }
    uint32_t *dst = (uint32_t *)get_display_line(dy);
    const uint16_t *src = (const uint16_t *)(pixels + WINDOW_LEFT);
    for (int i = 0; i < 160; i++)
    {
        uint16_t p = src[i];
        dst[i] = pixel_pairs[(p & 0x0F) | ((p >> 4) & 0xF0)];
    }
}

// =====================================================================================
// The C128's 80 columns (HSTX boards)
//
// The VDC's 640 pixels do not fit the framebuffer's 320 in RGB555, but they do as 4-bit
// colour numbers (320 bytes a line). While the 80-column screen is shown, the VIC leaves
// the framebuffer alone; core 0 draws the VDC's picture into it whenever it changes
// (cbm_vdc_line()), and core 1 shows it with a callback of its own: each byte two pixels
// through a table, each line twice (400 of the 480 lines), the border in the background
// colour. Drawing the VDC's cells in the scanline interrupt itself takes longer than a
// line. Otherwise - the 40 columns, the menu - the driver's own callback shows the
// framebuffer.
// =====================================================================================
#if HSTX
extern "C" void scanline_callbackfunc(uint32_t v_scanline, uint32_t active_line, uint32_t *buff);

#define VDC_LINE_BYTES (MODE_H_ACTIVE_PIXELS / 2)

static const uint8_t *vdcFrame;         // the framebuffer, VDC_LINE_BYTES a line
static volatile int vdcTop = 20;        // the picture's first line, in pairs of lines
static volatile int vdcLines = 200;
static volatile uint32_t vdcBorder;
static uint32_t vdc_pairs[256];         // a byte of two RGBI numbers: two RGB555 pixels
static uint32_t vdcSignature;

static void __not_in_flash_func(c128_scanline)(uint32_t v_scanline, uint32_t active_line, uint32_t *buff)
{
    if (!vdcShown)
    {
        scanline_callbackfunc(v_scanline, active_line, buff);
        return;
    }
    int line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    if (line < 0 || line >= MODE_V_ACTIVE_LINES)
    {
        return;
    }
    HSTX_vblank = false;
    int y = (line >> 1) - vdcTop;
    // volatile: a plain loop may become a call to memset, in flash
    volatile uint32_t *dst = buff;
    if ((unsigned)y >= (unsigned)vdcLines)
    {
        uint32_t border = vdcBorder;
        for (int i = 0; i < VDC_LINE_BYTES; i++)
        {
            dst[i] = border;
        }
        return;
    }
    const uint8_t *src = vdcFrame + y * VDC_LINE_BYTES;
    for (int i = 0; i < VDC_LINE_BYTES; i++)
    {
        dst[i] = vdc_pairs[src[i]];
    }
}

static void vdc_init_output(void)
{
    static const uint8_t rgbi[16][3] = {
        {0x00, 0x00, 0x00}, {0x55, 0x55, 0x55}, {0x00, 0x00, 0xAA}, {0x55, 0x55, 0xFF},
        {0x00, 0xAA, 0x00}, {0x55, 0xFF, 0x55}, {0x00, 0xAA, 0xAA}, {0x55, 0xFF, 0xFF},
        {0xAA, 0x00, 0x00}, {0xFF, 0x55, 0x55}, {0xAA, 0x00, 0xAA}, {0xFF, 0x55, 0xFF},
        {0xAA, 0x55, 0x00}, {0xFF, 0xFF, 0x55}, {0xAA, 0xAA, 0xAA}, {0xFF, 0xFF, 0xFF},
    };
    uint16_t palette[16];
    for (int i = 0; i < 16; i++)
    {
        palette[i] = ((rgbi[i][0] >> 3) << 10) | ((rgbi[i][1] >> 3) << 5) | (rgbi[i][2] >> 3);
    }
    for (int i = 0; i < 256; i++)
    {
        vdc_pairs[i] = palette[i & 15] | ((uint32_t)palette[i >> 4] << 16);
    }
    vdcFrame = hstx_getframebuffer();
    video_output_set_scanline_callback(c128_scanline);
}

// After each frame: the screen the C128 shows now (40/80 DISPLAY); the VDC's picture
// drawn again when it changed
static void vdc_update(bool shown)
{
    uint8_t *frame = hstx_getframebuffer();
    if (!shown)
    {
        if (vdcShown)
        {
            // Black until the VIC has drawn its picture again (next frame)
            memset(frame, 0, VDC_LINE_BYTES * 240);
            vdcShown = false;
            cbm_screen_invalidate();
        }
        return;
    }
    int lines = std::min(cbm_vdc_lines(), MODE_V_ACTIVE_LINES / 2);
    uint32_t signature = cbm_screen_signature();
    if (vdcShown && signature == vdcSignature && lines == vdcLines)
    {
        return;
    }
    vdcSignature = signature;
    for (int y = 0; y < lines; y++)
    {
        cbm_vdc_line(y, frame + y * VDC_LINE_BYTES);
    }
    uint8_t border[VDC_LINE_BYTES] __attribute__((aligned(4)));
    cbm_vdc_line(-1, border);
    vdcBorder = vdc_pairs[border[0]];
    vdcLines = lines;
    vdcTop = (MODE_V_ACTIVE_LINES / 2 - lines) / 2;
    vdcShown = true;
}

// Back to the framebuffer (for the menu), once core 1 is out of the callback
static void vdc_hide(void)
{
    if (vdcShown)
    {
        vdcShown = false;
        busy_wait_us(100);
    }
}
#else
static void vdc_init_output(void) {}
static void vdc_update(bool shown) {}
static void vdc_hide(void) {}
#endif

// Frame rate digits in the top border. The VIC redraws only lines that change, so the
// digits stay until the border changes; switching them off redraws the whole picture.
static void draw_text(int x, const char *text)
{
    if (vdcShown)
    {
        return;                         // (the framebuffer holds the 80 columns)
    }
    WORD fgc = c64_palette[1];
    WORD bgc = c64_palette[0];
    for (int row = 0; row < 8; row++)
    {
        WORD *dst = get_display_line(4 + row) + x;
        for (const char *c = text; *c; c++)
        {
            char bits = getcharslicefrom8x8font(*c, row);
            for (int b = 0; b < 8; b++) { *dst++ = (bits & 1) ? fgc : bgc; bits >>= 1; }
        }
    }
}

static void draw_fps(void)
{
    char s[3] = { (char)('0' + ((fps / 10) % 10)), (char)('0' + (fps % 10)), 0 };
    draw_text(4, s);
}

// A short message in the top border, e.g. after a shortcut changed something
static char osdText[24];
static int osdFrames = 0;

static void osd_show(const char *text)
{
    snprintf(osdText, sizeof(osdText), "%s", text);
    osdFrames = 100;                    // two seconds
}

static void draw_osd(void)
{
    if (osdFrames == 0)
    {
        return;
    }
    if (--osdFrames == 0)
    {
        cbm_screen_invalidate();        // the border comes back
        return;
    }
    draw_text(160 - 4 * (int)strlen(osdText), osdText);
}

// =====================================================================================
// Audio
//
// The SID's register writes are queued with their cycle (cbm/core/sid.c) and replayed
// into samples by cbm_audio_render(), which never gets ahead of the emulation.
//
// HSTX: core 1 renders, in its idle loop between scanline interrupts, as much as the
// HDMI data-island queue (or the I2S codec) has room for. A whole frame pushed at once
// (882 samples, 220 packets) would overrun the queue's 200-packet high-water mark, where
// hstx_push_audio_sample() drops packets. Parked while the machine is set up and while
// the menu runs (it plays sounds of its own).
// PicoDVI: core 1 encodes TMDS, so core 0 renders a frame's worth after each frame.
// =====================================================================================
static inline int16_t apply_dvi_gain(int x)
{
    int v = (x * DVI_AUDIO_GAIN_Q8) >> 8;
    return (int16_t)std::max(-32768, std::min(32767, v));
}

static inline bool audio_to_ext(void)
{
#if EXT_AUDIO_IS_ENABLED
#if HSTX
    return settings.flags.useExtAudio || Frens::isHeadPhoneJackConnected();
#else
    return settings.flags.useExtAudio;
#endif
#else
    return false;
#endif
}

// Samples the sink can take now
static int audio_free(bool toExt)
{
#if EXT_AUDIO_IS_ENABLED
    if (toExt)
    {
        return EXT_AUDIO_GET_FREE();
    }
#endif
#if HSTX
    int packets = HSTX_AUDIO_DI_HIGH_WATERMARK - (int)hstx_di_queue_get_level();
    return packets > 0 ? packets * 4 : 0;      // 4 samples per data island
#else
    return dvi_->getAudioRingBuffer().getFullWritableSize();   // (getWritableSize: up to the wrap)
#endif
}

// Samples waiting to be played (HDMI or PicoDVI; 0 means the sink ran dry)
static int audio_queued(void)
{
#if HSTX
    return (int)hstx_di_queue_get_level() * 4;
#else
    return dvi_->getAudioRingBuffer().getFullReadableSize();
#endif
}

static void __not_in_flash_func(audio_push)(const int16_t *buf, int samples, bool toExt)
{
    for (int i = 0; i < samples; i++)
    {
        int l = buf[i * 2], r = buf[i * 2 + 1];
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter) addSampleToVUMeter(l);
#endif
#if EXT_AUDIO_IS_ENABLED
        if (toExt)
        {
            EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
            continue;
        }
#endif
#if HSTX
        hstx_push_audio_sample(apply_dvi_gain(l), apply_dvi_gain(r));
#else
        auto &ring = dvi_->getAudioRingBuffer();
        if (ring.getWritableSize() == 0)
        {
            break;
        }
        int m = apply_dvi_gain(l);
        auto p = ring.getWritePointer();
        *p = {static_cast<short>(m), static_cast<short>(m)};
        ring.advanceWritePointer(1);
#endif
    }
}

#if HSTX
static volatile bool sidTaskEnabled = false;
static volatile bool sidTaskParked = true;
static int16_t sidTaskBuf[64 * 2];

static void __not_in_flash_func(sid_task)(void)
{
    if (!sidTaskEnabled)
    {
        sidTaskParked = true;
        return;
    }
    sidTaskParked = false;
#if ENABLE_VU_METER
    // The LEDs are only written from this core, so a write from core 0 cannot interleave
    static bool vuWasOn;
    bool vuOn = settings.flags.enableVUMeter != 0;
    if (vuWasOn && !vuOn) turnOffAllLeds();
    vuWasOn = vuOn;
#endif
    bool toExt = audio_to_ext();
    int room = audio_free(toExt);
    while (room > 0 && sidTaskEnabled)
    {
        int n = cbm_audio_render(sidTaskBuf, std::min(room, 64));
        if (n == 0)
        {
            break;                              // caught up with the emulation
        }
        audio_push(sidTaskBuf, n, toExt);
        room -= n;
    }
}

static void audio_start(void)
{
    sidTaskEnabled = true;
}

static void audio_park(void)
{
    sidTaskEnabled = false;
    while (!sidTaskParked)
    {
        tight_loop_contents();
    }
}

static void process_audio_frame(void) {}
#else
static int16_t audio_buf[(FRAME_SAMPLES + 64) * 2];   // stereo

static void audio_start(void) {}
static void audio_park(void) {}

// As much as the sink has room for: what does not fit stays in the SID's queue
static void __not_in_flash_func(process_audio_frame)(void)
{
    bool toExt = audio_to_ext();
    int samples = cbm_audio_render(audio_buf, std::min(audio_free(toExt), FRAME_SAMPLES + 64));
    audio_push(audio_buf, samples, toExt);
}
#endif

// =====================================================================================
// Input
//
// The keyboard has two layouts (Settings, Keyboard layout):
//
// US, the default: a key types what is printed on it. Shift+2 is @ (the C64's own key,
// without SHIFT), ' and " are SHIFT+7 and SHIFT+2, [ and ] SHIFT+: and SHIFT+;, \ is the
// pound sign, ^ (Shift+6) the up arrow, _ (Shift+-) the left arrow. Left Ctrl is CTRL,
// Left Alt the Commodore key, Insert INST. Characters the C64 has no key for ({ } | ~)
// do nothing.
//
// C64: by position - a key does what the C64 key in the same place does, so the C64's
// own layout applies (shifted 2 is ", @ is next to P), for games that use keys by place.
//   Tab CTRL   Left Ctrl, Left Alt C=   Insert pound   End =   ` <-   \ up arrow
//
// Both: Esc RUN/STOP, PgUp RESTORE, Home CLR/HOME, Backspace DEL, Caps Lock SHIFT LOCK,
// F2/F4/F6/F8 shifted F1/F3/F5/F7, cursor left and up shifted right and down, F12 the
// settings menu, the numeric keypad a joystick (8 4 6 2 and diagonals, 0 or 5 fire).
//
// The C128's own keys (as esp32cbm maps them): F9 ESC, F10 HELP, Right Alt ALT, End LINE
// FEED (US layout), Scroll Lock NO SCROLL, F11 or Print Screen 40/80 DISPLAY (it locks,
// and picks the screen shown), Caps Lock CAPS LOCK in C128 mode. Its separate cursor
// keys and keypad are not mapped: the C64's cursor keys work in both modes.
//
// Gamepads are joysticks: the first in port 2 (where most games read it), the second in
// port 1; Select+A swaps them. A fires, B is SPACE, X RETURN, Y F1, and Start pressed on
// its own is RUN/STOP. Select+Start opens the settings menu.
// =====================================================================================

// C64 key numbers: row * 8 + column of the keyboard matrix (see cbm/core/input.c)
enum
{
    K_DEL = 0, K_RETURN = 1, K_CRSR_RIGHT = 2, K_F7 = 3, K_F1 = 4, K_F3 = 5, K_F5 = 6, K_CRSR_DOWN = 7,
    K_3 = 8, K_W = 9, K_A = 10, K_4 = 11, K_Z = 12, K_S = 13, K_E = 14, K_LSHIFT = 15,
    K_5 = 16, K_R = 17, K_D = 18, K_6 = 19, K_C = 20, K_F = 21, K_T = 22, K_X = 23,
    K_7 = 24, K_Y = 25, K_G = 26, K_8 = 27, K_B = 28, K_H = 29, K_U = 30, K_V = 31,
    K_9 = 32, K_I = 33, K_J = 34, K_0 = 35, K_M = 36, K_K = 37, K_O = 38, K_N = 39,
    K_PLUS = 40, K_P = 41, K_L = 42, K_MINUS = 43, K_PERIOD = 44, K_COLON = 45, K_AT = 46, K_COMMA = 47,
    K_POUND = 48, K_ASTERISK = 49, K_SEMICOLON = 50, K_HOME = 51, K_RSHIFT = 52, K_EQUAL = 53, K_UP_ARROW = 54, K_SLASH = 55,
    K_1 = 56, K_LEFT_ARROW = 57, K_CTRL = 58, K_2 = 59, K_SPACE = 60, K_CBM = 61, K_Q = 62, K_RUN_STOP = 63,
    // The C128's rows K0-K2
    K_HELP = 64, K_ESC = 72, K_LINE_FEED = 75, K_ALT = 80, K_NO_SCROLL = 87,
};

// For each USB HID usage, unshifted and with Shift held: the C64 key, and what the C64's
// SHIFT does with it - follow the PC's Shift, be released (Shift+2 is @, a key of its
// own) or be held (' is SHIFT+7). Key 0xFF: none.
enum { S_AS_PC = 0, S_OFF = 1, S_ON = 2 };

typedef struct
{
    uint8_t key, shift;
} KeyMap;

static KeyMap keymap[2][256];           // [PC Shift held][HID usage]
static bool symbolicKeys = true;        // the US layout; false: by position

static void map_key(uint8_t hid, uint8_t key, uint8_t shift, uint8_t key_shifted, uint8_t shift_shifted)
{
    keymap[0][hid] = { key, shift };
    keymap[1][hid] = { key_shifted, shift_shifted };
}

// The same C64 key either way, SHIFT as the PC's (or held, for the F2 and cursor keys)
static void map_same(uint8_t hid, uint8_t key, uint8_t shift = S_AS_PC)
{
    map_key(hid, key, shift, key, shift);
}

static void build_keymap(bool symbolic)
{
    symbolicKeys = symbolic;
    for (int i = 0; i < 256; i++)
    {
        keymap[0][i] = keymap[1][i] = { 0xFF, S_AS_PC };
    }
    static const uint8_t letters[26] = {
        K_A, K_B, K_C, K_D, K_E, K_F, K_G, K_H, K_I, K_J, K_K, K_L, K_M,
        K_N, K_O, K_P, K_Q, K_R, K_S, K_T, K_U, K_V, K_W, K_X, K_Y, K_Z,
    };
    for (int i = 0; i < 26; i++)
    {
        map_same(HID_KEY_A + i, letters[i]);
    }
    static const uint8_t digits[10] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0 };
    for (int i = 0; i < 10; i++)
    {
        map_same(HID_KEY_1 + i, digits[i]);
    }
    map_same(HID_KEY_ENTER, K_RETURN);
    map_same(HID_KEY_ESCAPE, K_RUN_STOP);
    map_same(HID_KEY_BACKSPACE, K_DEL);
    map_same(HID_KEY_TAB, K_CTRL);
    map_same(HID_KEY_SPACE, K_SPACE);
    map_same(HID_KEY_HOME, K_HOME);
    map_same(HID_KEY_COMMA, K_COMMA);
    map_same(HID_KEY_PERIOD, K_PERIOD);
    map_same(HID_KEY_SLASH, K_SLASH);
    map_same(HID_KEY_F1, K_F1);
    map_same(HID_KEY_F2, K_F1, S_ON);
    map_same(HID_KEY_F3, K_F3);
    map_same(HID_KEY_F4, K_F3, S_ON);
    map_same(HID_KEY_F5, K_F5);
    map_same(HID_KEY_F6, K_F5, S_ON);
    map_same(HID_KEY_F7, K_F7);
    map_same(HID_KEY_F8, K_F7, S_ON);
    map_same(HID_KEY_ARROW_RIGHT, K_CRSR_RIGHT);
    map_same(HID_KEY_ARROW_LEFT, K_CRSR_RIGHT, S_ON);
    map_same(HID_KEY_ARROW_DOWN, K_CRSR_DOWN);
    map_same(HID_KEY_ARROW_UP, K_CRSR_DOWN, S_ON);
    if (!symbolic)
    {
        map_same(HID_KEY_MINUS, K_PLUS);
        map_same(HID_KEY_EQUAL, K_MINUS);
        map_same(HID_KEY_BRACKET_LEFT, K_AT);
        map_same(HID_KEY_BRACKET_RIGHT, K_ASTERISK);
        map_same(HID_KEY_BACKSLASH, K_UP_ARROW);
        map_same(HID_KEY_SEMICOLON, K_COLON);
        map_same(HID_KEY_APOSTROPHE, K_SEMICOLON);
        map_same(HID_KEY_GRAVE, K_LEFT_ARROW);
        map_same(HID_KEY_INSERT, K_POUND);
        map_same(HID_KEY_END, K_EQUAL);
        return;
    }
    // US: the shifted digits (! @ # $ % ^ & * ( )) as the C64 types them
    map_key(HID_KEY_1, K_1, S_AS_PC, K_1, S_ON);
    map_key(HID_KEY_2, K_2, S_AS_PC, K_AT, S_OFF);
    map_key(HID_KEY_3, K_3, S_AS_PC, K_3, S_ON);
    map_key(HID_KEY_4, K_4, S_AS_PC, K_4, S_ON);
    map_key(HID_KEY_5, K_5, S_AS_PC, K_5, S_ON);
    map_key(HID_KEY_6, K_6, S_AS_PC, K_UP_ARROW, S_OFF);
    map_key(HID_KEY_7, K_7, S_AS_PC, K_6, S_ON);
    map_key(HID_KEY_8, K_8, S_AS_PC, K_ASTERISK, S_OFF);
    map_key(HID_KEY_9, K_9, S_AS_PC, K_8, S_ON);
    map_key(HID_KEY_0, K_0, S_AS_PC, K_9, S_ON);
    map_key(HID_KEY_MINUS, K_MINUS, S_OFF, K_LEFT_ARROW, S_OFF);
    map_key(HID_KEY_EQUAL, K_EQUAL, S_OFF, K_PLUS, S_OFF);
    map_key(HID_KEY_BRACKET_LEFT, K_COLON, S_ON, 0xFF, S_AS_PC);
    map_key(HID_KEY_BRACKET_RIGHT, K_SEMICOLON, S_ON, 0xFF, S_AS_PC);
    map_key(HID_KEY_BACKSLASH, K_POUND, S_OFF, 0xFF, S_AS_PC);
    map_key(HID_KEY_SEMICOLON, K_SEMICOLON, S_OFF, K_COLON, S_OFF);
    map_key(HID_KEY_APOSTROPHE, K_7, S_ON, K_2, S_ON);
    map_key(HID_KEY_GRAVE, K_LEFT_ARROW, S_OFF, 0xFF, S_AS_PC);
    map_same(HID_KEY_INSERT, K_DEL, S_ON);
    map_same(HID_KEY_DELETE, K_DEL, S_OFF);
}

static int joystick_port = 2;           // the first gamepad's port (1 or 2)
static bool shift_lock = false;
static bool c128Mode = false;           // a C128 in its own mode, not C64 mode (after each frame)
static uint8_t keys_down[(CBM_KEYS + 7) / 8];   // as last sent to the core
static uint8_t prevButtons[2];
static bool startAlone = false;         // Start went down with nothing else
static int runStopFrames = 0;           // RUN/STOP held for a few frames after a Start tap

static inline void set_bit(uint8_t *bits, int k) { bits[k >> 3] |= 1u << (k & 7); }
static inline bool get_bit(const uint8_t *bits, int k) { return bits[k >> 3] & (1u << (k & 7)); }

// A pad's buttons in one byte (the same bit layout the NES pads shift out)
enum { PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_SELECT = 1 << 2, PAD_START = 1 << 3,
       PAD_UP = 1 << 4, PAD_DOWN = 1 << 5, PAD_LEFT = 1 << 6, PAD_RIGHT = 1 << 7 };

static void update_input(void)
{
    uint8_t want[sizeof(keys_down)] = {};
    uint8_t joy[2] = {};                // per pad, CBM_JOY_* bits
    uint8_t kbJoy = 0;

    // ---- Keyboard ----
    const auto &kb = io::getCurrentKeyboardState();
    static uint8_t prevKeys[6];
    if (kb.connected)
    {
        int pcShift = (kb.modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) ? 1 : 0;
        int shiftForce = S_AS_PC;
        for (int i = 0; i < 6; i++)
        {
            uint8_t hid = kb.keycode[i];
            if (hid == 0)
            {
                continue;
            }
            bool wasDown = memchr(prevKeys, hid, sizeof(prevKeys)) != nullptr;
            switch (hid)
            {
            case HID_KEY_F12:
                if (!wasDown) showSettings = true;
                continue;
            case HID_KEY_CAPS_LOCK:
                if (c128Mode)
                {
                    set_bit(want, CBM_KEY_CAPS_LOCK);
                }
                else if (!wasDown)
                {
                    shift_lock = !shift_lock;
                }
                continue;
            case HID_KEY_PAGE_UP:
                set_bit(want, CBM_KEY_RESTORE);
                continue;
            case HID_KEY_KEYPAD_8: kbJoy |= CBM_JOY_UP; continue;
            case HID_KEY_KEYPAD_2: kbJoy |= CBM_JOY_DOWN; continue;
            case HID_KEY_KEYPAD_4: kbJoy |= CBM_JOY_LEFT; continue;
            case HID_KEY_KEYPAD_6: kbJoy |= CBM_JOY_RIGHT; continue;
            case HID_KEY_KEYPAD_7: kbJoy |= CBM_JOY_UP | CBM_JOY_LEFT; continue;
            case HID_KEY_KEYPAD_9: kbJoy |= CBM_JOY_UP | CBM_JOY_RIGHT; continue;
            case HID_KEY_KEYPAD_1: kbJoy |= CBM_JOY_DOWN | CBM_JOY_LEFT; continue;
            case HID_KEY_KEYPAD_3: kbJoy |= CBM_JOY_DOWN | CBM_JOY_RIGHT; continue;
            case HID_KEY_KEYPAD_0:
            case HID_KEY_KEYPAD_5: kbJoy |= CBM_JOY_FIRE; continue;
            default:
                break;
            }
            if (model == CBM_MODEL_C128)
            {
                int k = hid == HID_KEY_F9 ? K_ESC : hid == HID_KEY_F10 ? K_HELP
                      : hid == HID_KEY_SCROLL_LOCK ? K_NO_SCROLL
                      : hid == HID_KEY_END && symbolicKeys ? K_LINE_FEED
                      : hid == HID_KEY_F11 || hid == HID_KEY_PRINT_SCREEN ? CBM_KEY_40_80 : -1;
                if (k >= 0)
                {
                    set_bit(want, k);
                    continue;
                }
            }
            const KeyMap &m = keymap[pcShift][hid];
            if (m.key != 0xFF)
            {
                set_bit(want, m.key);
                if (m.shift == S_ON) shiftForce = S_ON;
                else if (m.shift == S_OFF && shiftForce == S_AS_PC) shiftForce = S_OFF;
            }
        }
        // SHIFT: held or released for the key typed (Shift+2 is @), else as the PC's
        if (shiftForce == S_ON)
        {
            set_bit(want, K_LSHIFT);
        }
        else if (shiftForce == S_AS_PC)
        {
            if (kb.modifier & KEYBOARD_MODIFIER_LEFTSHIFT) set_bit(want, K_LSHIFT);
            if (kb.modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) set_bit(want, K_RSHIFT);
            if (shift_lock) set_bit(want, K_LSHIFT);
        }
        if (symbolicKeys)
        {
            if (kb.modifier & KEYBOARD_MODIFIER_LEFTCTRL) set_bit(want, K_CTRL);
            if (kb.modifier & KEYBOARD_MODIFIER_LEFTALT) set_bit(want, K_CBM);
        }
        else if (kb.modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTALT))
        {
            set_bit(want, K_CBM);
        }
        if (kb.modifier & KEYBOARD_MODIFIER_RIGHTCTRL) kbJoy |= CBM_JOY_FIRE;
        if ((kb.modifier & KEYBOARD_MODIFIER_RIGHTALT) && model == CBM_MODEL_C128) set_bit(want, K_ALT);
    }
    memcpy(prevKeys, kb.keycode, sizeof(prevKeys));

    // ---- Gamepads ----
    int pad = 0;
    for (int i = 0; i < 2 && pad < 2; i++)
    {
        auto &gp = io::getCurrentGamePadState(i);
        // A keyboard takes a pad slot too, with its letters as buttons: not a joystick
        if (gp.GamePadShortName && strcmp(gp.GamePadShortName, "KB") == 0)
        {
            continue;
        }
        uint8_t v = (gp.buttons & io::GamePadState::Button::LEFT ? PAD_LEFT : 0) |
                    (gp.buttons & io::GamePadState::Button::RIGHT ? PAD_RIGHT : 0) |
                    (gp.buttons & io::GamePadState::Button::UP ? PAD_UP : 0) |
                    (gp.buttons & io::GamePadState::Button::DOWN ? PAD_DOWN : 0) |
                    (gp.buttons & io::GamePadState::Button::A ? PAD_A : 0) |
                    (gp.buttons & io::GamePadState::Button::B ? PAD_B : 0) |
                    (gp.buttons & io::GamePadState::Button::SELECT ? PAD_SELECT : 0) |
                    (gp.buttons & io::GamePadState::Button::START ? PAD_START : 0);
#if NES_PIN_CLK != -1
        v |= nespad_states[pad];
#endif
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (pad == 0) v |= wiipad_read();
#endif
        bool x = gp.buttons & io::GamePadState::Button::X;
        bool y = gp.buttons & io::GamePadState::Button::Y;
        uint8_t pushed = v & ~prevButtons[pad];
        prevButtons[pad] = v;

        if (pad == 0)
        {
            // Emulator shortcuts. Select is only a modifier: nothing reaches the C64 while
            // it is held.
            if ((v & (PAD_SELECT | PAD_START | PAD_UP | PAD_A)) == (PAD_SELECT | PAD_START | PAD_UP | PAD_A))
            {
                reset_usb_boot(0, 0);
            }
            if (v & PAD_SELECT)
            {
                if (pushed & PAD_START) showSettings = true;
                else if (pushed & PAD_UP) scaleMode8_7_ = Frens::screenMode(-1);
                else if (pushed & PAD_DOWN) scaleMode8_7_ = Frens::screenMode(+1);
                else if (pushed & PAD_A)
                {
                    joystick_port = 3 - joystick_port;
                    osd_show(joystick_port == 1 ? "JOYSTICK PORT 1" : "JOYSTICK PORT 2");
                }
                startAlone = false;
                pad++;
                continue;
            }
            if ((v & PAD_START) && (pushed & PAD_A))
            {
                settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
                if (!settings.flags.displayFrameRate) cbm_screen_invalidate();
            }
            // Start on its own, pressed and released, is RUN/STOP
            if (pushed & PAD_START) startAlone = true;
            if ((v & PAD_START) && (v & ~PAD_START)) startAlone = false;
            if (!(v & PAD_START) && startAlone)
            {
                startAlone = false;
                runStopFrames = 4;
            }
            if (v & PAD_START)
            {
                pad++;
                continue;
            }
        }
        joy[pad] = (v & PAD_UP ? CBM_JOY_UP : 0) | (v & PAD_DOWN ? CBM_JOY_DOWN : 0) |
                   (v & PAD_LEFT ? CBM_JOY_LEFT : 0) | (v & PAD_RIGHT ? CBM_JOY_RIGHT : 0) |
                   (v & PAD_A ? CBM_JOY_FIRE : 0);
        if (v & PAD_B) set_bit(want, K_SPACE);
        if (x) set_bit(want, K_RETURN);
        if (y) set_bit(want, K_F1);
        pad++;
    }
    if (runStopFrames > 0)
    {
        runStopFrames--;
        set_bit(want, K_RUN_STOP);
    }

    // The keypad joystick goes with the first gamepad
    joy[0] |= kbJoy;
    cbm_joystick(joystick_port, joy[0]);
    cbm_joystick(3 - joystick_port, joy[1]);

    // Only the changes go to the core
    for (int k = 0; k < CBM_KEYS; k++)
    {
        bool down = get_bit(want, k);
        if (down != get_bit(keys_down, k))
        {
            cbm_key(k, down);
        }
    }
    memcpy(keys_down, want, sizeof(keys_down));
}

static void release_input(void)
{
    cbm_keys_up();
    memset(keys_down, 0, sizeof(keys_down));
    shift_lock = false;
    runStopFrames = 0;
}

// =====================================================================================
// Serial keyboard (Settings, Serial keyboard): text arriving on the UART console is typed
// into the machine - a BASIC listing pasted into a terminal, CP/M commands, a test script.
// Ctrl-] is 40/80 DISPLAY and Ctrl-^ RESTORE (see cbm_type_serial()).
//
// As in pico-994A: the UART interrupt fills a ring (16 KB in PSRAM; 4 KB of SRAM on a board
// without), XON/XOFF holds the sender off before it is full, and what a sender still had queued when a game starts or a paste is
// abandoned is thrown away. Each character goes to cbm_type_serial(), which holds its key
// until the machine has read it, so the paste runs as fast as the machine takes it. A key
// on the USB keyboard abandons the paste. Ctrl-\ is not typed: it prints the text screen
// shown (40 or 80 columns) on the console.
//
// UART only, and only where the board leaves its RX pin to it: stdio only ever transmitted,
// so a pin map that hands RX to something else (HW_CONFIG 1 puts a NES controller's clock
// there) builds without the option (SERIAL_KEYBOARD_AVAILABLE, at the top). HW_CONFIG 11,
// 12 and 13 have no UART console at all.
// =====================================================================================

#if SERIAL_KEYBOARD_AVAILABLE && defined(uart_default)
// A power of two (the indices are masked). With PSRAM big enough for a whole listing without
// XOFF: holding off a sender that is about to finish can strand its last bytes (pico-994A).
static unsigned serialRingSize;         // 0 until serial_keyboard_init()
static constexpr uint32_t SERIAL_DISCARD_QUIET_US = 500000;
static constexpr uint8_t ASCII_XON = 0x11;
static constexpr uint8_t ASCII_XOFF = 0x13;
static constexpr uint8_t SERIAL_SCREEN = 0x1C;  // Ctrl-\: print the screen

static uint8_t *serialRing;
// head advanced by the interrupt, tail by the frame loop: single producer, single consumer
static volatile unsigned serialHead = 0, serialTail = 0;
static uint8_t serialPrevByte = 0;                  // interrupt side only
static volatile bool serialDiscarding = false;      // swallowing a sender's leftover queue
static volatile uint32_t serialDiscardUntil = 0;
static volatile bool serialOverflowed = false;
static volatile uint32_t serialRxCount = 0;
static volatile uint32_t serialLastRxUs = 0;
static uint32_t serialTypedCount = 0;
static bool serialOverflowWarned = false;
static bool serialXoffSent = false;
static bool serialXoffUsed = false;

static inline unsigned serialRingUsed(void) { return (serialHead - serialTail) & (serialRingSize - 1); }

// Drained by interrupt: the frame loop sleeps until the next frame for much of the time,
// and the UART's 32-byte FIFO overruns in 2.8 ms at 115200 baud
static void __not_in_flash_func(serialUartIrq)(void)
{
    while (uart_is_readable(uart_default))
    {
        uint8_t c = (uint8_t)uart_get_hw(uart_default)->dr;    // reading DR clears RX/RT
        if (!settings.flags.serialKeyboard || c == ASCII_XON || c == ASCII_XOFF)
        {
            continue;
        }
        if (serialDiscarding)
        {
            serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
            continue;
        }
        // CRLF is one RETURN: the LF after a CR is dropped
        bool lfAfterCr = c == '\n' && serialPrevByte == '\r';
        serialPrevByte = c;
        if (lfAfterCr)
        {
            continue;
        }
        unsigned next = (serialHead + 1) & (serialRingSize - 1);
        if (next == serialTail)
        {
            serialOverflowed = true;
            continue;
        }
        serialRing[serialHead] = c;
        serialHead = next;
        serialRxCount++;
        serialLastRxUs = time_us_32();
    }
}

// After initAll(), which finds the PSRAM
static void serial_keyboard_init(void)
{
    serialRingSize = Frens::isPsramEnabled() ? 16384 : 4096;
    serialRing = Frens::isPsramEnabled() ? (uint8_t *)Frens::f_malloc(serialRingSize) : (uint8_t *)malloc(serialRingSize);
    // A disconnected RX left floating would frame garbage into the machine
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
    // stdio_uart claims the interrupt only for a chars-available callback, which nothing
    // registers; TX stays with stdio
    uint irqNum = UART_IRQ_NUM(uart_default);
    irq_set_exclusive_handler(irqNum, serialUartIrq);
    irq_set_enabled(irqNum, true);
    uart_set_irqs_enabled(uart_default, true, false);
    printf("Serial keyboard: UART RX on GPIO %d\n", PICO_DEFAULT_UART_RX_PIN);
}

// Drop what is queued; what the sender still has in flight is thrown away too
static void serial_keyboard_cancel(void)
{
    serialTail = serialHead;
    serialPrevByte = 0;
    serialOverflowed = false;
    serialOverflowWarned = false;
    serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
    serialDiscarding = true;
    serialRxCount = 0;
    serialTypedCount = 0;
    serialXoffUsed = false;
    if (serialXoffSent)
    {
        putchar_raw(ASCII_XON);
        serialXoffSent = false;
    }
}

// The text screen on the console: up to 80 x 50 (the C128, which needs PSRAM), else 40 x 25
static void serial_print_screen(void)
{
    char small[41 * 25 + 1];
    bool big = Frens::isPsramEnabled();
    int size = big ? 81 * 50 + 1 : (int)sizeof(small);
    char *text = big ? (char *)Frens::f_malloc(size) : small;
    cbm_screen_text(text, size);
    printf("--- screen ---\n%s--- end of screen ---\n", text);
    if (big)
    {
        Frens::f_free(text);
    }
}

// Once a frame, between frames
static void serial_keyboard_tick(void)
{
    if (!settings.flags.serialKeyboard || !serialRingSize)
    {
        if (serialRingSize && serialRingUsed())
        {
            serial_keyboard_cancel();
        }
        return;
    }
    if (serialDiscarding && (int32_t)(time_us_32() - serialDiscardUntil) > 0)
    {
        serialDiscarding = false;
    }
    if (serialOverflowed && !serialOverflowWarned)
    {
        serialOverflowWarned = true;
        printf("\nSerial keyboard: input overflow - enable XON/XOFF flow control\n");
    }
    // A paste is over once everything is typed and nothing has come for a second: the
    // totals tell "never received" from "mistyped". Anything echoed back by the sender
    // is then swallowed rather than typed.
    if (serialRxCount && !serialRingUsed() && !cbm_typing() && (int32_t)(time_us_32() - serialLastRxUs) > 1000000)
    {
        if (serialRxCount >= 100)
        {
            printf("\nSerial keyboard: %u characters received, %u typed%s\n", (unsigned)serialRxCount,
                   (unsigned)serialTypedCount, serialXoffUsed ? " (flow control was used)" : "");
            serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
            serialDiscarding = true;
        }
        serialRxCount = 0;
        serialTypedCount = 0;
        serialXoffUsed = false;
    }
    if (!serialXoffSent && serialRingUsed() > serialRingSize * 3 / 4)
    {
        putchar_raw(ASCII_XOFF);
        serialXoffSent = serialXoffUsed = true;
    }
    else if (serialXoffSent && serialRingUsed() < serialRingSize / 4)
    {
        putchar_raw(ASCII_XON);
        serialXoffSent = false;
    }
    // A hand on the USB keyboard wins
    const auto &kb = io::getCurrentKeyboardState();
    if (kb.connected && (kb.keycode[0] || kb.modifier))
    {
        if (serialRingUsed())
        {
            serial_keyboard_cancel();
        }
        return;
    }
    while (serialRingUsed())
    {
        uint8_t c = serialRing[serialTail];
        if (c == SERIAL_SCREEN)
        {
            serial_print_screen();
        }
        else if (!cbm_type_serial((char)c))
        {
            break;                      // the one before is still being typed
        }
        else
        {
            serialTypedCount++;
        }
        serialTail = (serialTail + 1) & (serialRingSize - 1);
    }
}
#else
static void serial_keyboard_init(void) {}
static void serial_keyboard_cancel(void) {}
static void serial_keyboard_tick(void) {}
#endif

// Tracks of a disk kept in memory: with PSRAM the whole disk (as GCR, about 300 KB);
// without, two, read from the card as the drive's head gets to them. A developer build
// can force the cache (-DC64_DISK_CACHE=<tracks>) to try it on a board with PSRAM.
static int disk_cache_tracks(void)
{
#ifdef C64_DISK_CACHE
    return C64_DISK_CACHE;
#else
    return Frens::isPsramEnabled() ? 0 : 2;
#endif
}

// =====================================================================================
// The C64's own settings: rows in the settings menu (menuSetEmulatorOptions in
// menu.h), kept in a file of their own as "name=value" lines, so the shared settings
// file stays as it is.
// =====================================================================================
#define C64_SETTINGS_FILE "/settings_C64x.dat"
#ifndef C64_FAST_LOAD
#define C64_FAST_LOAD 1                 // the default; a developer build can start with it off
#endif

static struct
{
    int joystickPort;                   // the first gamepad's port: 1 or 2
    int fastLoad;                       // LOAD from drive 8 out of the image at once: 0 or 1
    int keyboard;                       // 0: US (symbolic), 1: C64 (by position)
    int machine;                        // what .d64 and .g64 disks start on: 0 C64, 1 C128
} c64opt = { 2, C64_FAST_LOAD, 0, 0 };

static void c64_settings_save(void)
{
    char text[80];
    int n = snprintf(text, sizeof(text), "joystick_port=%d\nfast_load=%d\nkeyboard=%d\nmachine=%d\n",
                     c64opt.joystickPort, c64opt.fastLoad, c64opt.keyboard, c64opt.machine);
    FIL fil;
    UINT bw;
    if (f_open(&fil, C64_SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
    {
        f_write(&fil, text, n, &bw);
        f_close(&fil);
    }
}

static void c64_settings_load(void)
{
    FIL fil;
    char text[256];
    UINT n = 0;
    if (f_open(&fil, C64_SETTINGS_FILE, FA_READ) != FR_OK)
    {
        return;
    }
    f_read(&fil, text, sizeof(text) - 1, &n);
    f_close(&fil);
    text[n] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(nullptr, "\r\n"))
    {
        int v;
        if (sscanf(line, "joystick_port=%d", &v) == 1 && (v == 1 || v == 2))
        {
            c64opt.joystickPort = v;
        }
        else if (sscanf(line, "fast_load=%d", &v) == 1)
        {
            c64opt.fastLoad = v != 0;
        }
        else if (sscanf(line, "keyboard=%d", &v) == 1)
        {
            c64opt.keyboard = v != 0;
        }
        else if (sscanf(line, "machine=%d", &v) == 1)
        {
            c64opt.machine = v != 0;
        }
    }
}

static const char *const portNames[] = { "Port 2", "Port 1" };

static int opt_port_get(void)
{
    return c64opt.joystickPort == 1 ? 1 : 0;
}

static void opt_port_set(int choice)
{
    int port = choice ? 1 : 2;
    if (port != c64opt.joystickPort)
    {
        c64opt.joystickPort = joystick_port = port;
        c64_settings_save();
    }
}

static int opt_fastload_get(void)
{
    return c64opt.fastLoad;
}

static void opt_fastload_set(int choice)
{
    if (choice != c64opt.fastLoad)
    {
        c64opt.fastLoad = choice;
        cbm_fast_load(choice);
        c64_settings_save();
    }
}

static const char *const keyboardNames[] = { "US", "C64" };

static int opt_keyboard_get(void)
{
    return c64opt.keyboard;
}

static void opt_keyboard_set(int choice)
{
    if (choice != c64opt.keyboard)
    {
        c64opt.keyboard = choice;
        build_keymap(choice == 0);
        c64_settings_save();
    }
}

static const char *const machineNames[] = { "C64", "C128" };

static int opt_machine_get(void)
{
    return c64opt.machine;
}

static void opt_machine_set(int choice)
{
    if (choice != c64opt.machine)
    {
        c64opt.machine = choice;
        c64_settings_save();
    }
}

// The C128 needs HSTX (its 80 columns) and PSRAM (its second 64 KB and big ROMs)
static bool c128_possible(void)
{
    return HSTX && Frens::isPsramEnabled();
}

// The last row only where the C128 can run
static const MenuEmulatorOption c64Options[] = {
    {"Joystick port", "Port of gamepad 1 (Select+A swaps it)", 2, portNames, 0, opt_port_get, opt_port_set},
    {"Fast disk load", "LOAD from drive 8 at once, not at 1541 speed", 2, nullptr, 1, opt_fastload_get, opt_fastload_set},
    {"Keyboard layout", "US: keys as printed. C64: by position", 2, keyboardNames, 0, opt_keyboard_get, opt_keyboard_set},
    {"Machine", "What .d64 and .g64 disks start on", 2, machineNames, 0, opt_machine_get, opt_machine_set},
};

// =====================================================================================
// Disks and tapes in a running game: the settings menu's Drive 8 and Cassette rows
// (MenuDiskHooks and MenuCassetteHooks, menu.h). The images offered are the ones in the
// folder the game was started from, which is where a multi-disk game keeps its other
// disks; with more than fit in the menu's list, those whose names are closest to the
// game's, and always the one in use and one just made. New ones are made there too.
// =====================================================================================
#define MEDIA_MAX_NAME   64             // longer names are left out rather than cut
#define MEDIA_MAX_LISTED 68             // what the menu's list takes, less its own rows

typedef struct
{
    char name[MEDIA_MAX_NAME];
    int score;                          // characters in common with the game's name
} MediaEntry;

typedef struct
{
    const char *ext[3];                 // the extensions listed
    const char *(*in_use)(void);        // the path of the one in the drive or deck, or NULL
    MediaEntry *entries;                // allocated on first use, then kept
    int count;
    char made[MEDIA_MAX_NAME];          // one just made: listed however full the folder is
} MediaList;

static char mediaFolder[FF_MAX_LFN];
static const char *mediaReference = ""; // the name the lists are ordered by

static const char *cbm_disk_path_0(void) { return cbm_disk_path(0); }

static MediaList disks = { { ".d64", ".g64", nullptr }, cbm_disk_path_0 };   // the C128 adds .d71
static MediaList tapes = { { ".tap", nullptr, nullptr }, cbm_tape_path };

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool media_ext(const MediaList *l, const char *name)
{
    const char *dot = strrchr(name, '.');
    for (const char *e : l->ext)
    {
        if (dot && e && strcasecmp(dot, e) == 0) return true;
    }
    return false;
}

static int common_prefix(const char *a, const char *b)
{
    int n = 0;
    while (a[n] && b[n] && tolower((unsigned char)a[n]) == tolower((unsigned char)b[n]))
    {
        n++;
    }
    return n;
}

// The folder (and the name the lists are ordered by) of what was started
static void media_set_folder(const char *path)
{
    snprintf(mediaFolder, sizeof(mediaFolder), "%s", path);
    char *slash = strrchr(mediaFolder, '/');
    if (slash)
    {
        *slash = 0;
    }
    mediaReference = base_name(path);
}

static void media_refresh(MediaList *l)
{
    if (!l->entries)
    {
        l->entries = (MediaEntry *)cbm_port_malloc(sizeof(MediaEntry) * MEDIA_MAX_LISTED);   // (PSRAM if any)
        if (!l->entries)
        {
            l->count = 0;
            return;
        }
    }
    l->count = 0;
    DIR dir;
    if (f_opendir(&dir, mediaFolder[0] ? mediaFolder : "/") != FR_OK)
    {
        return;
    }
    const char *inUse = l->in_use() ? base_name(l->in_use()) : "";
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
    {
        if ((fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) || !media_ext(l, fno.fname) ||
            strlen(fno.fname) >= MEDIA_MAX_NAME)
        {
            continue;
        }
        int score = strcasecmp(fno.fname, l->made) == 0 || strcasecmp(fno.fname, inUse) == 0
                        ? MEDIA_MAX_NAME : common_prefix(fno.fname, mediaReference);
        int slot = l->count;
        if (l->count == MEDIA_MAX_LISTED)
        {
            // Full: it takes the place of the least alike, if it is more alike
            slot = 0;
            for (int i = 1; i < l->count; i++)
            {
                if (l->entries[i].score < l->entries[slot].score) slot = i;
            }
            if (l->entries[slot].score >= score)
            {
                continue;
            }
        }
        else
        {
            l->count++;
        }
        snprintf(l->entries[slot].name, MEDIA_MAX_NAME, "%s", fno.fname);
        l->entries[slot].score = score;
    }
    f_closedir(&dir);
    std::sort(l->entries, l->entries + l->count,
              [](const MediaEntry &a, const MediaEntry &b) { return strcasecmp(a.name, b.name) < 0; });
}

static const char *media_name(const MediaList *l, int i)
{
    return i >= 0 && i < l->count ? l->entries[i].name : "";
}

static void media_path(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/%s", mediaFolder, name);
}

// <name><ext> in the folder exists
static bool media_exists(const char *name, const char *ext)
{
    char path[FF_MAX_LFN];
    snprintf(path, sizeof(path), "%s/%s%s", mediaFolder, name, ext);
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

// "<prefix>1", "<prefix>2", ...: the first not in the folder yet
static void media_default_name(char *buf, size_t n, const char *prefix, const char *ext)
{
    for (int i = 1; i < 1000; i++)
    {
        snprintf(buf, n, "%s%d", prefix, i);
        if (!media_exists(buf, ext))
        {
            return;
        }
    }
}

// The index of a file just made, after a rescan
static int media_made(MediaList *l, const char *file)
{
    snprintf(l->made, sizeof(l->made), "%s", file);
    media_refresh(l);
    for (int i = 0; i < l->count; i++)
    {
        if (strcasecmp(l->entries[i].name, file) == 0) return i;
    }
    return -1;
}

// ---- Drive 8 ----

static int disk_hook_num_drives(void) { return 1; }
static int disk_hook_num_images(void) { return disks.count; }
static const char *disk_hook_image_name(int i) { return media_name(&disks, i); }
static const char *disk_hook_drive_label(int drive) { return "Drive 8"; }
static void disk_hook_refresh(void) { media_refresh(&disks); }

// Blank disks: a D64, double-sided (D71) on the C128's 1571
static const char *blank_ext(void) { return model == CBM_MODEL_C128 ? ".d71" : ".d64"; }
static void disk_hook_default_name(char *buf, size_t n) { media_default_name(buf, n, "BLANK", blank_ext()); }
static int disk_hook_name_exists(const char *name) { return media_exists(name, blank_ext()); }

static const char *disk_hook_mounted_name(int drive)
{
    const char *path = cbm_disk_path(0);
    return path ? base_name(path) : nullptr;
}

static int disk_hook_mount(int drive, int index)
{
    if (index < 0)
    {
        cbm_disk_eject(0);
        return 0;
    }
    if (index >= disks.count)
    {
        return -1;
    }
    char path[FF_MAX_LFN];
    media_path(path, sizeof(path), disks.entries[index].name);
    cbm_disk_cache(disk_cache_tracks());
    return cbm_disk_insert(0, path) ? 0 : -1;
}

static int disk_hook_create(const char *name)
{
    char file[MEDIA_MAX_NAME + 8], path[FF_MAX_LFN];
    snprintf(file, sizeof(file), "%s%s", name, blank_ext());
    media_path(path, sizeof(path), file);
    if (!cbm_disk_create(path, model == CBM_MODEL_C128 ? CBM_D71_BYTES : CBM_D64_BYTES))
    {
        printf("Cannot create %s\n", path);
        return -1;
    }
    return media_made(&disks, file);
}

static const MenuDiskHooks diskHooks = {
    disk_hook_num_drives,
    disk_hook_num_images,
    disk_hook_image_name,
    disk_hook_mounted_name,
    disk_hook_mount,
    disk_hook_refresh,
    disk_hook_default_name,
    disk_hook_name_exists,
    disk_hook_create,
    disk_hook_drive_label,
    mediaFolder,
    16,
};

// ---- Cassette: the menu's mode numbers (menu.cpp) ----

enum { TAPE_EMPTY = 0, TAPE_PLAY = 1, TAPE_RECORD = 3 };

static int tape_hook_num_tapes(void) { return tapes.count; }
static const char *tape_hook_tape_name(int i) { return media_name(&tapes, i); }
static void tape_hook_refresh(void) { media_refresh(&tapes); }
static void tape_hook_default_name(char *buf, size_t n) { media_default_name(buf, n, "TAPE", ".tap"); }
static int tape_hook_name_exists(const char *name, int mode) { return media_exists(name, ".tap"); }
static void tape_hook_rewind(void) { cbm_tape_rewind(); }

static int tape_hook_selected(void)
{
    const char *path = cbm_tape_path();
    for (int i = 0; path && i < tapes.count; i++)
    {
        if (strcasecmp(tapes.entries[i].name, base_name(path)) == 0) return i;
    }
    return -1;
}

static int tape_hook_mode(void)
{
    cbm_tape_status_t s;
    cbm_tape_status(&s);
    return s.recording ? TAPE_RECORD : cbm_tape_path() ? TAPE_PLAY : TAPE_EMPTY;
}

// Eject, play a tape from the list (PLAY down), or record a new one (RECORD and PLAY)
static int tape_hook_commit(int index, int mode, const char *name)
{
    char path[FF_MAX_LFN];
    if (mode == TAPE_EMPTY)
    {
        cbm_tape_eject();
        return 0;
    }
    if (mode == TAPE_PLAY)
    {
        if (index < 0 || index >= tapes.count)
        {
            return -1;
        }
        media_path(path, sizeof(path), tapes.entries[index].name);
        if (!cbm_tape_insert(path))
        {
            return -1;
        }
        cbm_tape_play(true);
        return 0;
    }
    char file[MEDIA_MAX_NAME + 8];
    snprintf(file, sizeof(file), "%s.tap", name ? name : "TAPE");
    media_path(path, sizeof(path), file);
    if (!cbm_tape_record(path))
    {
        printf("Cannot record %s\n", path);
        return -1;
    }
    snprintf(tapes.made, sizeof(tapes.made), "%s", file);
    return 0;
}

static const MenuCassetteHooks cassetteHooks = {
    tape_hook_num_tapes,
    tape_hook_tape_name,
    tape_hook_selected,
    tape_hook_mode,
    tape_hook_default_name,
    tape_hook_name_exists,
    tape_hook_commit,
    tape_hook_rewind,
    tape_hook_refresh,
    "Record (TAP)",
    mediaFolder,
};

// =====================================================================================
// Starting what was picked in the menu
//
// Cartridges go in before the first frame (inserting one resets the machine). Programs
// and disks wait until BASIC is up, then LOAD and RUN are typed, as a user would.
// =====================================================================================
enum class Media { None, Program, Disk, Cartridge, Tape };

static Media media = Media::None;
static int autostartFrame = 0;          // 0: done
static const char *autostartText = nullptr;     // typed once BASIC is back at READY
static int readyFrames = 0;

// =====================================================================================
// BASIC marker file
//
// The menu is built around picking a file, but plain BASIC needs none. A small marker
// file gives it an entry like any other title; selecting it starts the C64 with nothing
// loaded, at READY. (As pico-994A does for TI BASIC.) Where the C128 can run, a second
// one starts it.
// =====================================================================================
static void ensure_marker(const char *name, const char *note)
{
    static const char *dir = "/roms/C64";
    FILINFO fno;
    if (f_stat(dir, &fno) != FR_OK)
    {
        return;                         // no C64 folder: the menu starts elsewhere
    }
    char path[FF_MAX_LFN];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (f_stat(path, &fno) == FR_OK)
    {
        return;
    }
    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_NEW) == FR_OK)
    {
        // The content is never read: the extension is what counts
        UINT bw;
        f_write(&fil, note, strlen(note), &bw);
        f_close(&fil);
        printf("Created %s\n", path);
    }
}

static void ensure_basic_markers(void)
{
    ensure_marker("C64 BASIC.c64", "pico-c64: starts the C64 with nothing loaded (BASIC)\n");
    if (c128_possible())
    {
        ensure_marker("C128 BASIC.c128", "pico-c64: starts the C128 with nothing loaded (BASIC 7.0)\n");
    }
}

// The machine for what was picked: the C128 for its marker and .d71 disks, and for .d64
// and .g64 disks when the Machine setting says so; the C64 for everything else
static cbm_model_t pick_model(const char *path)
{
#if defined(C64_AUTOSTART) && defined(C64_AUTOSTART_C128)
    if (strcmp(path, C64_AUTOSTART) == 0 && c128_possible())
    {
        return CBM_MODEL_C128;          // developer option: the autostart file on a C128
    }
#endif
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".c128") || !strcasecmp(ext, ".d71")))
    {
        return CBM_MODEL_C128;
    }
    if (ext && (!strcasecmp(ext, ".d64") || !strcasecmp(ext, ".g64")) && c64opt.machine && c128_possible())
    {
        return CBM_MODEL_C128;
    }
    return CBM_MODEL_C64;
}

static Media media_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return Media::None;
    if (!strcasecmp(ext, ".prg") || !strcasecmp(ext, ".t64")) return Media::Program;
    if (!strcasecmp(ext, ".d64") || !strcasecmp(ext, ".g64") || !strcasecmp(ext, ".d71")) return Media::Disk;
    if (!strcasecmp(ext, ".crt")) return Media::Cartridge;
    if (!strcasecmp(ext, ".tap")) return Media::Tape;
    return Media::None;
}

// The menu puts the file it starts at ROM_FILE_ADDR: in flash, or in PSRAM when the board
// has it. A start without the menu (C64_AUTOSTART) leaves whatever was there before, so
// the image is compared with the file's first and last bytes before it is used.
static bool rom_image_is(const char *path, size_t *size)
{
    FIL fil;
    if (ROM_FILE_ADDR == 0 || f_open(&fil, path, FA_READ) != FR_OK)
    {
        return false;
    }
    const uint8_t *image = (const uint8_t *)ROM_FILE_ADDR;
    *size = f_size(&fil);
    uint8_t buf[64];
    UINT n = 0;
    bool same = *size >= sizeof(buf) && f_read(&fil, buf, sizeof(buf), &n) == FR_OK && n == sizeof(buf) &&
                memcmp(buf, image, sizeof(buf)) == 0 && f_lseek(&fil, *size - sizeof(buf)) == FR_OK &&
                f_read(&fil, buf, sizeof(buf), &n) == FR_OK && n == sizeof(buf) &&
                memcmp(buf, image + *size - sizeof(buf), sizeof(buf)) == 0;
    f_close(&fil);
    return same;
}

// The C128 starts a disk with "CBM" at the start of track 1, sector 0 by itself (CP/M,
// many C128 programs). That sector is a .d64's or .d71's first 256 bytes.
static bool disk_boots(const char *path)
{
    const char *ext = strrchr(path, '.');
    FIL fil;
    if (!ext || !strcasecmp(ext, ".g64") || f_open(&fil, path, FA_READ) != FR_OK)
    {
        return false;
    }
    char sig[3];
    UINT n = 0;
    bool boots = f_read(&fil, sig, sizeof(sig), &n) == FR_OK && n == sizeof(sig) && memcmp(sig, "CBM", 3) == 0;
    f_close(&fil);
    return boots;
}

// LOAD and RUN for programs and disks, once BASIC is up (again after a reset). On the
// C128, RUN"*" for a disk that does not start itself (BASIC 7 moves a C64 BASIC program
// to its own start).
static void autostart_begin(const char *path)
{
    autostartText = nullptr;
    autostartFrame = 0;
    readyFrames = 0;
    if (model == CBM_MODEL_C128)
    {
        if (media == Media::Disk && !disk_boots(path))
        {
            autostartText = "run\"*\"\n";
        }
        return;
    }
    autostartFrame = (media == Media::Program || media == Media::Disk || media == Media::Tape) ? 150 : 0;
}

static bool insert_media(const char *path, char *err, size_t errSize)
{
    media = media_type(path);
    autostart_begin(path);
    switch (media)
    {
    case Media::Cartridge:
    {
        // In place where the menu put it; otherwise read into memory (PSRAM)
        size_t size;
        bool ok = rom_image_is(path, &size) ? cbm_cart_insert_image((const uint8_t *)ROM_FILE_ADDR, size, path)
                                            : cbm_cart_insert(path);
        if (!ok)
        {
            snprintf(err, errSize, "Cannot use cartridge");
            return false;
        }
        printf("Cartridge: %s\n", cbm_cart_info());
        break;
    }
    case Media::Disk:
        if (!mem.drive)
        {
            snprintf(err, errSize, model == CBM_MODEL_C128 ? "No 1571 ROM in /bios" : "No 1541 ROM in /bios");
            return false;
        }
        cbm_disk_cache(disk_cache_tracks());
        if (!cbm_disk_insert(0, path))
        {
            snprintf(err, errSize, "Cannot read disk");
            return false;
        }
        break;
    case Media::Tape:
        // In the datasette with PLAY down: LOAD finds it at once
        if (!cbm_tape_insert(path))
        {
            snprintf(err, errSize, "Cannot read tape");
            return false;
        }
        cbm_tape_play(true);
        break;
    default:
        break;
    }
    return true;
}

// What the C64 wrote to a disk goes to the card once the drive has stopped (twice a
// second), so it is there even if the board is switched off
static void disk_flush_tick(uint32_t frame)
{
    if (frame % 25 || !cbm_disk_dirty())
    {
        return;
    }
    cbm_status_t s;
    cbm_get_status(&s);
    if (!s.drive_motor[0])
    {
        cbm_disk_flush();
    }
}

// At READY: the KERNAL waits for a key (C64 $E5CD-$E5D6, C128 $C25E-$C263), drive and
// tape stopped
static bool at_ready(void)
{
    cbm_status_t s;
    cbm_get_status(&s);
    cbm_tape_status_t t;
    cbm_tape_status(&t);
    bool waiting = s.c128_mode ? s.pc >= 0xC25E && s.pc <= 0xC263 : s.pc >= 0xE5CD && s.pc <= 0xE5D6;
    return waiting && !s.z80 && !s.drive_motor[0] && !t.motor;
}

static void autostart_tick(const char *path, uint32_t frame)
{
    if (autostartFrame && frame >= (uint32_t)autostartFrame)
    {
        autostartFrame = 0;
        if (media == Media::Program)
        {
            if (!cbm_program_load(path, true))
            {
                printf("Cannot load %s\n", path);
            }
        }
        else if (media == Media::Disk || media == Media::Tape)
        {
            cbm_type(media == Media::Disk ? "load\"*\",8,1\n" : "load\n");
            autostartText = "run\n";
            readyFrames = -100;         // typing it takes a while
        }
    }
    if (autostartText)
    {
        readyFrames = at_ready() ? readyFrames + 1 : std::min(readyFrames, 0);
        if (readyFrames >= 25)
        {
            cbm_type(autostartText);
            autostartText = nullptr;
        }
    }
}

// =====================================================================================
// Per-frame housekeeping
// =====================================================================================

// 50 Hz on a 60 Hz display: one frame in six is shown twice (as in pico-infonesPlus)
static void paceFrame(bool init)
{
    static absolute_time_t next_frame_time;
    absolute_time_t now = get_absolute_time();
    if (init || absolute_time_diff_us(now, next_frame_time) <= -FRAME_US)
    {
        next_frame_time = now;
    }
    sleep_until(next_frame_time);
    next_frame_time = delayed_by_us(next_frame_time, FRAME_US);
}

static void processPerFrame(void)
{
    Frens::pollHeadPhoneJack();
    EXT_AUDIO_POLL_HEADPHONE();
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    Frens::blinkLed((count / 60) & 1);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();
    update_input();
}

// Time spent per frame, printed every 5 seconds: how much of the 20 ms the C64 takes
static struct
{
    uint32_t frames, emu, emuMax, audio, audioMax;
    int levelMin, levelMax;             // samples waiting in the audio sink at frame start
} stats = {0, 0, 0, 0, 0, INT32_MAX, 0};

static void report_stats(void)
{
    if (++stats.frames < 250)
    {
        return;
    }
    cbm_status_t s;
    cbm_get_status(&s);
    printf("per frame: emulation %lu us (max %lu), audio %lu us (max %lu), queued audio %d-%d, drive %s, PC $%04X\n",
           (unsigned long)(stats.emu / stats.frames), (unsigned long)stats.emuMax,
           (unsigned long)(stats.audio / stats.frames), (unsigned long)stats.audioMax,
           stats.levelMin, stats.levelMax, s.drive_motor[0] ? "busy" : "idle", s.pc);
    memset(&stats, 0, sizeof(stats));
    stats.levelMin = INT32_MAX;
}

// =====================================================================================
// Menus over a running game: the settings menu, and the tape list when the C64 asks
// for a tape it has not got
// =====================================================================================
static void overlay_begin(void)
{
    vdc_hide();                         // the menu draws into the framebuffer
    audio_park();
    if (cbm_disk_dirty())
    {
        cbm_disk_flush();               // (in case the board is switched off from there)
    }
}

static void overlay_end(void)
{
    cbm_screen_invalidate();
    release_input();
    audio_start();
    paceFrame(true);
}

// The KERNAL's "PRESS PLAY ON TAPE" (or "PRESS RECORD & PLAY ON TAPE") waits at
// $F821-$F837, checking the STOP key at $F8D0-$F8E1. With no tape in the datasette for
// a few frames, that is the moment to choose one: 0 to play, 1 to record, -1 nothing.
// Once per wait - choosing none leaves the C64 waiting, as it would. (The C64's KERNAL
// only: a C128 in C128 mode has its own.)
static int tape_prompt_wanted(void)
{
    static int frames = 0;
    static bool asked = false;
    cbm_status_t s;
    cbm_get_status(&s);
    bool waiting = !s.c128_mode && !s.z80 &&
                   ((s.pc >= 0xF821 && s.pc <= 0xF837) || (s.pc >= 0xF8D0 && s.pc <= 0xF8E1));
    if (!waiting || cbm_tape_path())
    {
        frames = 0;
        asked = false;
        return -1;
    }
    if (asked || ++frames < 5)
    {
        return -1;
    }
    asked = true;
    // Which message: "RECORD" on the cursor's line (screen codes R E C O R D)
    static const uint8_t record[] = { 18, 5, 3, 15, 18, 4 };
    uint16_t line = cbm_peek(0xD1) | (cbm_peek(0xD2) << 8);
    for (int x = 0; x + 6 <= 40; x++)
    {
        int k = 0;
        while (k < 6 && (cbm_peek(line + x + k) & 0x7F) == record[k]) k++;
        if (k == 6) return 1;
    }
    return 0;
}

// =====================================================================================
// Developer option: start a file without going through the menu (timing runs on a
// board without anyone at it). EXTRA_CMAKE_ARGS=-DC64_AUTOSTART=/roms/C64/x.d64 ./bld.sh
// When the file is not there, the C64 folders on the card are listed on the console.
// =====================================================================================
#ifdef C64_AUTOSTART
static void list_dir(const char *path, int depth)
{
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK)
    {
        printf("  (no %s)\n", path);
        return;
    }
    int n = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] && n++ < 60)
    {
        printf("  %s/%s%s %lu\n", path, fno.fname, (fno.fattrib & AM_DIR) ? "/" : "", (unsigned long)fno.fsize);
        if ((fno.fattrib & AM_DIR) && depth > 0)
        {
            char sub[FF_MAX_LFN];
            snprintf(sub, sizeof(sub), "%s/%s", path, fno.fname);
            list_dir(sub, depth - 1);
        }
    }
    f_closedir(&dir);
}

static bool autostart_path(char *selectedRom)
{
    FILINFO fno;
    if (f_stat(C64_AUTOSTART, &fno) != FR_OK)
    {
        printf("Autostart: no %s\n", C64_AUTOSTART);
        list_dir("/roms/C64", 1);
        list_dir("/bios", 0);
        return false;
    }
    strcpy(selectedRom, C64_AUTOSTART);
    // As the menu does on a board with PSRAM: the file at ROM_FILE_ADDR. (The call
    // empties the path it is given, so it gets a copy.)
    if (Frens::isPsramEnabled())
    {
        char path[FF_MAX_LFN];
        uint32_t crc;
        strcpy(path, C64_AUTOSTART);
        ROM_FILE_ADDR = (uintptr_t)Frens::flashromtoPsram(path, false, crc, 0);
    }
    return true;
}
#endif

// =====================================================================================
// main
// =====================================================================================
int main()
{
    char selectedRom[FF_MAX_LFN];
    ErrorMessage[0] = selectedRom[0] = 0;

    // Where the Overclock setting exists, the menu stores the chosen clock in FlashParams
    // and reboots; it is picked up here (as in pico-genesisPlus).
    vreg_voltage voltage = VOLTAGE;
#if OVERCLOCK_SETTING
    Frens::setOverclockLimits(EMULATOR_CLOCKFREQ_KHZ, EMULATOR_MAX_CLOCKFREQ_KHZ, VOLTAGE, MAX_VOLTAGE);
    const Frens::FlashParams *flashParams = (const Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if (Frens::validateFlashParams(*flashParams))
    {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
    }
#else
    Frens::setOverclockLimits(CPUFreqKHz, CPUFreqKHz, VOLTAGE, VOLTAGE);
#endif
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);

    printf("==========================================================================================\n");
    printf("pico-c64 (Commodore 64/128) %s\n", SWVERSION);
    printf("Build date: %s  time: %s\n", __DATE__, __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d\n", clock_get_hz(clk_hstx) / 1000);
#endif
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");

    FrensSettings::initSettings(FrensSettings::C64);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);
    // All of SRAM's heap claimed once (initAll does it only without PSRAM): newlib's
    // malloc would otherwise sbrk a whole big block even when most of it is free at the
    // top of the heap, and fail - or panic - with the memory sram_free() counts
    Frens::claimHeap();
    Frens::dumpHeapStats("claimHeap");
    serial_keyboard_init();

    init_settings_visibility();
#if HSTX
    video_output_set_background_task(sid_task);
#endif
    vdc_init_output();
    g_available_screen_modes = g_available_screen_modes_c64;
    c64_settings_load();
    // Read from the card by the emulator itself: started without a copy in flash (or
    // PSRAM) and the reboot that takes. Cartridges are read from that copy. .c64 and
    // .c128: the BASIC markers; .d71 and .c128 only where the C128 can run.
    const bool c128 = c128_possible();
    menuSetStreamedExtensions(c128 ? ".prg .t64 .d64 .g64 .d71 .tap .c64 .c128" : ".prg .t64 .d64 .g64 .tap .c64");
    const char *menuExtensions = c128 ? ".prg .t64 .d64 .g64 .d71 .tap .crt .c64 .c128" : ".prg .t64 .d64 .g64 .tap .crt .c64";
    cbm_fast_load(c64opt.fastLoad);
    build_keymap(c64opt.keyboard == 0);
    menuSetEmulatorOptions(c64Options, sizeof(c64Options) / sizeof(c64Options[0]) - (c128 ? 0 : 1));
    if (!g_available_screen_modes[static_cast<int>(settings.screenMode)])
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);

    build_palette();
    ensure_basic_markers();
    bool showSplash = true;
#ifdef C64_AUTOSTART
    if (!isFatalError)
    {
        autostart_path(selectedRom);
    }
#endif

    while (true)
    {
        if (strlen(selectedRom) == 0)
        {
            menu("pico-c64", ErrorMessage, isFatalError, showSplash, menuExtensions, selectedRom);
            printf("Selected: %s\n", selectedRom);
        }
        *ErrorMessage = 0;
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

        model = pick_model(selectedRom);
        if (model == CBM_MODEL_C128 && !c128)
        {
            snprintf(ErrorMessage, 40, "The C128 needs HDMI (HSTX) and PSRAM");
            selectedRom[0] = 0;
            continue;
        }
        const char *machineName = model == CBM_MODEL_C128 ? "C128" : "C64";
        if (!load_machine(ErrorMessage, 40))
        {
            printf("%s\n", ErrorMessage);
            free_machine();
            selectedRom[0] = 0;
            continue;
        }
        cbm_config_t cfg = {};
        cfg.model = model;
        cfg.ram = mem.ram;
        cfg.ram1 = mem.ram1;
        cfg.basic = mem.basic;
        cfg.kernal = mem.kernal;
        cfg.chargen = mem.chargen;
        cfg.c64_rom = mem.c64rom;
        cfg.vdc_ram = mem.vdcram;
        cfg.drive_rom = mem.drive;
        cfg.line = c64_line;
        disks.ext[2] = model == CBM_MODEL_C128 ? ".d71" : nullptr;
        cbm_audio_enable(true);
        audio_park();
        if (!cbm_init(&cfg) || !insert_media(selectedRom, ErrorMessage, 40))
        {
            if (!*ErrorMessage) snprintf(ErrorMessage, 40, "Cannot start the %s", machineName);
            printf("%s\n", ErrorMessage);
            free_machine();
            selectedRom[0] = 0;
            continue;
        }
        printf("%s running\n", machineName);
        Frens::dumpHeapStats("machine running");
        cbm_screen_invalidate();
        release_input();
        serial_keyboard_cancel();       // a new game: nothing left over from before
        joystick_port = c64opt.joystickPort;    // Select+A swaps it for this game only
        c128Mode = false;
        media_set_folder(selectedRom);
        menuSetDiskHooks(mem.drive ? &diskHooks : nullptr);
        menuSetCassetteHooks(&cassetteHooks);
        audio_start();

        paceFrame(true);
        uint32_t frame = 0;
        uint32_t fpsStart = Frens::time_us();
        key_done = false;
        while (!key_done)
        {
            paceFrame(false);
            processPerFrame();
            serial_keyboard_tick();
            autostart_tick(selectedRom, frame);
            disk_flush_tick(frame++);

            int level = audio_queued();
            stats.levelMin = std::min(stats.levelMin, level);
            stats.levelMax = std::max(stats.levelMax, level);
            uint32_t t0 = Frens::time_us();
            cbm_run_frame();
            uint32_t t1 = Frens::time_us();
            process_audio_frame();
            cbm_status_t st;
            cbm_get_status(&st);
            c128Mode = st.c128_mode;
            vdc_update(st.vdc);
            uint32_t t2 = Frens::time_us();
            stats.emu += t1 - t0;
            stats.emuMax = std::max(stats.emuMax, t1 - t0);
            stats.audio += t2 - t1;
            stats.audioMax = std::max(stats.audioMax, t2 - t1);
            report_stats();

            if ((frame % 50) == 0)
            {
                uint32_t now = Frens::time_us();
                fps = (50u * 1000000u + (now - fpsStart) / 2) / (now - fpsStart);
                fpsStart = now;
            }
            if (settings.flags.displayFrameRate)
            {
                draw_fps();
            }
            draw_osd();

            int tapeWanted = tape_prompt_wanted();
            if (tapeWanted >= 0)
            {
                overlay_begin();
                menuCassettePrompt(tapeWanted);
                overlay_end();
            }
            if (showSettings)
            {
                showSettings = false;
                overlay_begin();
                int rval = showSettingsMenu(true);
                if (rval == 3)          // Quit game
                {
                    key_done = true;
                }
                else if (rval == 5)     // Reset game
                {
                    // As switching the C64 off and on: disk and cartridge stay in
                    cbm_reset();
                    autostart_begin(selectedRom);
                    serial_keyboard_cancel();
                    frame = 0;
                }
                overlay_end();
            }
        }

        audio_park();
        free_machine();
        Frens::dumpHeapStats("after free_machine");
        selectedRom[0] = 0;
        showSplash = false;
    }
    return 0;
}
