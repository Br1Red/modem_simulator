#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "modem_simulator_lib.h"

void modem_close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int modem_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int modem_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

ssize_t modem_retrying_read(int fd, uint8_t *buffer, size_t size)
{
    for (;;) {
        ssize_t count = read(fd, buffer, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return count;
    }
}

static ssize_t retrying_write(int fd, const uint8_t *buffer, size_t size)
{
    for (;;) {
        ssize_t count = write(fd, buffer, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return count;
    }
}

void modem_write_all(int fd, const void *data, size_t size)
{
    const uint8_t *buffer = data;
    size_t offset = 0;

    while (offset < size) {
        ssize_t written = retrying_write(fd, buffer + offset, size - offset);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd out = {.fd = fd, .events = POLLOUT};
                while (poll(&out, 1, -1) < 0 && errno == EINTR) {
                }
                continue;
            }
            break;
        }
        offset += (size_t)written;
    }
}

struct modem_control_shared *modem_control_map_file(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return NULL;
    }

    struct modem_control_shared *mapped = mmap(NULL, sizeof(*mapped), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        return NULL;
    }

    if (mapped->magic != MODEM_CONTROL_MAGIC || mapped->version != MODEM_CONTROL_VERSION) {
        munmap(mapped, sizeof(*mapped));
        return NULL;
    }

    return mapped;
}