#include "app.h"
#include "config_var.h"
#include "net/client.h"
#include "ui/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) == -1) {
        return 0;
    }
    return (uint64_t)time.tv_sec * 1000u + (uint64_t)time.tv_nsec / 1000000u;
}

typedef struct {
    AppState* app;
    NetClient* net;
    bool* running;
} Runtime;

static int dispatch_app_event(Runtime* runtime, const AppEvent* event, uint64_t now_ms);

static int dispatch_net_events(Runtime* runtime, NetEventList* events, uint64_t now_ms) {
    for (size_t i = 0; i < events->count; ++i) {
        const AppEvent event = {
            .type = APP_EVENT_NETWORK,
            .data.network = &events->items[i],
        };
        if (dispatch_app_event(runtime, &event, now_ms) == -1) {
            return -1;
        }
    }
    return 0;
}

static int execute_effect(
    Runtime* runtime,
    const AppEffect* effect,
    uint64_t now_ms
) {
    if (effect->type == APP_EFFECT_QUIT) {
        *runtime->running = false;
        return 0;
    }

    NetEventList events;
    net_event_list_init(&events);
    int result = 0;
    switch (effect->type) {
    case APP_EFFECT_NET_CONNECT:
        result = net_client_connect(runtime->net, now_ms, &events);
        break;
    case APP_EFFECT_NET_SEND:
    {
        const ClientRequest request = {
            .method = effect->method,
            .path = effect->path,
            .body = effect->payload.ptr,
            .body_len = effect->payload.len,
            .content_type = effect->content_type,
            .completion = effect->completion,
        };
        result = net_client_send_request(runtime->net, &request, now_ms, &events);
        break;
    }
    case APP_EFFECT_NET_DISCONNECT:
        result = net_client_disconnect(runtime->net, &events);
        break;
    case APP_EFFECT_QUIT:
        break;
    }
    if (result == 0) {
        result = dispatch_net_events(runtime, &events, now_ms);
    }
    net_event_list_free(&events);
    return result;
}

static int dispatch_app_event(Runtime* runtime, const AppEvent* event, uint64_t now_ms) {
    AppEffectList effects;
    app_effect_list_init(&effects);
    int result = app_reduce(runtime->app, event, &effects);
    for (size_t i = 0; result == 0 && i < effects.count; ++i) {
        result = execute_effect(runtime, &effects.items[i], now_ms);
    }
    app_effect_list_free(&effects);
    return result;
}

static int handle_network_poll(
    Runtime* runtime,
    short revents,
    uint64_t now_ms
) {
    NetEventList events;
    net_event_list_init(&events);
    int result = net_client_on_poll(runtime->net, revents, now_ms, &events);
    if (result == 0) {
        result = dispatch_net_events(runtime, &events, now_ms);
    }
    net_event_list_free(&events);
    return result;
}

static int handle_network_timeout(Runtime* runtime, uint64_t now_ms) {
    NetEventList events;
    net_event_list_init(&events);
    int result = net_client_on_timeout(runtime->net, now_ms, &events);
    if (result == 0) {
        result = dispatch_net_events(runtime, &events, now_ms);
    }
    net_event_list_free(&events);
    return result;
}

// Terminal state captured before the TUI takes over, so a fatal signal can put
// the terminal back. tui_shutdown() cannot be reused here: it writes through
// stdio, which is not async-signal-safe, and a crash mid-render would deadlock
// on stdout's lock instead of restoring anything.
static struct termios saved_termios;
static int saved_stdin_flags = -1;
static volatile sig_atomic_t terminal_captured = 0;

static void capture_terminal_state(void) {
    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) {
        return;
    }
    saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (saved_stdin_flags == -1) {
        return;
    }
    terminal_captured = 1;
}

// Only write(2), tcsetattr(3) and fcntl(2) are used, all of which POSIX lists
// as async-signal-safe. The escape sequences mirror tui_shutdown's: disable the
// mouse reporting modes, reset SGR, show the cursor, leave the alternate screen.
static void restore_terminal_from_handler(void) {
    if (terminal_captured == 0) {
        return;
    }
    static const char restore[] =
        "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";
    (void)!write(STDOUT_FILENO, restore, sizeof(restore) - 1u);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    (void)fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags);
}

// Restore, then die of the original signal under its default disposition so the
// exit status and any core dump still reflect the real fault.
static void fatal_signal_handler(int signal_number) {
    restore_terminal_from_handler();
    (void)signal(signal_number, SIG_DFL);
    (void)raise(signal_number);
}

static int install_signal_handlers(void) {
    // SIGQUIT and SIGHUP join the graceful set: left at their defaults they kill
    // the process outright, and the raw-mode termios and O_NONBLOCK stdin are
    // properties of the shared file description, so they would outlive it and
    // strand the user's shell.
    static const int graceful[] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP };
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(graceful) / sizeof(graceful[0]); ++i) {
        if (sigaction(graceful[i], &action, NULL) == -1) {
            return -1;
        }
    }

    // The terminal cannot be handed back on these, only put right on the way out
    static const int fatal[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
    struct sigaction fatal_action;
    memset(&fatal_action, 0, sizeof(fatal_action));
    fatal_action.sa_handler = fatal_signal_handler;
    sigemptyset(&fatal_action.sa_mask);
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i) {
        if (sigaction(fatal[i], &fatal_action, NULL) == -1) {
            return -1;
        }
    }
    return 0;
}

int main(void) {
    struct config_var config;
    if (config_var_init(&config) == -1) {
        return 1;
    }
    if (install_signal_handlers() == -1) {
        config_var_free(&config);
        return 1;
    }

    // must run before the TUI switches the terminal into raw mode, so what the
    // handlers restore is the shell's own state
    capture_terminal_state();

    TerminalUi ui;
    if (terminal_ui_init(&ui) == -1) {
        fprintf(stderr, "tetrisu requires an interactive terminal\n");
        config_var_free(&config);
        return 1;
    }

    AppState app;
    app_init(&app);
    NetClient net;
    net_client_init(&net, config.address, config.port, config.ca_path);
    bool running = true;
    Runtime runtime = {.app = &app, .net = &net, .running = &running};
    bool failed = false;

    const AppEvent start = {.type = APP_EVENT_START};
    if (dispatch_app_event(&runtime, &start, monotonic_ms()) == -1) {
        failed = true;
        running = false;
    }

    UiCommandList initial_commands;
    ui_command_list_init(&initial_commands);
    if (running && terminal_ui_poll_input(&ui) == -1) {
        failed = true;
        running = false;
    }
    if (running) {
        AppView view;
        app_build_view(&app, &view);
        terminal_ui_update(&ui, &view, &initial_commands);
    }
    ui_command_list_free(&initial_commands);
    if (running) {
        AppView view;
        app_build_view(&app, &view);
        terminal_ui_draw(&ui, &view);
        if (terminal_ui_present(&ui) == -1) {
            failed = true;
            running = false;
        }
        app.view_dirty = false;
    }

    while (running && !stop_requested) {
        struct pollfd descriptors[2];
        descriptors[0] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        nfds_t descriptor_count = 1;
        const int network_fd = net_client_fd(&net);
        if (network_fd >= 0) {
            descriptors[1] = (struct pollfd){
                .fd = network_fd,
                .events = net_client_poll_events(&net),
            };
            descriptor_count = 2;
        }

        const uint64_t before_poll = monotonic_ms();
        const int timeout_ms = net_client_timeout_ms(&net, before_poll);
        const int ready_count = poll(descriptors, descriptor_count, timeout_ms);
        const int poll_error = ready_count == -1 ? errno : 0;
        const uint64_t now_ms = monotonic_ms();
        if (ready_count == -1 && poll_error != EINTR) {
            failed = true;
            break;
        }
        if (stop_requested) {
            break;
        }
        if (ready_count > 0 &&
            (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if (terminal_ui_poll_input(&ui) == -1) {
            failed = true;
            break;
        }

        if (ready_count > 0 && descriptor_count == 2 && descriptors[1].revents != 0 &&
            handle_network_poll(&runtime, descriptors[1].revents, now_ms) == -1) {
            failed = true;
            break;
        }
        if (handle_network_timeout(&runtime, now_ms) == -1) {
            failed = true;
            break;
        }

        UiCommandList commands;
        ui_command_list_init(&commands);
        AppView input_view;
        app_build_view(&app, &input_view);
        terminal_ui_update(&ui, &input_view, &commands);
        for (size_t i = 0; running && i < commands.count; ++i) {
            const AppEvent event = {
                .type = APP_EVENT_COMMAND_SUBMITTED,
                .data.command = &commands.items[i],
            };
            if (dispatch_app_event(&runtime, &event, now_ms) == -1) {
                failed = true;
                running = false;
            }
        }
        ui_command_list_free(&commands);

        if (running && (app.view_dirty || ui.dirty)) {
            AppView view;
            app_build_view(&app, &view);
            terminal_ui_draw(&ui, &view);
            if (terminal_ui_present(&ui) == -1) {
                failed = true;
                break;
            }
            app.view_dirty = false;
        }
    }

    net_client_free(&net);
    app_free(&app);
    terminal_ui_free(&ui);
    config_var_free(&config);
    if (failed) {
        fprintf(stderr, "tetrisu stopped after an internal terminal/client error\n");
    }
    return failed ? 1 : 0;
}
