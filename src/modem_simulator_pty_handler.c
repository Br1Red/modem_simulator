#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "modem_simulator_lib.h"

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void die_errno(const char *message)
{
    fprintf(stderr, "modemsimulator-pty-handler: %s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void die_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [--tty-file PATH] [--control-file PATH] -- COMMAND [ARG...]\n"
            "\n"
            "Creates a pseudo-terminal and connects its serial byte stream to COMMAND stdio.\n"
            "The slave tty path is printed to stderr and optionally written to PATH.\n"
            "The control file is used by the LD_PRELOAD ioctl shim for DTR/DSR/CD.\n",
            program_name);
    exit(EXIT_FAILURE);
}

static void write_tty_file(const char *path, const char *tty_name)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        die_errno("open tty file");
    }

    modem_write_all(fd, tty_name, strlen(tty_name));
    modem_write_all(fd, "\n", 1);
    close(fd);
}

static pid_t spawn_command(char **command, int *child_stdin, int *child_stdout)
{
    int to_child[2];
    int from_child[2];

    if (pipe(to_child) < 0) {
        die_errno("pipe to child");
    }
    if (pipe(from_child) < 0) {
        die_errno("pipe from child");
    }

    pid_t pid = fork();
    if (pid < 0) {
        die_errno("fork");
    }

    if (pid == 0) {
        close(to_child[1]);
        close(from_child[0]);

        if (dup2(to_child[0], STDIN_FILENO) < 0) {
            die_errno("dup2 stdin");
        }
        if (dup2(from_child[1], STDOUT_FILENO) < 0) {
            die_errno("dup2 stdout");
        }

        close(to_child[0]);
        close(from_child[1]);

        execvp(command[0], command);
        die_errno("execvp");
    }

    close(to_child[0]);
    close(from_child[1]);

    *child_stdin = to_child[1];
    *child_stdout = from_child[0];
    return pid;
}

struct modem_control {
    int fd;
    struct modem_control_shared *shared;
    bool kernel_supported;
    bool dtr;
    bool dsr;
};

static bool get_kernel_modem_bits(int fd, int *bits)
{
    if (ioctl(fd, TIOCMGET, bits) < 0) {
        return false;
    }
    return true;
}

static bool set_kernel_modem_bits(int fd, int bits)
{
    if (ioctl(fd, TIOCMSET, &bits) < 0) {
        return false;
    }
    return true;
}

static int modem_control_bits(const struct modem_control *control)
{
    int bits = 0;

    if (control->kernel_supported && get_kernel_modem_bits(control->fd, &bits)) {
        return bits;
    }
    if (control->shared != NULL) {
        return (int)control->shared->bits;
    }
    return control->dtr ? TIOCM_DTR : 0;
}

static void modem_control_apply(struct modem_control *control)
{
    int bits = modem_control_bits(control);
    bool dsr = control->dsr;

    if (control->shared != NULL && (control->shared->flags & MODEM_CONTROL_DSR_ALWAYS) != 0) {
        dsr = true;
    }

    if (dsr) {
        bits |= TIOCM_DSR;
    } else {
        bits &= ~TIOCM_DSR;
    }

    if (control->kernel_supported && !set_kernel_modem_bits(control->fd, bits)) {
        control->kernel_supported = false;
    }
    if (control->shared != NULL) {
        control->shared->bits = (uint32_t)bits;
        control->shared->generation++;
    }
}

static void modem_control_refresh_dtr(struct modem_control *control)
{
    bool dtr = (modem_control_bits(control) & TIOCM_DTR) != 0;
    if (dtr == control->dtr) {
        return;
    }

    control->dtr = dtr;
    control->dsr = dtr;
    modem_control_apply(control);
}

static struct modem_control_shared *open_control_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        die_errno("open control file");
    }
    if (ftruncate(fd, (off_t)sizeof(struct modem_control_shared)) < 0) {
        die_errno("ftruncate control file");
    }

    struct modem_control_shared *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        die_errno("mmap control file");
    }

    shared->magic = MODEM_CONTROL_MAGIC;
    shared->version = MODEM_CONTROL_VERSION;
    shared->bits = TIOCM_DTR | TIOCM_DSR;
    shared->generation = 1;
    shared->flags = 0;
    return shared;
}

static void make_default_control_path(char *path, size_t size)
{
    snprintf(path, size, "/tmp/modemsimulator-pty-handler-%ld.ctrl", (long)getpid());
}

static int relay_loop(int pty_master,
                      int pty_slave_keeper,
                      struct modem_control_shared *shared,
                      int child_stdin,
                      int child_stdout,
                      pid_t child_pid)
{
    bool pty_open = true;
    bool child_stdout_open = true;
    uint8_t buffer[4096];
    int child_status = 0;
    int bits = 0;
    struct modem_control control = {
        .fd = pty_slave_keeper,
        .shared = shared,
        .kernel_supported = get_kernel_modem_bits(pty_slave_keeper, &bits),
        .dtr = true,
        .dsr = true,
    };

    if (modem_set_nonblock(pty_master) < 0 || modem_set_nonblock(child_stdout) < 0) {
        die_errno("set non-blocking mode");
    }

    modem_control_refresh_dtr(&control);

    while (!stop_requested && (pty_open || child_stdout_open)) {
        struct pollfd pollfds[2];
        nfds_t nfds = 0;

        if (pty_open) {
            pollfds[nfds++] = (struct pollfd){.fd = pty_master, .events = POLLIN | POLLHUP};
        }
        if (child_stdout_open) {
            pollfds[nfds++] = (struct pollfd){.fd = child_stdout, .events = POLLIN | POLLHUP};
        }

        int ready = poll(pollfds, nfds, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            die_errno("poll");
        }
        if (ready == 0) {
            modem_control_refresh_dtr(&control);
            continue;
        }

        nfds_t index = 0;
        if (pty_open) {
            short revents = pollfds[index++].revents;
            if (revents & POLLIN) {
                ssize_t count = modem_retrying_read(pty_master, buffer, sizeof(buffer));
                if (count > 0) {
                    modem_control_refresh_dtr(&control);
                    modem_write_all(child_stdin, buffer, (size_t)count);
                } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    modem_close_if_open(&child_stdin);
                    pty_open = false;
                }
            }
            if (revents & POLLHUP) {
                modem_close_if_open(&child_stdin);
                pty_open = false;
            }
        }

        if (child_stdout_open) {
            short revents = pollfds[index].revents;
            if (revents & POLLIN) {
                ssize_t count = modem_retrying_read(child_stdout, buffer, sizeof(buffer));
                if (count > 0) {
                    modem_write_all(pty_master, buffer, (size_t)count);
                } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    child_stdout_open = false;
                }
            }
            if (revents & POLLHUP) {
                child_stdout_open = false;
            }
        }

        pid_t waited = waitpid(child_pid, &child_status, WNOHANG);
        if (waited == child_pid) {
            child_stdout_open = false;
            pty_open = false;
        } else if (waited < 0 && errno != EINTR) {
            break;
        }
    }

    if (stop_requested) {
        kill(child_pid, SIGTERM);
    }

    modem_close_if_open(&child_stdin);
    modem_close_if_open(&child_stdout);
    modem_close_if_open(&pty_slave_keeper);
    close(pty_master);
    if (shared != NULL) {
        munmap(shared, sizeof(*shared));
    }

    while (waitpid(child_pid, &child_status, 0) < 0 && errno == EINTR) {
    }

    if (WIFEXITED(child_status)) {
        return WEXITSTATUS(child_status);
    }
    if (WIFSIGNALED(child_status)) {
        return 128 + WTERMSIG(child_status);
    }
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    const char *tty_file = NULL;
    const char *control_file = NULL;
    char default_control_file[128];
    int command_index = 1;

    while (command_index < argc) {
        if (strcmp(argv[command_index], "--tty-file") == 0) {
            if (command_index + 1 >= argc) {
                die_usage(argv[0]);
            }
            tty_file = argv[command_index + 1];
            command_index += 2;
        } else if (strcmp(argv[command_index], "--control-file") == 0) {
            if (command_index + 1 >= argc) {
                die_usage(argv[0]);
            }
            control_file = argv[command_index + 1];
            command_index += 2;
        } else if (strcmp(argv[command_index], "--") == 0) {
            command_index++;
            break;
        } else {
            break;
        }
    }

    if (command_index >= argc) {
        die_usage(argv[0]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (control_file == NULL) {
        make_default_control_path(default_control_file, sizeof(default_control_file));
        control_file = default_control_file;
    }

    int pty_master = -1;
    int pty_slave = -1;
    char tty_name[128];
    if (openpty(&pty_master, &pty_slave, tty_name, NULL, NULL) < 0) {
        die_errno("openpty");
    }

    struct termios termios_options;
    if (tcgetattr(pty_slave, &termios_options) < 0) {
        die_errno("tcgetattr");
    }
    cfmakeraw(&termios_options);
    cfsetspeed(&termios_options, B115200);
    if (tcsetattr(pty_slave, TCSANOW, &termios_options) < 0) {
        die_errno("tcsetattr");
    }

    if (modem_set_cloexec(pty_master) < 0 || modem_set_cloexec(pty_slave) < 0) {
        die_errno("set close-on-exec");
    }

    if (tty_file != NULL) {
        write_tty_file(tty_file, tty_name);
    }

    struct modem_control_shared *shared = open_control_file(control_file);

    fprintf(stderr, "%s\n", tty_name);
    fprintf(stderr, "control file: %s\n", control_file);
    fflush(stderr);

    if (setenv("MODEM_PTY_HANDLER_CONTROL", control_file, 1) < 0) {
        die_errno("setenv MODEM_PTY_HANDLER_CONTROL");
    }

    int child_stdin = -1;
    int child_stdout = -1;
    pid_t child_pid = spawn_command(&argv[command_index], &child_stdin, &child_stdout);

    return relay_loop(pty_master, pty_slave, shared, child_stdin, child_stdout, child_pid);
}