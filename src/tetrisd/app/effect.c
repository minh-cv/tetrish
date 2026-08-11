#include "app/effect.h"
#include <stdlib.h>
#include <string.h>

int app_effect_sink_init(AppEffectSink* sink, size_t capacity, size_t arena_capacity) {
    sink->effects = malloc(capacity * sizeof(*sink->effects));
    if (sink->effects == NULL) {
        return -1;
    }
    sink->arena = malloc(arena_capacity);
    if (sink->arena == NULL) {
        free(sink->effects);
        sink->effects = NULL;
        return -1;
    }
    sink->count = 0;
    sink->capacity = capacity;
    sink->arena_used = 0;
    sink->arena_capacity = arena_capacity;
    return 0;
}

void app_effect_sink_free(AppEffectSink* sink) {
    free(sink->effects);
    sink->effects = NULL;
    free(sink->arena);
    sink->arena = NULL;
    sink->count = 0;
    sink->capacity = 0;
    sink->arena_used = 0;
    sink->arena_capacity = 0;
}

void app_effect_sink_reset(AppEffectSink* sink) {
    sink->count = 0;
    sink->arena_used = 0;
}

/*
    Copies `len` bytes into the arena and reports where they landed. The caller
    checks the effect vector first, so a successful copy is never left orphaned
    by a subsequent append failure.
*/
static int arena_put(AppEffectSink* sink, const char* data, size_t len, size_t* out_offset) {
    if (len > sink->arena_capacity - sink->arena_used) {
        return -1;
    }
    *out_offset = sink->arena_used;
    if (len != 0) {
        memcpy(sink->arena + sink->arena_used, data, len);
        sink->arena_used += len;
    }
    return 0;
}

int app_effect_reply(AppEffectSink* sink, PlayerRef target, HtttpStatus status,
                     const char* body, size_t body_len) {
    if (sink->count == sink->capacity) {
        return -1;
    }

    size_t body_offset;
    if (arena_put(sink, body, body_len, &body_offset) == -1) {
        return -1;
    }

    AppEffect effect = {0};
    effect.kind = APP_EFFECT_REPLY;
    effect.target = target;
    effect.status = status;
    effect.body_offset = body_offset;
    effect.body_len = body_len;

    sink->effects[sink->count++] = effect;
    return 0;
}

int app_effect_event(AppEffectSink* sink, PlayerRef target, const char* method,
                     const char* path, size_t path_len,
                     const char* body, size_t body_len, bool coalescable) {
    if (sink->count == sink->capacity) {
        return -1;
    }

    // one rollback point for both copies, so a half-written event cannot leak
    // arena space when the second copy is the one that does not fit
    const size_t mark = sink->arena_used;

    size_t path_offset;
    size_t body_offset;
    if (arena_put(sink, path, path_len, &path_offset) == -1 ||
        arena_put(sink, body, body_len, &body_offset) == -1) {
        sink->arena_used = mark;
        return -1;
    }

    AppEffect effect = {0};
    effect.kind = APP_EFFECT_EVENT;
    effect.target = target;
    effect.method = method;
    effect.path_offset = path_offset;
    effect.path_len = path_len;
    effect.body_offset = body_offset;
    effect.body_len = body_len;
    effect.coalescable = coalescable;

    sink->effects[sink->count++] = effect;
    return 0;
}

const char* app_effect_body(const AppEffectSink* sink, const AppEffect* effect) {
    return sink->arena + effect->body_offset;
}

const char* app_effect_path(const AppEffectSink* sink, const AppEffect* effect) {
    return sink->arena + effect->path_offset;
}
