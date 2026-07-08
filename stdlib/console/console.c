#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern void *arc_alloc(size_t size);

static void write_long(long val) {
    char buf[32];
    int i = 31;
    buf[i] = '\0';
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { buf[--i] = '0'; }
    else { while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; } }
    if (neg) buf[--i] = '-';
    write(1, buf + i, 31 - i);
}

long io_print(const char *buf) {
    return write(1, buf, strlen(buf));
}

long io_println(const char *buf) {
    long r = write(1, buf, strlen(buf));
    write(1, "\n", 1);
    return r;
}

long io_print_int(long val) {
    write_long(val);
    return 0;
}

long io_println_int(long val) {
    write_long(val);
    write(1, "\n", 1);
    return 0;
}

long io_print_float(double val) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", val);
    write(1, buf, n);
    return 0;
}

long io_println_float(double val) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", val);
    write(1, buf, n);
    write(1, "\n", 1);
    return 0;
}

long program_exit_c(long code) {
    _exit(code);
    return 0;
}

long penguin_strlen(const char *s) {
    return (long)strlen(s);
}

char *penguin_str_concat(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *out = arc_alloc(la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}

char *int_to_string(long val) {
    char buf[32];
    int i = 31;
    buf[i] = '\0';
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { buf[--i] = '0'; }
    else { while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; } }
    if (neg) buf[--i] = '-';
    size_t len = 31 - i;
    char *out = arc_alloc(len + 1);
    memcpy(out, buf + i, len + 1);
    return out;
}

char *float_to_string(double val) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", val);
    char *out = arc_alloc(n + 1);
    memcpy(out, buf, n + 1);
    return out;
}

char *bool_to_string(long val) {
    const char *s = val ? "true" : "false";
    size_t len = val ? 4 : 5;
    char *out = arc_alloc(len + 1);
    memcpy(out, s, len + 1);
    return out;
}

long parse_int(const char *s) {
    return strtol(s, NULL, 10);
}

double parse_float(const char *s) {
    return strtod(s, NULL);
}

char *input(void) {
    int capacity = 16; // Start with a small buffer
    int length = 0;
    char *buffer = malloc(capacity * sizeof(char));
    
    // Check if memory allocation failed
    if (buffer == NULL) {
        return NULL; 
    }

    int ch;
    // getchar() blocks until the user presses Enter
    while ((ch = getchar()) != '\n' && ch != EOF) {
        // If we run out of space, double the capacity
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *temp = realloc(buffer, capacity * sizeof(char));
            if (temp == NULL) {
                free(buffer); // Clean up memory on failure
                return NULL;
            }
            buffer = temp;
        }
        buffer[length++] = (char)ch;
    }

    // Null-terminate the string
    buffer[length] = '\0';

    // If EOF was reached immediately without reading any characters
    if (ch == EOF && length == 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}