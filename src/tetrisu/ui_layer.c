#include "ui_layer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int UiData_init(UiData* data) {
    data->backend = UI_BACKEND_LINE;
    data->frame_timerfd = -1;
    data->tui_up = false;
    data->prompt_shown = false;
    return 0;
}

void UiData_free(UiData* data) {
    if (data->frame_timerfd != -1) {
        close(data->frame_timerfd);
        data->frame_timerfd = -1;
    }
}

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

void UiData_render(UiData* data, ViewModel* m_view, bool frame_tick, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        return;
    }
    if (data->backend == UI_BACKEND_LINE) {
        render_line(data, m_view);
        m_view->dirty = false;
        return;
    }
    (void)frame_tick;
    m_view->dirty = false;
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

int UiData_set_backend(UiData* data, UiBackend backend, unsigned frame_interval_ms) {
    (void)frame_interval_ms;
    if (data->backend == backend) {
        return 0;
    }
    // the full-screen backend arrives with the game mode; until then the only
    // legal transition is the identity one
    return -1;
}
