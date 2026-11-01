# Host harness

`c64_host` runs the vendored Commodore core (`cbm/`) on Linux, without a board. It is
used to test the core and the changes made to it before they go to hardware: booting,
programs, cartridges, disk images through the emulated 1541, and test suites.

## Building

```sh
hosttest/build.sh            # -> hosttest/c64_host, with AddressSanitizer and UBSan
NOASAN=1 hosttest/build.sh   # without sanitizers, for long runs and speed comparisons
```

## Running

```sh
CBM_ROMS=<dir> hosttest/c64_host <c64|c128> [frames] [out.ppm]
python3 hosttest/ppm2png.py out.ppm
```

The ROMs are read from `CBM_ROMS` (default `./roms`) under these names: `c64basic.rom`,
`c64kernal.rom`, `c64chargen.rom`, `1541.rom`; for the C128 `c128basic.rom`,
`c128kernal.rom`, `c128chargen.rom`, `c128c64.rom`, `1571.rom`. ROMs are not part of the
repository.

Media and actions are given through the environment:

| Variable | Meaning |
|---|---|
| `CBM_DISK8`, `CBM_DISK9` | disk image (`.d64`, `.d71`, `.g64`) in drive 8 or 9 |
| `CBM_CART` | cartridge (`.crt`) |
| `CBM_PRG` | program (`.prg`, `.t64`), loaded at frame 150 and started |
| `CBM_SCRIPT` | a file of lines `<frame> <command>`, see below |
| `CBM_STATUS=<n>` | print the CPU and drive state every n frames |
| `CBM_TRACE=<pc>[:<n>]` | disassemble n instructions once PC is reached |
| `CBM_WAV` | record the sound to a WAV file |
| `CBM_NEWDISK` | create a blank disk image first (`.d71`: double-sided) |
| `CBM_DISK_CACHE=<n>` | keep n tracks of a disk in memory, read from the file as the head gets there |
| `CBM_FAST_LOAD` | LOAD from drive 8 straight out of the disk image |
| `CBM_TAPE` | a `.tap` in the datasette, PLAY down |
| `CBM_SINK` | the VIC hands its lines to a callback, as on the board; a C128 also gets the board's memory layout (RAM bank 1 apart, a 16 KB VDC) and its 80 columns from `cbm_vdc_line()` |

Script commands: `type <text>` (`\n` is RETURN; letters must be lower case, upper case
types shifted keys), `key <n> 0|1`, `joy <bits>`, `shot <file.ppm>`, `text [addr]`
(print the text screen), `insert <drive> <path>`, `eject <drive>`, `cart <path>`,
`prg <path>`, `tape <path>`, `play 0|1`, `record <path>`, `rewind`, `tapeeject`, `peek <addr>`,
`dump <file>`, `dumpram <file>`, `reset`.

## Example: a disk game

```sh
printf '150 type load"*",8,1\\n\n3000 type run\\n\n3500 shot game.ppm\n' > game.script
CBM_ROMS=~/roms/C64/bios CBM_DISK8=game.d64 CBM_SCRIPT=game.script hosttest/c64_host c64 3600
```
