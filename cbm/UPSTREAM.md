# Vendored Commodore core

The C64/C128 emulation in this directory is taken from two MIT-licensed
projects by Robert Lucencic. Their licence texts are `LICENSE.esp32cbm` and
`LICENSE.esp32emu`.

| Directory | Source | Commit |
|---|---|---|
| `core/` | github.com/rlucencic/esp32cbm `components/cbm` | 6d4f4d189d5e117b54fe06f1ea1d8ec626874409 |
| `m6502/` | github.com/rlucencic/esp32emu `components/m6502` | 3503969dadda952ceec24a87b34a29184fc19c29 |
| `z80/` | github.com/rlucencic/esp32emu `components/z80` | 3503969dadda952ceec24a87b34a29184fc19c29 |
| `port/emudbg.h` | github.com/rlucencic/esp32emu `components/emudbg/include` | 3503969dadda952ceec24a87b34a29184fc19c29 |

Not taken over: `space.c` (needs esp32emu's diskspace component), the
debugger runtime, `m6502_dbg.c`, `z80_dbg.c` and the ESP-IDF board code.
`port/port_stubs.c` stands in for what the core still refers to.

## Changes to the vendored files

Every change made to a file in `core/`, `m6502/` or `z80/` is listed here,
so the core can be updated from upstream again.

1. `m6502/m6502.c`, `m6502/include/m6502.h`: code size. `rd()`, `wr()` and
   `resync()` are inlined at every bus access; recomputing the run limits
   there made `m6502_run` 188 KB of Cortex-M33 code (GCC 13, `-O2`). The slow
   paths now work out the limits relative to the instruction's start into
   new `m6502_t` fields (`rel_lim`, `rel_wlim`, `rel_until`), which
   `resync()` copies, and `rd_slow()`/`wr_slow()` are marked `cold`.
   `m6502_run` is 90 KB. Behaviour is unchanged (Lorenz 2.15 disks 1-4 pass
   on the host harness).
2. `m6502/include/m6502.h`, `m6502/m6502.c`: with `CBM_PORT`, `M6502_HOT`
   puts code in `.time_critical.cbm` (SRAM on the RP2350), and the new
   `M6502_RUN_HOT` does the same for `m6502_run` (upstream keeps it out of
   the ESP32's IRAM).
3. `core/vic.c`, `core/include/cbm.h`, `core/cbm.c`, `core/cbm_internal.h`:
   the VIC-II can hand each changed line to the board as colour numbers
   (`cbm_config_t.line`) instead of drawing RGB565 into `cfg.frame`, which
   is then not needed (240 KB). `cbm_screen_invalidate()` (`vic_invalidate()`)
   makes the next frame draw every line again.
4. `core/cbm_internal.h`: with `CBM_PORT`, allocation, logging and file
   access come from `port/cbm_port.h`. The core's `free()` calls on its own
   allocations are `cbm_free()` (`gcr.c`, `prg.c`, `drive.c`, `cart.c`), so
   a port can place them in PSRAM.
5. `core/sid.c`, `core/include/cbm.h`: 44.1 kHz instead of 30,789 Hz. The
   step is fractional, exactly 882 samples per 19,656-cycle frame. The event
   queue holds 2,048 events with 32-bit times (16 KB instead of 128 KB), and
   the clock shared with the render side is 32 bits (64-bit atomics are not
   lock-free on Cortex-M33). Times are compared wrap-safe.
6. `core/sid.c`: `mix << 8` on a negative value (undefined behaviour,
   reported by UBSan) is `mix * 256`.
7. `core/vic.c`: the per-line functions (drawing, line start and end,
   stalls, register access) are `CBM_HOT`, in SRAM on the RP2350.
8. `core/vic.c`: sprite-sprite collisions are found in `draw_sprites()`,
   from each sprite's pixels as they are drawn (`s_spr[]` holds the sprites
   seen at each x), instead of `sprite_collisions()` testing all eight
   sprites at every pixel of the line. The same pixels count, over the same
   range, so the result is the same; it took half the time of a game with
   several sprites per line (Yie Ar Kung Fu: 22 ms per frame on PicoDVI at
   252 MHz before, 9 ms after). Identical screenshots before and after.
9. `core/cart.c`, `core/include/cbm.h`: a cartridge's banks are pointers
   into the .CRT image (`c.chip[bank][ROML/ROMH]`) instead of a 1 MB copy.
   `cbm_cart_insert_image()` takes an image already in memory (a board's
   flash or PSRAM) and reads it in place; `cbm_cart_insert()` reads the file
   into a buffer of its own size and does the same. A short chip at the very
   end of the image is copied into an 8 KB buffer padded with $FF, so no read
   goes past the image. All 2,544 cartridges in the test set give the same
   screenshot (frame 400) as before.
10. `core/gcr.c`, `core/gcr.h`, `core/drive.c`, `core/include/cbm.h`: a track
    cache for boards without PSRAM (`cbm_disk_cache()`, `gcr_cache_tracks`).
    With n > 0 the image stays in its file, open while the disk is in: the
    track lengths, a D64's error bytes and ID are read at insert, a track is
    read in (a D64's sectors read and encoded) by `gcr_track()` when the head
    has been on it for 20,000 drive cycles (the DOS steps a half track every
    16.5 ms, so a seek reads in no tracks it passes), into one of n buffers;
    a changed track is written back before its buffer is used again.
    `encode_track()` and `decode_track()` work on one track's sectors.
    Without a cache (the default) the whole disk is read in as before.
    Lorenz disk 1 with 1- and 2-track caches, D64 saves (byte-identical
    images) and loads give the same results as without.
11. `core/fastload.c` (new), `core/cbm.c`, `core/drive.c`,
    `core/cbm_internal.h`, `core/include/cbm.h`: fast load
    (`cbm_fast_load()`). The 6510 stops at the KERNAL's LOAD ($F4A5, through
    the vector at $0330) via `stop_pc`; a LOAD of a program file from drive 8
    is read out of the disk image and returned from as the KERNAL does.
    Anything else goes on into the ROM and the emulated drive. The 77 D64s in
    the test set load the same bytes as through the drive. `drive_disk()`
    gives the disk in a drive.
12. `core/gcr.c`: `gcr_read_sector()` reads a D64/D71 sector straight from
    the image (memory, or the file a track at a time with a cache) when its
    track has no writes not yet flushed and it has no error byte; anything
    else is decoded from the GCR as before. The error bytes are kept in
    whole-disk mode too.
13. `core/gcr.c`, `core/drive.c`, `core/include/cbm.h`: `gcr_create()`
    writes a blank image 4 KB at a time instead of building it in memory
    (byte-identical to before), and `cbm_disk_create()` names the disk after
    its file (upper case, 16 characters) instead of "BLANK".

14. `core/tape.c` (new), `core/cia.c`, `core/cbm.c`, `core/cbm_internal.h`,
    `core/include/cbm.h`: the datasette (`cbm_tape_*()`), which upstream does
    not have. A .TAP (versions 0 and 1) turns while PLAY is down and the 6510
    runs the motor (port bit 5); each pulse's falling edge reaches CIA 1's
    FLAG at its cycle (`cia_flag()`; the next edge is one of the events the
    CPU stops at, in `update_events()`). PLAY is the sense line (port bit 4,
    read as 0 when down). Recording: the time between rising edges of the
    write line (port bit 3) into a new TAP version 1. `cbm_port_refresh()`
    updates the port's inputs from outside the CPU. The KERNAL's loader,
    Freeload and a second turbo tape load, and a tape saved and loaded back
    gives the program back.

15. `core/c128.c`, `core/vdc.c`, `core/cbm.c`, `core/cbm_internal.h`,
    `core/include/cbm.h`, `z80/include/z80.h`: the C128 on a board with
    little SRAM. `cfg.ram1` puts RAM bank 1 apart from bank 0 (in PSRAM;
    `cbm_ram_bank()` wherever the MMU picks a bank). `cfg.vdc_ram` gives the
    VDC 16 KB of the board's (a C128 with 4416 chips: addresses wrap at
    16 KB) instead of 64 KB of its own. Without `cfg.vdc_frame` the board
    draws the 80 columns itself, line by line with `cbm_vdc_line()`: 640
    pixels, 8 a character cell, as RGBI colour numbers two to a byte (the
    same picture as `draw()`, without the stretch to 800), when
    `cbm_screen_signature()` changes. `cbm_status_t.c128_mode`. With
    `CBM_PORT` the Z80 (`Z80_HOT`) and the MMU's paths (`c128_mem_update()`,
    page $FF, the Z80's memory) and the VDC's registers go to SRAM. BASIC 7,
    a D71 SAVE and LOAD, GO64 and CP/M 3.0 (the system disk from zimmers.net)
    behave as before on the host with this layout.

16. `core/input.c`, `core/cbm.c`, `core/vic.c`, `core/vdc.c`, `core/cbm_internal.h`,
    `core/include/cbm.h`: a keyboard over a serial line, `cbm_type_serial()`. Each key is
    held until the machine has read its row on its own (CIA 1 port B read with that row
    selected alone, `input_rows_read()`), and let go until a read sees it up; modifiers
    go down first and come up last, as CP/M scans one row an interrupt. The next key
    waits while the KERNAL's keyboard buffer is full, while a drive's motor runs (CP/M
    drops what is typed while it reloads), and after a reset until BASIC first waits for
    a key. `cbm_screen_text()`: the text screen shown, for a console (VIC or VDC, ASCII
    under CP/M). `cbm_vdc_line()` decodes a character row once and expands 8 pixels with
    one lookup. Compile-time logs `CBM_VDC_LOG`, `CBM_FEED_LOG`.

17. `core/sid.c`, `core/include/cbm.h`: `cfg.audio_events`, the SID event queue's size
    (a power of two, 0 for upstream's 2048), for boards short of memory that play a
    frame's worth after each frame.

18. `core/sid.c`: the sound. A sawtooth or a pulse on its own is band-limited at its
    steps (PolyBLEP): a 1.5 kHz sawtooth's aliasing below 5 kHz is 52 dB below the tone
    instead of 23. The filter is the state variable filter's trapezoidal form, in tune and
    stable up to 12 kHz at 44.1 kHz (upstream's is clamped at about 9 kHz), with reSID's
    Q (0.707-1.707; upstream's reaches 2.7). The mix is 4.4 dB lower (`MIX_SCALE`) so the
    filter's resonance and the DC blocker's swing no longer clip (Alien Syndrome: up to 75
    clipped samples a second before, none after), followed by the output stage's 16 kHz
    low pass. Compile-time log `CBM_SID_LOG` (the register writes, as replayed).

## Known problems

- Frankie Goes to Hollywood (Ocean, G64 of the original): its copy
  protection makes the drive return from the DOS's GCR conversion ($F685)
  through an overwritten stack; the load never ends. Upstream's.

