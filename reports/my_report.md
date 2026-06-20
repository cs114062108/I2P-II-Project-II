# My Report
## Policy Implementation
### Alpha-Beta Pruning: $\alpha$-$\beta$ 剪枝

### PVS (Principal Variation Search): 主要路徑搜尋

### Quiescence Search: 靜態搜尋
#### What is Horrizon Effect? 什麼是「地平線效應」？
當普通 Alpha-Beta 搜尋達到最大深度（`depth = 0`）時，它會強行終止搜尋並調用靜態評估函數。如果此時盤面上正處於一個「慘烈的交換過程中」（例如：我方的皇后正被對手的卒威脅，下一手就會被吃掉），此時進行評估會得到極不精確的分數。AI 會因為「看不見下一手」而做出愚蠢的決定。
#### 為何 Quiescence Search 能解決這個問題
當 `depth <= 0` 時，不立刻回傳評估分，而是進入一個只搜「捕獲步（Captures）」或「喧鬧步（Noisy moves）」的特殊搜尋階段，直到盤面達到「平靜狀態（Quiet）」再進行評估。
#### Implementation in code
```cpp
/* alphabeta.cpp */
/* static int quiescence(){} */
```
```diff
/* alphabeta.cpp */
/* static int quiescence(){} */
// ...
    // Evaluate the leaf node when maximum search depth is reached
    if (depth <= 0) {
-       int score = state->evaluate(params.use_kp_eval, params.use_eval_mobility, history); 
+       int score = quiescence(state, alpha, beta, history, ply, ctx, p);
        history.pop(state->hash());
        return score;
    }
// ...
```
## I don't know what to write here
