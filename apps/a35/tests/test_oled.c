#include "oled.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

static int open_fails, ioctl_fails, short_write, interrupt_once;
static unsigned writes, opens, closes, fail_write_at;

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int __wrap_open(const char *path, int flags, ...) {
    (void)path;
    require((flags & O_CLOEXEC) != 0, "OLED descriptor can escape into child processes");
    ++opens;
    if (open_fails) {
        errno = ENOENT;
        return -1;
    }
    return 42;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)request;
    require(fd == 42, "invalid OLED descriptor");
    if (ioctl_fails) {
        errno = ENXIO;
        return -1;
    }
    return 0;
}

int __wrap_close(int fd) {
    require(fd == 42, "OLED descriptor closed more than once");
    ++closes;
    return 0;
}

ssize_t __wrap_write(int fd, const void *data, size_t size) {
    (void)data;
    require(fd == 42 && size == 2, "invalid OLED transaction");
    ++writes;
    if (interrupt_once) {
        interrupt_once = 0;
        errno = EINTR;
        return -1;
    }
    if (writes == fail_write_at) {
        errno = ENXIO;
        return short_write ? 1 : -1;
    }
    return (ssize_t)size;
}

int main(void) {
    open_fails = 1;
    require(oled_init() == -1 && closes == 0, "failed open acquired a resource");
    open_fails = 0;
    ioctl_fails = 1;
    require(oled_init() == -1 && closes == 1, "failed I2C setup leaked a descriptor");
    ioctl_fails = 0;
    fail_write_at = 1;
    require(oled_init() == -1 && errno == ENXIO && closes == 2 && writes == 1,
            "initialization continued after an I2C failure");
    writes = 0;
    short_write = 1;
    require(oled_init() == -1 && errno == EIO && closes == 3 && writes == 1,
            "short I2C initialization was accepted");
    writes = 0;
    short_write = 0;
    fail_write_at = 0;
    interrupt_once = 1;
    require(oled_init() == 0 && writes > 1, "interrupted I2C transaction was not retried");
    unsigned previous_opens = opens;
    require(oled_init() == 0 && opens == previous_opens, "reinitialization leaked a descriptor");
    writes = 0;
    fail_write_at = 1;
    short_write = 1;
    require(oled_show_nav(NULL, 0) == -1 && errno == EIO && writes == 1,
            "display continued after a short I2C transaction");
    writes = 0;
    short_write = 0;
    require(oled_clear() == -1 && errno == ENXIO && writes == 1,
            "display error did not reach the caller");
    oled_close();
    oled_close();
    require(closes == 4, "OLED close was not idempotent");
    puts("OLED I2C error propagation and descriptor ownership checks passed");
    return 0;
}
