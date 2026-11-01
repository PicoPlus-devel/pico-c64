/*
 * The Pico port's side of cbm_internal.h (selected with -DCBM_PORT):
 * allocation, logging and file access for the vendored core.
 */
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "cbm_fileio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.cpp: SRAM for small blocks, PSRAM (when the board has it) for big ones;
 * NULL when there is no room (the core checks, the SDK's malloc would panic)
 */
void *cbm_port_malloc(size_t n);
void *cbm_port_calloc(size_t n, size_t size);
void cbm_port_free(void *p);

#ifdef __cplusplus
}
#endif

#define CBM_LOG(fmt, ...)   printf("cbm: " fmt "\n", ##__VA_ARGS__)
#define cbm_malloc(n)       cbm_port_malloc(n)
#define cbm_calloc(n, s)    cbm_port_calloc((n), (s))
#define cbm_free(p)         cbm_port_free(p)
