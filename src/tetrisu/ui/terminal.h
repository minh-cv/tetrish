#ifndef TETRISH_TETRISU_UI_TERMINAL_H
#define TETRISH_TETRISU_UI_TERMINAL_H

#include "app.h"
#include "command/router.h"

#include <stdbool.h>
#include <stddef.h>

#define UI_COMMAND_LIST_CAPACITY 8u

typedef struct {
    ParsedCommand items[UI_COMMAND_LIST_CAPACITY];
    size_t count;
} UiCommandList;

typedef struct {
    char line[COMMAND_MAX_BYTES + 1u];
    size_t length;
    size_t cursor;
    size_t dropped_events_seen;
    char status[128];
    bool initialized;
    bool dirty;
} TerminalUi;

/*!
    @brief initialize an empty list of UI-submitted commands
    @post @p list owns no command argument and has count `0`
*/
void ui_command_list_init(UiCommandList* list);

/*!
    @brief release command arguments owned by @p list
    @post @p list owns no allocation and has count `0`
*/
void ui_command_list_free(UiCommandList* list);

/*!
    @brief enter raw alternate-screen mode using the current terminal size

    @pre @p ui is not initialized
    @pre stdin and stdout refer to a terminal
    @post on success, @p ui is the logical owner of the global tui.h backend
    @post on failure, raw/alternate-screen mode is not active

    @return `0` on success, `-1` when the terminal backend cannot initialize
*/
int terminal_ui_init(TerminalUi* ui);

/*!
    @brief restore the terminal and release the global tui.h backend
    @pre @p ui was initialized or is zero-initialized
    @post raw/alternate-screen mode is not active and @p ui is uninitialized
*/
void terminal_ui_free(TerminalUi* ui);

/*!
    @brief refresh tui.h's borrowed current-frame input state without consuming intent

    @pre @p ui is initialized
    @post tui.h has non-blockingly drained stdin exactly once for this reactor pass
    @post previous transient clicked/repeated/count state has been reset
    @post the raw-copy ring is empty because tetrisu does not consume it

    @return `0` on success, `-1` on terminal backend failure
*/
int terminal_ui_poll_input(TerminalUi* ui);

/*!
    @brief interpret the already-polled tui.h state as command-editor intent

    Input is borrowed directly from tui.h's ordered current-frame events; no
    second input-event abstraction or blocking read is introduced. Complete
    submitted lines are parsed and routed into commands owned by @p commands.

    @pre @p ui is initialized and `terminal_ui_poll_input()` succeeded this pass
    @pre @p commands is initialized and empty
    @post each current-frame input event is interpreted exactly once
    @post editor-only intent changes only @p ui; Enter/Ctrl-C may append commands
*/
void terminal_ui_update(TerminalUi* ui, UiCommandList* commands);

/*!
    @brief render an immutable application view and the local command editor
    @pre @p ui is initialized and @p view borrows valid application state
    @post only tui.h's cell back buffer and @p ui dirty state change
*/
void terminal_ui_draw(TerminalUi* ui, const AppView* view);

/*!
    @brief present the tui.h cell buffer to stdout
    @pre @p ui is initialized and the current frame was explicitly polled
    @post on success, the displayed terminal matches the back buffer
    @return `0` on success, `-1` on terminal output failure
*/
int terminal_ui_present(TerminalUi* ui);

#endif
