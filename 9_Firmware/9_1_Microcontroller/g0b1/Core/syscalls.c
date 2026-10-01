/* Minimal newlib-nano stubs: _write -> uart_write (USART2), _sbrk bounded by the linker heap limit. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include "hal_uart.h"

extern char _sheap, _eheap;

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    if (len < 0 || uart_write(buf, (size_t)len) != 0) {   /* 50 ms timeout inside hal_uart */
        errno = EIO;
        return -1;
    }
    return len;
}

void *_sbrk(ptrdiff_t incr)
{
    static char *brk = &_sheap;
    char *prev = brk;
    if (brk + incr > &_eheap) {
        errno = ENOMEM;
        return (void *)-1;
    }
    brk += incr;
    return prev;
}

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; for (;;) { } }
