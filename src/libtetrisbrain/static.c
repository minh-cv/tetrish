#include "tetrisbrain/static.h"

const TetrominoDirection TETROMINO_RIGHT_DIRECTION[TETROMINO_DIRECTION_COUNT] = {
    [TETROMINO_DIRECTION_0] = TETROMINO_DIRECTION_R,
    [TETROMINO_DIRECTION_R] = TETROMINO_DIRECTION_2,
    [TETROMINO_DIRECTION_2] = TETROMINO_DIRECTION_L,
    [TETROMINO_DIRECTION_L] = TETROMINO_DIRECTION_0,
};

const TetrominoDirection TETROMINO_LEFT_DIRECTION[TETROMINO_DIRECTION_COUNT] = {
    [TETROMINO_DIRECTION_0] = TETROMINO_DIRECTION_L,
    [TETROMINO_DIRECTION_R] = TETROMINO_DIRECTION_0,
    [TETROMINO_DIRECTION_2] = TETROMINO_DIRECTION_R,
    [TETROMINO_DIRECTION_L] = TETROMINO_DIRECTION_2,
};

const int TETROMINO_OFFSET[TETROMINO_TYPE_COUNT][TETROMINO_DIRECTION_COUNT][5][2] = {
    [TETROMINO_TYPE_L] = {
        [TETROMINO_DIRECTION_0] = {{0, 0}, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, 0, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {0, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
    [TETROMINO_TYPE_J] = {
        [TETROMINO_DIRECTION_0] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, 0, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {0, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
    [TETROMINO_TYPE_S] = {
        [TETROMINO_DIRECTION_0] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, 0, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {0, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
    [TETROMINO_TYPE_T] = {
        [TETROMINO_DIRECTION_0] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, 0, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {0, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
    [TETROMINO_TYPE_Z] = {
        [TETROMINO_DIRECTION_0] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, 0, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {0, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
    [TETROMINO_TYPE_I] = {
        [TETROMINO_DIRECTION_0] = { 0, 0,	-1, 0,	+2, 0,	-1, 0,	+2, 0,},
        [TETROMINO_DIRECTION_R] = {-1, 0,	 0, 0,	 0, 0,	 0, +1,	 0, -2,},
        [TETROMINO_DIRECTION_2] = {-1, +1,	+1, +1,	-2, +1,	+1, 0,	-2, 0,},
        [TETROMINO_DIRECTION_L] = { 0, +1,	 0, +1,	 0, +1,	 0, -1,	 0, +2,},
    },
    [TETROMINO_TYPE_O] = {
        [TETROMINO_DIRECTION_0] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_R] = {0, -1, 1, 0, 1, -1, 0, 2, 1, 2,},
        [TETROMINO_DIRECTION_2] = {-1, -1, 0, 0, 0, 0, 0, 0, 0, 0,},
        [TETROMINO_DIRECTION_L] = {-1, 0, -1, 0, -1, -1, 0, 2, -1, 2,},
    },
};

#define TETROMINO_I_BOX_SIZE 5
#define TETROMINO_JLOSTZ_BOX_SIZE 3

const int TETROMINO_BOX_SIZE[TETROMINO_TYPE_COUNT] = {
    [TETROMINO_TYPE_I] = TETROMINO_I_BOX_SIZE,
    [TETROMINO_TYPE_L] = TETROMINO_JLOSTZ_BOX_SIZE,
    [TETROMINO_TYPE_O] = TETROMINO_JLOSTZ_BOX_SIZE,
    [TETROMINO_TYPE_S] = TETROMINO_JLOSTZ_BOX_SIZE,
    [TETROMINO_TYPE_T] = TETROMINO_JLOSTZ_BOX_SIZE,
    [TETROMINO_TYPE_J] = TETROMINO_JLOSTZ_BOX_SIZE,
    [TETROMINO_TYPE_Z] = TETROMINO_JLOSTZ_BOX_SIZE,

};

static const bool TETROMINO_I_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_I_BOX_SIZE][TETROMINO_I_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 1, 1, 1, 1,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 0, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        1, 1, 1, 1, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
    },
    [TETROMINO_DIRECTION_L] = {
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, 0, 0,
    },
};

static const bool TETROMINO_O_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        0, 1, 1,
        0, 1, 1,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 0, 0,
        0, 1, 1,
        0, 1, 1,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        1, 1, 0,
        1, 1, 0,
    },
    [TETROMINO_DIRECTION_L] = {
        1, 1, 0,
        1, 1, 0,
        0, 0, 0,
    },
};

static const bool TETROMINO_L_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        0, 0, 1,
        1, 1, 1,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 1, 0,
        0, 1, 0,
        0, 1, 1,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        1, 1, 1,
        1, 0, 0,
    },
    [TETROMINO_DIRECTION_L] = {
        1, 1, 0,
        0, 1, 0,
        0, 1, 0,
    },
};

static const bool TETROMINO_J_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        1, 0, 0,
        1, 1, 1,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 1, 1,
        0, 1, 0,
        0, 1, 0,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        1, 1, 1,
        0, 0, 1,
    },
    [TETROMINO_DIRECTION_L] = {
        0, 1, 0,
        0, 1, 0,
        1, 1, 0,
    },
};

static const bool TETROMINO_S_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        0, 1, 1,
        1, 1, 0,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 1, 0,
        0, 1, 1,
        0, 0, 1,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        0, 1, 1,
        1, 1, 0,
    },
    [TETROMINO_DIRECTION_L] = {
        1, 0, 0,
        1, 1, 0,
        0, 1, 0,
    },
};

static const bool TETROMINO_Z_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        1, 1, 0,
        0, 1, 1,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 0, 1,
        0, 1, 1,
        0, 1, 0,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        1, 1, 0,
        0, 1, 1,
    },
    [TETROMINO_DIRECTION_L] = {
        0, 1, 0,
        1, 1, 0,
        1, 0, 0,
    },
};

static const bool TETROMINO_T_CELL[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE] = {
    [TETROMINO_DIRECTION_0] = {
        0, 1, 0,
        1, 1, 1,
        0, 0, 0,
    },
    [TETROMINO_DIRECTION_R] = {
        0, 1, 0,
        0, 1, 1,
        0, 1, 0,
    },
    [TETROMINO_DIRECTION_2] = {
        0, 0, 0,
        1, 1, 1,
        0, 1, 0,
    },
    [TETROMINO_DIRECTION_L] = {
        0, 1, 0,
        1, 1, 0,
        0, 1, 0,
    },
};


const void* const TETROMINO_CELL[TETROMINO_TYPE_COUNT] = {
    [TETROMINO_TYPE_L] = &TETROMINO_L_CELL,
    [TETROMINO_TYPE_O] = &TETROMINO_O_CELL,
    [TETROMINO_TYPE_I] = &TETROMINO_I_CELL,
    [TETROMINO_TYPE_S] = &TETROMINO_S_CELL,
    [TETROMINO_TYPE_T] = &TETROMINO_T_CELL,
    [TETROMINO_TYPE_J] = &TETROMINO_J_CELL,
    [TETROMINO_TYPE_Z] = &TETROMINO_Z_CELL,
};

const int TETROMINO_STARTING_ROW[TETROMINO_TYPE_COUNT] = {
    [TETROMINO_TYPE_I] = 17, // 22,
    [TETROMINO_TYPE_L] = 18,
    [TETROMINO_TYPE_O] = 18,
    [TETROMINO_TYPE_S] = 18,
    [TETROMINO_TYPE_T] = 18,
    [TETROMINO_TYPE_J] = 18,
    [TETROMINO_TYPE_Z] = 18,
};

const int TETROMINO_STARTING_COL[TETROMINO_TYPE_COUNT] = {
    [TETROMINO_TYPE_I] = 2,
    [TETROMINO_TYPE_L] = 3,
    [TETROMINO_TYPE_O] = 3,
    [TETROMINO_TYPE_S] = 3,
    [TETROMINO_TYPE_T] = 3,
    [TETROMINO_TYPE_J] = 3,
    [TETROMINO_TYPE_Z] = 3,
};

static bool is_tetromino_i_cell_occupied(TetrominoType type, TetrominoDirection direction, int row, int col) {
    (void)type;
    return (*(const bool(*)[TETROMINO_DIRECTION_COUNT][TETROMINO_BOX_SIZE[TETROMINO_TYPE_I]][TETROMINO_BOX_SIZE[TETROMINO_TYPE_I]])
        (TETROMINO_CELL[TETROMINO_TYPE_I]))[direction][row][col];
}

static bool is_tetromino_jlostz_cell_occupied(TetrominoType type, TetrominoDirection direction, int row, int col) {
    return (*(const bool(*)[TETROMINO_DIRECTION_COUNT][TETROMINO_JLOSTZ_BOX_SIZE][TETROMINO_JLOSTZ_BOX_SIZE])
        (TETROMINO_CELL[type]))[direction][row][col];
}

bool (* const IS_TETROMINO_CELL_OCCUPIED[TETROMINO_TYPE_COUNT])(TetrominoType type, TetrominoDirection direction, int row, int col) = {
    [TETROMINO_TYPE_I] = is_tetromino_i_cell_occupied,
    [TETROMINO_TYPE_L] = is_tetromino_jlostz_cell_occupied,
    [TETROMINO_TYPE_O] = is_tetromino_jlostz_cell_occupied,
    [TETROMINO_TYPE_S] = is_tetromino_jlostz_cell_occupied,
    [TETROMINO_TYPE_T] = is_tetromino_jlostz_cell_occupied,
    [TETROMINO_TYPE_J] = is_tetromino_jlostz_cell_occupied,
    [TETROMINO_TYPE_Z] = is_tetromino_jlostz_cell_occupied,
};

const int VISIBLE_ROW_IDX = 20;