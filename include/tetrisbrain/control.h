#ifndef TETRISH_BRAIN_CONTROL_H
#define TETRISH_BRAIN_CONTROL_H

#include "tetrisbrain/input.h"
#include "tetrisbrain/state.h"

bool apply_spawn(State* s);
bool apply_player_inputs(State* s, const bool (*inputs)[PLAYER_INPUT_KEY_COUNT]);
void apply_attack(State* src, State* dest);

#endif