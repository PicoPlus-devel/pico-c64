# CHANGELOG

**pico-c64** is a Commodore 64 and Commodore 128 emulator for RP2350 boards (Raspberry Pi Pico 2 and variants),
based on the [esp32cbm](https://github.com/rlucencic/esp32cbm) emulation core by Robert
Lucencic. Programs, disk images, tapes and cartridges are loaded from an SD card, video and
audio go out over HDMI or DVI, and a USB keyboard works as the C64 keyboard.

[Binaries for every board configuration are at the end of this page](#downloads___).

# Getting started

1. Format an SD card as FAT32 (recommended) or exFAT and copy your C64 files (`.prg`, `.t64`,
   `.d64`, `.g64`, `.tap`, `.crt`) to `/roms/C64`. Subdirectories are supported.
2. Copy the C64 ROM files to `/roms/C64/bios` (or `/bios`): `c64basic.rom`, `c64kernal.rom`,
   `c64chargen.rom`, and `1541.rom` for the disk drive. For the Commodore 128 also
   `c128basic.rom`, `c128kernal.rom`, `c128chargen.rom`, `c128c64.rom` and `1571.rom`. They are
   not distributed with the emulator; without them nothing runs.
3. Pick the `.uf2` for your board from the [table at the bottom of this page](#downloads___),
   hold **BOOTSEL** while connecting the board over USB, and copy the file to the drive that
   appears.

[Full setup instructions are in the readme](https://github.com/PicoPlus-devel/pico-c64#setup-overview).

> [!IMPORTANT]
> RP2350 only. The original Raspberry Pi Pico (RP2040) is not fast enough for the C64.

# v0.2

- The arrow keys now work in CP/M programs such as SuperCalc. Shift with an arrow key edits
  the CP/M command line.
- The readme explains how to use the C128's 80-column screen.
- Less output on the serial console.

# v0.1

First release.

## What works

- **The Commodore 64**, with its graphics and sound, at full speed.
- **Programs** (`.prg`, `.t64`) start straight from the menu.
- **Disk drive 8** with `.d64` and `.g64` images, read and write, including games with their
  own fast loaders. **Fast disk load** (on by default) loads programs at once instead of at
  the speed of a real drive. Disks can be changed and blank disks created from the settings
  menu.
- **Tapes** (`.tap`), including turbo loaders, and saving to tape.
- **Cartridges** (`.crt`), including Ocean, EasyFlash and Magic Desk cartridges.
- **BASIC** without loading anything, through the `C64 BASIC` entry in the menu.
- **The Commodore 128** on boards with HDMI (HSTX) and PSRAM: BASIC 7.0 on the 40- and
  80-column screens, C64 mode, CP/M 3.0 and a 1571 drive with `.d71` disks.
- **USB keyboard** in a US layout (keys type what is printed on them) or the C64's own layout.
- **Two joysticks** from USB gamepads, NES/SNES controllers, Wii controllers or the numeric
  keypad.
- **Serial keyboard**: text sent to the board's serial console is typed into the computer,
  such as a BASIC listing pasted from a PC or CP/M commands.
- **Boards without PSRAM** run the C64 too: the first start copies the ROMs from the SD card
  into the board's flash, which takes a few seconds.
- Builds for all 12 RP2350 board configurations, plus the
  [pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader) variant.

## Limitations

- PAL only; the picture does not show the side borders.
- Tapes load at the speed of a real datasette.
- Not available yet: NTSC machines, the RAM Expansion Unit, save states.

<a name="downloads___"></a>
## Downloads by configuration

Binaries for each configuration are listed below. For board-by-board wiring, PCB designs and
3D-printable cases, refer to the
[pico-infonesPlus documentation](https://github.com/PicoPlus-devel/pico-infonesPlus#setup). The
boards and their pinouts are the same, but pico-c64 runs on RP2350 boards only.

### Standalone boards

| Board | Binary |
|:--|:--|
| Adafruit Metro RP2350 | [picoC64_AdafruitMetroRP2350_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_AdafruitMetroRP2350_arm.uf2) |
| Adafruit Fruit Jam | [picoC64_AdafruitFruitJam_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_AdafruitFruitJam_arm_piousb.uf2) |
| Adafruit Feather RP2350 + TLV320DAC3100 | [picoC64_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2) |
| Waveshare RP2350-PiZero | [picoC64_WaveShareRP2350PiZero_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_WaveShareRP2350PiZero_arm_piousb.uf2) |

### Breadboard / Custom PCB

Adafruit DVI Breakout + MicroSD card breakout, or the custom PCB.

| Board | Binary |
|:--|:--|
| Pico 2 / Pimoroni Pico Plus 2 | [picoC64_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_AdafruitDVISD_pico2_arm.uf2) |

### PCB Waveshare RP2350-Zero (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_WaveShareRP2350ZeroWithPCB_arm.uf2)

### PCB Waveshare RP2350-USBA (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_WaveShare2350USBA_arm_piousb.uf2)

### Pimoroni Pico DV Demo Base

| Board | Binary |
|:--|:--|
| Pico 2 / Pimoroni Pico Plus 2 | [picoC64_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_PimoroniDVI_pico2_arm.uf2) |

### SpotPear HDMI

For more info about the SpotPear HDMI see https://spotpear.com/index/product/detail/id/1207.html and https://spotpear.com/index/study/detail/id/971.html.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoC64_SpotpearHDMI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoC64_MurmulatorM1_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoC64_MurmulatorM2_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_MurmulatorM2_arm.uf2) |

### Olimex RP2040-PICO-PC

| Board | Binary |
|:--|:--|
| Pico 2 | [picoC64_OlimexPicoPC_arm.uf2](https://github.com/PicoPlus-devel/pico-c64/releases/latest/download/picoC64_OlimexPicoPC_arm.uf2) |

There is no Pico 2 W binary for this board.
