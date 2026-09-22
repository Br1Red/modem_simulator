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
#include <time.h>
#include <unistd.h>

#include "modem_simulator_lib.h"

static volatile sig_atomic_t incoming_call_requested;

static void request_incoming_call(int signal_number)
{
    (void)signal_number;
    incoming_call_requested = 1;
}

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
    bool quiet;
    bool verbose;
    bool dsr_always;
    enum modem_mode mode;
    enum dtr_policy dtr_policy;
    char **dial_command;
    char **incoming_command;
    bool incoming_call;
    bool ring_indicator;
    int s0_auto_answer_rings;
    unsigned int ring_count;
    long long next_ring_ms;
    long long ring_indicator_until_ms;
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

static void modem_result(struct modem_state *state, int numeric, const char *verbose)
{
    if (state->quiet) {
        return;
    }
    if (state->verbose) {
        modem_write(verbose);
    } else {
        char response[16];
        snprintf(response, sizeof(response), "%d\r\n", numeric);
        modem_write(response);
    }
}

static void result_ok(struct modem_state *state)
{
    modem_result(state, 0, "OK\r\n");
}

static void result_error(struct modem_state *state)
{
    modem_result(state, 4, "ERROR\r\n");
}

static long long now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((long long)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
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
    if (state->dsr_always) {
        bits |= TIOCM_DSR;
    }

    if (bits != state->control->bits) {
        state->control->bits = bits;
        state->control->generation++;
    }
}

static void set_ring_indicator(struct modem_state *state, bool enabled)
{
    if (state->control == NULL) {
        return;
    }

    uint32_t bits = state->control->bits;
    if (enabled) {
        bits |= TIOCM_RNG;
    } else {
        bits &= ~TIOCM_RNG;
    }

    if (bits != state->control->bits) {
        state->control->bits = bits;
        state->control->generation++;
    }
    state->ring_indicator = enabled;
}

static void reset_settings(struct modem_state *state)
{
    state->echo = true;
    state->quiet = false;
    state->verbose = true;
    state->dsr_always = false;
    state->dtr_policy = DTR_HANGUP;
    state->s0_auto_answer_rings = 0;
}

static bool spawn_child(struct child_process *child, char **command)
{
    int to_child[2];
    int from_child[2];

    if (command == NULL || command[0] == NULL) {
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

        execvp(command[0], command);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    child->pid = pid;
    child->stdin_fd = to_child[1];
    child->stdout_fd = from_child[0];
    child->command = command;
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
    set_ring_indicator(state, false);
    state->incoming_call = false;
    state->mode = MODE_COMMAND;
    state->plus_count = 0;
    modem_result(state, 3, "NO CARRIER\r\n");
}

static void reject_incoming_call(struct modem_state *state)
{
    report_no_carrier(state);
}

static void hangup(struct modem_state *state)
{
    kill_child(&state->child);
    report_no_carrier(state);
}

static void enter_data_mode(struct modem_state *state)
{
    set_ring_indicator(state, false);
    state->incoming_call = false;
    set_carrier(state, true);
    state->mode = MODE_DATA;
    state->plus_count = 0;
    modem_result(state, 1, "CONNECT 9600\r\n");
}

static void start_incoming_call(struct modem_state *state)
{
    if (state->child.running) {
        return;
    }

    state->incoming_call = true;
    state->ring_count = 0;
    state->next_ring_ms = 0;
    state->ring_indicator_until_ms = 0;
}

static void answer_call(struct modem_state *state)
{
    if (!state->incoming_call) {
        report_no_carrier(state);
        return;
    }
    if (!state->child.running && !spawn_child(&state->child, state->incoming_command)) {
        report_no_carrier(state);
        return;
    }

    enter_data_mode(state);
}

static void handle_ringing(struct modem_state *state)
{
    if (!state->incoming_call || state->child.running) {
        return;
    }

    long long current_ms = now_ms();
    if (state->ring_indicator && current_ms >= state->ring_indicator_until_ms) {
        set_ring_indicator(state, false);
    }
    if (state->next_ring_ms != 0 && current_ms < state->next_ring_ms) {
        return;
    }

    state->ring_count++;
    set_ring_indicator(state, true);
    state->ring_indicator_until_ms = current_ms + 2000;
    state->next_ring_ms = current_ms + 3000;
    modem_result(state, 2, "RING\r\n");

    if (state->s0_auto_answer_rings > 0 && state->ring_count >= (unsigned int)state->s0_auto_answer_rings) {
        answer_call(state);
    }
}

static void handle_command(struct modem_state *state)
{
    if (state->echo) {
        modem_write_all(STDOUT_FILENO, state->line, state->line_length);
        modem_write("\r\n");
    }

    upper_ascii(state->line);

    if (strncmp(state->line, "AT", 2) != 0) {
        result_error(state);
        return;
    }

    char *command = state->line + 2;
    if (*command == '\0') {
        result_ok(state);
        return;
    }

    bool command_error = false;
    bool show_identification = false;
    bool show_s0 = false;
    bool terminal_response = false;
    while (*command != '\0') {
        if (strcmp(command, "Z") == 0) {
            reset_settings(state);
            break;
        } else if (strncmp(command, "E0", 2) == 0) {
            state->echo = false;
            command += 2;
        } else if (strncmp(command, "E1", 2) == 0) {
            state->echo = true;
            command += 2;
        } else if (strncmp(command, "Q0", 2) == 0) {
            state->quiet = false;
            command += 2;
        } else if (strncmp(command, "Q1", 2) == 0) {
            state->quiet = true;
            command += 2;
        } else if (strncmp(command, "V0", 2) == 0) {
            state->verbose = false;
            command += 2;
        } else if (strncmp(command, "V1", 2) == 0) {
            state->verbose = true;
            command += 2;
        } else if (strncmp(command, "I", 1) == 0) {
            show_identification = true;
            command++;
        } else if (strncmp(command, "D", 1) == 0) {
            if (!state->child.running && !spawn_child(&state->child, state->dial_command)) {
                report_no_carrier(state);
            } else {
                enter_data_mode(state);
            }
            terminal_response = true;
            break;
        } else if (strncmp(command, "&D", 2) == 0 && command[2] != '\0') {
            switch (command[2]) {
            case '0':
                state->dtr_policy = DTR_IGNORE;
                break;
            case '1':
                state->dtr_policy = DTR_ESCAPE;
                break;
            case '2':
                state->dtr_policy = DTR_HANGUP;
                break;
            case '3':
                state->dtr_policy = DTR_HANGUP_RESET;
                break;
            default:
                command_error = true;
                break;
            }
            command += 3;
        } else if (strncmp(command, "&C", 2) == 0 && (command[2] == '0' || command[2] == '1')) {
            command += 3;
        } else if (strncmp(command, "&S", 2) == 0 && (command[2] == '0' || command[2] == '1')) {
            state->dsr_always = command[2] == '0';
            if (state->control != NULL) {
                if (state->dsr_always) {
                    state->control->flags |= MODEM_CONTROL_DSR_ALWAYS;
                    state->control->bits |= TIOCM_DSR;
                } else {
                    state->control->flags &= ~MODEM_CONTROL_DSR_ALWAYS;
                    if (!current_dtr(state)) {
                        state->control->bits &= ~TIOCM_DSR;
                    }
                }
                state->control->generation++;
            }
            command += 3;
        } else if (strncmp(command, "S0=", 3) == 0) {
            char *end = NULL;
            long value = strtol(command + 3, &end, 10);
            if (end != NULL && *end == '\0' && value >= 0 && value <= 255) {
                state->s0_auto_answer_rings = (int)value;
            } else {
                command_error = true;
            }
            break;
        } else if (strcmp(command, "S0?") == 0) {
            show_s0 = true;
            break;
        } else if (command[0] == 'O' && command[1] == '\0') {
            if (state->child.running) {
                enter_data_mode(state);
            } else {
                report_no_carrier(state);
            }
            terminal_response = true;
            break;
        } else if (command[0] == 'A' && command[1] == '\0') {
            answer_call(state);
            terminal_response = true;
            break;
        } else if (strncmp(command, "H", 1) == 0 && (command[1] == '\0' || (command[1] == '0' && command[2] == '\0'))) {
            if (state->incoming_call && !state->child.running) {
                reject_incoming_call(state);
            } else {
                hangup(state);
            }
            terminal_response = true;
            break;
        } else {
            command_error = true;
            break;
        }
    }

    if (terminal_response) {
        return;
    }
    if (command_error) {
        result_error(state);
    } else if (show_identification) {
        modem_write("stdio modem simulator\r\n");
        result_ok(state);
    } else if (show_s0) {
        char response[32];
        snprintf(response, sizeof(response), "%03d\r\n", state->s0_auto_answer_rings);
        modem_write(response);
        result_ok(state);
    } else {
        result_ok(state);
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

static void handle_requested_incoming_call(struct modem_state *state)
{
    if (!incoming_call_requested) {
        return;
    }

    incoming_call_requested = 0;
    if (state->mode == MODE_COMMAND) {
        start_incoming_call(state);
    }
}

static bool is_command_option(const char *argument)
{
    return strcmp(argument, "--incoming-call") == 0 || strcmp(argument, "--dial-command") == 0 ||
           strcmp(argument, "--incoming-command") == 0 || strcmp(argument, "--dsr-always-on") == 0 ||
           strcmp(argument, "--") == 0;
}

static char **parse_command_argument(int argc, char **argv, int *command_index)
{
    if (*command_index >= argc) {
        return NULL;
    }

    int start = *command_index;
    while (*command_index < argc && !is_command_option(argv[*command_index])) {
        (*command_index)++;
    }

    int count = *command_index - start;
    if (count == 0) {
        return NULL;
    }

    char **command = calloc((size_t)count + 1, sizeof(*command));
    if (command == NULL) {
        return NULL;
    }

    memcpy(command, &argv[start], (size_t)count * sizeof(*command));
    return command;
}

int main(int argc, char **argv)
{
    char *default_command[] = {"/bin/cat", NULL};
    char **dial_command = default_command;
    char **incoming_command = default_command;
    bool incoming_call = false;
    bool dsr_always_on = false;
    int command_index = 1;

    while (command_index < argc) {
        if (strcmp(argv[command_index], "--incoming-call") == 0) {
            incoming_call = true;
            command_index++;
        } else if (strcmp(argv[command_index], "--dsr-always-on") == 0) {
            dsr_always_on = true;
            command_index++;
        } else if (strcmp(argv[command_index], "--dial-command") == 0) {
            command_index++;
            dial_command = parse_command_argument(argc, argv, &command_index);
            if (dial_command == NULL) {
                return 2;
            }
        } else if (strcmp(argv[command_index], "--incoming-command") == 0) {
            command_index++;
            incoming_command = parse_command_argument(argc, argv, &command_index);
            if (incoming_command == NULL) {
                return 2;
            }
        } else if (strcmp(argv[command_index], "--") == 0) {
            command_index++;
            break;
        } else {
            break;
        }
    }

    if (command_index < argc) {
        dial_command = &argv[command_index];
        incoming_command = &argv[command_index];
    }

    struct modem_state state = {
        .echo = true,
        .verbose = true,
        .dsr_always = dsr_always_on,
        .mode = MODE_COMMAND,
        .dtr_policy = DTR_HANGUP,
        .dial_command = dial_command,
        .incoming_command = incoming_command,
        .dtr = true,
        .child = {
            .pid = -1,
            .stdin_fd = -1,
            .stdout_fd = -1,
        },
    };

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, request_incoming_call);
    modem_set_nonblock(STDIN_FILENO);
    state.control = open_control_file_from_env();
    state.dtr = current_dtr(&state);
    if (state.control != NULL && state.dsr_always) {
        state.control->flags |= MODEM_CONTROL_DSR_ALWAYS;
        state.control->bits |= TIOCM_DSR;
        state.control->generation++;
    }
    if (incoming_call) {
        start_incoming_call(&state);
    }

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

        handle_requested_incoming_call(&state);

        if (reap_child_if_dead(&state.child)) {
            report_no_carrier(&state);
        }

        refresh_dtr(&state);
        handle_ringing(&state);

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