#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

enum modem_mode {
    MODE_COMMAND,
    MODE_DATA,
};

enum dtr_policy {
    DTR_IGNORE,
    DTR_ESCAPE,
    DTR_HANGUP,
    DTR_HANGUP_RESET,
};

struct child_process {
    char **command;
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    bool running;
};

struct modem_state {
    bool echo;
    enum modem_mode mode;
    enum dtr_policy dtr_policy;
    char line[512];
    size_t line_length;
    size_t plus_count;
    bool dtr;
    struct modem_control_shared *control;
    struct child_process child;
};

static void modem_write(const char *text)
{
    modem_write_all(STDOUT_FILENO, text, strlen(text));
}

static void cleanup_child(struct child_process *child)
{
    modem_close_if_open(&child->stdin_fd);
    modem_close_if_open(&child->stdout_fd);
    child->pid = -1;
    child->running = false;
}

static struct modem_control_shared *open_control_file_from_env(void)
{
    const char *path = getenv("MODEM_PTY_HANDLER_CONTROL");
    if (path == NULL || *path == '\0') {
        return NULL;
    }

    return modem_control_map_file(path);
}

static bool current_dtr(const struct modem_state *state)
{
    if (state->control == NULL) {
        return true;
    }
    return (state->control->bits & TIOCM_DTR) != 0;
}

static void set_carrier(struct modem_state *state, bool enabled)
{
    if (state->control == NULL) {
        return;
    }

    uint32_t bits = state->control->bits;
    if (enabled) {
        bits |= TIOCM_CAR;
    } else {
        bits &= ~TIOCM_CAR;
    }

    if (bits != state->control->bits) {
        state->control->bits = bits;
        state->control->generation++;
    }
}

static void reset_settings(struct modem_state *state)
{
    state->echo = true;
    state->dtr_policy = DTR_HANGUP;
}

static bool spawn_child(struct child_process *child)
{
    int to_child[2];
    int from_child[2];

    if (child->command == NULL || child->command[0] == NULL) {
        return false;
    }
    if (pipe(to_child) < 0) {
        return false;
    }
    if (pipe(from_child) < 0) {
        close(to_child[0]);
        close(to_child[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return false;
    }

    if (pid == 0) {
        close(to_child[1]);
        close(from_child[0]);

        if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }

        close(to_child[0]);
        close(from_child[1]);

        execvp(child->command[0], child->command);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    child->pid = pid;
    child->stdin_fd = to_child[1];
    child->stdout_fd = from_child[0];
    child->running = true;
    modem_set_nonblock(child->stdout_fd);
    return true;
}

static void kill_child(struct child_process *child)
{
    int status;

    if (!child->running) {
        return;
    }

    kill(child->pid, SIGTERM);
    if (waitpid(child->pid, &status, WNOHANG) == 0) {
        kill(child->pid, SIGKILL);
        while (waitpid(child->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    cleanup_child(child);
}

static bool reap_child_if_dead(struct child_process *child)
{
    int status;

    if (!child->running) {
        return false;
    }

    pid_t waited = waitpid(child->pid, &status, WNOHANG);
    if (waited == child->pid) {
        cleanup_child(child);
        return true;
    }
    return false;
}

static void upper_ascii(char *line)
{
    for (char *cursor = line; *cursor != '\0'; cursor++) {
        *cursor = (char)toupper((unsigned char)*cursor);
    }
}

static void report_no_carrier(struct modem_state *state)
{
    set_carrier(state, false);
    state->mode = MODE_COMMAND;
    state->plus_count = 0;
    modem_write("NO CARRIER\r\n");
}

static void hangup(struct modem_state *state)
{
    kill_child(&state->child);
    report_no_carrier(state);
}

static void enter_data_mode(struct modem_state *state)
{
    set_carrier(state, true);
    state->mode = MODE_DATA;
    state->plus_count = 0;
    modem_write("CONNECT 9600\r\n");
}

static void handle_command(struct modem_state *state)
{
    if (state->echo) {
        modem_write_all(STDOUT_FILENO, state->line, state->line_length);
        modem_write("\r\n");
    }

    upper_ascii(state->line);

    if (strcmp(state->line, "AT") == 0) {
        modem_write("OK\r\n");
    } else if (strcmp(state->line, "ATZ") == 0) {
        reset_settings(state);
        modem_write("OK\r\n");
    } else if (strcmp(state->line, "ATE0") == 0) {
        state->echo = false;
        modem_write("OK\r\n");
    } else if (strcmp(state->line, "ATE1") == 0) {
        state->echo = true;
        modem_write("OK\r\n");
    } else if (strcmp(state->line, "ATI") == 0) {
        modem_write("stdio modem simulator\r\nOK\r\n");
    } else if (strncmp(state->line, "ATD", 3) == 0) {
        if (!state->child.running && !spawn_child(&state->child)) {
            report_no_carrier(state);
        } else {
            enter_data_mode(state);
        }
    } else if (strncmp(state->line, "AT&D", 4) == 0 && strlen(state->line) == 5) {
        switch (state->line[4]) {
        case '0':
            state->dtr_policy = DTR_IGNORE;
            modem_write("OK\r\n");
            break;
        case '1':
            state->dtr_policy = DTR_ESCAPE;
            modem_write("OK\r\n");
            break;
        case '2':
            state->dtr_policy = DTR_HANGUP;
            modem_write("OK\r\n");
            break;
        case '3':
            state->dtr_policy = DTR_HANGUP_RESET;
            modem_write("OK\r\n");
            break;
        default:
            modem_write("ERROR\r\n");
            break;
        }
    } else if (strcmp(state->line, "ATO") == 0) {
        if (state->child.running) {
            enter_data_mode(state);
        } else {
            report_no_carrier(state);
        }
    } else if (strcmp(state->line, "ATH") == 0) {
        hangup(state);
    } else {
        modem_write("ERROR\r\n");
    }
}

static void handle_dtr_drop(struct modem_state *state)
{
    switch (state->dtr_policy) {
    case DTR_IGNORE:
        break;
    case DTR_ESCAPE:
        if (state->child.running) {
            state->mode = MODE_COMMAND;
            state->plus_count = 0;
        }
        break;
    case DTR_HANGUP:
        if (state->child.running) {
            hangup(state);
        }
        break;
    case DTR_HANGUP_RESET:
        if (state->child.running) {
            hangup(state);
        }
        reset_settings(state);
        break;
    }
}

static void refresh_dtr(struct modem_state *state)
{
    bool dtr = current_dtr(state);
    if (state->dtr && !dtr) {
        handle_dtr_drop(state);
    }
    state->dtr = dtr;
}

static void handle_command_byte(struct modem_state *state, unsigned char byte)
{
    if (byte == '\r' || byte == '\n') {
        if (state->line_length > 0) {
            state->line[state->line_length] = '\0';
            handle_command(state);
            state->line_length = 0;
        }
        return;
    }

    if (state->line_length + 1 < sizeof(state->line)) {
        state->line[state->line_length++] = (char)byte;
    }
}

static void forward_pending_pluses(struct modem_state *state)
{
    static const char pluses[] = "+++";

    if (state->plus_count > 0 && state->child.running) {
        modem_write_all(state->child.stdin_fd, pluses, state->plus_count);
    }
    state->plus_count = 0;
}

static void handle_data_byte(struct modem_state *state, unsigned char byte)
{
    if (!state->child.running) {
        report_no_carrier(state);
        handle_command_byte(state, byte);
        return;
    }

    if (byte == '+') {
        state->plus_count++;
        if (state->plus_count == 3) {
            state->mode = MODE_COMMAND;
            state->plus_count = 0;
            modem_write("OK\r\n");
        }
        return;
    }

    forward_pending_pluses(state);
    modem_write_all(state->child.stdin_fd, &byte, 1);
}

static void handle_modem_input(struct modem_state *state)
{
    unsigned char buffer[4096];
    ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));

    if (count <= 0) {
        return;
    }

    for (ssize_t offset = 0; offset < count; offset++) {
        if (state->mode == MODE_COMMAND) {
            handle_command_byte(state, buffer[offset]);
        } else {
            handle_data_byte(state, buffer[offset]);
        }
    }
}

static void handle_child_output(struct modem_state *state)
{
    unsigned char buffer[4096];
    ssize_t count = read(state->child.stdout_fd, buffer, sizeof(buffer));

    if (count > 0) {
        modem_write_all(STDOUT_FILENO, buffer, (size_t)count);
    } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        cleanup_child(&state->child);
        report_no_carrier(state);
    }
}

int main(int argc, char **argv)
{
    char *default_command[] = {"/bin/cat", NULL};
    char **child_command = default_command;

    if (argc > 1) {
        if (strcmp(argv[1], "--") == 0) {
            if (argc > 2) {
                child_command = &argv[2];
            }
        } else {
            child_command = &argv[1];
        }
    }

    struct modem_state state = {
        .echo = true,
        .mode = MODE_COMMAND,
        .dtr_policy = DTR_HANGUP,
        .dtr = true,
        .child = {
            .command = child_command,
            .pid = -1,
            .stdin_fd = -1,
            .stdout_fd = -1,
        },
    };

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    modem_set_nonblock(STDIN_FILENO);
    state.control = open_control_file_from_env();
    state.dtr = current_dtr(&state);

    for (;;) {
        struct pollfd pollfds[2];
        nfds_t nfds = 0;

        pollfds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN | POLLHUP};
        if (state.mode == MODE_DATA && state.child.running) {
            pollfds[nfds++] = (struct pollfd){.fd = state.child.stdout_fd, .events = POLLIN | POLLHUP};
        }

        int ready = poll(pollfds, nfds, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (reap_child_if_dead(&state.child)) {
            report_no_carrier(&state);
        }

        refresh_dtr(&state);

        if (ready == 0) {
            continue;
        }

        if (pollfds[0].revents & POLLIN) {
            handle_modem_input(&state);
        }
        if (pollfds[0].revents & POLLHUP) {
            break;
        }
        if (nfds > 1 && (pollfds[1].revents & POLLIN)) {
            handle_child_output(&state);
        }
        if (nfds > 1 && (pollfds[1].revents & POLLHUP)) {
            cleanup_child(&state.child);
            report_no_carrier(&state);
        }
    }

    kill_child(&state.child);
    return 0;
}