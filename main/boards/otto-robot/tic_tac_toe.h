#pragma once

// Self-contained tic-tac-toe game state and AI. No LVGL/ESP-IDF dependency -
// the display (otto_emoji_display.*) only ever gets told "draw mark M in
// cell N", it never sees this class. Cells are numbered 1-9, phone-keypad
// style (1-2-3 top row, 4-5-6 middle, 7-8-9 bottom), since that's the
// standard convention for playing by voice with no board to point at.
class TicTacToe {
public:
    enum class Cell { kEmpty, kX, kO };
    enum class Result { kInProgress, kXWins, kOWins, kDraw };

    TicTacToe() { Reset(); }

    void Reset();

    // Marks cell_number (1-9) as X for the human player. Returns false
    // without changing anything if cell_number is out of range, already
    // taken, or the game is already over.
    bool PlayHuman(int cell_number);

    // Picks an empty cell and marks it O for Otto, using a full minimax
    // search (the board is tiny - 9 cells - so an exhaustive search is
    // cheap; there's no need for heuristics or alpha-beta pruning here).
    // Returns the 1-9 cell number it played, or 0 if the game is already
    // over / the board is already full.
    int PlayOtto();

    Cell GetCell(int cell_number) const;  // cell_number: 1-9
    Result GetResult() const;
    bool IsGameOver() const { return GetResult() != Result::kInProgress; }

private:
    // Index 0-8 = cell number 1-9.
    Cell board_[9];
};
