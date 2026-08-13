#include "net/client.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SYSTEM_TEST_TIMEOUT_MS 10000u

typedef struct {
    bool state_with_name;
    bool reply;
    bool failed;
} ObservedEvents;

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == -1) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static bool bytes_contain(const OwnedBytes* bytes, const char* needle) {
    const size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > bytes->len) {
        return false;
    }
    for (size_t i = 0; i <= bytes->len - needle_length; ++i) {
        if (memcmp(bytes->ptr + i, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void observe_events(
    const NetEventList* events,
    const char* expected_reply,
    ObservedEvents* observed
) {
    for (size_t i = 0; i < events->count; ++i) {
        const NetEvent* event = &events->items[i];
        if (event->type == NET_EVENT_ERROR || event->type == NET_EVENT_DISCONNECTED) {
            fprintf(
                stderr,
                "network failure: %s\n",
                event->error.detail == NULL ? "disconnected" : event->error.detail
            );
            observed->failed = true;
        } else if (event->type == NET_EVENT_STATE_PUSH &&
                   bytes_contain(&event->payload, "reference-player")) {
            observed->state_with_name = true;
        } else if ((event->type == NET_EVENT_REPLY || event->type == NET_EVENT_ECHO) &&
                   (expected_reply == NULL || bytes_contain(&event->payload, expected_reply))) {
            observed->reply = true;
        }
    }
}

static int advance_client(
    NetClient* client,
    const char* expected_reply,
    ObservedEvents* observed
) {
    const int fd = net_client_fd(client);
    if (fd < 0) {
        return -1;
    }
    int timeout = net_client_timeout_ms(client, monotonic_ms());
    if (timeout < 0 || timeout > 1000) {
        timeout = 1000;
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
    } else if (ready == -1 && poll_error != EINTR) {
        result = -1;
    }
    if (result == 0) {
        result = net_client_on_timeout(client, now, &events);
    }
    observe_events(&events, expected_reply, observed);
    net_event_list_free(&events);
    return result == 0 && !observed->failed ? 0 : -1;
}

static int wait_connected(NetClient* client, ObservedEvents* observed) {
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (client->state != NET_CLIENT_READY_IDLE && monotonic_ms() < deadline) {
        if (advance_client(client, NULL, observed) == -1) {
            return -1;
        }
    }
    return client->state == NET_CLIENT_READY_IDLE ? 0 : -1;
}

static int send_and_wait(
    NetClient* client,
    const ClientRequest* request,
    const char* expected_reply,
    ObservedEvents* observed
) {
    NetEventList events;
    net_event_list_init(&events);
    const int sent = net_client_send_request(
        client,
        request,
        monotonic_ms(),
        &events
    );
    observe_events(&events, expected_reply, observed);
    net_event_list_free(&events);
    if (sent == -1 || observed->failed) {
        return -1;
    }

    observed->reply = false;
    const uint64_t deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed->reply && monotonic_ms() < deadline) {
        if (advance_client(client, expected_reply, observed) == -1) {
            return -1;
        }
    }
    return observed->reply && client->state == NET_CLIENT_READY_IDLE ? 0 : -1;
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
    ObservedEvents observed = {0};
    NetEventList events;
    net_event_list_init(&events);
    const int connected = net_client_connect(&client, monotonic_ms(), &events);
    observe_events(&events, NULL, &observed);
    net_event_list_free(&events);
    if (connected == -1 || observed.failed || wait_connected(&client, &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    static const unsigned char name[] = "reference-player";
    const ClientRequest set_name = {
        .method = "SET_PLAYER_NAME",
        .path = "",
        .body = name,
        .body_len = sizeof(name) - 1,
        .content_type = "text/plain",
    };
    if (send_and_wait(&client, &set_name, "reference-player", &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    const ClientRequest whoami = {
        .method = "WHOAMI",
        .path = "",
        .body = NULL,
        .body_len = 0,
        .content_type = "text/plain",
    };
    if (send_and_wait(&client, &whoami, "reference-player", &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    static const unsigned char debug_body[] = "debug-round-trip";
    const ClientRequest debug = {
        .method = "HTTTP",
        .path = "",
        .body = debug_body,
        .body_len = sizeof(debug_body) - 1,
        .content_type = "text/plain",
    };
    if (send_and_wait(&client, &debug, "debug-round-trip", &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    const ClientRequest unknown = {
        .method = "UNKNOWN",
        .path = "",
        .body = NULL,
        .body_len = 0,
        .content_type = "text/plain",
    };
    if (send_and_wait(&client, &unknown, "unsupported method", &observed) == -1) {
        net_client_free(&client);
        return 1;
    }

    const uint64_t state_deadline = monotonic_ms() + SYSTEM_TEST_TIMEOUT_MS;
    while (!observed.state_with_name && monotonic_ms() < state_deadline) {
        if (advance_client(&client, NULL, &observed) == -1) {
            net_client_free(&client);
            return 1;
        }
    }
    net_client_free(&client);
    if (!observed.state_with_name) {
        fputs("timed out waiting for named STATE push\n", stderr);
        return 1;
    }
    return 0;
}
