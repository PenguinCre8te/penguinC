#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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
    char *out = malloc(la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}
