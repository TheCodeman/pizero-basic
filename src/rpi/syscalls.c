/**
 * @file syscalls.c
 * @brief What newlib needs from the machine: the heap (_sbrk), the console
 * (_write, _read) and the clock (_gettimeofday). Files go through FatFs
 * directly, so the file calls are stubs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>

#include "terminal.h"
#include "rpi.h"

extern char __heap_start[];
/// The heap ends here: the top of the ARM's RAM (main sets it).
char *heap_limit = (char *)0x1C000000;
static char *heap_end = __heap_start;

void *_sbrk(ptrdiff_t incr) {
    char *prev = heap_end;
    if (incr > 0 && (size_t)incr > (size_t)(heap_limit - heap_end)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return prev;
}

/// Bytes the heap can still grow by (for FRE()).
size_t heap_room(void) { return (size_t)(heap_limit - heap_end); }

int _write(int fd, const char *buf, int len) {
    if (fd != 1 && fd != 2) {
        errno = EBADF;
        return -1;
    }
    terminal_write(buf, len);
    return len;
}

int _read(int fd, char *buf, int len) {
    if (fd != 0) {
        errno = EBADF;
        return -1;
    }
    int n = 0;
    while (n < len) {
        int c = terminal_getc_wait();
        if (c == '\r')
            c = '\n';
        buf[n++] = (char)c;
        if (c == '\n')
            break;
    }
    return n;
}

/// The wall clock: microseconds since 1970 at timer 0 (settimeofday sets it;
/// the Pi has no clock of its own, so it starts at 1 Jan 1970).
static int64_t clock_offset_us;

int _gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint64_t us = timer_us() + clock_offset_us;
    tv->tv_sec = (time_t)(us / 1000000u);
    tv->tv_usec = (suseconds_t)(us % 1000000u);
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tz;
    if (tv)
        clock_offset_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - (int64_t)timer_us();
    return 0;
}

clock_t _times(struct tms *t) {
    clock_t c = (clock_t)(timer_us() / 10000u);
    if (t) {
        t->tms_utime = c;
        t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
    }
    return c;
}

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; errno = ENOSYS; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _isatty(int fd) { return fd <= 2; }
int _fstat(int fd, struct stat *st) {
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _unlink(const char *p) { (void)p; errno = ENOSYS; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
void _exit(int code) {
    (void)code;
    board_reboot();
}

/// -nostartfiles leaves these out; __libc_init_array calls them.
void _init(void) {}
void _fini(void) {}
