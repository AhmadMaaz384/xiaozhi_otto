#include "tic_tac_toe.h"

#include <algorithm>

namespace {

// The 8 ways to win: 3 rows, 3 columns, 2 diagonals.
constexpr int kLines[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},  // rows
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},  // columns
    {0, 4, 8}, {2, 4, 6},             // diagonals
};

TicTacToe::Result EvaluateBoard(const TicTacToe::Cell* board) {
    for (const auto& line : kLines) {
        TicTacToe::Cell a = board[line[0]], b = board[line[1]], c = board[line[2]];
        if (a != TicTacToe::Cell::kEmpty && a == b && b == c) {
            return a == TicTacToe::Cell::kX ? TicTacToe::Result::kXWins : TicTacToe::Result::kOWins;
        }
    }
    for (int i = 0; i < 9; i++) {
        if (board[i] == TicTacToe::Cell::kEmpty) {
            return TicTacToe::Result::kInProgress;
        }
    }
    return TicTacToe::Result::kDraw;
}

// Score from O's (Otto's) perspective: positive = good for O, negative =
// good for X. `depth` is folded into the score so Otto prefers a faster win
// and a slower loss, rather than being indifferent between winning now vs.
// winning in 3 moves.
int Minimax(TicTacToe::Cell* board, bool maximizing, int depth) {
    switch (EvaluateBoard(board)) {
        case TicTacToe::Result::kOWins:
            return 10 - depth;
        case TicTacToe::Result::kXWins:
            return depth - 10;
        case TicTacToe::Result::kDraw:
            return 0;
        case TicTacToe::Result::kInProgress:
            break;
    }

    int best = maximizing ? -100 : 100;
    for (int i = 0; i < 9; i++) {
        if (board[i] != TicTacToe::Cell::kEmpty) {
            continue;
        }
        board[i] = maximizing ? TicTacToe::Cell::kO : TicTacToe::Cell::kX;
        int score = Minimax(board, !maximizing, depth + 1);
        board[i] = TicTacToe::Cell::kEmpty;
        best = maximizing ? std::max(best, score) : std::min(best, score);
    }
    return best;
}

}  // namespace

void TicTacToe::Reset() {
    for (auto& cell : board_) {
        cell = Cell::kEmpty;
    }
}

bool TicTacToe::PlayHuman(int cell_number) {
    if (cell_number < 1 || cell_number > 9 || IsGameOver()) {
        return false;
    }
    int index = cell_number - 1;
    if (board_[index] != Cell::kEmpty) {
        return false;
    }
    board_[index] = Cell::kX;
    return true;
}

int TicTacToe::PlayOtto() {
    if (IsGameOver()) {
        return 0;
    }

    int best_score = -100;
    int best_index = -1;
    for (int i = 0; i < 9; i++) {
        if (board_[i] != Cell::kEmpty) {
            continue;
        }
        board_[i] = Cell::kO;
        int score = Minimax(board_, false, 0);
        board_[i] = Cell::kEmpty;
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }
    if (best_index < 0) {
        return 0;  // board was already full
    }
    board_[best_index] = Cell::kO;
    return best_index + 1;
}

TicTacToe::Cell TicTacToe::GetCell(int cell_number) const {
    if (cell_number < 1 || cell_number > 9) {
        return Cell::kEmpty;
    }
    return board_[cell_number - 1];
}

TicTacToe::Result TicTacToe::GetResult() const {
    return EvaluateBoard(board_);
}
