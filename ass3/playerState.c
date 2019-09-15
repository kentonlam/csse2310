#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "playerState.h"

// see header
void ps_init(PlayerState* playerState, GameState* gameState, int playerIndex) {
    playerState->playerIndex = playerIndex;
    playerState->gameState = gameState;
    playerState->hand = NULL; // initialised when hand set.
}

// see header
void ps_destroy(PlayerState* playerState) {
    if (playerState->gameState != NULL) {
        gs_destroy(playerState->gameState);
    }
    if (playerState->hand != NULL) {
        deck_destroy(playerState->hand);
    }
}

// see header
void ps_set_hand(PlayerState* playerState, Deck* hand) {
    playerState->hand = hand;
}

// see header
void ps_play(PlayerState* playerState, Card card) {
    assert(playerState->playerIndex == playerState->gameState->currPlayer);
    deck_remove_card(playerState->hand, card);
}