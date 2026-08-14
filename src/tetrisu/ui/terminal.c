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
    ui->command_mode = true;
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
    case COMMAND_ROUTE_INVALID_ARGUMENT: return "Invalid command argument";
    case COMMAND_ROUTE_TOO_MANY_ARGUMENTS: return "Too many command arguments";
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

static void append_game_intent(UiCommandList* commands, GameIntentType intent) {
    if (commands->count == UI_COMMAND_LIST_CAPACITY) {
        return;
    }
    ParsedCommand command;
    memset(&command, 0, sizeof(command));
    command.type = COMMAND_GAME;
    command.game_intent = intent;
    commands->items[commands->count++] = command;
}

static bool append_game_key(const TuiInputEvent* event, UiCommandList* commands) {
    if (event->key == TUI_KEY_LEFT || event->ch == 'a') {
        append_game_intent(commands, GAME_INTENT_MOVE_LEFT);
    }
    else if (event->key == TUI_KEY_RIGHT || event->ch == 'd') {
        append_game_intent(commands, GAME_INTENT_MOVE_RIGHT);
    }
    else if (event->key == TUI_KEY_DOWN || event->ch == 's') {
        append_game_intent(commands, GAME_INTENT_DROP_SOFT);
    }
    else if (event->key == TUI_KEY_UP || event->ch == 'x') {
        append_game_intent(commands, GAME_INTENT_ROTATE_CW);
    }
    else if (event->ch == 'z') {
        append_game_intent(commands, GAME_INTENT_ROTATE_CCW);
    }
    else if (event->key == TUI_KEY_SPACE) {
        append_game_intent(commands, GAME_INTENT_DROP_HARD);
    }
    else if (event->ch == 'c') {
        append_game_intent(commands, GAME_INTENT_HOLD);
    }
    else {
        return false;
    }
    return true;
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

void terminal_ui_update(
    TerminalUi* ui,
    const AppView* view,
    UiCommandList* commands
) {
    const TuiInputEvent* events = tui_input_events();
    const size_t count = tui_input_event_count();
    for (size_t i = 0; i < count; ++i) {
        const TuiInputEvent* event = &events[i];
        if (event->type == TUI_INPUT_KEY &&
            (event->modifiers & TUI_MOD_CONTROL) != 0 && event->ch == 'c') {
            append_quit(commands);
            continue;
        }
        const bool game_mode = view->game_phase == APP_GAME_ACTIVE && !ui->command_mode;
        if (game_mode && event->type == TUI_INPUT_TEXT && event->ch == ':') {
            ui->command_mode = true;
            ui->dirty = true;
            continue;
        }
        if (game_mode &&
            (event->type == TUI_INPUT_KEY || event->type == TUI_INPUT_TEXT) &&
            append_game_key(event, commands)) {
            continue;
        }
        /* Space is reported once as a key and once as text by tui.h. */
        if (game_mode && event->type == TUI_INPUT_TEXT && event->ch == ' ') {
            continue;
        }
        if (event->type == TUI_INPUT_TEXT) {
            insert_character(ui, event->ch);
            continue;
        }
        if (event->type != TUI_INPUT_KEY) {
            continue;
        }
        if (event->key == TUI_KEY_ESCAPE && view->game_phase == APP_GAME_ACTIVE) {
            ui->command_mode = !ui->command_mode;
            ui->dirty = true;
        } else if (event->key == TUI_KEY_ENTER) {
            submit_line(ui, commands);
            ui->command_mode = false;
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

/*!
    @brief write one cell as a whole glyph, which may be multi-byte UTF-8

    The board paints with backgrounds rather than letters, and its ghost is
    U+2591, so a cell is a string here rather than the single @c char the
    text helpers pass down.
*/
static void put_glyph(
    int x, int y, const char* glyph, uint16_t style, uint32_t fg, uint32_t bg
) {
    if (x < 0 || y < 0 || x >= tui_width() || y >= tui_height()) {
        return;
    }
    TuiCell* cell = &tui_get_buffer()[(size_t)y * (size_t)tui_width() + (size_t)x];
    memset(cell->ch, 0, sizeof(cell->ch));
    const size_t length = strlen(glyph);
    const size_t copied = length < sizeof(cell->ch) ? length : sizeof(cell->ch) - 1;
    memcpy(cell->ch, glyph, copied);
    // one column wide whatever the encoding: every glyph drawn here is either
    // ASCII or a block element, and the board's geometry depends on it
    cell->width = 1;
    cell->fg = fg;
    cell->bg = bg;
    cell->style = style;
}

static void put_character(int x, int y, char ch, uint16_t style, uint32_t color) {
    const char glyph[2] = {ch, '\0'};
    put_glyph(x, y, glyph, style, color, TUI_COLOR_DEFAULT);
}

static void put_text(int x, int y, const char* text, uint16_t style, uint32_t color) {
    for (int offset = 0; text[offset] != '\0' && x + offset < tui_width(); ++offset) {
        put_character(x + offset, y, text[offset], style, color);
    }
}

//! @brief @return the row after the last one written, so callers can stack
static int put_bytes(int x, int y, int max_y, const OwnedBytes* bytes) {
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
    return current_y + 1;
}

static uint32_t cell_color(TetrominoCellType cell) {
    static const uint32_t colors[TETROMINO_CELL_COUNT] = {
        [TETROMINO_CELL_I] = 0x40D8E8u,
        [TETROMINO_CELL_J] = 0x4080E8u,
        [TETROMINO_CELL_L] = 0xE89030u,
        [TETROMINO_CELL_O] = 0xE8D840u,
        [TETROMINO_CELL_S] = 0x50C860u,
        [TETROMINO_CELL_T] = 0xA860D8u,
        [TETROMINO_CELL_Z] = 0xE85050u,
        [TETROMINO_CELL_EMPTY] = 0x383838u,
        [TETROMINO_CELL_GARBAGE] = 0x909090u,
    };
    return cell >= 0 && cell < TETROMINO_CELL_COUNT
        ? colors[cell]
        : TUI_COLOR_DEFAULT;
}

static bool piece_occupies(
    const Tetromino* piece,
    int board_row,
    int board_col
) {
    const int local_row = board_row - piece->row_offset;
    const int local_col = board_col - piece->col_offset;
    const int box = TETROMINO_BOX_SIZE[piece->type];
    return local_row >= 0 && local_row < box && local_col >= 0 && local_col < box &&
        IS_TETROMINO_CELL_OCCUPIED[piece->type](
            piece->type, piece->direction, local_row, local_col
        );
}

//! @brief U+2591 LIGHT SHADE, the ghost's texture
#define GHOST_GLYPH "\xe2\x96\x91"

//! @brief the well's floor, dark enough that a piece colour reads against it
#define BOARD_EMPTY_BG 0x1C1C1Cu

/*!
    @brief paint one board cell, which is two columns wide

    Two columns because a terminal cell is about twice as tall as it is wide,
    so a one-column square reads as a rectangle and the well looks squashed.
*/
static void put_block(int x, int y, const char* glyph, uint32_t fg, uint32_t bg) {
    put_glyph(x, y, glyph, TUI_STYLE_NONE, fg, bg);
    put_glyph(x + 1, y, glyph, TUI_STYLE_NONE, fg, bg);
}

static void draw_board(const ProtoStateRequest* state, int x, int y) {
    const BoardState* board = &state->board_state;
    Tetromino ghost = board->tetromino;
    ghost.col_offset = board->ghost.ghost_col_offset;
    ghost.row_offset = board->ghost.ghost_row_offset;

    for (int visible_row = 0; visible_row < 20; ++visible_row) {
        const int row = VISIBLE_ROW_IDX + visible_row;
        put_character(x, y + visible_row, '|', TUI_STYLE_DIM, 0x707070u);
        for (int col = 0; col < BOARD_WIDTH; ++col) {
            const TetrominoCellType locked = board->board.cells[row][col];
            const bool active = piece_occupies(&board->tetromino, row, col);
            const bool ghost_cell = !active && locked == TETROMINO_CELL_EMPTY &&
                piece_occupies(&ghost, row, col);
            const int cell_x = x + 1 + col * 2;

            // a filled cell is its colour, not a letter on its colour: the
            // shape is what the player reads, and glyphs break up the mass
            if (active) {
                put_block(cell_x, y + visible_row, " ", TUI_COLOR_DEFAULT,
                          cell_color((TetrominoCellType)board->tetromino.type));
            }
            else if (locked != TETROMINO_CELL_EMPTY) {
                put_block(cell_x, y + visible_row, " ", TUI_COLOR_DEFAULT, cell_color(locked));
            }
            else if (ghost_cell) {
                // textured in the piece's own colour, so where it will land is
                // obvious without competing with the piece itself
                put_block(cell_x, y + visible_row, GHOST_GLYPH,
                          cell_color((TetrominoCellType)board->tetromino.type), BOARD_EMPTY_BG);
            }
            else {
                put_block(cell_x, y + visible_row, " ", TUI_COLOR_DEFAULT, BOARD_EMPTY_BG);
            }
        }
        put_character(x + 21, y + visible_row, '|', TUI_STYLE_DIM, 0x707070u);
    }
}

/*!
    @brief draw @p type 's spawn shape with its top-left at (@p x , @p y )

    Rows above the piece are skipped rather than reserved, so an I piece
    (whose occupied row sits inside a 4x4 box) lines up with an S piece
    (3x3) instead of floating a row lower. The caller can therefore budget
    the same height for every preview slot.

    A type outside the tetromino range is the preview-cap sentinel and draws
    nothing, which is how a hidden next piece renders as an empty slot.
*/
static void draw_mini_piece(TetrominoType type, int x, int y) {
    if (type < 0 || type >= TETROMINO_TYPE_COUNT) {
        return;
    }
    const int box = TETROMINO_BOX_SIZE[type];

    int first_row = box;
    for (int i = 0; i < box && first_row == box; ++i) {
        for (int j = 0; j < box; ++j) {
            if (IS_TETROMINO_CELL_OCCUPIED[type](type, TETROMINO_DIRECTION_0, i, j)) {
                first_row = i;
                break;
            }
        }
    }

    for (int i = first_row; i < box; ++i) {
        for (int j = 0; j < box; ++j) {
            if (IS_TETROMINO_CELL_OCCUPIED[type](type, TETROMINO_DIRECTION_0, i, j)) {
                put_block(x + j * 2, y + (i - first_row), " ", TUI_COLOR_DEFAULT,
                          cell_color((TetrominoCellType)type));
            }
        }
    }
}

static TetrominoType next_piece(const BagState* bag, int offset) {
    const int index = bag->bag1_offset + offset;
    return index < TETROMINO_TYPE_COUNT
        ? bag->bag1[index]
        : bag->bag2[index - TETROMINO_TYPE_COUNT];
}

static void draw_game_sidebar(const ProtoStateRequest* state, int x, int y) {
    char line[80];
    (void)snprintf(line, sizeof(line), "score: %d", state->game_score);
    put_text(x, y, line, TUI_STYLE_BOLD, 0xE8D870u);
    (void)snprintf(line, sizeof(line), "combo: %d", state->combo_counter);
    put_text(x, y + 1, line, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
    (void)snprintf(line, sizeof(line), "b2b: %d", state->back_to_back_count);
    put_text(x, y + 2, line, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
    (void)snprintf(line, sizeof(line), "garbage: %d", state->garbage_balance);
    put_text(x, y + 3, line, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);

    // three rows per slot: every spawn orientation is one or two rows tall
    // once draw_mini_piece trims the empty ones, leaving a blank separator
    static const int SLOT_HEIGHT = 3;
    static const int PREVIEW_COUNT = 5;

    put_text(x, y + 5, "hold:", TUI_STYLE_BOLD, 0x80C0FFu);
    if (state->hold_state.hold_status == HOLD_EMPTY) {
        put_text(x + 2, y + 6, "--", TUI_STYLE_DIM, 0x606060u);
    }
    else {
        draw_mini_piece(state->hold_state.hold_type, x + 2, y + 6);
    }

    const int next_y = y + 6 + SLOT_HEIGHT;
    put_text(x, next_y, "next:", TUI_STYLE_BOLD, 0x80C0FFu);
    for (int i = 0; i < PREVIEW_COUNT; ++i) {
        draw_mini_piece(
            next_piece(&state->bag_state, i + 1), x + 2, next_y + 1 + i * SLOT_HEIGHT
        );
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

static const char* game_phase_text(AppGamePhase phase) {
    switch (phase) {
    case APP_GAME_NO_ROOM: return "no-room";
    case APP_GAME_LOBBY: return "lobby";
    case APP_GAME_ACTIVE: return "active";
    }
    return "unknown";
}

/*!
    @brief what to do next, given where the session actually is

    The command list below is fixed, so this is the only part that answers
    "and now what", which is the question someone staring at an empty client
    is usually asking.
*/
static const char* next_step_text(const AppView* view) {
    if (view->connection != APP_CONNECTION_READY) {
        return "waiting for the server; 'reconnect' to try again";
    }
    switch (view->game_phase) {
    case APP_GAME_NO_ROOM:
        return "'create' a room, or 'join <code>' one a friend gave you";
    case APP_GAME_LOBBY:
        return "'start' when everyone has joined; share the room code above";
    case APP_GAME_ACTIVE:
        return "playing";
    }
    return "";
}

/*!
    @brief the screen outside a game: what can be typed, and what it does

    Drawn wherever the board is not, so the client is self-describing rather
    than depending on the player having read the docs. Rows stop at @p max_y
    so the status and input lines are never overwritten.

    The last response sits between the two lists rather than after them. On a
    narrow terminal the lists stack, and something has to lose: a reply is
    what the player just asked for, while the key list is reference for keys
    that only work on the screen this one is replaced by. So the keys yield.
*/
static void draw_usage(int x, int y, int max_y, const AppView* view) {
    static const char* const COMMANDS[][2] = {
        {"create [seats] [public] [cross]", "open a room; bare create is solo"},
        {"join <room-id>", "enter the room with that code"},
        {"start", "begin the match"},
        {"leave", "leave the room"},
        {"rooms [page]", "list public rooms, twenty a page"},
        {"reconnect, disconnect", "restart or drop the session"},
        {"help, quit", "these commands; leave tetrisu"},
    };
    static const char* const GAME_KEYS[][2] = {
        {"left, a", "move left"},
        {"right, d", "move right"},
        {"down, s", "soft drop"},
        {"space", "hard drop"},
        {"up, x", "rotate cw"},
        {"z", "rotate ccw"},
        {"c", "hold"},
        {": or Esc", "command line"},
    };
    static const size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(*COMMANDS);
    static const size_t KEY_COUNT = sizeof(GAME_KEYS) / sizeof(*GAME_KEYS);
    // clear of the widest command, which is create's, indented by two
    static const int DESCRIPTION_X = 35;
    static const int KEY_DESCRIPTION_X = 12;
    // second column only where the first's descriptions cannot reach it
    static const int KEYS_X = 70;
    static const int KEYS_WIDTH = 22;

    put_text(x, y, next_step_text(view), TUI_STYLE_BOLD, 0x80E080u);

    int row = y + 2;
    put_text(x, row, "commands", TUI_STYLE_BOLD, 0x80C0FFu);
    for (size_t i = 0; i < COMMAND_COUNT && row + 1 + (int)i < max_y; ++i) {
        const int line = row + 1 + (int)i;
        put_text(x + 2, line, COMMANDS[i][0], TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
        put_text(x + DESCRIPTION_X, line, COMMANDS[i][1], TUI_STYLE_DIM, TUI_COLOR_DEFAULT);
    }

    int left_bottom = row + (int)COMMAND_COUNT + 1;
    if (view->last_message->len != 0 && left_bottom + 2 < max_y) {
        put_text(x, left_bottom + 1, "last response", TUI_STYLE_BOLD, 0xE0C080u);
        left_bottom = put_bytes(x, left_bottom + 2, max_y, view->last_message);
    }

    // in a narrow terminal the keys follow the left column instead of sitting
    // beside it; below it, they are the first thing to fall off the screen
    const bool side_by_side = tui_width() >= KEYS_X + KEYS_WIDTH;
    const int keys_x = side_by_side ? x + KEYS_X : x;
    int keys_y = side_by_side ? row : left_bottom + 1;

    put_text(keys_x, keys_y, "in game", TUI_STYLE_BOLD, 0x80C0FFu);
    for (size_t i = 0; i < KEY_COUNT && keys_y + 1 + (int)i < max_y; ++i) {
        const int line = keys_y + 1 + (int)i;
        put_text(keys_x + 2, line, GAME_KEYS[i][0], TUI_STYLE_NONE, TUI_COLOR_DEFAULT);
        put_text(keys_x + KEY_DESCRIPTION_X, line, GAME_KEYS[i][1], TUI_STYLE_DIM, TUI_COLOR_DEFAULT);
    }
}

void terminal_ui_draw(TerminalUi* ui, const AppView* view) {
    tui_clear();
    put_text(0, 0, "tetrisu - non-blocking client [Ctrl-C quit]", TUI_STYLE_BOLD, 0x70C0FFu);

    char state_line[160];
    char room[32] = "";
    if (view->has_room_id) {
        // the code a friend needs, kept on screen rather than only announced
        // once in the notification line that the next event overwrites
        (void)snprintf(room, sizeof(room), "    room: %zu", view->room_id);
    }
    (void)snprintf(
        state_line,
        sizeof(state_line),
        "connection: %s    request: %s    game: %s    queued: %zu%s",
        connection_text(view->connection),
        request_text(view->request),
        game_phase_text(view->game_phase),
        view->queued_inputs,
        room
    );
    put_text(0, 1, state_line, TUI_STYLE_NONE, TUI_COLOR_DEFAULT);

    const int input_y = tui_height() - 1;
    const int status_y = tui_height() - 2;
    if (view->has_game_state && tui_height() >= 24 && tui_width() >= 40) {
        draw_board(view->game_state, 0, 3);
        draw_game_sidebar(view->game_state, 24, 3);
        // only beside a board: out of a game the usage owns this space, and
        // the status line already carries what a response said
        if (view->last_message->len != 0 && tui_width() > 48) {
            put_text(48, 3, "Last response body", TUI_STYLE_BOLD, 0xE0C080u);
            put_bytes(48, 4, status_y, view->last_message);
        }
    }
    else {
        draw_usage(0, 3, status_y, view);
    }

    const char* status = ui->status[0] != '\0' ? ui->status : view->notification;
    put_text(0, status_y, status, TUI_STYLE_DIM, TUI_COLOR_DEFAULT);
    const bool show_prompt = ui->command_mode || view->game_phase != APP_GAME_ACTIVE;
    put_text(
        0, input_y,
        show_prompt ? "> " : "[game mode; ':' or Esc for commands]",
        TUI_STYLE_BOLD, 0x70C0FFu
    );

    const int available = show_prompt && tui_width() > 2 ? tui_width() - 2 : 0;
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
