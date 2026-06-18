#include <utility>
#include "state.hpp"
#include "alphabeta.hpp"

/*===========================================================
 * Alpha-Beta - Recursive Evaluation Helper (eval_ctx)
 *
 * Negamax search with Alpha-Beta pruning. Caller manages memory.
 *===========================================================*/
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
    if (ctx.stop) {
        return 0;
    }

    /* === Lazy move generation (sets game state) === */
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
    if (depth <= 0) {
        int score = state->evaluate(params.use_kp_eval, params.use_eval_mobility, &history);
        history.pop(state->hash());
        return score;
    }

    /* === Negamax Alpha-Beta search loop === */
    int best_score = M_MAX; // Initialize with the lowest possible score boundary

    for (Move& action: state->legal_actions) {
        // Create the child state after applying the current action
        State *next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();

        int score;
        if (same) {
            // Same player moves (do not negate score or flip alpha/beta window)
            score = eval_ctx(next, depth-1, alpha, beta, history, ply+1, ctx, params);
        } else {
            // Standard Negamax alternation (negate score and flip alpha/beta window)
            score = -eval_ctx(next, depth-1, -beta, -alpha, history, ply+1, ctx, params);
        }

        // Free dynamically allocated state memory to prevent leaks
        delete next;

        // Trace the best score for this state
        if (score > best_score) {
            best_score = score;
        }

        // Alpha update: alpha represents the lower bound of what we can guarantee
        if (best_score > alpha) {
            alpha = best_score;
        }

        // Beta pruning (Beta cutoff)
        // If our lower bound exceeds or meets the upper bound, the opponent will avoid this branch
        if (alpha >= beta) {
            break;
        }
    }

    history.pop(state->hash());
    return best_score;
}


/*===========================================================
 * Alpha-Beta - Root Search Entrypoint (search)
 *
 * Iterate legal move, call eval_ctx with alpha-beta bounds, and return the best move
 *===========================================================*/
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

    if (!state->legal_actions.size()) {
        state->get_legal_actions();
    }

    // Initialize root alpha/beta window bounds
    int alpha = M_MAX - 10;
    int beta = P_MAX + 10;
    int best_score = M_MAX - 10;

    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for (Move& action: state->legal_actions) {
        // Generate the child state for the root move
        State *next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();

        int score;
        if (same) {
            score = eval_ctx(next, depth-1, alpha, beta, history, 1, ctx, params);
        } else {
            score = -eval_ctx(next, depth-1, -beta, -alpha, history, 1, ctx, params);
        }

        // Free child state memory after search
        delete next;

        // Keep this move if it yields a better evaluation score
        if (score > best_score) {
            best_score = score;
            result.best_move = action;

            // Optional partial updates for engine logs / UI integration
            if (params.report_partial && ctx.on_root_update) {
                ctx.on_root_update({result.best_move, best_score, depth, move_index+1, total_moves});
            }

            // Update the root-level alpha bound
            if (best_score > alpha) {
                alpha = best_score;
            }

            move_index++;
        }
    }

    result.score = best_score;
    return result;
}

/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
