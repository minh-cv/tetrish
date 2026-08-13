#include "game/request.h"

#include "proto.h"

#include <string.h>

int game_request_from_intent(GameIntentType intent, ClientRequest* out) {
    memset(out, 0, sizeof(*out));
    out->path = "/";
    out->content_type = "application/tetris-command";
    out->completion = CLIENT_REQUEST_EXPECT_REPLY;

    switch (intent) {
    case GAME_INTENT_CREATE:
        out->method = "CREATE";
        break;
    case GAME_INTENT_JOIN:
        out->method = "JOIN";
        break;
    case GAME_INTENT_START:
        out->method = "START";
        break;
    case GAME_INTENT_MOVE_LEFT:
        out->method = "MOVE";
        out->body = proto_serialize_move_request(PROTO_MOVE_LEFT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_MOVE_RIGHT:
        out->method = "MOVE";
        out->body = proto_serialize_move_request(PROTO_MOVE_RIGHT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_ROTATE_CW:
        out->method = "ROTATE";
        out->body = proto_serialize_rotate_request(PROTO_ROTATE_CW, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_ROTATE_CCW:
        out->method = "ROTATE";
        out->body = proto_serialize_rotate_request(PROTO_ROTATE_CCW, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_DROP_SOFT:
        out->method = "DROP";
        out->body = proto_serialize_drop_request(PROTO_DROP_SOFT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_DROP_HARD:
        out->method = "DROP";
        out->body = proto_serialize_drop_request(PROTO_DROP_HARD, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_HOLD:
        out->method = "HOLD";
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_LEAVE:
        out->method = "LEAVE";
        break;
    default:
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}
