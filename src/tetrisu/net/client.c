#include "net/client.h"

#include "net/htttp_codec.h"
#include "net/inbound_policy.h"
#include "proto.h"

#include <limits.h>
#include <poll.h>
#include <string.h>

#define CONNECT_TIMEOUT_MS 5000u
#define HANDSHAKE_TIMEOUT_MS 5000u
#define REQUEST_TIMEOUT_MS 5000u

static void reset_connection(NetClient* client) {
    socket_transport_reset(&client->transport);
    tetrissh_channel_free(&client->secure);
    owned_bytes_free(&client->pending_plaintext);
    client->state = NET_CLIENT_DISCONNECTED;
    client->pending_completion = CLIENT_REQUEST_EXPECT_REPLY;
    client->deadline_ms = 0;
    client->has_deadline = false;
}

static int emit_event(
    NetEventList* events,
    NetEventType type,
    const void* payload,
    size_t payload_length,
    ClientError error
) {
    NetEvent event = {.type = type, .error = error};
    owned_bytes_init(&event.payload);
    if (owned_bytes_copy(&event.payload, payload, payload_length) == -1) {
        return -1;
    }
    if (net_event_list_push(events, &event) == -1) {
        owned_bytes_free(&event.payload);
        return -1;
    }
    return 0;
}

static int emit_error(NetClient* client, NetEventList* events, ClientError error) {
    reset_connection(client);
    return emit_event(events, NET_EVENT_ERROR, NULL, 0, error);
}

static int begin_handshake(NetClient* client, uint64_t now_ms, NetEventList* events) {
    ClientError error = client_error(CLIENT_ERROR_NONE, 0, NULL);
    if (tetrissh_channel_start(&client->secure, &error) == -1) {
        return emit_error(client, events, error);
    }
    client->state = NET_CLIENT_HANDSHAKING;
    client->deadline_ms = now_ms + HANDSHAKE_TIMEOUT_MS;
    client->has_deadline = true;
    return emit_event(
        events,
        NET_EVENT_HANDSHAKING,
        NULL,
        0,
        client_error(CLIENT_ERROR_NONE, 0, NULL)
    );
}

void net_client_init(NetClient* client, const char* address, int port, const char* ca_path) {
    memset(client, 0, sizeof(*client));
    client->state = NET_CLIENT_DISCONNECTED;
    client->address = address;
    client->port = port;
    socket_transport_init(&client->transport);
    tetrissh_channel_init(&client->secure, ca_path);
    owned_bytes_init(&client->pending_plaintext);
}

void net_client_free(NetClient* client) {
    reset_connection(client);
}

int net_client_connect(NetClient* client, uint64_t now_ms, NetEventList* events) {
    reset_connection(client);
    if (emit_event(
        events,
        NET_EVENT_CONNECTING,
        NULL,
        0,
        client_error(CLIENT_ERROR_NONE, 0, NULL)
    ) == -1) {
        return -1;
    }

    ClientError error = client_error(CLIENT_ERROR_NONE, 0, NULL);
    const SocketConnectResult result = socket_transport_connect_start(
        &client->transport,
        client->address,
        client->port,
        &error
    );
    if (result == SOCKET_CONNECT_FAILED) {
        return emit_error(client, events, error);
    }
    if (result == SOCKET_CONNECT_CONNECTED) {
        return begin_handshake(client, now_ms, events);
    }
    client->state = NET_CLIENT_CONNECTING;
    client->deadline_ms = now_ms + CONNECT_TIMEOUT_MS;
    client->has_deadline = true;
    return 0;
}

int net_client_disconnect(NetClient* client, NetEventList* events) {
    reset_connection(client);
    return emit_event(
        events,
        NET_EVENT_DISCONNECTED,
        NULL,
        0,
        client_error(CLIENT_ERROR_NONE, 0, NULL)
    );
}

int net_client_send(
    NetClient* client,
    const unsigned char* payload,
    size_t length,
    uint64_t now_ms,
    NetEventList* events
) {
    const ClientRequest request = {
        .method = "HTTTP",
        .path = "",
        .body = payload,
        .body_len = length,
        .content_type = "text/plain",
        .completion = CLIENT_REQUEST_EXPECT_REPLY,
    };
    return net_client_send_request(client, &request, now_ms, events);
}

int net_client_send_request(
    NetClient* client,
    const ClientRequest* request,
    uint64_t now_ms,
    NetEventList* events
) {
    if (client->state != NET_CLIENT_READY_IDLE) {
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_PROTOCOL, 0, "send rejected while client is not idle")
        );
    }

    if (request == NULL ||
        (request->completion != CLIENT_REQUEST_EXPECT_REPLY &&
         request->completion != CLIENT_REQUEST_COMPLETE_ON_SEND)) {
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_PROTOCOL, 0, "invalid request completion policy")
        );
    }
    OwnedBytes plaintext;
    owned_bytes_init(&plaintext);
    ClientError error = client_error(CLIENT_ERROR_NONE, 0, NULL);
    if (htttp_codec_encode_request(request, &plaintext) == -1) {
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_NOMEM, 0, "cannot encode HTTTP request")
        );
    }
    if (tetrissh_channel_submit(&client->secure, plaintext.ptr, plaintext.len, &error) == -1) {
        owned_bytes_free(&plaintext);
        return emit_error(client, events, error);
    }
    owned_bytes_move(&client->pending_plaintext, &plaintext);
    client->pending_completion = request->completion;
    client->state = NET_CLIENT_READY_SENDING;
    client->deadline_ms = now_ms + REQUEST_TIMEOUT_MS;
    client->has_deadline = true;
    const int emitted = emit_event(
        events,
        NET_EVENT_SEND_ACCEPTED,
        NULL,
        0,
        client_error(CLIENT_ERROR_NONE, 0, NULL)
    );
    if (emitted == 0) {
        events->items[events->count - 1].completion = request->completion;
    }
    return emitted;
}

int net_client_fd(const NetClient* client) {
    return client->transport.fd;
}

short net_client_poll_events(const NetClient* client) {
    if (client->state == NET_CLIENT_CONNECTING) {
        return POLLOUT;
    }
    if (client->state == NET_CLIENT_DISCONNECTED) {
        return 0;
    }
    const unsigned wanted = tetrissh_channel_want(&client->secure);
    short events = 0;
    if ((wanted & SECURE_CHANNEL_WANT_READ) != 0) {
        events |= POLLIN;
    }
    if ((wanted & SECURE_CHANNEL_WANT_WRITE) != 0) {
        events |= POLLOUT;
    }
    return events;
}

static int accept_plaintext(NetClient* client, OwnedBytes* plaintext, NetEventList* events) {
    const bool awaiting = client->state == NET_CLIENT_READY_AWAITING_REPLY;
    const bool exact_echo = awaiting && plaintext->len == client->pending_plaintext.len &&
        memcmp(plaintext->ptr, client->pending_plaintext.ptr, plaintext->len) == 0;

    OwnedHtttpMessage message;
    memset(&message, 0, sizeof(message));
    if (htttp_codec_decode_owned(plaintext, &message) == -1) {
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_HTTTP_PARSE, 0, "server sent invalid HTTTP")
        );
    }

    const NetInboundDisposition disposition = net_inbound_classify(
        &message,
        awaiting,
        exact_echo
    );
    const OwnedBytes body = htttp_codec_borrow_body(&message);
    int result = 0;
    if (disposition == NET_INBOUND_STATE_PUSH) {
        ProtoStateRequest state;
        if (proto_parse_state_request(body.ptr, body.len, &state) == -1) {
            owned_htttp_message_free(&message);
            return emit_error(
                client,
                events,
                client_error(CLIENT_ERROR_PROTOCOL, 0, "server sent invalid STATE body")
            );
        }
        result = emit_event(
            events, NET_EVENT_STATE_PUSH, NULL, 0,
            client_error(CLIENT_ERROR_NONE, 0, NULL)
        );
        if (result == 0) {
            events->items[events->count - 1].state = state;
        }
    } else if (disposition == NET_INBOUND_REPLY || disposition == NET_INBOUND_LEGACY_ECHO) {
        result = emit_event(
            events,
            disposition == NET_INBOUND_REPLY ? NET_EVENT_REPLY : NET_EVENT_ECHO,
            body.ptr,
            body.len,
            client_error(CLIENT_ERROR_NONE, 0, NULL)
        );
        if (result == 0) {
            if (disposition == NET_INBOUND_REPLY) {
                events->items[events->count - 1].response_status =
                    (int)message.view.response.status;
            }
            owned_bytes_free(&client->pending_plaintext);
            client->state = NET_CLIENT_READY_IDLE;
            client->has_deadline = false;
        }
    } else if (disposition == NET_INBOUND_UNSOLICITED_REPLY) {
        // no request state to settle: nothing was outstanding, which is how
        // this got here. The report goes up and the session carries on.
        result = emit_event(
            events, NET_EVENT_UNSOLICITED_REPLY, body.ptr, body.len,
            client_error(CLIENT_ERROR_NONE, 0, NULL)
        );
        if (result == 0) {
            events->items[events->count - 1].response_status =
                (int)message.view.response.status;
        }
    } else {
        owned_htttp_message_free(&message);
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_PROTOCOL, 0, "unsolicited server message rejected")
        );
    }
    owned_htttp_message_free(&message);
    return result;
}

static int advance_secure(
    NetClient* client,
    short revents,
    uint64_t now_ms,
    NetEventList* events
) {
    SecureChannelStep step;
    memset(&step, 0, sizeof(step));
    const int step_result = tetrissh_channel_step(
        &client->secure,
        &client->transport,
        (revents & POLLIN) != 0,
        (revents & POLLOUT) != 0,
        &step
    );
    if ((step.events & SECURE_CHANNEL_EVENT_ERROR) != 0) {
        const ClientError error = step.error;
        tetrissh_channel_step_free(&step);
        return emit_error(client, events, error);
    }
    if ((step.events & SECURE_CHANNEL_EVENT_CLOSED) != 0) {
        tetrissh_channel_step_free(&step);
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_TRANSPORT, 0, "server closed the connection")
        );
    }
    if (step_result == -1) {
        tetrissh_channel_step_free(&step);
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_INTERNAL, 0, "secure channel failed")
        );
    }
    if ((step.events & SECURE_CHANNEL_EVENT_HANDSHAKE_READY) != 0) {
        client->state = NET_CLIENT_READY_IDLE;
        client->has_deadline = false;
        if (emit_event(
            events,
            NET_EVENT_CONNECTED,
            NULL,
            0,
            client_error(CLIENT_ERROR_NONE, 0, NULL)
        ) == -1) {
            tetrissh_channel_step_free(&step);
            return -1;
        }
    }
    if ((step.events & SECURE_CHANNEL_EVENT_APP_SENT) != 0) {
        if (client->pending_completion == CLIENT_REQUEST_EXPECT_REPLY) {
            client->state = NET_CLIENT_READY_AWAITING_REPLY;
            client->deadline_ms = now_ms + REQUEST_TIMEOUT_MS;
            client->has_deadline = true;
        }
        else {
            owned_bytes_free(&client->pending_plaintext);
            client->state = NET_CLIENT_READY_IDLE;
            client->has_deadline = false;
            if (emit_event(
                events, NET_EVENT_SEND_COMPLETED, NULL, 0,
                client_error(CLIENT_ERROR_NONE, 0, NULL)
            ) == -1) {
                tetrissh_channel_step_free(&step);
                return -1;
            }
            events->items[events->count - 1].completion =
                CLIENT_REQUEST_COMPLETE_ON_SEND;
        }
    }
    int result = 0;
    if ((step.events & SECURE_CHANNEL_EVENT_PLAINTEXT) != 0) {
        result = accept_plaintext(client, &step.plaintext, events);
    }
    tetrissh_channel_step_free(&step);
    return result;
}

int net_client_on_poll(
    NetClient* client,
    short revents,
    uint64_t now_ms,
    NetEventList* events
) {
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        return emit_error(
            client,
            events,
            client_error(CLIENT_ERROR_TRANSPORT, 0, "socket poll failed")
        );
    }
    if (client->state == NET_CLIENT_CONNECTING && (revents & POLLOUT) != 0) {
        ClientError error = client_error(CLIENT_ERROR_NONE, 0, NULL);
        if (socket_transport_connect_finish(&client->transport, &error) == -1) {
            return emit_error(client, events, error);
        }
        return begin_handshake(client, now_ms, events);
    }
    if (client->state != NET_CLIENT_DISCONNECTED) {
        const int result = advance_secure(client, revents, now_ms, events);
        if (result != 0 || client->state == NET_CLIENT_DISCONNECTED) {
            return result;
        }
        if ((revents & POLLHUP) != 0 && (revents & POLLIN) == 0) {
            return emit_error(
                client,
                events,
                client_error(CLIENT_ERROR_TRANSPORT, 0, "server hung up")
            );
        }
    }
    return 0;
}

int net_client_timeout_ms(const NetClient* client, uint64_t now_ms) {
    if (!client->has_deadline) {
        return -1;
    }
    if (now_ms >= client->deadline_ms) {
        return 0;
    }
    const uint64_t remaining = client->deadline_ms - now_ms;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int net_client_on_timeout(NetClient* client, uint64_t now_ms, NetEventList* events) {
    if (!client->has_deadline || now_ms < client->deadline_ms) {
        return 0;
    }
    return emit_error(
        client,
        events,
        client_error(CLIENT_ERROR_TIMEOUT, 0, "network operation timed out")
    );
}
