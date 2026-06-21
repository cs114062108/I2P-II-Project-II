#pragma once
#include <cstdint>
#include <vector>
#include "state.hpp"

/*=============================================================================
 * Transposition Table (TT) Implementation
 *
 * This file defines the core data structures and operations for a high-
 * performance Transposition Table tailored to work with Alpha-Beta and PVS.
 *=============================================================================*/

// TT Entry Flags indicating the bound type of the stored score
enum TTFlag : uint8_t {
    TT_EXACT       = 0, // The score is an exact value (PV node)
    TT_LOWERBOUND  = 1, // Beta cutoff caused a lower bound (Fail-High node, score >= beta)
    TT_UPPERBOUND  = 2  // All moves searched, none exceeded alpha (Fail-Low node, score <= alpha)
};

// Represents a single entry in the Transposition Table.
// Keep it compact (ideally 16 bytes) to maximize cache-locality and fit in CPU L1/L2 cache lines.
struct TTEntry {
    uint64_t key;       // 64-bit Zobrist Hash key of the position
    int16_t  score;     // Evaluation score
    uint16_t depth;     // Search depth at which this score was calculated
    TTFlag   flag;      // Exact, Lowerbound, or Upperbound flag
    uint8_t  age;       // Generation counter to implement aging replacement schemes
    Move     best_move; // The best move found in this position (used for Move Ordering first)
};

class TranspositionTable {
private:
    std::vector<TTEntry> table;
    size_t num_entries;
    uint8_t current_age; // Used to invalidate or de-prioritize old entries from previous moves

public:
    // Initialize the table with a specific size in MB (e.g., 64MB or 128MB)
    TranspositionTable(size_t size_mb);

    // Advance the age at the start of each search turn to handle old cache eviction
    void increment_age();

    void clear();

    /*=========================================================================
     * PROBE (Lookup)
     *
     * Retrieves an entry from the TT.
     * Returns true if a usable entry is found, and populates output parameters.
     *=========================================================================*/
    bool probe(uint64_t key, int depth, int ply, int alpha, int beta, int& tt_score, Move& tt_move);

    /*=========================================================================
     * STORE (Save)
     *
     * Stores a search result into the TT.
     * Implements a replacement scheme (Depth-Preferred + Age-Preferred).
     *=========================================================================*/
    void store(uint64_t key, int depth, int ply, int score, TTFlag flag, const Move& best_move);
};

typedef TranspositionTable TransTable;

/*=============================================================================
 * INTEGRATION EXAMPLE (How to use TT inside Alpha-Beta/PVS eval_ctx)
 *
 * void search_example(State* state, int depth, int alpha, int beta, int ply) {
 * int tt_score;
 * Move tt_move;
 * * // 1. Probe the table
 * if (tt.probe(state->hash(), depth, ply, alpha, beta, tt_score, tt_move)) {
 * return tt_score; // Perfect cache hit! Skip searching this entire sub-tree.
 * }
 *
 * // 2. Move Ordering (Use the tt_move if available)
 * // Vector ordered_moves = state->order_moves(..., tt_move);
 * * int best_score = M_MAX;
 * Move best_action;
 * * // ... Standard Alpha-Beta Loop ...
 * * // 3. Determine Flag and Store the result
 * TTFlag flag = TT_EXACT;
 * if (best_score <= alpha_orig) flag = TT_UPPERBOUND;
 * else if (best_score >= beta)  flag = TT_LOWERBOUND;
 * * tt.store(state->hash(), depth, ply, best_score, flag, best_action);
 * }
 *=============================================================================*/