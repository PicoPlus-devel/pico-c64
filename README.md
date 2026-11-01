# pico-c64

## Introduction

**pico-c64** is a Commodore 64 and Commodore 128 emulator for RP2350-based microcontrollers.
It is based on the
emulation core of [esp32cbm](https://github.com/rlucencic/esp32cbm) by Robert Lucencic, with
its 6502 and Z80 cores from [esp32emu](https://github.com/rlucencic/esp32emu), integrated
with the video, audio, menu and SD card framework from
[pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus).

> [!IMPORTANT]
> RP2350 only (Pico 2 and variants). The RP2040 is not fast enough: the 6510 and the 1541
> drive are emulated cycle by cycle.

This project is part of a family of Raspberry Pi Pico emulator projects:

- NES: [pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus)
- Sega Master System / Game Gear: [pico-smsplus](https://github.com/PicoPlus-devel/pico-smsplus)
- Game Boy / Game Boy Color: [pico-peanutGB](https://github.com/PicoPlus-devel/pico-peanutGB)
- Sega Mega Drive / Genesis: [pico-genesisPlus](https://github.com/PicoPlus-devel/pico-genesisPlus)
- PC Engine / TurboGrafx-16: [pico-pcePlus](https://github.com/PicoPlus-devel/pico-pcePlus)
- Odyssey 2 / VideoPac: [pico-pacPlus](https://github.com/PicoPlus-devel/pico-pacPlus)
- Super Nintendo: [pico_snesPlus](https://github.com/PicoPlus-devel/pico-snesPlus)
- TI-99/4A: [pico-994A](https://github.com/PicoPlus-devel/pico-994A)

There is also an all-in-one solution that runs several of these emulators from a single
menu: [pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader).

***

## What it emulates

- **A PAL Commodore 64**: the 6510 cycle by cycle, including its undocumented
  instructions; the VIC-II with bad lines and sprite DMA; two 6526 CIAs; the SID (6581).
- **A 1541-II disk drive** with its own 6502 running the drive's DOS ROM, so fast loaders
  work. `.d64` and `.g64` images, read and write. See [Disks](#disks).
- **Fast disk load**, which reads a `LOAD` from drive 8 straight out of the disk image.
- **A datasette** with `.tap` images, including turbo loaders, and recording to tape. See
  [Tape](#tape).
- **Cartridges** (`.crt`). See [Cartridges](#cartridges).
- **Programs** (`.prg`, `.t64`), loaded into memory and started.
- **A USB keyboard**, in a US or a C64 layout, and **two joystick ports** from USB gamepads,
  GPIO NES/SNES controllers, Wii controllers or the numeric keypad.
- **A PAL Commodore 128** on boards with HSTX video and PSRAM: BASIC 7.0, the 40- and
  80-column screens, 2 MHz mode, C64 mode, the Z80 and CP/M 3.0, and a 1571 drive with
  `.d71` images. See [Commodore 128](#commodore-128).

- **Typing over the serial port**: text sent to the board's UART console is typed into
  the machine, see [Serial keyboard](#serial-keyboard).

Not available yet: NTSC machines, the RAM Expansion Unit and save states. See [Not yet done](#not-yet-done).

***

## Setup Overview

1. Prepare an SD card formatted as FAT32 (recommended) or exFAT.
2. Put the Commodore ROM files on the card, see [ROM files](#rom-files). Nothing runs
   without them.
3. Copy programs, disk images, tapes and cartridges to the card, preferably in `/roms/C64`.
   Subdirectories are supported.
4. Insert the SD card and use the menu to browse, select and play.

## ROM files

The Commodore ROMs are not distributed with the emulator. They are read from the SD card,
from the first of these folders that has them: `/roms/C64/bios`, `/bios/c64`, `/bios`,
`/roms/bios`.

| ROM | File names accepted | Size |
|:--|:--|:--|
| BASIC V2 (901226-01) | `c64basic.rom`, `basic`, `basic.901226-01.bin` | 8 KB |
| KERNAL (901227-03) | `c64kernal.rom`, `kernal`, `kernal.901227-03.bin` | 8 KB |
| Character set (901225-01) | `c64chargen.rom`, `chargen`, `characters.901225-01.bin` | 4 KB |
| 1541-II DOS (251968-03) | `1541.rom`, `dos1541`, `1541-II.251968-03.bin` | 16 KB |

The first three are required. Without the 1541 ROM there is no disk drive; programs,
tapes and cartridges still work.

The Commodore 128 needs its own ROMs, in the same folders:

| ROM | File names accepted | Size |
|:--|:--|:--|
| BASIC 7.0, low and high (318022-02) | `c128basic.rom`, `basic.318022-02.bin` | 32 KB |
| KERNAL (318020-05) | `c128kernal.rom`, `kernal.318020-05.bin` | 16 KB |
| Character set (390059-01) | `c128chargen.rom`, `characters.390059-01.bin` | 8 KB |
| C64 mode (251913-01) | `c128c64.rom`, `64c.251913-01.bin` | 16 KB |
| 1571 DOS (310654-05) | `1571.rom`, `dos1571`, `1571-rom.310654-05.bin` | 32 KB |

The first four are required for the C128; without the 1571 ROM it has no disk drive.

## Starting a program

The menu lists these files:

| Extension | What happens |
|:--|:--|
| `.prg`, `.t64` | The program is loaded into memory and started with `RUN`. |
| `.d64`, `.g64` | The disk goes into drive 8; `LOAD"*",8,1` is typed, then `RUN` once the C64 is back at `READY`. With **Machine** set to C128, the disk starts on a C128 instead. |
| `.d71` | The disk goes into the C128's drive 8, see [Commodore 128](#commodore-128). |
| `.tap` | The tape goes into the datasette with PLAY down; `LOAD` is typed, then `RUN` once the C64 is back at `READY`. |
| `.crt` | The cartridge is inserted and the C64 starts with it. |
| `.c64` | The C64 starts with nothing loaded, at the BASIC prompt. See below. |
| `.c128` | The C128 starts with nothing loaded, at the BASIC 7.0 prompt. |

**BASIC.** The menu is built around picking a file, but plain BASIC needs none. The file
`C64 BASIC.c64` is created in `/roms/C64` at start-up when that folder exists; selecting it
starts the C64 at `READY`. On boards that can run the C128, `C128 BASIC.c128` is created
there too. Their content is not used.

## Disks

Drive 8 is a 1541-II. Disk images are read and written: what the C64 saves is written back
to the image file on the SD card when the drive stops, when the settings menu is opened and
when the game is quit.

**Fast disk load** (Settings, on by default) reads a `LOAD` from drive 8 straight out of
the disk image, instead of at the 1541's speed of about 400 bytes a second. It applies only
to the C64's own LOAD routine: a directory (`LOAD"$",8`), a file that is not on the disk,
and every program that loads with a loader of its own go to the emulated drive as before.
Switch it off for software that needs the drive's exact behaviour.

**Changing disks.** The **Drive 8** row of the settings menu, opened during a game, lists
the `.d64` and `.g64` files in the folder the game was started from, which is where a
multi-disk game keeps its other disks. When the folder holds more than the list can show,
the names closest to the game's are listed. `<no disk>` ejects the disk.

**Creating a blank disk.** In the same list, **Create blank disk** asks for a name (with a
USB keyboard; without one, the name shown is used), creates `<name>.d64` in that folder,
formats it with that name and puts it into drive 8.

With PSRAM the whole disk is held in memory. Without PSRAM, two tracks at a time are,
read from the SD card as the drive's head reaches them.

## Tape

The datasette plays `.tap` images (versions 0 and 1). Each pulse reaches the C64 at its
exact cycle, so the KERNAL's loader and turbo loaders work. A tape loads at the speed of a
real datasette.

The **Cassette** row of the settings menu, opened during a game, offers:

- `<no tape>`: ejects the tape.
- **Record (TAP)**: asks for a name and records what the C64 saves into a new
  `<name>.tap`, with RECORD and PLAY down.
- **Rewind**, while a tape is in.
- The `.tap` files in the folder the game was started from: the tape goes in with PLAY down.

When the C64 shows `PRESS PLAY ON TAPE` or `PRESS RECORD & PLAY ON TAPE` and there is no
tape in the datasette, the tape list or the name prompt for a recording opens by itself.

## Commodore 128

The C128 runs on boards whose video output uses HSTX (HW_CONFIG 2, 5, 8, 13, 14 and 15)
and that have PSRAM: its second 64 KB of RAM and the larger ROMs are held there. On other
boards `.d71` files and the C128 entries are not offered.

It starts from `C128 BASIC.c128`, from a `.d71` disk, or from a `.d64` or `.g64` disk when
**Machine** in the settings menu is set to C128. A disk that starts itself (a boot sector,
as on the CP/M system disk) is left to the C128; otherwise `RUN"*"` is typed once the C128
is at `READY`. Programs (`.prg`, `.t64`), tapes and cartridges always start on a C64.

**80 columns.** The C128 has two screens. **40/80 DISPLAY** (F11 or Print Screen) switches
the picture between the 40-column screen and the 80-column screen; it locks, as on the
C128, which reads it at start-up to choose the screen BASIC uses. `ESC` `X` (F9, then X)
moves BASIC to the other screen.

**C64 mode.** `GO64` switches to C64 mode, with the C64's BASIC and KERNAL.

**CP/M.** With the C128 CP/M system disk in drive 8, the C128 starts CP/M 3.0 on its Z80.

**Disks.** Drive 8 is a 1571. It reads `.d64`, `.g64` and double-sided `.d71` images, and
**Create blank disk** creates a `.d71`. Fast disk load applies to the C64 only; the 1571
loads in burst mode, which is faster than a 1541.

## Cartridges

Supported `.crt` types: normal 8 KB, 16 KB and Ultimax cartridges, Ocean, Fun Play, Super
Games, System 3, Dinamic, Magic Desk, Simons' BASIC and EasyFlash (read only). The
cartridge is read in place from where the menu loaded it, in flash or in PSRAM.

## Keyboard

A USB keyboard is the C64 keyboard. **Keyboard layout** in the settings menu selects how
keys are mapped.

**US** (default): a key types what is printed on it.

| PC key | C64 |
|:--|:--|
| Shift+2 | `@` |
| Shift+6 | `↑` |
| Shift+7, Shift+8 | `&`, `*` |
| `'`, Shift+`'` | `'`, `"` |
| `[`, `]` | `[`, `]` |
| `\` | `£` |
| Shift+`-` | `←` |
| Left Ctrl | CTRL |
| Left Alt | Commodore key |
| Insert | INST |
| Delete, Backspace | DEL |

`{`, `}`, `|` and `~` have no key on the C64 and do nothing.

**C64**: a key does what the C64 key in the same position does, for software that uses keys
by position. Tab is CTRL, Left Ctrl and Left Alt the Commodore key, Insert `£`, End `=`,
`` ` `` `←` and `\` `↑`.

**Both layouts:**

| PC key | C64 |
|:--|:--|
| Esc | RUN/STOP |
| Page Up | RESTORE |
| Home | CLR/HOME |
| Caps Lock | SHIFT LOCK |
| F1, F3, F5, F7 | F1, F3, F5, F7 |
| F2, F4, F6, F8 | SHIFT + F1, F3, F5, F7 |
| Cursor keys | CRSR keys (left and up with SHIFT) |
| Numeric keypad | Joystick: 8 4 6 2 and the diagonals, 0 or 5 fire (Right Ctrl fires too) |
| F12 | Settings menu |

**Commodore 128 keys:**

| PC key | C128 |
|:--|:--|
| F9 | ESC |
| F10 | HELP |
| F11, Print Screen | 40/80 DISPLAY |
| Right Alt | ALT |
| End | LINE FEED (US layout) |
| Scroll Lock | NO SCROLL |
| Caps Lock | CAPS LOCK in C128 mode, SHIFT LOCK in C64 mode |

The C128's separate cursor keys and numeric keypad are not mapped: the cursor keys work as
on the C64, and the numeric keypad remains a joystick.

## Serial keyboard

With **Serial keyboard** switched on in the settings menu, text sent to the board's UART
console (115200 baud) is typed into the machine: a BASIC listing pasted into a terminal
program, or commands for CP/M. Each key is held until the machine has read it, and typing
waits while BASIC's keyboard buffer is full or a disk drive is running, so nothing is lost
while a line is being processed or a program is loading. Typing runs at up to about 25
characters a second.

- Letters are typed without SHIFT, whatever their case, so a listing in capitals types as
  BASIC keywords.
- Return or Enter is RETURN; Backspace is DEL; Ctrl-A to Ctrl-Z are CTRL with that letter.
- Esc is RUN/STOP on the C64 and ESC on the C128; Tab is the C128's TAB.
- `_` is the left arrow and `\` the pound sign.
- Ctrl-] is 40/80 DISPLAY and Ctrl-^ is RESTORE.
- Ctrl-\ is not typed: it prints the text screen shown (40 or 80 columns) on the console.

The board answers XON/XOFF when its input buffer fills; enable software flow control in the
terminal program for long listings. A key pressed on the USB keyboard abandons what is
still waiting to be typed. The option is not shown on boards whose UART receive pin is
used for something else, or that have no UART console.

## Controls

Gamepads are joysticks. The first is in port 2, where most games read it, and the second
in port 1.

| Button | Function |
|:--|:--|
| D-pad | Joystick |
| A | Fire |
| B | SPACE |
| X | RETURN |
| Y | F1 |
| START (pressed on its own) | RUN/STOP |
| SELECT + START | Settings menu |
| SELECT + A | Swap the joystick ports for this game |
| SELECT + UP / DOWN | Screen mode |
| START + A | Frame rate display |
| SELECT + START + UP + A | Reboot into BOOTSEL mode |

**Joystick port** in the settings menu selects the port of the first gamepad at the start
of every game.

## Settings of the C64

Besides the settings every emulator of the family has, the settings menu offers:

| Setting | Values |
|:--|:--|
| Joystick port | Port 2 (default) or Port 1 for the first gamepad |
| Fast disk load | On (default) or Off |
| Keyboard layout | US (default) or C64 |
| Machine | C64 (default) or C128: the computer `.d64` and `.g64` disks start on. Only on boards that can run the C128. |
| Serial keyboard | Off (default) or On: see [Serial keyboard](#serial-keyboard). A setting shared with the other emulators. |

They are kept in `/settings_C64x.dat` on the SD card.

**Overclock** (HSTX boards, from the ROM browser) runs the RP2350 at 384 MHz instead of
252 MHz, for demanding demos and loaders.

## Display and timing

The C64 is a PAL machine and runs at 50 frames a second; the HDMI/DVI output runs at
60 Hz, so one frame in six is shown twice. The picture is the 320 pixels of the C64's
display window with 20 lines of border above and below; the side borders are not shown.

## Supported hardware

Built for every RP2350 configuration `pico_shared` supports:

| HW_CONFIG | Board |
|-----------|-------|
| 1 | Pimoroni Pico DV Demo Base |
| 2 | Adafruit DVI + microSD breakouts / custom PCB |
| 5 | Adafruit Metro RP2350 |
| 6 | Waveshare RP2350-Zero with custom PCB |
| 7 | Waveshare RP2350-PiZero |
| 8 | Adafruit Fruit Jam (default) |
| 9 | Waveshare RP2350-USB-A |
| 10 | Spotpear HDMI board |
| 12 | Murmulator M1 |
| 13 | Murmulator M2 |
| 14 | Adafruit Feather RP2350 |
| 15 | Olimex RP2040-PICO-PC with a Raspberry Pi Pico 2 |

HW_CONFIG 3 and 4 are RP2040-only boards; 11 is a deprecated pinout.

PSRAM is optional. Without it, cartridges are read from flash and disks through the
two-track cache described under [Disks](#disks).

## Building

```
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_PIO_USB_PATH=/path/to/Pico-PIO-USB
git clone --recursive https://github.com/PicoPlus-devel/pico-c64.git
cd pico-c64
./bld.sh -c 8 -2          # one configuration
./buildAll.sh             # every configuration, into releases/
```

`./bld.sh -h` lists all options. `hosttest/` builds the emulation core for Linux, for
testing without a board; see [hosttest/README.md](hosttest/README.md).

***

## Known issues

- **Frankie Goes to Hollywood** (G64 of the original release) does not load: its copy
  protection is not handled by the drive emulation.
- The side borders are not shown, see [Display and timing](#display-and-timing).
- Commodore 128: the VDC has 16 KB of video memory, as in the original C128 (not the
  64 KB of the C128DCR); there are no function ROMs; 2 MHz mode does not disturb the
  40-column picture as on a real C128; the 1571 does not read MFM disks of other CP/M
  machines.

## Not yet done

- Faster loading of tapes.
- `.wav` tapes.
- NTSC machines.
- The RAM Expansion Unit.
- Save states.
- A second disk drive.

***

## Credits

- **esp32cbm** and **esp32emu** by [Robert Lucencic](https://github.com/rlucencic): the
  Commodore 64/128 emulation core and the 6502 and Z80 cores this port is built on.
- **PicoDVI** by [@shuichi_takano](https://github.com/shuichi-takano) and the HSTX HDMI
  driver by fliperama86.
- NES/Wii controller support from [@PaintYourDragon](https://github.com/PaintYourDragon)
  and Adafruit.

See [LICENSE](LICENSE) for the terms of each part.
