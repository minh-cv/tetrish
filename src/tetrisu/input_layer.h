#ifndef TETRISH_TETRISU_INPUT_LAYER_H
#define TETRISH_TETRISU_INPUT_LAYER_H

#include "cmdline.h"
#include "tui.h"
#include "type.h"

/*!
    @brief What the terminal produced, in a shape the application layer can
    consume without knowing which frontend is up. Both backends feed the same
    queue, so command handling is written once.
*/
typedef enum {
    INPUT_EVENT_COMMAND,      // a tokenized line (line mode)
    INPUT_EVENT_KEY,          // one decoded key (raw mode)
    INPUT_EVENT_DIAGNOSTIC,   // an unusable line; the session continues
    INPUT_EVENT_EOF,          // stdin closed
} InputEventType;

typedef struct {
    InputEventType type;
    struct {
        char* argv[CMDLINE_MAX_ARGS];
        size_t argc;
    } command;                // owned by the event
    struct {
        TuiKey key;
        unsigned char ch;
        uint8_t modifiers;
    } key;
    const char* diagnostic;   // a string literal, never owned
} InputEvent;

#define RING_BUFFER_ELEM_TYPE InputEvent
#define RING_BUFFER_TYPEDEF InputEventQueue
#include "collection/ring_buffer.h"

/*!
    @brief `INPUT_MODE_LINE` is cooked stdin assembled into lines and
    tokenized; `INPUT_MODE_RAW` is the tui's decoded keys. The two disciplines
    cannot be live at once, which is why switching is a whole operation rather
    than a flag the reader consults.
*/
typedef enum {
    INPUT_MODE_LINE,
    INPUT_MODE_RAW,
} InputMode;

typedef struct {
    InputMode mode;
    char* line_buf;          // partial line carried across ticks in line mode
    size_t line_used;
    size_t line_capacity;
    bool line_overflowed;    // discarding the rest of an oversized line
    bool eof;
    bool stdin_nonblocking;
    int saved_stdin_flags;
} InputData;

int InputData_init(InputData* data, size_t line_capacity);
void InputData_free(InputData* data);

/*!
    @post every event in @p m_event_q is freed, including command argv storage
*/
void InputData_reset(InputData* data, InputEventQueue* m_event_q);

/*!
    @brief read what stdin has available and append events to @p m_event_q

    @post EOF appends INPUT_EVENT_EOF exactly once and latches @c eof
    @post a line longer than @c line_capacity is discarded up to the next
          newline and reported as a diagnostic; it is not a fault
*/
void InputData_read(InputData* data, InputEventQueue* m_event_q, ClientFault* fault);

/*!
    @brief switch terminal discipline

    @pre  called only at tick end, from client_apply_mode
    @pre  entering INPUT_MODE_RAW requires tui_init to have succeeded
    @post buffered bytes belonging to the old discipline are discarded, so no
          keystroke is reinterpreted under the new one
*/
int InputData_set_mode(InputData* data, InputMode mode);

#endif
