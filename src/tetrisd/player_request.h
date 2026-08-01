#ifndef TETRISH_TETRISD_PLAYER_REQUEST_H
#define TETRISH_TETRISD_PLAYER_REQUEST_H

#include "htttp.h"
#include "type.h"
int player_handle_request(PlayerFdData* player, HtttpRequest* request, Server* server, HtttpResponse* response, HtttpMessageOwnership* ownership);

#endif
