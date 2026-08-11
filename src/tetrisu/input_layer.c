#include "input_layer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void event_free(InputEvent* event) {
    if (event->type == INPUT_EVENT_COMMAND) {
        cmdline_free(event->command.argv, event->command.argc);
        event->command.argc = 0;
    }
}

static int push(InputEventQueue* q, InputEvent* event) {
    if (InputEventQueue_push_back(q, event) == -1) {
        event_free(event);
        return -1;
    }
    return 0;
}

static int push_simple(InputEventQueue* q, InputEventType type, const char* diagnostic) {
    InputEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.diagnostic = diagnostic;
    return push(q, &event);
}

int InputData_init(InputData* data, size_t line_capacity) {
    data->line_buf = malloc(line_capacity);
    if (data->line_buf == NULL) {
        return -1;
    }
    data->mode = INPUT_MODE_LINE;
    data->line_used = 0;
    data->line_capacity = line_capacity;
    data->line_overflowed = false;
    data->eof = false;
    data->stdin_nonblocking = false;
    data->saved_stdin_flags = 0;

    /*
        Line mode reads until EAGAIN rather than once per readable event, so
        stdin has to be non-blocking; the original flags are restored at free
        because the descriptor outlives the process's use of it.
    */
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != -1) {
        data->saved_stdin_flags = flags;
        data->stdin_nonblocking = true;
    }
    return 0;
}

void InputData_free(InputData* data) {
    if (data->stdin_nonblocking) {
        fcntl(STDIN_FILENO, F_SETFL, data->saved_stdin_flags);
        data->stdin_nonblocking = false;
    }
    free(data->line_buf);
    data->line_buf = NULL;
}

void InputData_reset(InputData* data, InputEventQueue* m_event_q) {
    (void)data;
    const size_t count = InputEventQueue_size(m_event_q);
    for (size_t i = 0; i < count; i++) {
        event_free(InputEventQueue_front(m_event_q));
        InputEventQueue_pop_front(m_event_q);
    }
}

static int emit_line(InputData* data, InputEventQueue* m_event_q) {
    data->line_buf[data->line_used] = '\0';
    data->line_used = 0;

    InputEvent event;
    memset(&event, 0, sizeof(event));
    event.type = INPUT_EVENT_COMMAND;

    const CmdlineStatus status = cmdline_split(data->line_buf, event.command.argv, &event.command.argc);
    if (status != CMDLINE_OK) {
        return push_simple(m_event_q, INPUT_EVENT_DIAGNOSTIC, cmdline_status_string(status));
    }
    if (event.command.argc == 0) {
        return 0;
    }
    return push(m_event_q, &event);
}

static int consume_line_bytes(InputData* data, const char* buf, size_t len, InputEventQueue* m_event_q) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') {
            if (data->line_used + 1 >= data->line_capacity) {
                // drop the whole line rather than half of it, so a truncated
                // command can never be run by accident
                data->line_overflowed = true;
                data->line_used = 0;
                continue;
            }
            data->line_buf[data->line_used++] = buf[i];
            continue;
        }

        if (data->line_overflowed) {
            data->line_overflowed = false;
            data->line_used = 0;
            if (push_simple(m_event_q, INPUT_EVENT_DIAGNOSTIC, "line too long, discarded") == -1) {
                return -1;
            }
            continue;
        }
        if (emit_line(data, m_event_q) == -1) {
            return -1;
        }
    }
    return 0;
}

static void read_line_mode(InputData* data, InputEventQueue* m_event_q, ClientFault* fault) {
    for (;;) {
        char buf[1024];
        const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));

        if (n == 0) {
            data->eof = true;
            if (push_simple(m_event_q, INPUT_EVENT_EOF, NULL) == -1) {
                *fault = FAULT_LOCAL;
            }
            return;
        }
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("read stdin");
            *fault = FAULT_LOCAL;
            return;
        }

        if (consume_line_bytes(data, buf, (size_t)n, m_event_q) == -1) {
            // the queue is full: the rest of this read is dropped, which is
            // preferable to blocking the tick on a burst of pasted input
            return;
        }
    }
}

static void read_raw_mode(InputData* data, InputEventQueue* m_event_q, ClientFault* fault) {
    (void)data;
    if (tg_result_err(tui_poll_events())) {
        *fault = FAULT_LOCAL;
        return;
    }

    const size_t count = tui_input_event_count();
    const TuiInputEvent* const events = tui_input_events();
    for (size_t i = 0; i < count; i++) {
        if (events[i].type == TUI_INPUT_MOUSE) {
            continue;
        }

        InputEvent event;
        memset(&event, 0, sizeof(event));
        event.type = INPUT_EVENT_KEY;
        event.key.key = events[i].key;
        event.key.ch = events[i].ch;
        event.key.modifiers = events[i].modifiers;
        if (push(m_event_q, &event) == -1) {
            return;
        }
    }
}

void InputData_read(InputData* data, InputEventQueue* m_event_q, ClientFault* fault) {
    if (*fault != FAULT_NONE || data->eof) {
        return;
    }
    if (data->mode == INPUT_MODE_LINE) {
        read_line_mode(data, m_event_q, fault);
    }
    else {
        read_raw_mode(data, m_event_q, fault);
    }
}

int InputData_set_mode(InputData* data, InputMode mode) {
    if (data->mode == mode) {
        return 0;
    }

    // bytes buffered under the old discipline would be reinterpreted under the
    // new one, so both directions start from nothing
    data->line_used = 0;
    data->line_overflowed = false;
    if (mode == INPUT_MODE_RAW) {
        tui_stdin_clear();
    }

    data->mode = mode;
    return 0;
}
