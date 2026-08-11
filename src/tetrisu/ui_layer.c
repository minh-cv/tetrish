#include "ui_layer.h"
#include "tuiui.h"
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

// two terminal columns per board cell, so a cell reads as a square
#define CELL_WIDTH 2
#define BOARD_ORIGIN_X 2
#define BOARD_ORIGIN_Y 1
#define SIDEBAR_X (BOARD_ORIGIN_X + CELL_WIDTH * GAME_VIEW_COLS + 3)

#define TUI_FALLBACK_WIDTH 80
#define TUI_FALLBACK_HEIGHT 28

static const uint32_t TYPE_COLOR[TETROMINO_TYPE_COUNT] = {
    [TETROMINO_TYPE_I] = 0x00CFE8u,
    [TETROMINO_TYPE_J] = 0x3B6FE0u,
    [TETROMINO_TYPE_L] = 0xE08A2Bu,
    [TETROMINO_TYPE_O] = 0xE3D33Au,
    [TETROMINO_TYPE_S] = 0x4FCB5Au,
    [TETROMINO_TYPE_T] = 0xB05CE0u,
    [TETROMINO_TYPE_Z] = 0xE04B4Bu,
};

static const uint32_t COLOR_EMPTY = 0x141821u;
static const uint32_t COLOR_GARBAGE = 0x8A8F9Au;
static const uint32_t COLOR_TEXT = 0xD7DAE0u;
static const uint32_t COLOR_MUTED = 0x7E8490u;
static const uint32_t COLOR_FRAME = 0x39404Eu;

static TuiUiContext g_ui;

int UiData_init(UiData* data) {
    data->backend = UI_BACKEND_LINE;
    data->frame_timerfd = -1;
    data->tui_up = false;
    data->prompt_shown = false;
    tuiui_init(&g_ui);
    return 0;
}

static void tui_teardown(UiData* data) {
    if (data->tui_up) {
        tui_shutdown();
        data->tui_up = false;
    }
    if (data->frame_timerfd != -1) {
        close(data->frame_timerfd);
        data->frame_timerfd = -1;
    }
}

void UiData_free(UiData* data) {
    tui_teardown(data);
}

/* ---- line backend ---- */

static void render_line(UiData* data, ViewModel* m_view) {
    bool wrote = false;
    while (!TranscriptQueue_empty(&m_view->transcript)) {
        TranscriptLine* const line = TranscriptQueue_front(&m_view->transcript);
        if (data->prompt_shown) {
            // the prompt was left on the line the user typed on, so a line of
            // output that follows it needs its own line to start on
            fputc('\n', stdout);
            data->prompt_shown = false;
        }
        printf("%s\n", line->text);
        free(line->text);
        TranscriptQueue_pop_front(&m_view->transcript);
        wrote = true;
    }

    if (wrote || !data->prompt_shown) {
        fputs("> ", stdout);
        data->prompt_shown = true;
    }
    fflush(stdout);
}

/* ---- tui backend ---- */

static TuiUiStyle style_of(uint32_t fg, uint32_t bg) {
    const TuiUiStyle style = {fg, bg, TUI_STYLE_NONE};
    return style;
}

static uint32_t cell_color(char cell) {
    switch (cell) {
    case 'I': return TYPE_COLOR[TETROMINO_TYPE_I];
    case 'J': return TYPE_COLOR[TETROMINO_TYPE_J];
    case 'L': return TYPE_COLOR[TETROMINO_TYPE_L];
    case 'O': return TYPE_COLOR[TETROMINO_TYPE_O];
    case 'S': return TYPE_COLOR[TETROMINO_TYPE_S];
    case 'T': return TYPE_COLOR[TETROMINO_TYPE_T];
    case 'Z': return TYPE_COLOR[TETROMINO_TYPE_Z];
    case '#': return COLOR_GARBAGE;
    default: return COLOR_EMPTY;
    }
}

static void draw_cell_block(int col, int row, const char* glyph, TuiUiStyle style) {
    for (int i = 0; i < CELL_WIDTH; i++) {
        tuiui_draw_cell(&g_ui, BOARD_ORIGIN_X + CELL_WIDTH * col + i, BOARD_ORIGIN_Y + row,
                        glyph, 1, style);
    }
}

static void draw_board(const GameView* game) {
    for (int r = 0; r < GAME_VIEW_ROWS; r++) {
        for (int c = 0; c < GAME_VIEW_COLS; c++) {
            const char cell = game->cells[r][c];
            const uint32_t color = cell_color(cell);
            draw_cell_block(c, r, " ", style_of(color, color));
        }
    }
}

/*
    The falling piece and its ghost are drawn from the piece descriptor rather
    than read out of the board, because the server sends them separately: the
    board it sends contains only what has locked.
*/
static void draw_piece(TetrominoType type, TetrominoDirection direction, int row, int col,
                       const char* glyph, uint32_t fg, uint32_t bg) {
    const int box = TETROMINO_BOX_SIZE[type];
    for (int i = 0; i < box; i++) {
        for (int j = 0; j < box; j++) {
            if (!IS_TETROMINO_CELL_OCCUPIED[type](type, direction, i, j)) {
                continue;
            }
            const int board_row = row + i - VISIBLE_ROW_IDX;
            const int board_col = col + j;
            if (board_row < 0 || board_row >= GAME_VIEW_ROWS ||
                board_col < 0 || board_col >= GAME_VIEW_COLS) {
                continue;
            }
            draw_cell_block(board_col, board_row, glyph, style_of(fg, bg));
        }
    }
}

static void draw_mini_piece(TetrominoType type, int x, int y) {
    const int box = TETROMINO_BOX_SIZE[type];
    const uint32_t color = TYPE_COLOR[type];
    for (int i = 0; i < box; i++) {
        for (int j = 0; j < box; j++) {
            if (!IS_TETROMINO_CELL_OCCUPIED[type](type, TETROMINO_DIRECTION_0, i, j)) {
                continue;
            }
            for (int k = 0; k < CELL_WIDTH; k++) {
                tuiui_draw_cell(&g_ui, x + CELL_WIDTH * j + k, y + i, " ", 1, style_of(color, color));
            }
        }
    }
}

static void draw_text(int x, int y, uint32_t fg, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

static void draw_text(int x, int y, uint32_t fg, const char* format, ...) {
    char line[128];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    tuiui_draw_text(&g_ui, x, y, line, style_of(fg, TUI_COLOR_DEFAULT));
}

static const char* status_text(GameStatus status) {
    switch (status) {
    case GAME_STATUS_LOBBY: return "waiting";
    case GAME_STATUS_RUNNING: return "running";
    case GAME_STATUS_ENDED: return "game over";
    }
    return "?";
}

static void draw_sidebar(const ViewModel* view) {
    const GameView* const game = &view->game;
    int y = BOARD_ORIGIN_Y;

    draw_text(SIDEBAR_X, y++, COLOR_TEXT, "room %s  [%s]", game->room, status_text(game->status));
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "you  %s (#%ld)", view->player_name, view->player_id);
    y++;

    const GameSeat* const seat = &game->seats[game->you];
    draw_text(SIDEBAR_X, y++, COLOR_TEXT, "lines %d", seat->lines);
    draw_text(SIDEBAR_X, y++, COLOR_TEXT, "score %d", seat->score);
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "combo %d  b2b %d", game->combo, game->back_to_back);
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "garbage %d", game->garbage);
    y++;

    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "hold");
    if (game->has_hold) {
        draw_mini_piece(game->hold, SIDEBAR_X, y);
    }
    y += 3;

    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "next");
    for (size_t i = 0; i < game->next_count && i < 4; i++) {
        draw_mini_piece(game->next[i], SIDEBAR_X, y);
        y += 3;
    }

    y = BOARD_ORIGIN_Y + GAME_VIEW_ROWS - 4;
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "move a/d or arrows");
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "rotate z/x   hold c");
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "soft s   hard space");
    draw_text(SIDEBAR_X, y++, COLOR_MUTED, "q to leave the room");
}

static void draw_others(const GameView* game) {
    // one line per other seat, which is all a single-board layout has room
    // for; the battle-royale layout replaces this with a board per seat
    int y = BOARD_ORIGIN_Y + GAME_VIEW_ROWS + 1;
    for (size_t i = 0; i < game->seat_count; i++) {
        if (i == game->you) {
            continue;
        }
        draw_text(BOARD_ORIGIN_X, y++, COLOR_MUTED, "%s  lines %d  score %d  %s",
                  game->seats[i].name, game->seats[i].lines, game->seats[i].score,
                  game->seats[i].status);
    }
}

static void render_tui(const ViewModel* m_view) {
    tui_clear();

    if (!m_view->game_valid) {
        draw_text(BOARD_ORIGIN_X, BOARD_ORIGIN_Y, COLOR_TEXT, "waiting for the first state push...");
        tui_present();
        return;
    }

    const GameView* const game = &m_view->game;

    // a frame around the well, so the playfield edge is visible against a
    // terminal background of any colour
    const TgRecti frame = {BOARD_ORIGIN_X - 1, BOARD_ORIGIN_Y - 1,
                           CELL_WIDTH * GAME_VIEW_COLS + 2, GAME_VIEW_ROWS + 2};
    tuiui_draw_box(&g_ui, frame, style_of(COLOR_FRAME, TUI_COLOR_DEFAULT),
                   style_of(COLOR_FRAME, TUI_COLOR_DEFAULT));

    draw_board(game);
    if (game->status == GAME_STATUS_RUNNING) {
        draw_piece(game->piece_type, game->piece_direction, game->ghost_row, game->ghost_col,
                   "░", TYPE_COLOR[game->piece_type], COLOR_EMPTY);
        draw_piece(game->piece_type, game->piece_direction, game->piece_row, game->piece_col,
                   " ", TYPE_COLOR[game->piece_type], TYPE_COLOR[game->piece_type]);
    }

    draw_sidebar(m_view);
    draw_others(game);

    if (game->status == GAME_STATUS_ENDED) {
        draw_text(BOARD_ORIGIN_X + 3, BOARD_ORIGIN_Y + GAME_VIEW_ROWS / 2, COLOR_TEXT, "GAME OVER");
    }

    tui_present();
}

void UiData_render(UiData* data, ViewModel* m_view, bool frame_tick, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        return;
    }

    if (data->backend == UI_BACKEND_LINE) {
        render_line(data, m_view);
        m_view->dirty = false;
        return;
    }

    // the tui redraws the whole screen, so it runs on the frame tick rather
    // than on every state change; a burst of pushes still costs one draw
    if (frame_tick) {
        render_tui(m_view);
        m_view->dirty = false;
    }
}

void UiData_read_frame_timer(UiData* data, ClientFault* fault) {
    if (data->frame_timerfd == -1) {
        return;
    }
    uint64_t expirations;
    const ssize_t n = read(data->frame_timerfd, &expirations, sizeof(expirations));
    if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("read frame timer");
        *fault = FAULT_LOCAL;
    }
}

/*
    tui_init takes a fixed size and corestack has no resize handling, so the
    size is read once here. A terminal resized mid-game keeps drawing at the
    old size until the mode is left and re-entered.
*/
static TgSizei terminal_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        const TgSizei size = {ws.ws_col, ws.ws_row};
        return size;
    }
    const TgSizei fallback = {TUI_FALLBACK_WIDTH, TUI_FALLBACK_HEIGHT};
    return fallback;
}

static int frame_timer_arm(unsigned frame_interval_ms) {
    const int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        return -1;
    }

    const long interval_ns = 1000L * 1000L * (long)frame_interval_ms;
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_nsec = interval_ns;
    spec.it_interval.tv_nsec = interval_ns;
    if (timerfd_settime(timerfd, 0, &spec, NULL) == -1) {
        close(timerfd);
        return -1;
    }
    return timerfd;
}

int UiData_set_backend(UiData* data, UiBackend backend, unsigned frame_interval_ms) {
    if (data->backend == backend) {
        return 0;
    }

    if (backend == UI_BACKEND_LINE) {
        tui_teardown(data);
        data->backend = UI_BACKEND_LINE;
        data->prompt_shown = false;
        return 0;
    }

    if (tg_result_err(tui_init(terminal_size()))) {
        return -1;
    }
    data->tui_up = true;

    data->frame_timerfd = frame_timer_arm(frame_interval_ms);
    if (data->frame_timerfd == -1) {
        tui_teardown(data);
        return -1;
    }

    data->backend = UI_BACKEND_TUI;
    return 0;
}
