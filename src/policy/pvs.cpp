#include <utility>
#include <algorithm>
#include "state.hpp"
#include "pvs.hpp"

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
    history.push(state->hash());

    if (depth <= 0) {
        int score = state->evaluate(
            params.use_kp_eval, params.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === PVS Negamax loop === */
    int best_score = M_MAX;
    bool is_first_move = true;

    for (Move& action : state->legal_actions) {
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
    ctx.reset();
    PVSParams p = PVSParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (state->legal_actions.empty()) {
        state->get_legal_actions();
    }

    int alpha = M_MAX - 10;
    int beta = P_MAX + 10;
    int best_score = M_MAX - 10;
    
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool is_first_move = true;

    for (auto& action : state->legal_actions) {
        State* next = static_cast<State*>(state->next_state(action));
        bool same = next->same_player_as_parent();
        
        int score;
        if (is_first_move) {
            /* === Root First move: Full window search === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, beta, history, 1, ctx, p);
            } else {
                score = -eval_ctx(next, depth - 1, -beta, -alpha, history, 1, ctx, p);
            }
            is_first_move = false;
        } else {
            /* === Root Subsequent moves: Null window search first === */
            if (same) {
                score = eval_ctx(next, depth - 1, alpha, alpha + 1, history, 1, ctx, p);
                if (score > alpha && score < beta) {
                    score = eval_ctx(next, depth - 1, score, beta, history, 1, ctx, p);
                }
            } else {
                score = -eval_ctx(next, depth - 1, -alpha - 1, -alpha, history, 1, ctx, p);
                if (score > alpha && score < beta) {
                    score = -eval_ctx(next, depth - 1, -beta, -score, history, 1, ctx, p);
                }
            }
        }

        delete next;

        if (score > best_score) {
            best_score = score;
            result.best_move = action;

            if (p.report_partial && ctx.on_root_update) {
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
        }
        
        move_index++;
    }

    result.score = best_score;
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