/*
 * What the vendored core expects from esp32emu but we leave out:
 *
 * - the debugger's CPU adapters (cbm.c names them when it registers the
 *   machine; with EMUDBG_OFF nothing ever uses them)
 * - space.c, the free-space counter for esp32cbm's web page (it needs
 *   esp32emu's diskspace component)
 */
#include "cbm_internal.h"
#include "emudbg.h"

const emudbg_arch_t emudbg_arch_6502;
const emudbg_arch_t emudbg_arch_z80;

void space_update(int drive, const struct gcr_disk *disk)
{
    (void)drive;
    (void)disk;
}

bool cbm_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    (void)drive;
    (void)free_bytes;
    (void)size;
    return false;
}
