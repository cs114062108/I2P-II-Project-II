#include <utility>
#include <algorithm>
#include "state.hpp"
#include "alphabeta.hpp"

/*============================================================
 * Quiescence Search (quiescence)
 *
 * Search only capture moves to reach a stable state, preventing the Horizon Effect.
 * Includes depth guards and move ordering to prevent search explosion.
 *============================================================*/
static int quiescence(
    State *state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const ABParams& params
) {
    ctx.nodes++;
    // Active check: immediately abort if time is up
    if (ctx.stop) {
        return 0;
    }

    // Establish the "Standing Pat" score as a baseline.
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
    if (ply > 10) { 
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
 * Alpha-Beta — Recursive Evaluation Helper (eval_ctx)
 *
 * Negamax search with Alpha-Beta pruning. Caller manages memory.
 *============================================================*/
static int eval_ctx(
    State *state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const ABParams& params
) {
    ctx.nodes++;
    if (ply > ctx.seldepth) {
        ctx.seldepth = ply;
    }
    // Active check: immediately abort if time is up
    if (ctx.stop) {
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */
    if (state->game_state == WIN) {
        // Prefer shorter paths to win by subtracting ply
        return P_MAX - ply;
    }

    if (state->game_state == DRAW) {
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if (state->check_repetition(history, rep_score)) {
        return rep_score;
    }
    history.push(state->hash());

    // Evaluate the leaf node when maximum search depth is reached
    // Instead of directly calling static evaluation, use Quiescence Search
    if (depth <= 0) {
        //int score = state->evaluate(params.use_kp_eval, params.use_eval_mobility, &history); 
        int score = quiescence(state, alpha, beta, history, ply, ctx, params);
        history.pop(state->hash());
        return score;
    }

    /* === Negamax Alpha-Beta search loop === */
    int best_score = M_MAX; // Initialize with the lowest possible score boundary

    // Apply high-performance Move Ordering using State's member function
    std::vector<Move> ordered_moves = state->order_moves(state->legal_actions);

    for (Move& action : ordered_moves) {
        // Active check inside the loop to prevent continuing evaluation on timeout
        if (ctx.stop) {
            history.pop(state->hash());
            return 0;
        }
        
        // Create the child state after applying the current action
        State *next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();

        int score;
        if (same) {
            // Same player moves (do not negate score or flip alpha/beta window)
            score = eval_ctx(next, depth - 1, alpha, beta, history, ply + 1, ctx, params);
        } else {
            // Standard Negamax alternation (negate score and flip alpha/beta window)
            score = -eval_ctx(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, params);
        }

        // Free dynamically allocated state memory to prevent leaks
        delete next;

        // Track the best score for this state
        if (score > best_score) {
            best_score = score;
        }

        // Alpha cutoff / update: alpha represents the lower bound of what we can guarantee
        if (best_score > alpha) {
            alpha = best_score;
        }

        // Beta pruning: If our best score is greater than or equal to beta (the upper bound 
        // the opponent allows us to get), the opponent will never allow this branch.
        if (alpha >= beta) {
            break;
        }
    }

    history.pop(state->hash());
    return best_score;
}

/*============================================================
 * Alpha-Beta — Root Search Entrypoint (search)
 *
 * Iterate legal moves, call eval_ctx with alpha-beta bounds, and return the best move.
 *============================================================*/
SearchResult AlphaBeta::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
) {
    ctx.reset();
    ABParams params = ABParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (state->legal_actions.empty()) {
        state->get_legal_actions();
    }
    
    // Sort root moves using State's member function to start with the most promising branch
    std::vector<Move> ordered_moves = state->order_moves(state->legal_actions);

    // Setup fallback best move in case we stop immediately or have no time
    if (!state->legal_actions.empty()) {
        result.best_move = ordered_moves[0];
    }

    // Initialize root alpha/beta window bounds
    int alpha = M_MAX - 10;
    int beta = P_MAX + 10;
    int best_score = M_MAX - 10;
    
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for (Move& action : ordered_moves) {
        // If the stop flag was set, immediately abort root loop and return the best move we've finished evaluating
        if (ctx.stop) {
            break;
        }

        // Generate the child state for the root move
        State* next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();
        
        int score;
        if (same) {
            score = eval_ctx(next, depth - 1, alpha, beta, history, 1, ctx, params);
        } else {
            score = -eval_ctx(next, depth - 1, -beta, -alpha, history, 1, ctx, params);
        }

        // Free child state memory after search
        delete next;

        // CRITICAL PROTECTION: If search was aborted during the evaluation, 
        // DO NOT use the returned score to overwrite our best move, as the score is incomplete/corrupted.
        if (ctx.stop) {
            break;
        }

        // Keep this move if it yields a better evaluation score
        if (score > best_score) {
            best_score = score;
            result.best_move = action;

            // Optional partial updates for engine logs / UI integration
            if (params.report_partial && ctx.on_root_update) {
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }

        // Update the root-level alpha bound
        if (best_score > alpha) {
            alpha = best_score;
        }
        
        move_index++;
    }

    result.score = best_score;
    return result;
}

/*============================================================
 * AlphaBeta — default_params / param_defs
 *============================================================*/
ParamMap AlphaBeta::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
