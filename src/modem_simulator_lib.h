#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MODEM_CONTROL_MAGIC 0x4d535054u
#define MODEM_CONTROL_VERSION 1u

struct modem_control_shared {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t bits;
    volatile uint32_t generation;
};

void modem_close_if_open(int *fd);
int modem_set_cloexec(int fd);
int modem_set_nonblock(int fd);
ssize_t modem_retrying_read(int fd, uint8_t *buffer, size_t size);
void modem_write_all(int fd, const void *buffer, size_t size);
struct modem_control_shared *modem_control_map_file(const char *path);
