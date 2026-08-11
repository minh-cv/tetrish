#ifndef TETRISH_TETRISU_UI_LAYER_H
#define TETRISH_TETRISU_UI_LAYER_H

#include "app_layer.h"

/*!
    @brief Reads the view model and draws it. It owns the frame timer and, in
    game mode, the tui singleton.

    `UI_BACKEND_LINE` appends new transcript entries to stdout and renders
    whenever the view is dirty. `UI_BACKEND_TUI` draws the board and renders on
    the frame tick only.
*/
typedef enum {
    UI_BACKEND_LINE,
    UI_BACKEND_TUI,
} UiBackend;

typedef struct {
    UiBackend backend;
    int frame_timerfd;      // -1 unless UI_BACKEND_TUI
    bool tui_up;
    bool prompt_shown;
} UiData;

int UiData_init(UiData* data);

/*!
    @post tui_shutdown has run if it was up, so the terminal is restored even
          on an error path
*/
void UiData_free(UiData* data);

/*!
    @brief draw one frame

    @pre  the application layer has already run this tick
    @post transcript entries drawn are popped and freed
    @post @p m_view->dirty is cleared
    @post in UI_BACKEND_TUI nothing is drawn unless @p frame_tick
*/
void UiData_render(UiData* data, ViewModel* m_view, bool frame_tick, ClientFault* fault);

/*!
    @brief consume the timerfd expiration count

    @pre the frame timer slot was reported readable
*/
void UiData_read_frame_timer(UiData* data, ClientFault* fault);

/*!
    @brief switch backend

    @pre  called only at tick end, from client_apply_mode, and paired with the
          matching InputData_set_mode
    @post entering UI_BACKEND_TUI calls tui_init and arms the frame timer at
          @p frame_interval_ms ; leaving it disarms and closes the timer and
          calls tui_shutdown
    @post the caller registers or closes the timer's poller slot to match
*/
int UiData_set_backend(UiData* data, UiBackend backend, unsigned frame_interval_ms);

#endif
