#include <utility>
#include <algorithm>
#include "state.hpp"
#include "pvs.hpp"
#include "tt.hpp" // Include our high-performance Transposition Table
#include <iostream>

// Instantiate a static Transposition Table with 2MB of memory.
// This size is optimal for fast allocations while holding millions of transposition entries.
static TransTable g_tt(2);

/*============================================================
 * Quiescence Search (quiescence)
 *
 * Search only capture moves to reach a stable state, preventing the Horizon Effect.
 *============================================================*/
static int quiescence(
    State *state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const PVSParams& params
) {
    ctx.nodes++;
    // Active check: immediately abort if time is up
    if (ctx.stop) {
        return 0;
    }

    // 1. Establish the "Standing Pat" score as a baseline.
    // Since a player can always choose not to capture anything, this is our lower bound.
    int stand_pat = state->evaluate(params.use_kp_eval, params.use_eval_mobility, &history);
    
    // Beta cutoff
    if (stand_pat >= beta) {
        return stand_pat;
    }
    
    // Update alpha bound
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    // Prevent QS from searching too deep (prevents timeouts/memory explosion)
    if (ply > 8) { 
        return stand_pat;
    }

    // Generate moves if not already available
    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }

    auto oppn_board = state->board.board[1 - state->player];

    // Filter only capture moves for QS
    std::vector<Move> captures;
    for (const auto& action : state->legal_actions) {
        int dest_r = action.second.first;
        int dest_c = action.second.second;
        if (oppn_board[dest_r][dest_c] != 0) {
            captures.push_back(action);
        }
    }

    // Order capture moves using MVV-LVA defined in State class
    std::vector<Move> ordered_captures = state->order_moves(captures);

    // 2. Iterate and evaluate only noisy moves (captures)
    for (Move& action : ordered_captures) {
        // Active check inside the loop to abort quickly during heavy captures
        if (ctx.stop) {
            return 0;
        }

        State *next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();

        int score;
        if (same) {
            score = quiescence(next, alpha, beta, history, ply + 1, ctx, params);
        } else {
            score = -quiescence(next, -beta, -alpha, history, ply + 1, ctx, params);
        }

        delete next;

        if (score >= beta) {
            return score; // Beta cutoff
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    return alpha;
}

/*============================================================
 * PVS — Recursive Evaluation Helper (eval_ctx)
 *
 * Negamax with Principal Variation Search (PVS) / NegaScout.
 *============================================================*/
static int eval_ctx(
    State *state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const PVSParams& params
) {
    ctx.nodes++;
    if (ply > ctx.seldepth) {
        ctx.seldepth = ply;
    }
    // Active check: immediately abort if time is up
    if (ctx.stop) {
        return 0;
    }
    
    /* === Lazy move generation === */
    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */
    if (state->game_state == WIN) {
        return P_MAX - ply; // Prefer shorter paths to victory
    }

    if (state->game_state == DRAW) {
        return 0;
    }

    /* === Repetition check === */
    int rep_score;
    if (state->check_repetition(history, rep_score)) {
        return rep_score;
    }

    // Save the original alpha bound to correctly classify the TT entry flag later
    int alpha_orig = alpha;

    // 1. Probe Transposition Table
    int tt_score = 0;
    Move tt_move;
    bool tt_hit = g_tt.probe(state->hash(), depth, ply, alpha, beta, tt_score, tt_move);
    if (tt_hit) {
        return tt_score; // Perfect cache hit! Bypass searching this entire sub-tree.
    }

    history.push(state->hash());

    // Evaluate the leaf node using Quiescence Search
    if (depth <= 0) {
        //int score = state->evaluate(params.use_kp_eval, params.use_eval_mobility, &history);
        int score = quiescence(state, alpha, beta, history, ply, ctx, params);
        history.pop(state->hash());
        return score;
    }

    /* === Move Ordering === */
    // Standard move ordering using MVV-LVA
    // Apply high-performance Move Ordering using State's member function
    std::vector<Move> ordered_moves = state->order_moves(state->legal_actions);
    
    // Dynamic Move Ordering: If the TT recommended a best move from previous searches,
    // we bubble it to the absolute front of the list, overriding any static evaluation.
    if (tt_move != Move()) {
        auto it = std::find(ordered_moves.begin(), ordered_moves.end(), tt_move);
        if (it != ordered_moves.end()) {
            Move m = *it;
            ordered_moves.erase(it);
            ordered_moves.insert(ordered_moves.begin(), m);
        }
    }

    /* === PVS Negamax loop === */
    int best_score = M_MAX;
    Move best_move;
    bool is_first_move = true;

    for (Move& action: ordered_moves) {
        // Active check inside the loop to prevent continuing evaluation on timeout
        if (ctx.stop) {
            history.pop(state->hash());
            return 0;
        }

        State *next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();

        int score;
        if (is_first_move) {
            /* === First move: Search with full window === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, beta, history, ply + 1, ctx, params);
            } else {
                score = -eval_ctx(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, params);
            }
            is_first_move = false;
        } else {
            /* === Subsequent moves: Search with narrow/null window [alpha, alpha + 1] === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, alpha + 1, history, ply + 1, ctx, params);
                
                // Fail-high: If the null-window search yields a score better than alpha, re-search with full window
                if (score > alpha && score < beta) {
                    score = eval_ctx(next, depth - 1, score, beta, history, ply + 1, ctx, params);
                }
            } else {
                score = -eval_ctx(next, depth - 1, -alpha - 1, -alpha, history, ply + 1, ctx, params);
                
                // Fail-high: Re-search with full window from the perspective of the current player
                if (score > alpha && score < beta) {
                    score = -eval_ctx(next, depth - 1, -beta, -score, history, ply + 1, ctx, params);
                }
            }
        }

        delete next;

        if (score > best_score) {
            best_score = score;
            best_move = action;
        }

        if (best_score > alpha) {
            alpha = best_score;
        }

        // Alpha-Beta Pruning boundary check
        if (alpha >= beta) {
            break;
        }
    }

    history.pop(state->hash());

    // 2. Store search results back to Transposition Table
    // Do not save to cache if the search was aborted, as the score would be incomplete/corrupted.
    if (!ctx.stop) {
        TTFlag flag = TT_EXACT;
        if (best_score <= alpha_orig) {
            flag = TT_UPPERBOUND;
        } else if (best_score >= beta) {
            flag = TT_LOWERBOUND;
        }
        g_tt.store(state->hash(), depth, ply, best_score, flag, best_move);
    }

    return best_score;
}

/*============================================================
 * PVS — Root Search Entrypoint (search)
 *============================================================*/
SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
) {
    std::cout << "Root hash: " << state->hash() << std::endl;
    
    ctx.reset();
    PVSParams params = PVSParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (state->legal_actions.empty()) {
        state->get_legal_actions();
    }

    // Advance TT age at the start of each new search iteration to handle cache aging eviction
    g_tt.increment_age();

    // Probe the root state to fetch potential best_move cached from previous depths
    int tt_score = 0;
    Move tt_move;
    g_tt.probe(state->hash(), depth, 0, M_MAX - 10, P_MAX + 10, tt_score, tt_move);

    // Setup fallback best move in case of immediate stop
    if (!state->legal_actions.empty()) {
        result.best_move = state->legal_actions[0];
    }
    // If we have a cached best move from previous searches, prioritize using it as the primary fallback
    if (tt_move != Move()) {
        auto it = std::find(state->legal_actions.begin(), state->legal_actions.end(), tt_move);
        if (it != state->legal_actions.end()) {
            result.best_move = *it;
        }
    }

    int alpha = M_MAX - 10;
    int beta = P_MAX + 10;
    int best_score = M_MAX - 10;
    
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool is_first_move = true;

    // Sort root moves using State's member function to start with the most promising branch
    std::vector<Move> ordered_moves = state->order_moves(state->legal_actions);

    // Bubble up the TT move to the very front at the root level
    if (tt_move != Move()) {
        auto it = std::find(ordered_moves.begin(), ordered_moves.end(), tt_move);
        if (it != ordered_moves.end()) {
            Move m = *it;
            ordered_moves.erase(it);
            ordered_moves.insert(ordered_moves.begin(), m);
        }
    }

    for (Move& action: ordered_moves) {
        // If the stop flag was set, immediately abort root loop and return the best move we've finished evaluating
        if (ctx.stop) {
            break;
        }

        State* next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();
        
        int score;
        if (is_first_move) {
            /* === Root First move: Full window search === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, beta, history, 1, ctx, params);
            } else {
                score = -eval_ctx(next, depth - 1, -beta, -alpha, history, 1, ctx, params);
            }
            is_first_move = false;
        } else {
            /* === Root Subsequent moves: Null window search first === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, alpha + 1, history, 1, ctx, params);
                if (score > alpha && score < beta) {
                    score = eval_ctx(next, depth - 1, score, beta, history, 1, ctx, params);
                }
            } else {
                score = -eval_ctx(next, depth - 1, -alpha - 1, -alpha, history, 1, ctx, params);
                if (score > alpha && score < beta) {
                    score = -eval_ctx(next, depth - 1, -beta, -score, history, 1, ctx, params);
                }
            }
        }

        delete next;

        // CRITICAL PROTECTION: If search was aborted during the evaluation, 
        // DO NOT use the returned score to overwrite our best move, as the score is incomplete/corrupted.
        if (ctx.stop) {
            break;
        }

        if (score > best_score) {
            best_score = score;
            result.best_move = action;

            if (params.report_partial && ctx.on_root_update) {
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
        }
        
        move_index++;
    }

    result.score = best_score;

    // Cache the best move at the root level if the search was fully completed
    if (!ctx.stop && best_score > M_MAX - 10) {
        g_tt.store(state->hash(), depth, 0, best_score, TT_EXACT, result.best_move);
    }

    return result;
}

/*============================================================
 * PVS — default_params / param_defs
 *============================================================*/
ParamMap PVS::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> PVS::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}