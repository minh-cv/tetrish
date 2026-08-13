#include "app/app_layer.h"
#include "app/dispatch.h"
#include "app/room.h"
#include "app/util.h"
#include "dtor.h"
#include "htttp.h"
#include "logger.h"
#include "proto.h"
#include "type.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(SparseSet_Player_free)
static DTOR_WRAPPER_DEFINE(SparseSet_Room_free)
static DTOR_WRAPPER_DEFINE(SparseSet_bool_free)
static DTOR_WRAPPER_DEFINE(Vec_GarbageEvent_free)

/*!
    @see htttp_layer.c
*/
static void response_queue_drain(HtttpOutboundMessageQueue* q) {
    const size_t count = HtttpOutboundMessageQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        HtttpOutboundMessage* m = HtttpOutboundMessageQueue_front(q);
        htttp_message_free(&m->message, &m->ownership);
        HtttpOutboundMessageQueue_pop_front(q);
    }
}

/*!
    @brief mark @p fd failed and discard what it staged in @p m_response_qs
*/
static void fail_fd(size_t fd, SparseSet_HtttpOutboundMessageQueue* m_response_qs, SparseSet_bool* err_fds) {
    *SparseSet_bool_activate(err_fds, fd) = true;
    if (SparseSet_HtttpOutboundMessageQueue_contains(m_response_qs, fd)) {
        response_queue_drain(SparseSet_HtttpOutboundMessageQueue_get(m_response_qs, fd));
        SparseSet_HtttpOutboundMessageQueue_erase(m_response_qs, fd);
    }
}

int AppData_init(AppData* data, size_t max_entries, size_t max_rooms,
                 size_t max_players_per_room) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (SparseSet_Player_init(&data->players, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_Player_free, &data->players);

    data->member_pool = calloc(max_rooms * max_players_per_room, sizeof(RoomMember));
    if (data->member_pool == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, data->member_pool);
    data->max_players_per_room = max_players_per_room;

    if (Vec_GarbageEvent_init(&data->garbage_events, max_rooms * max_players_per_room) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_GarbageEvent_free, &data->garbage_events);
    rng_init(&data->garbage_rng, (uint64_t)time(NULL) ^ (uint64_t)getpid());
    data->garbage_injected = 0;
    data->garbage_dropped_no_target = 0;

    if (SparseSet_Room_init(&data->rooms, max_rooms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_Room_free, &data->rooms);

    if (SparseSet_bool_init(&data->in_game_rooms, max_rooms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_bool_free, &data->in_game_rooms);

    if (Vec_RoomIdx_init(&data->free_room_idxs, max_rooms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    for (size_t i = 0; i < max_rooms; i++) {
        const int err = Vec_RoomIdx_push_back(&data->free_room_idxs, &i);
        assert(err != -1 && "free list holds one entry per room key");
        (void)err;
    }

    DTOR_RETURN(dtor, 0);
}

void AppData_free(AppData* data) {
    Vec_GarbageEvent_free(&data->garbage_events);
    Vec_RoomIdx_free(&data->free_room_idxs);
    SparseSet_bool_free(&data->in_game_rooms);
    SparseSet_Room_free(&data->rooms);
    free(data->member_pool);
    SparseSet_Player_free(&data->players);
}

void AppData_accept(AppData* data, const Vec_Fd* fds, SparseSet_bool* err_fds) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd_raw = *Vec_Fd_at(fds, i);
        assert(fd_raw >= 0 && (size_t)fd_raw < data->players.capacity);
        const size_t fd = (size_t)fd_raw;

        if (SparseSet_Player_contains(&data->players, fd)) {
            assert(false && "accepted fd must not already be in entries");
            continue;
        }
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        Player entry;
        memset(&entry, 0, sizeof(entry));
        entry.room_idx = ROOM_IDX_NONE;
        const int err = SparseSet_Player_insert(&data->players, fd, &entry);
        assert(err != -1);
        (void)err;
    }
}

void AppData_close(AppData* data, const SparseSet_bool* close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (!SparseSet_Player_contains(&data->players, fd)) {
            continue;
        }

        room_leave(data, (Fd)fd);
        memset(SparseSet_Player_get(&data->players, fd), 0, sizeof(Player));
        SparseSet_Player_erase(&data->players, fd);
    }
}

static DispatchResult respond_one_frame(AppData* data, Fd fd, const HtttpParsedMessage* parsed, HtttpOutboundMessage* outbound) {
    HtttpStatus status = HTTTP_STATUS_BAD_REQUEST;
    const char* body = NULL;
    size_t body_len = 0;
    switch (parsed->status) {
    case FRAME_OK:
        if (parsed->message.is_request) {
            return respond_one_request(data, fd, &parsed->message.request, outbound);
        }
        else {
            return DISPATCH_NO_RESPONSE;
        }
        break;
    case FRAME_DECRYPT_ERROR:
        status = HTTTP_STATUS_BAD_REQUEST;
        body = "Cannot decrypt message";
        body_len = strlen(body);
        break;
    case FRAME_PAYLOAD_TOO_LARGE:
        status = HTTTP_STATUS_PAYLOAD_TOO_LARGE;
        body = "Payload too large";
        body_len = strlen(body);
        break;
    case FRAME_HTTTP_PARSE_ERROR:
        status = HTTTP_STATUS_BAD_REQUEST;
        body = "Cannot parse request";
        body_len = strlen(body);
        break;
    }
    
    outbound->message.is_request = false;
    if (htttp_make_default_response(status, body, body_len, false, &outbound->message.response, &outbound->ownership) == -1) {
        return DISPATCH_ERR;
    }
    return DISPATCH_RESPOND;
}

void AppData_respond(AppData* data, const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
                     SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                     SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_HtttpParsedMessageQueue_size(m_parsed_qs); i++) {
        const size_t fd = SparseSet_HtttpParsedMessageQueue_key_at_idx(m_parsed_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_parsed_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_Player_contains(&data->players, fd)) {
            assert(false && "m_parsed_qs fd must have been accepted");
            continue;
        }
        HtttpParsedMessageQueue* q = SparseSet_HtttpParsedMessageQueue_at_idx(m_parsed_qs, i);

        bool failed = false;
        const size_t count = HtttpParsedMessageQueue_size(q);
        HtttpOutboundMessageQueue* out = SparseSet_HtttpOutboundMessageQueue_activate(m_response_qs, fd);

        for (size_t j = 0; j < count; j++) {
            const HtttpParsedMessage* parsed = HtttpParsedMessageQueue_at(q, j);

            HtttpOutboundMessage response = {
                .message.is_request = false,
            };
            const DispatchResult result = respond_one_frame(data, (Fd)fd, parsed, &response);
            if (result == DISPATCH_ERR) {
                failed = true;
                break;
            }
            if (result == DISPATCH_NO_RESPONSE) {
                continue;
            }

            const int err = HtttpOutboundMessageQueue_push_back(out, &response);
            assert(err != -1 && "parsed_qs and response_qs share cfg.client_capacity");
            if (err == -1) {
                htttp_message_free(&response.message, &response.ownership);
                failed = true;
                break;
            }
        }

        if (failed) {
            fail_fd(fd, m_response_qs, err_fds);
        }
    }
}

/*
    Hide the pieces the room's preview setting does not reveal. The queue
    runs bag1[bag1_offset] (the piece in play), the rest of bag1, then bag2;
    the first max_preview entries after the piece in play stay, later ones
    are overwritten with TETROMINO_TYPE_COUNT, which no real piece uses.
*/
static void mask_bag_previews(BagState* bag, int max_preview) {
    int j = bag->bag1_offset + 1 + max_preview;
    if (j < 0) {
        j = 0;
    }
    for (; j < ROOM_PREVIEW_MAX; j++) {
        TetrominoType* slot = j < TETROMINO_TYPE_COUNT
            ? &bag->bag1[j]
            : &bag->bag2[j - TETROMINO_TYPE_COUNT];
        *slot = TETROMINO_TYPE_COUNT;
    }
}

/*!
    @brief build the @c STATE request carrying @p member 's board into
           @p outbound

    The request is addressed to the room it reports on, `/room/<room_idx>`,
    rather than to the member it goes to.

    @pre @p room_idx is the key of @p member 's room in @c rooms , and
         @p room_in_game and @p max_preview are that room's status and
         preview setting

    @post on success @p outbound holds a message whose memory is owned by
          the caller
    @post on failure @p outbound is unmodified and nothing was allocated

    @return -1 if the body, the Content-Length header or the path could not
            be allocated, 0 otherwise
*/
static int make_state_request(const RoomMember* member, size_t room_idx, bool room_in_game,
                              int max_preview, HtttpOutboundMessage* outbound) {
    DTOR_DEFINE(errdtor, 3);
    DTOR_DEFINE(dtor, 1);

    const State* game = &member->game;
    ProtoStateRequest state = {
        .board_state = game->board_state,
        .combo_counter = game->combo_counter,
        .hold_state = game->hold_state,
        .bag_state = game->bag_state,
        .garbage_balance = game->garbage_balance,
        .back_to_back_count = game->back_to_back_count,
        .game_score = game->score,
        .is_game_active = room_in_game && member->alive,
    };
    mask_bag_previews(&state.bag_state, max_preview);

    unsigned char* body;
    size_t body_len;
    if (proto_serialize_state_request(&state, &body, &body_len) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, body);

    char scratch[32];
    const int written = snprintf(scratch, sizeof(scratch), "%zu", body_len);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    char* const content_length = strdup(scratch);
    if (content_length == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, content_length);

    char* path = malloc_sprintf("/room/%zu", room_idx);
    if (path == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, path);

    const HtttpOutboundMessage new_outbound = {
        .message.is_request = true,
        .message.request = {
            .method = "STATE",
            .path = path,
            .header = {
                {"Content-Length", content_length},
                {"Content-Type", "application/tetris-state"},
            },
            .header_count = 2,
            .body = body,
            .body_len = body_len,
        },
        .ownership = {
            .is_value_owned[0] = true,
            .is_path_owned = true,
            .is_body_owned = true,
        },
    };
    *outbound = new_outbound;
    DTOR_RETURN(dtor, 0);
}

/*
    The producer half of an attack: bank each member's accumulated outgoing
    garbage as one event and zero the balance. The engine accumulates
    outgoing rows as a negative garbage_balance at lock time; nothing else
    ever drains the negative side, so this is the only producer. A room
    whose game ended on this tick's frames emits nothing: its boards are
    already meaningless.
*/
static void sweep_garbage(Room* room, size_t room_idx, Vec_GarbageEvent* m_garbage_events) {
    if (room->status != ROOM_IN_GAME) {
        return;
    }
    for (size_t i = 0; i < room->member_count; i++) {
        RoomMember* member = &room->members[i];
        if (!member->alive || member->game.garbage_balance >= 0) {
            continue;
        }

        int lines = -member->game.garbage_balance;
        member->game.garbage_balance = 0;
        if (lines > UINT16_MAX) {
            lines = UINT16_MAX;
        }
        const GarbageEvent event = {
            .version = GARBAGE_EVENT_VERSION,
            .cross_room = room->config.cross_room_garbage ? 1 : 0,
            .n_lines = (uint16_t)lines,
            .source_fd = member->fd,
            .source_room_idx = (uint32_t)room_idx,
            .seq = 0,
        };
        const int err = Vec_GarbageEvent_push_back(m_garbage_events, &event);
        assert(err != -1 && "garbage_events holds one event per seat");
        (void)err;
    }
}

void AppData_room_tick(AppData* data, uint64_t expirations,
                       SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                       Vec_GarbageEvent* m_garbage_events,
                       SparseSet_bool* err_fds) {
    if (expirations == 0) {
        return;
    }

    //! TODO: make this configurable instead of hardcoding
    if (expirations > 5) {
        expirations = 5;
    }

    for (size_t i = SparseSet_bool_size(&data->in_game_rooms); i-- > 0;) {
        const size_t room_idx = SparseSet_bool_key_at_idx(&data->in_game_rooms, i);
        Room* room = SparseSet_Room_get(&data->rooms, room_idx);
        assert(room->status == ROOM_IN_GAME && "in_game_rooms holds the rooms in a game");

        // the first frame is the one carrying the recorded inputs; room_tick
        // clears them, so the catch-up frames advance on none
        for (uint64_t f = 0; f < expirations && room->status == ROOM_IN_GAME; f++) {
            room_tick(data, room_idx);
        }

        sweep_garbage(room, room_idx, m_garbage_events);

        const bool room_in_game = room->status == ROOM_IN_GAME;
        for (size_t m = 0; m < room->member_count; m++) {
            const RoomMember* member = &room->members[m];
            const Fd fd_raw = member->fd;
            assert(fd_raw >= 0 && SparseSet_Player_contains(&data->players, (size_t)fd_raw));
            const size_t fd = (size_t)fd_raw;
            if (SparseSet_bool_contains(err_fds, fd)) {
                continue;
            }

            HtttpOutboundMessage push;
            if (make_state_request(member, room_idx, room_in_game,
                                   room->config.max_preview, &push) == -1) {
                fail_fd(fd, m_response_qs, err_fds);
                continue;
            }

            HtttpOutboundMessageQueue* out = SparseSet_HtttpOutboundMessageQueue_activate(m_response_qs, fd);
            if (HtttpOutboundMessageQueue_push_back(out, &push) == -1) {
                htttp_message_free(&push.message, &push.ownership);
            }
        }
    }
}

//! @brief a random living opponent in the event's own room, or NULL
static RoomMember* pick_same_room(AppData* data, const GarbageEvent* event) {
    const size_t room_idx = event->source_room_idx;
    if (room_idx >= data->rooms.capacity ||
        !SparseSet_Room_contains(&data->rooms, room_idx)) {
        return NULL;
    }
    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    if (room->status != ROOM_IN_GAME) {
        return NULL;
    }

    int count = 0;
    for (size_t i = 0; i < room->member_count; i++) {
        const RoomMember* member = &room->members[i];
        if (member->alive && member->fd != event->source_fd) {
            count++;
        }
    }
    if (count == 0) {
        return NULL;
    }

    int pick = rng_below(&data->garbage_rng, count);
    for (size_t i = 0; i < room->member_count; i++) {
        RoomMember* member = &room->members[i];
        if (member->alive && member->fd != event->source_fd && pick-- == 0) {
            return member;
        }
    }
    assert(false && "the counted candidate must be found again");
    return NULL;
}

/*!
    @brief a random living member of a room that is not the event's own and
           also opted into cross-room garbage, or NULL

    Cross-room garbage only travels between such rooms: an isolated room
    never receives what it could never send.
*/
static RoomMember* pick_cross_room(AppData* data, const GarbageEvent* event) {
    int count = 0;
    for (size_t r = 0; r < SparseSet_bool_size(&data->in_game_rooms); r++) {
        const size_t room_idx = SparseSet_bool_key_at_idx(&data->in_game_rooms, r);
        if (room_idx == event->source_room_idx) {
            continue;
        }
        const Room* room = SparseSet_Room_get(&data->rooms, room_idx);
        if (!room->config.cross_room_garbage) {
            continue;
        }
        for (size_t i = 0; i < room->member_count; i++) {
            const RoomMember* member = &room->members[i];
            if (member->alive && member->fd != event->source_fd) {
                count++;
            }
        }
    }
    if (count == 0) {
        return NULL;
    }

    int pick = rng_below(&data->garbage_rng, count);
    for (size_t r = 0; r < SparseSet_bool_size(&data->in_game_rooms); r++) {
        const size_t room_idx = SparseSet_bool_key_at_idx(&data->in_game_rooms, r);
        if (room_idx == event->source_room_idx) {
            continue;
        }
        Room* room = SparseSet_Room_get(&data->rooms, room_idx);
        if (!room->config.cross_room_garbage) {
            continue;
        }
        for (size_t i = 0; i < room->member_count; i++) {
            RoomMember* member = &room->members[i];
            if (member->alive && member->fd != event->source_fd && pick-- == 0) {
                return member;
            }
        }
    }
    assert(false && "the counted candidate must be found again");
    return NULL;
}

void AppData_apply_garbage(AppData* data, const Vec_GarbageEvent* events) {
    for (size_t i = 0; i < Vec_GarbageEvent_size(events); i++) {
        const GarbageEvent* event = Vec_GarbageEvent_at(events, i);
        if (event->version != GARBAGE_EVENT_VERSION) {
            data->garbage_dropped_no_target++;
            LOGGER_LOG(LOG_WARN, "garbage", "event seq=%u dropped, version %u unknown",
                       event->seq, event->version);
            continue;
        }

        RoomMember* target = event->cross_room
            ? pick_cross_room(data, event)
            : pick_same_room(data, event);
        if (target == NULL) {
            data->garbage_dropped_no_target++;
            LOGGER_LOG(LOG_INFO, "garbage", "event seq=%u from fd=%d dropped, no target",
                       event->seq, event->source_fd);
            continue;
        }

        queue_garbage(&target->game, (int)event->n_lines);
        data->garbage_injected++;
        LOGGER_LOG(LOG_INFO, "garbage", "event seq=%u fd=%d -> fd=%d, %u lines",
                   event->seq, event->source_fd, target->fd, event->n_lines);
    }
}

void AppData_reset(AppData* data) {
    Vec_GarbageEvent_reset(&data->garbage_events);
}
