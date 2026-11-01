/*
 * <stdio.h> file access on top of FatFs, for the vendored core.
 *
 * The core opens disk images, cartridges and programs with fopen() and
 * friends; pico_shared exposes FatFs directly. Only the calls the core makes
 * are implemented (the same approach as pico-994A's ti99_fileio.h). FIL is
 * about 560 bytes, so handles come from the heap, not the stack.
 *
 * Include after <stdio.h>: the names are redirected with macros.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CBM_FILE CBM_FILE;

CBM_FILE *cbm_fopen(const char *path, const char *mode);
int cbm_fclose(CBM_FILE *f);
size_t cbm_fread(void *ptr, size_t size, size_t nmemb, CBM_FILE *f);
size_t cbm_fwrite(const void *ptr, size_t size, size_t nmemb, CBM_FILE *f);
int cbm_fseek(CBM_FILE *f, long offset, int whence);
long cbm_ftell(CBM_FILE *f);

#ifdef __cplusplus
}
#endif

#ifndef CBM_FILEIO_NO_REDIRECT
#define FILE    CBM_FILE
#define fopen   cbm_fopen
#define fclose  cbm_fclose
#define fread   cbm_fread
#define fwrite  cbm_fwrite
#define fseek   cbm_fseek
#define ftell   cbm_ftell
#endif
