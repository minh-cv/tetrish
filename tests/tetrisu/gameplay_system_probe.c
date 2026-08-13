#include "game/request.h"
#include "net/client.h"

#include "tetrisbrain/state.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SYSTEM_TEST_TIMEOUT_MS 10000u

typedef struct {
    bool failed;
    bool reply;
    bool send_completed;
    bool active_state;
    bool held_piece;
    int response_status;
    unsigned state_count;
} Observed;

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == -1) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void observe_events(const NetEventList* events, Observed* observed) {
    for (size_t i = 0; i < events->count; ++i) {
        const NetEvent* event = &events->items[i];
        switch (event->type) {
        case NET_EVENT_ERROR:
        case NET_EVENT_DISCONNECTED:
            fprintf(
                stderr, "network failure: %s\n",
                event->error.detail == NULL ? "disconnected" : event->error.detail
            );
            observed->failed = true;
            break;
        case NET_EVENT_REPLY:
            observed->reply = true;
            observed->response_status = event->response_status;
            break;
        case NET_EVENT_SEND_COMPLETED:
            observed->send_completed = true;
            break;
        case NET_EVENT_STATE_PUSH:
            observed->state_count++;
            if (event->state.is_game_active) {
                observed->active_state = true;
            }
            if (event->state.hold_state.hold_status != HOLD_EMPTY) {
                observed->held_piece = true;
            }
            break;
        default:
            break;
        }
    }
}

static int advance_client(NetClient* client, Observed* observed) {
    const int fd = net_client_fd(client);
    if (fd < 0) {
        return -1;
    }
    int timeout = net_client_timeout_ms(client, monotonic_ms());
    if (timeout < 0 || timeout > 250) {
        timeout = 250;
    }
    struct pollfd descriptor = {
        .fd = fd,
        .events = net_client_poll_events(client),
    };
    const int ready = poll(&descriptor, 1, timeout);
    const int poll_error = ready == -1 ? errno : 0;
    const uint64_t now = monotonic_ms();

    NetEventList events;
    net_event_list_init(&events);
    int result = 0;
    if (ready > 0) {
        result = net_client_on_poll(client, descriptor.revents, now, &events);
    }
    else if (ready == -1 && poll_error != EINTR) {
        result = -1;
    }
    if (result == 0) {
        result = net_client_on_timeout(client, now, &events);
    }
    observe_events(&events, observed);
    net_event_list_free(&events);
    return result == 0 && !observed->failed ? 0 : -1;
}

static int wait_connected(NetClient* client, Observed* observed) {
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (client->state != NET_CLIENT_READY_IDLE && monotonic_ms() < deadline) {
        if (advance_client(client, observed) == -1) {
            return -1;
        }
    }
    return client->state == NET_CLIENT_READY_IDLE ? 0 : -1;
}

static int submit_intent(
    NetClient* client,
    GameIntentType intent,
    Observed* observed
) {
    ClientRequest request;
    if (game_request_from_intent(intent, &request) == -1) {
        return -1;
    }
    NetEventList events;
    net_event_list_init(&events);
    const int result = net_client_send_request(
        client, &request, monotonic_ms(), &events
    );
    observe_events(&events, observed);
    net_event_list_free(&events);
    return result == 0 && !observed->failed ? 0 : -1;
}

static int send_expect_reply(
    NetClient* client,
    GameIntentType intent,
    int expected_status,
    Observed* observed
) {
    observed->reply = false;
    observed->response_status = 0;
    if (submit_intent(client, intent, observed) == -1) {
        return -1;
    }
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed->reply && monotonic_ms() < deadline) {
        if (advance_client(client, observed) == -1) {
            return -1;
        }
    }
    if (!observed->reply || observed->response_status != expected_status ||
        client->state != NET_CLIENT_READY_IDLE) {
        fprintf(
            stderr, "expected status %d, received %d\n",
            expected_status, observed->response_status
        );
        return -1;
    }
    return 0;
}

static int send_one_way(
    NetClient* client,
    GameIntentType intent,
    Observed* observed
) {
    observed->send_completed = false;
    if (submit_intent(client, intent, observed) == -1) {
        return -1;
    }
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed->send_completed && monotonic_ms() < deadline) {
        if (advance_client(client, observed) == -1) {
            return -1;
        }
    }
    if (!observed->send_completed || client->state != NET_CLIENT_READY_IDLE ||
        net_client_timeout_ms(client, monotonic_ms()) != -1) {
        fputs("one-way request did not return to idle after write\n", stderr);
        return -1;
    }
    return 0;
}

static int wait_for_active_state(NetClient* client, Observed* observed) {
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed->active_state && monotonic_ms() < deadline) {
        if (advance_client(client, observed) == -1) {
            return -1;
        }
    }
    return observed->active_state ? 0 : -1;
}

static int wait_for_hold(NetClient* client, Observed* observed) {
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed->held_piece && monotonic_ms() < deadline) {
        if (advance_client(client, observed) == -1) {
            return -1;
        }
    }
    return observed->held_piece ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s ADDRESS PORT CA_PATH\n", argv[0]);
        return 2;
    }
    char* end = NULL;
    const long port = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || port <= 0 || port > 65535) {
        return 2;
    }

    NetClient client;
    net_client_init(&client, argv[1], (int)port, argv[3]);
    Observed observed = {0};
    NetEventList events;
    net_event_list_init(&events);
    const int connected = net_client_connect(&client, monotonic_ms(), &events);
    observe_events(&events, &observed);
    net_event_list_free(&events);
    if (connected == -1 || observed.failed ||
        wait_connected(&client, &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    if (send_expect_reply(&client, GAME_INTENT_CREATE, 201, &observed) == -1 ||
        send_expect_reply(&client, GAME_INTENT_CREATE, 409, &observed) == -1 ||
        send_expect_reply(&client, GAME_INTENT_START, 200, &observed) == -1 ||
        wait_for_active_state(&client, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_MOVE_LEFT, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_MOVE_RIGHT, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_ROTATE_CW, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_ROTATE_CCW, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_DROP_SOFT, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_HOLD, &observed) == -1 ||
        wait_for_hold(&client, &observed) == -1 ||
        send_one_way(&client, GAME_INTENT_DROP_HARD, &observed) == -1 ||
        send_expect_reply(&client, GAME_INTENT_LEAVE, 200, &observed) == -1 ||
        send_expect_reply(&client, GAME_INTENT_JOIN, 201, &observed) == -1 ||
        send_expect_reply(&client, GAME_INTENT_LEAVE, 200, &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    net_client_free(&client);
    if (observed.state_count == 0) {
        fputs("no STATE push observed\n", stderr);
        return 1;
    }
    return 0;
}
