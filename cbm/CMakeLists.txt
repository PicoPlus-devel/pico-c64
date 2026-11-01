# =====================================================================================
# cbmcore - Commodore 64/128 emulation core, vendored from esp32cbm and esp32emu
# (MIT, see UPSTREAM.md for the commits and the changes made here).
# =====================================================================================
add_library(cbmcore INTERFACE)

target_sources(cbmcore INTERFACE
    # --- The machine: 6510/8502, VIC-II, CIAs, SID, PLA, C128 MMU and VDC ---------
    ${CMAKE_CURRENT_SOURCE_DIR}/core/cbm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/vic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/cia.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/sid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/c128.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/vdc.c
    # --- Media: cartridges, programs, disk drives -----------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/core/cart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/prg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/via.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/gcr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/drive.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/fastload.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/tape.c
    # --- CPUs ------------------------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/m6502/m6502.c
    ${CMAKE_CURRENT_SOURCE_DIR}/z80/z80.c
    # --- Port glue -------------------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/port/port_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/port/cbm_fileio.c
)

target_include_directories(cbmcore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/m6502/include
    ${CMAKE_CURRENT_SOURCE_DIR}/z80/include
    ${CMAKE_CURRENT_SOURCE_DIR}/port
)

target_compile_definitions(cbmcore INTERFACE
    CBM_PORT=1
    EMUDBG_OFF=1
    CONFIG_Z80_TIMING_TSTATES=1
    CONFIG_Z80_READ_SPECIAL=1
)
