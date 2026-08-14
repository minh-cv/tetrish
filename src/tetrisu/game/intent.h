#ifndef TETRISH_TETRISU_GAME_INTENT_H
#define TETRISH_TETRISU_GAME_INTENT_H

/*!
    @brief A user-visible operation in tetrisd's gameplay protocol.

    Directional variants are separate values so a parsed intent is complete:
    neither the reducer nor the network layer has to reinterpret text.
*/
typedef enum {
    GAME_INTENT_CREATE,
    GAME_INTENT_JOIN,
    GAME_INTENT_START,
    GAME_INTENT_MOVE_LEFT,
    GAME_INTENT_MOVE_RIGHT,
    GAME_INTENT_ROTATE_CW,
    GAME_INTENT_ROTATE_CCW,
    GAME_INTENT_DROP_SOFT,
    GAME_INTENT_DROP_HARD,
    GAME_INTENT_PAUSE,
    GAME_INTENT_HOLD,
    GAME_INTENT_LEAVE,
    /*
        After LEAVE deliberately: game_intent_is_input bounds the one-way
        inputs by the MOVE_LEFT..HOLD range, and a browse is not one of them.
    */
    GAME_INTENT_ROOM_LIST,
} GameIntentType;

/*!
    @brief report whether @p intent is a one-way in-game input
    @post no state changes
*/
static inline int game_intent_is_input(GameIntentType intent) {
    return intent >= GAME_INTENT_MOVE_LEFT && intent <= GAME_INTENT_HOLD;
}

#endif
