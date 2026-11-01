/*
 * cbm_fileio.c - see cbm_fileio.h
 */
#define CBM_FILEIO_NO_REDIRECT 1

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "cbm_fileio.h"

struct CBM_FILE {
    FIL fil;
};

CBM_FILE *cbm_fopen(const char *path, const char *mode)
{
    if (path == NULL || mode == NULL) {
        return NULL;
    }
    BYTE flags;
    bool update = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r':
        flags = FA_READ | (update ? FA_WRITE : 0);
        break;
    case 'w':
        flags = FA_WRITE | FA_CREATE_ALWAYS | (update ? FA_READ : 0);
        break;
    case 'a':
        flags = FA_WRITE | FA_OPEN_APPEND | (update ? FA_READ : 0);
        break;
    default:
        return NULL;
    }
    CBM_FILE *f = malloc(sizeof(CBM_FILE));
    if (f == NULL) {
        return NULL;
    }
    if (f_open(&f->fil, path, flags) != FR_OK) {
        free(f);
        return NULL;
    }
    return f;
}

int cbm_fclose(CBM_FILE *f)
{
    if (f == NULL) {
        return EOF;
    }
    FRESULT fr = f_close(&f->fil);
    free(f);
    return fr == FR_OK ? 0 : EOF;
}

size_t cbm_fread(void *ptr, size_t size, size_t nmemb, CBM_FILE *f)
{
    UINT n = 0;
    if (f == NULL || size == 0 || nmemb == 0 || f_read(&f->fil, ptr, (UINT)(size * nmemb), &n) != FR_OK) {
        return 0;
    }
    return n / size;
}

size_t cbm_fwrite(const void *ptr, size_t size, size_t nmemb, CBM_FILE *f)
{
    UINT n = 0;
    if (f == NULL || size == 0 || nmemb == 0 || f_write(&f->fil, ptr, (UINT)(size * nmemb), &n) != FR_OK) {
        return 0;
    }
    return n / size;
}

int cbm_fseek(CBM_FILE *f, long offset, int whence)
{
    if (f == NULL) {
        return -1;
    }
    FSIZE_t base = whence == SEEK_CUR ? f_tell(&f->fil) : whence == SEEK_END ? f_size(&f->fil) : 0;
    return f_lseek(&f->fil, base + offset) == FR_OK ? 0 : -1;
}

long cbm_ftell(CBM_FILE *f)
{
    return f ? (long)f_tell(&f->fil) : -1;
}
