/*
 * files.c — Platform-independent file I/O for penguinC stdlib
 *
 * Uses C stdio (fopen/fclose/fread/fwrite/fseek/ftell/feof)
 * for cross-platform compatibility.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* File handle wrapper stored as opaque pointer (returned as i64) */
typedef struct {
    FILE *fp;
    char *path;
    int mode; /* 0=read, 1=write, 2=append */
} FileHandle;

/* Open a file. Returns handle (i64), or 0 on failure. */
long files_open(const char *path, long mode) {
    const char *mode_str;
    switch (mode) {
        case 0:  mode_str = "rb";  break; /* read */
        case 1:  mode_str = "wb";  break; /* write */
        case 2:  mode_str = "ab";  break; /* append */
        default: mode_str = "rb";  break;
    }

    FILE *fp = fopen(path, mode_str);
    if (!fp) return 0;

    FileHandle *h = (FileHandle *)malloc(sizeof(FileHandle));
    if (!h) { fclose(fp); return 0; }

    h->fp   = fp;
    h->path = strdup(path);
    h->mode = (int)mode;

    return (long)(intptr_t)h;
}

/* Close a file. */
void files_close(long handle) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h) return;
    if (h->fp) fclose(h->fp);
    free(h->path);
    free(h);
}

/* Read up to `count` bytes into `buf`. Returns bytes read. */
long files_read(long handle, char *buf, long count) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp || !buf || count <= 0) return 0;
    return (long)fread(buf, 1, (size_t)count, h->fp);
}

/* Write `count` bytes from `buf`. Returns bytes written. */
long files_write(long handle, const char *buf, long count) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp || !buf) return 0;
    
    // Ignore passed count, use actual string length
    size_t actual_len = strlen(buf); 
    return (long)fwrite(buf, 1, actual_len, h->fp);
}

/* Seek to position. whence: 0=beginning, 1=current, 2=end. Returns 0 on success. */
long files_seek(long handle, long offset, long whence) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp) return -1;
    int stdio_whence;
    switch (whence) {
        case 0:  stdio_whence = SEEK_SET; break;
        case 1:  stdio_whence = SEEK_CUR; break;
        case 2:  stdio_whence = SEEK_END; break;
        default: stdio_whence = SEEK_SET; break;
    }
    return fseek(h->fp, offset, stdio_whence);
}

/* Tell current position. Returns position or -1 on error. */
long files_tell(long handle) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp) return -1;
    return ftell(h->fp);
}

/* Check if at end of file. Returns 1 if at EOF, 0 otherwise. */
long files_eof(long handle) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp) return 1;
    return feof(h->fp) ? 1 : 0;
}

/* Flush file buffer. Returns 0 on success. */
long files_flush(long handle) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp) return -1;
    return fflush(h->fp);
}

/* Read entire file into a malloc'd string. Caller must free. Returns length. */
long files_read_all(long handle, char **out_buf) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp || !out_buf) { if (out_buf) *out_buf = NULL; return 0; }

    /* Get file size */
    long saved_pos = ftell(h->fp);
    fseek(h->fp, 0, SEEK_END);
    long size = ftell(h->fp);
    fseek(h->fp, saved_pos, SEEK_SET);

    if (size <= 0) { *out_buf = NULL; return 0; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { *out_buf = NULL; return 0; }

    long n = (long)fread(buf, 1, (size_t)size, h->fp);
    buf[n] = '\0';
    *out_buf = buf;
    return n;
}

/* Write a null-terminated string. Returns bytes written. */
long files_write_str(long handle, const char *str) {
    FileHandle *h = (FileHandle *)(intptr_t)handle;
    if (!h || !h->fp || !str) return 0;
    long len = (long)strlen(str);
    return (long)fwrite(str, 1, (size_t)len, h->fp);
}
