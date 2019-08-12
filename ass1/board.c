#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "deck.h"
#include "board.h"
#include "util.h"

// see header
bool is_on_board(BoardState* boardState, int r, int c) {
    return (0 <= r && r < boardState->height)
            && (0 <= c && c < boardState->width);
}

// see header
void init_board(BoardState* boardState, int width, int height) {
    boardState->board = malloc(sizeof(Card) * width * height);
    boardState->width = width;
    boardState->height = height;
    boardState->numPlaced = 0;

    for (int i = 0; i < width * height; i++) {
        // printf("initialising %d to null\n", i);
        boardState->board[i] = NULL_CARD;
    }
}

/* Returns true if there exists a card at the row/col. */
bool has_card_at(BoardState* boardState, int row, int col) {
    assert(is_on_board(boardState, row, col));
    return !is_null_card(*get_board_cell(boardState, row, col));
}

// used when loading saved file.
void count_cards(BoardState* boardState) {
    int count = 0;
    for (int r = 0; r < boardState->height; r++) {
        for (int c = 0; c < boardState->width; c++) {
            if (has_card_at(boardState, r, c)) {
                count++;
            }
        }
    }
    DEBUG_PRINTF("counted %d cards on board\n", count);
    boardState->numPlaced = count;
}

// pointer allows us to set the card the row/col cell.
Card* get_board_cell(BoardState* boardState, int row, int col) {
    assert(is_on_board(boardState, row, col));
    return boardState->board + row * boardState->width + col; // get_board_cell
}

// WARNING: lazy implementation. for negatives, only valid up to -d.
// This is fine for the uses here because we only use it to wrap around.
int mod(int x, int d) {
    if (x < 0) {
        return x + d;
    } else {
        return x % d;
    }
}

/* Returns true if there exists a card adjacent to the given row/col. */
bool has_adjacent(BoardState* boardState, int row, int col) {
    assert(is_on_board(boardState, row, col));
    int w = boardState->width;
    int h = boardState->height;
    // because board wraps around, we use mod.
    // if this was python, i'd use an iterator.
    return has_card_at(boardState, mod(row - 1, h), mod(col, w))
            || has_card_at(boardState, mod(row + 1, h), mod(col, w))
            || has_card_at(boardState, mod(row, h), mod(col - 1, w))
            || has_card_at(boardState, mod(row, h), mod(col + 1, w));
}

// see header
bool place_card(BoardState* boardState, int row, int col, Card card) {
    assert(is_on_board(boardState, row, col));
    // if already card at this pos, fail.
    if (has_card_at(boardState, row, col)) {
        return false;
    }
    // require either adjacent card or board is empty.
    if (!has_adjacent(boardState, row, col) && !is_board_empty(boardState)) {
        return false;
    }
    *get_board_cell(boardState, row, col) = card;
    boardState->numPlaced++;
    return true;
}

// see header
void print_board(BoardState* boardState) {
    fprint_board(boardState, stdout, BLANK_CHAR_PRINT);
}

// see header
bool fprint_board(BoardState* boardState, FILE* file, char blank) {
    int w = boardState->width;
    int h = boardState->height;
    // this code is made to print fast, which is why it is so ugly.
    // 2 chars per card on the board, one for newine per row and 1 for \0.
    char* str = malloc(sizeof(char) * 2 * w * h + h + 1);
    int pos = 0; // keep track of our position through the allocated str.
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            fmt_card_c(str + pos,
                    *get_board_cell(boardState, r, c), BLANK_CHAR_PRINT);
            pos += 2;
        }
        str[pos] = '\n';
        pos++;
    }
    str[pos] = '\0';
    bool success = fprintf(file, "%s", str) >= 0;
    free(str);
    return success;
}

// see header
bool is_board_full(BoardState* boardState) {
    // efficient checking using number of cards placed
    return boardState->numPlaced == boardState->width * boardState->height;
}

// see header
bool is_board_empty(BoardState* boardState) {
    return boardState->numPlaced == 0;
}

// see header
bool is_size_valid(int width, int height) {
    return MIN_SIZE <= width && width <= MAX_SIZE &&
            MIN_SIZE <= height && height <= MAX_SIZE;
}

