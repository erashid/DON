#ifndef _PGNFILE_H_INC_
#define _PGNFILE_H_INC_

#include "Type.h"

#define STRING_SIZE 256

typedef struct pgn_t
{
    FILE *file;

    int char_hack;
    int char_line;
    int char_column;
    bool char_unread;
    bool char_first;

    int token_type;
    char token_string[STRING_SIZE];
    int token_length;
    int token_line;
    int token_column;
    bool token_unread;
    bool token_first;

    char result[STRING_SIZE];
    char fen[STRING_SIZE];

    char WhiteELO[STRING_SIZE];
    char BlackELO[STRING_SIZE];

    int move_line;
    int move_column;
    int game_nb;

} pgn_t;

extern void open_pgn  (pgn_t *pgn, const char *fn_pgn);
extern void close_pgn (pgn_t *pgn);

extern bool next_game_pgn (pgn_t *pgn);
extern bool next_move_pgn (pgn_t *pgn, char *move_s, int size);

#endif