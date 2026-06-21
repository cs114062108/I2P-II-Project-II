#include <cstdint>
#include <vector>
#include "state.hpp"
#include "tt.hpp"

/*=============================================================================
 * Transposition Table (TT) Implementation
 *
 * This file defines the core data structures and operations for a high-
 * performance Transposition Table tailored to work with Alpha-Beta and PVS.
 *=============================================================================*/

// Initialize the table with a specific size in MB (e.g., 64MB or 128MB)
TranspositionTable::TranspositionTable(size_t size_mb) {
    size_t bytes = size_mb * 1024 * 1024;
    num_entries = bytes / sizeof(TTEntry);
    
    // Ensure num_entries is a power of two to optimize index lookup via bitwise AND
    if ((num_entries & (num_entries - 1)) != 0) {
        size_t power = 1;
        while (power < num_entries) power <<= 1;
        num_entries = power;
    }
    
    table.resize(num_entries, TTEntry{0, 0, 0, TT_EXACT, 0, Move()});
    current_age = 0;
}

// Advance the age at the start of each search turn to handle old cache eviction
void TransTable::increment_age() {
    current_age++;
}

void TransTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{0, 0, 0, TT_EXACT, 0, Move()});
    current_age = 0;
}

/*=========================================================================
 * PROBE (Lookup)
 *
 * Retrieves an entry from the TT.
 * Returns true if a usable entry is found, and populates output parameters.
 *=========================================================================*/
bool TransTable::probe(uint64_t key, int depth, int ply, int alpha, int beta, int& tt_score, Move& tt_move) {
    size_t index = key & (num_entries - 1); // Fast modulo using bitwise AND
    const TTEntry& entry = table[index];

    if (entry.key == key) {
        tt_move = entry.best_move; // We can ALWAYS use the best move for Move Ordering!

        // Only use the score if it was searched at least as deep as the current request
        if (entry.depth >= depth) {
            int score = entry.score;

            // Adjust Mate Score for Distance Pruning:
            // Since mate scores depend on the distance from the root (ply), we must translate
            // the stored absolute mate score back into a ply-relative mate score.
            if (score > P_MAX - 100) score -= ply;
            else if (score < M_MAX + 100) score += ply;

            if (entry.flag == TT_EXACT) {
                tt_score = score;
                return true;
            }
            if (entry.flag == TT_LOWERBOUND && score >= beta) {
                tt_score = score; // Causes an immediate beta-cutoff
                return true;
            }
            if (entry.flag == TT_UPPERBOUND && score <= alpha) {
                tt_score = score; // Causes an immediate fail-low return
                return true;
            }
        }
    }
    return false;
}

/*=========================================================================
 * STORE (Save)
 *
 * Stores a search result into the TT.
 * Implements a replacement scheme (Depth-Preferred + Age-Preferred).
 *=========================================================================*/
void TransTable::store(uint64_t key, int depth, int ply, int score, TTFlag flag, const Move& best_move) {
    size_t index = key & (num_entries - 1);
    TTEntry& entry = table[index];

    // Adjust Mate Score for Distance Pruning:
    // We must store the absolute distance to mate, independent of the current search ply.
    if (score > P_MAX - 100) score += ply;
    else if (score < M_MAX + 100) score -= ply;

    // Replacement Strategy: 
    // Overwrite if:
    // 1. The slot is empty (key == 0).
    // 2. The new search is deeper or equal (entry.depth <= depth).
    // 3. The entry belongs to a previous search turn (entry.age != current_age).
    bool overwrite = (entry.key == 0) || 
                        (entry.depth <= depth) || 
                        (entry.age != current_age);

    if (overwrite) {
        entry.key = key;
        entry.score = static_cast<int16_t>(score);
        entry.depth = static_cast<uint16_t>(depth);
        entry.flag = flag;
        entry.age = current_age;
        entry.best_move = best_move;
    }
}

/*=============================================================================
 * INTEGRATION EXAMPLE (How to use TT inside Alpha-Beta/PVS eval_ctx)
 *
 * void search_example(State* state, int depth, int alpha, int beta, int ply) {
 *      int tt_score;
 *      Move tt_move;
 *      
 *      // 1. Probe the table
 *      if (tt.probe(state->hash(), depth, ply, alpha, beta, tt_score, tt_move)) {
 *          return tt_score; // Perfect cache hit! Skip searching this entire sub-tree.
 *      }
 *
 *      // 2. Move Ordering (Use the tt_move if available)
 *      // Vector ordered_moves = state->order_moves(..., tt_move);
 *      int best_score = M_MAX;
 *      Move best_action;
 *      
 *      // ... Standard Alpha-Beta Loop ...
 *      // 3. Determine Flag and Store the result
 *      TTFlag flag = TT_EXACT;
 *      if (best_score <= alpha_orig) flag = TT_UPPERBOUND;
 *      else if (best_score >= beta)  flag = TT_LOWERBOUND;
 *      tt.store(state->hash(), depth, ply, best_score, flag, best_action);
 * }
 *=============================================================================*/