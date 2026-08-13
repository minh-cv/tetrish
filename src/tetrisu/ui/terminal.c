#include "ui/terminal.h"

#include "tui.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void set_status(TerminalUi* ui, const char* status) {
    (void)snprintf(ui->status, sizeof(ui->status), "%s", status);
    ui->dirty = true;
}

void ui_command_list_init(UiCommandList* list) {
    memset(list, 0, sizeof(*list));
}

void ui_command_list_free(UiCommandList* list) {
    for (size_t i = 0; i < list->count; ++i) {
        parsed_command_free(&list->items[i]);
    }
    memset(list, 0, sizeof(*list));
}

int terminal_ui_init(TerminalUi* ui) {
    memset(ui, 0, sizeof(*ui));
    struct winsize terminal_size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal_size) == -1 ||
        terminal_size.ws_col == 0 || terminal_size.ws_row == 0) {
        return -1;
    }
    const TgSizei size = {
        .w = (int32_t)terminal_size.ws_col,
        .h = (int32_t)terminal_size.ws_row,
    };
    if (tui_init(size) != TG_OK) {
        return -1;
    }
    ui->initialized = true;
    ui->dirty = true;
    return 0;
}

void terminal_ui_free(TerminalUi* ui) {
    if (ui->initialized) {
        tui_shutdown();
    }
    memset(ui, 0, sizeof(*ui));
}

static void delete_before_cursor(TerminalUi* ui) {
    if (ui->cursor == 0) {
        return;
    }
    memmove(
        ui->line + ui->cursor - 1,
        ui->line + ui->cursor,
        ui->length - ui->cursor + 1
    );
    --ui->cursor;
    --ui->length;
    ui->dirty = true;
}

static void delete_at_cursor(TerminalUi* ui) {
    if (ui->cursor == ui->length) {
        return;
    }
    memmove(
        ui->line + ui->cursor,
        ui->line + ui->cursor + 1,
        ui->length - ui->cursor
    );
    --ui->length;
    ui->dirty = true;
}

static void insert_character(TerminalUi* ui, unsigned char ch) {
    if (ui->length == COMMAND_MAX_BYTES) {
        set_status(ui, "Command is at the 4096-byte limit");
        return;
    }
    ui->status[0] = '\0';
    memmove(
        ui->line + ui->cursor + 1,
        ui->line + ui->cursor,
        ui->length - ui->cursor + 1
    );
    ui->line[ui->cursor++] = (char)ch;
    ++ui->length;
    ui->dirty = true;
}

static const char* parse_error_text(CommandParseResult result) {
    switch (result) {
    case COMMAND_PARSE_EMPTY: return "Empty command";
    case COMMAND_PARSE_TOO_LONG: return "Command is too long";
    case COMMAND_PARSE_TOO_MANY_ARGS: return "Too many command arguments";
    case COMMAND_PARSE_UNTERMINATED_QUOTE: return "Unterminated quote";
    case COMMAND_PARSE_BAD_ESCAPE: return "Invalid double-quote escape";
    case COMMAND_PARSE_NOMEM: return "Cannot allocate parsed command";
    case COMMAND_PARSE_OK: break;
    }
    return "Cannot parse command";
}

static const char* route_error_text(CommandRouteResult result) {
    switch (result) {
    case COMMAND_ROUTE_UNKNOWN: return "Unknown command; type help";
    case COMMAND_ROUTE_MISSING_ARGUMENT: return "This command requires an argument";
    case COMMAND_ROUTE_NOMEM: return "Cannot allocate command";
    case COMMAND_ROUTE_OK: break;
    }
    return "Cannot route command";
}

static void submit_line(TerminalUi* ui, UiCommandList* commands) {
    if (commands->count == UI_COMMAND_LIST_CAPACITY) {
        set_status(ui, "Too many commands in one input frame");
        return;
    }
    CommandArgv argv;
    memset(&argv, 0, sizeof(argv));
    const CommandParseResult parsed = command_argv_parse(ui->line, ui->length, &argv);
    if (parsed != COMMAND_PARSE_OK) {
        set_status(ui, parse_error_text(parsed));
    } else {
        ParsedCommand command;
        memset(&command, 0, sizeof(command));
        const CommandRouteResult routed = command_route(&argv, &command);
        if (routed == COMMAND_ROUTE_OK) {
            commands->items[commands->count++] = command;
            ui->status[0] = '\0';
        } else {
            set_status(ui, route_error_text(routed));
        }
        command_argv_free(&argv);
    }
    memset(ui->line, 0, sizeof(ui->line));
    ui->length = 0;
    ui->cursor = 0;
    ui->dirty = true;
}

static void append_quit(UiCommandList* commands) {
    if (commands->count == UI_COMMAND_LIST_CAPACITY) {
        return;
    }
    ParsedCommand command;
    memset(&command, 0, sizeof(command));
    command.type = COMMAND_QUIT;
    commands->items[commands->count++] = command;
}

int terminal_ui_poll_input(TerminalUi* ui) {
    if (tui_poll_events() != TG_OK) {
        return -1;
    }
    tui_stdin_clear();
    const size_t dropped = tui_input_events_dropped();
    if (dropped > ui->dropped_events_seen) {
        ui->dropped_events_seen = dropped;
        set_status(ui, "Input events were dropped");
    }
    return 0;
}

void terminal_ui_update(TerminalUi* ui, UiCommandList* commands) {
    const TuiInputEvent* events = tui_input_events();
    const size_t count = tui_input_event_count();
    for (size_t i = 0; i < count; ++i) {
        const TuiInputEvent* event = &events[i];
        if (event->type == TUI_INPUT_TEXT) {
            insert_character(ui, event->ch);
            continue;
        }
        if (event->type != TUI_INPUT_KEY) {
            continue;
        }
        if ((event->modifiers & TUI_MOD_CONTROL) != 0 && event->ch == 'c') {
            append_quit(commands);
        } else if (event->key == TUI_KEY_ENTER) {
            submit_line(ui, commands);
        } else if (event->key == TUI_KEY_BACKSPACE) {
            delete_before_cursor(ui);
        } else if (event->key == TUI_KEY_DELETE) {
            delete_at_cursor(ui);
        } else if (event->key == TUI_KEY_LEFT && ui->cursor > 0) {
            --ui->cursor;
            ui->dirty = true;
        } else if (event->key == TUI_KEY_RIGHT && ui->cursor < ui->length) {
            ++ui->cursor;
            ui->dirty = true;
        } else if (event->key == TUI_KEY_HOME) {
            ui->cursor = 0;
            ui->dirty = true;
        } else if (event->key == TUI_KEY_END) {
            ui->cursor = ui->length;
            ui->dirty = true;
        }
    }
}

static void put_character(int x, int y, char ch, uint16_t style, uint32_t color) {
    if (x < 0 || y < 0 || x >= tui_width() || y >= tui_height()) {
        return;
    }
    TuiCell* cell = &tui_get_buffer()[(size_t)y * (size_t)tui_width() + (size_t)x];
    memset(cell->ch, 0, sizeof(cell->ch));
    cell->ch[0] = ch;
    cell->width = 1;
    cell->fg = color;
    cell->bg = TUI_COLOR_DEFAULT;
    cell->style = style;
}

static void put_text(int x, int y, const char* text, uint16_t style, uint32_t color) {
    for (int offset = 0; text[offset] != '\0' && x + offset < tui_width(); ++offset) {
        put_character(x + offset, y, text[offset], style, color);
    }
}

static void put_bytes(int x, int y, int max_y, const OwnedBytes* bytes) {
    int current_x = x;
    int current_y = y;
    for (size_t i = 0; i < bytes->len && current_y < max_y; ++i) {
        const unsigned char byte = bytes->ptr[i];
        if (byte == '\n' || current_x >= tui_width()) {
            ++current_y;
            current_x = x;
            if (byte == '\n') {
                continue;
            }
        }
        const char shown = byte >= 32 && byte < 127 ? (char)byte : '.';
        put_character(current_x++, current_y, shown, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
    }
}

static const char* connection_text(AppConnectionState state) {
    switch (state) {
    case APP_CONNECTION_DISCONNECTED: return "disconnected";
    case APP_CONNECTION_CONNECTING: return "connecting";
    case APP_CONNECTION_HANDSHAKING: return "handshaking";
    case APP_CONNECTION_READY: return "ready";
    }
    return "unknown";
}

static const char* request_text(AppRequestState state) {
    switch (state) {
    case APP_REQUEST_IDLE: return "idle";
    case APP_REQUEST_SUBMITTING: return "submitting";
    case APP_REQUEST_PENDING: return "pending";
    }
    return "unknown";
}

void terminal_ui_draw(TerminalUi* ui, const AppView* view) {
    tui_clear();
    put_text(0, 0, "tetrisu - non-blocking client [Ctrl-C quit]", TUI_STYLE_BOLD, 0x70C0FFu);

    char state_line[128];
    (void)snprintf(
        state_line,
        sizeof(state_line),
        "connection: %s    request: %s",
        connection_text(view->connection),
        request_text(view->request)
    );
    put_text(0, 1, state_line, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);

    const int input_y = tui_height() - 1;
    const int status_y = tui_height() - 2;
    const int split_y = tui_height() > 12 ? tui_height() / 2 : 6;
    put_text(0, 3, "STATE push", TUI_STYLE_BOLD, 0x80E080u);
    put_bytes(0, 4, split_y, view->remote_state);
    put_text(0, split_y, "Last reply / legacy echo", TUI_STYLE_BOLD, 0xE0C080u);
    put_bytes(0, split_y + 1, status_y, view->last_message);

    const char* status = ui->status[0] != '\0' ? ui->status : view->notification;
    put_text(0, status_y, status, TUI_STYLE_DIM, TUI_COLOR_DEFAULT);
    put_text(0, input_y, "> ", TUI_STYLE_BOLD, 0x70C0FFu);

    const int available = tui_width() > 2 ? tui_width() - 2 : 0;
    size_t first = 0;
    if ((int)ui->cursor >= available && available > 0) {
        first = ui->cursor - (size_t)available + 1;
    }
    for (size_t i = first; i < ui->length && (int)(i - first) < available; ++i) {
        put_character(2 + (int)(i - first), input_y, ui->line[i], TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
    }
    if (available > 0) {
        const int cursor_x = 2 + (int)(ui->cursor - first);
        const char cursor_char = ui->cursor < ui->length ? ui->line[ui->cursor] : ' ';
        put_character(cursor_x, input_y, cursor_char, TUI_STYLE_REVERSE, TUI_COLOR_DEFAULT);
    }
    ui->dirty = false;
}

int terminal_ui_present(TerminalUi* ui) {
    (void)ui;
    return tui_present() == TG_OK ? 0 : -1;
}
