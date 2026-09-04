#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "modem_simulator_lib.h"

static int (*real_ioctl)(int fd, unsigned long request, ...);
static struct modem_control_shared *shared_control;
static int shared_status;

static void load_real_ioctl(void)
{
    if (real_ioctl == NULL) {
        union {
            void *object;
            int (*function)(int fd, unsigned long request, ...);
        } symbol = {.object = dlsym(RTLD_NEXT, "ioctl")};
        real_ioctl = symbol.function;
    }
}

static struct modem_control_shared *get_shared_control(void)
{
    if (shared_status != 0) {
        return shared_control;
    }
    shared_status = -1;

    const char *path = getenv("MODEM_PTY_HANDLER_CONTROL");
    if (path == NULL || *path == '\0') {
        return NULL;
    }

    shared_control = modem_control_map_file(path);
    if (shared_control == NULL) {
        return NULL;
    }
    shared_status = 1;
    return shared_control;
}

static int emulate_tiocmget(void *argument)
{
    struct modem_control_shared *shared = get_shared_control();
    if (shared == NULL || argument == NULL) {
        errno = ENOTTY;
        return -1;
    }

    *(int *)argument = (int)shared->bits;
    return 0;
}

static int emulate_tiocmset(unsigned long request, void *argument)
{
    struct modem_control_shared *shared = get_shared_control();
    if (shared == NULL || argument == NULL) {
        errno = ENOTTY;
        return -1;
    }

    int mask = *(int *)argument;
    uint32_t bits = shared->bits;
    uint32_t modem_bits = bits & (uint32_t)(TIOCM_DSR | TIOCM_CAR);

    if (request == TIOCMSET) {
        bits = modem_bits | (uint32_t)(mask & (TIOCM_DTR | TIOCM_RTS));
    } else if (request == TIOCMBIS) {
        bits = modem_bits | ((bits | (uint32_t)mask) & (uint32_t)(TIOCM_DTR | TIOCM_RTS));
    } else if (request == TIOCMBIC) {
        bits = modem_bits | ((bits & ~(uint32_t)mask) & (uint32_t)(TIOCM_DTR | TIOCM_RTS));
    }

    shared->bits = bits;
    shared->generation++;
    return 0;
}

static int emulate_tiocmiwait(void *argument)
{
    struct modem_control_shared *shared = get_shared_control();
    if (shared == NULL || argument == NULL) {
        errno = ENOTTY;
        return -1;
    }

    int mask = *(int *)argument;
    uint32_t generation = shared->generation;
    uint32_t bits = shared->bits;
    struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 10000000};

    for (;;) {
        nanosleep(&pause_time, NULL);
        if (shared->generation != generation && (((int)(bits ^ shared->bits)) & mask) != 0) {
            return 0;
        }
        generation = shared->generation;
        bits = shared->bits;
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void *argument;

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);

    if (request == TIOCMGET) {
        int result = emulate_tiocmget(argument);
        if (result == 0) {
            return 0;
        }
    } else if (request == TIOCMSET || request == TIOCMBIS || request == TIOCMBIC) {
        int result = emulate_tiocmset(request, argument);
        if (result == 0) {
            return 0;
        }
    } else if (request == TIOCMIWAIT) {
        return emulate_tiocmiwait(argument);
    }

    load_real_ioctl();
    if (real_ioctl == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_ioctl(fd, request, argument);
}