#ifndef LONGESTPATH_H
#define LONGESTPATH_H

#include "board.h"

typedef struct Position {
    int r;
    int c;
} Position;

int compute_longest_path(BoardState* boardState, char target, Position pos,
        int length);

#endif
