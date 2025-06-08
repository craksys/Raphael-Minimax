#include <GameEngine/consts.h>
#include <GameEngine/utils.h>
#include <Raphael/Raphaelv2.0.h>
#include <Raphael/SEE.h>
#include <Raphael/consts.h>
#include <math.h>

#include <chess.hpp>
#include <future>
#include <iomanip>

using namespace Raphael;
using std::async, std::ref;
using std::cout, std::flush;
using std::fixed, std::setprecision;
using std::max, std::min;
using std::mutex, std::lock_guard;
using std::string;
namespace ch = std::chrono;

extern const bool UCI;



v2_0::RaphaelParams::RaphaelParams() {}


v2_0::v2_0(string name_in): GamePlayer(name_in), tt(DEF_TABLE_SIZE) {}
v2_0::v2_0(string name_in, EngineOptions options)
    : GamePlayer(name_in), tt(options.tablesize), net() {}


void v2_0::set_options(EngineOptions options) { tt = TranspositionTable(options.tablesize); }

void v2_0::set_searchoptions(SearchOptions options) { searchopt = options; }


chess::Move v2_0::get_move(
    chess::Board board,
    const int t_remain,
    const int t_inc,
    volatile sf::Event& event,
    volatile bool& halt
) {
    int depth = 1;
    int eval = 0;
    int alpha = -INT_MAX;
    int beta = INT_MAX;
    history.clear();

    // set up nnue board
    net.set_board(board);

    // if ponderhit, start with ponder result and depth
    if (board.hash() != ponderkey) {
        itermove = chess::Move::NO_MOVE;
        prevPlay = chess::Move::NO_MOVE;
        consecutives = 1;
        nodes = 0;
    } else {
        depth = ponderdepth;
        eval = pondereval;
        alpha = eval - params.ASPIRATION_WINDOW;
        beta = eval + params.ASPIRATION_WINDOW;
    }

    // stop search after an appropriate duration
    start_search_timer(board, t_remain, t_inc);

    // begin iterative deepening
    while (!halt && depth <= MAX_DEPTH) {
        // max depth override
        if (searchopt.maxdepth != -1 && depth > searchopt.maxdepth) break;

        // stable pv, skip
        if (eval >= params.MIN_SKIP_EVAL && consecutives >= params.PV_STABLE_COUNT
            && !searchopt.infinite)
            halt = true;
        int itereval = negamax(board, depth, 0, params.MAX_EXTENSIONS, halt);

        // not timeout
        if (!halt) {
            eval = itereval;

            // re-search required
            if ((eval <= alpha) || (eval >= beta)) {
                alpha = -INT_MAX;
                beta = INT_MAX;
                continue;
            }

            // narrow window
            alpha = eval - params.ASPIRATION_WINDOW;
            beta = eval + params.ASPIRATION_WINDOW;
            depth++;

            // count consecutive bestmove
            if (itermove == prevPlay)
                consecutives++;
            else {
                prevPlay = itermove;
                consecutives = 1;
            }
        }

        // checkmate, no need to continue
        if (tt.isMate(eval)) {
            if (UCI) {
                auto now = ch::high_resolution_clock::now();
                auto dtime = ch::duration_cast<ch::milliseconds>(now - start_t).count();
                auto nps = (dtime) ? nodes * 1000 / dtime : 0;
                char sign = (eval >= 0) ? '\0' : '-';
                lock_guard<mutex> lock(cout_mutex);
                cout << "info depth " << depth - 1 << " time " << dtime << " nodes " << nodes
                     << " score mate " << sign << MATE_EVAL - abs(eval) << " nps " << nps << " pv "
                     << get_pv_line(board, depth - 1) << "\n";
                cout << "bestmove " << chess::uci::moveToUci(itermove) << "\n" << flush;
            }
#ifndef MUTEEVAL
            else {
                // get absolute evaluation (i.e, set to white's perspective)
                char sign = (whiteturn == (eval > 0)) ? '\0' : '-';
                lock_guard<mutex> lock(cout_mutex);
                cout << "Eval: " << sign << "#" << MATE_EVAL - abs(eval) << "\tNodes: " << nodes
                     << "\n"
                     << flush;
            }
#endif
            halt = true;
            return itermove;
        } else if (UCI) {
            auto now = ch::high_resolution_clock::now();
            auto dtime = ch::duration_cast<ch::milliseconds>(now - start_t).count();
            auto nps = (dtime) ? nodes * 1000 / dtime : 0;
            lock_guard<mutex> lock(cout_mutex);
            cout << "info depth " << depth - 1 << " time " << dtime << " nodes " << nodes
                 << " score cp " << eval << " nps " << nps << " pv "
                 << get_pv_line(board, depth - 1) << "\n"
                 << flush;
        }
    }

    if (UCI) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "bestmove " << chess::uci::moveToUci(itermove) << "\n" << flush;
    }
#ifndef MUTEEVAL
    else {
        // get absolute evaluation (i.e, set to white's perspective)
        if (!whiteturn) eval *= -1;
        lock_guard<mutex> lock(cout_mutex);
        cout << "Eval: " << fixed << setprecision(2) << eval / 100.0f << "\tDepth: " << depth - 1
             << "\tNodes: " << nodes << "\n"
             << flush;
    }
#endif
    return itermove;
}

void v2_0::ponder(chess::Board board, volatile bool& halt) {
    ponderdepth = 1;
    pondereval = 0;
    itermove = chess::Move::NO_MOVE;
    search_t = 0;  // infinite time

    // predict opponent's move from pv
    auto ttkey = board.hash();
    auto ttentry = tt.get(ttkey, 0);

    // no valid response in pv or timeout
    if (halt || !tt.valid(ttentry, ttkey, 0)) {
        consecutives = 1;
        return;
    }

    // play opponent's move and store key to check for ponderhit
    board.makeMove(ttentry.move);
    ponderkey = board.hash();
    history.clear();

    // set up nnue board
    net.set_board(board);

    int alpha = -INT_MAX;
    int beta = INT_MAX;
    nodes = 0;
    consecutives = 1;

    // begin iterative deepening for our best response
    while (!halt && ponderdepth <= MAX_DEPTH) {
        int itereval = negamax(board, ponderdepth, 0, params.MAX_EXTENSIONS, halt);

        if (!halt) {
            pondereval = itereval;

            // re-search required
            if ((pondereval <= alpha) || (pondereval >= beta)) {
                alpha = -INT_MAX;
                beta = INT_MAX;
                continue;
            }

            // narrow window
            alpha = pondereval - params.ASPIRATION_WINDOW;
            beta = pondereval + params.ASPIRATION_WINDOW;
            ponderdepth++;

            // count consecutive bestmove
            if (itermove == prevPlay)
                consecutives++;
            else {
                prevPlay = itermove;
                consecutives = 1;
            }
        }

        // checkmate, no need to continue (but don't edit halt)
        if (tt.isMate(pondereval)) break;
    }
}


string v2_0::get_pv_line(chess::Board board, int depth) const {
    // get first move
    auto ttkey = board.hash();
    auto ttentry = tt.get(ttkey, 0);
    chess::Move pvmove;

    string pvline = "";

    while (depth && tt.valid(ttentry, ttkey, 0)) {
        pvmove = ttentry.move;
        pvline += chess::uci::moveToUci(pvmove) + " ";
        board.makeMove(pvmove);
        ttkey = board.hash();
        ttentry = tt.get(ttkey, 0);
        depth--;
    }
    return pvline;
}


void v2_0::reset() {
    tt.clear();
    killers.clear();
    history.clear();
    itermove = chess::Move::NO_MOVE;
    prevPlay = chess::Move::NO_MOVE;
    consecutives = 0;
    searchopt = SearchOptions();
}


void v2_0::start_search_timer(const chess::Board& board, const int t_remain, const int t_inc) {
    // if movetime is specified, use that instead
    if (searchopt.movetime != -1) {
        search_t = searchopt.movetime;
        start_t = ch::high_resolution_clock::now();
        return;
    }

    // set to infinite if other searchoptions are specified
    if (searchopt.maxdepth != -1 || searchopt.maxnodes != -1 || searchopt.infinite) {
        search_t = 0;
        start_t = ch::high_resolution_clock::now();
        return;
    }

    float n = chess::builtin::popcount(board.occ());
    // 0~1, higher the more time it uses (max at 20 pieces left)
    float ratio = 0.0044f * (n - 32) * (-n / 32) * pow(2.5f + n / 32, 3);
    // use 1~5% of the remaining time based on the ratio + buffered increment
    int duration = t_remain * (0.01f + 0.04f * ratio) + max(t_inc - 30, 1);
    // try to use all of our time if timer resets after movestogo (unless it's 1, then be fast)
    if (searchopt.movestogo > 1) duration += (t_remain - duration) / searchopt.movestogo;
    search_t = min(duration, t_remain);
    start_t = ch::high_resolution_clock::now();
}

bool v2_0::is_time_over(volatile bool& halt) const {
    // if max nodes is specified, check that instead
    if (searchopt.maxnodes != -1 && nodes >= searchopt.maxnodes) {
        halt = true;
        return true;
    }
    // otherwise, check timeover every 2048 nodes
    if (search_t && !(nodes & 2047)) {
        auto now = ch::high_resolution_clock::now();
        auto dtime = ch::duration_cast<ch::milliseconds>(now - start_t).count();
        if (dtime >= search_t) halt = true;
    }
    return halt;
}


int v2_0::negamax(
    chess::Board& board,
    const int depth,
    const int ply,
    const int ext,
    volatile bool& halt
) {
    if (is_time_over(halt)) return 0;
    nodes++;

    // terminal/noisy cutoff
    if (depth <= 0 || ply == MAX_DEPTH - 1)
        return quiescence(board, ply, halt);

    // repetition/draw
    if (ply && (board.isRepetition(1) || board.isHalfMoveDraw()))
        return 0;

    // generate
    chess::Movelist movelist;
    chess::movegen::legalmoves<chess::MoveGenType::ALL>(movelist, board);
    if (movelist.empty())
        return board.inCheck() ? -MATE_EVAL + ply : 0;

    order_moves(movelist, board, ply);
    int bestEval = -INT_MAX;
    chess::Move bestmove = movelist[0];
    if (!ply) itermove = bestmove;

    for (const auto& mv : movelist) {
        net.make_move(ply+1, mv, board);
        board.makeMove(mv);
        int eval = -negamax(board, depth-1, ply+1, ext, halt);
        board.unmakeMove(mv);
        if (halt) return 0;
        if (eval > bestEval) {
            bestEval = eval;
            if (!ply) itermove = mv;
        }
    }

    // still cache exact to TT if you like
    tt.set({ board.hash(), depth, tt.EXACT, itermove, bestEval }, ply);
    return bestEval;
}

int v2_0::quiescence(chess::Board& board, const int ply, volatile bool& halt) {
    if (is_time_over(halt)) return 0;
    nodes++;

    // standing pat
    int bestEval = net.evaluate(ply, whiteturn)
                   * (100 - board.halfMoveClock()) / 100;

    if (ply == MAX_DEPTH - 1)
        return bestEval;

    // only captures
    chess::Movelist movelist;
    chess::movegen::legalmoves<chess::MoveGenType::CAPTURE>(movelist, board);
    order_moves(movelist, board, ply);

    for (const auto& mv : movelist) {
        net.make_move(ply+1, mv, board);
        board.makeMove(mv);
        int eval = -quiescence(board, ply+1, halt);
        board.unmakeMove(mv);
        if (halt) return 0;
        if (eval > bestEval) bestEval = eval;
    }
    return bestEval;
}


void v2_0::order_moves(chess::Movelist& movelist, const chess::Board& board, const int ply) const {
    for (auto& move : movelist) score_move(move, board, ply);
    movelist.sort();
}

void v2_0::score_move(chess::Move& move, const chess::Board& board, const int ply) const {
    // prioritize best move from previous iteraton
    if (move == tt.get(board.hash(), 0).move) {
        move.setScore(INT16_MAX);
        return;
    }

    int16_t score = 0;

    // calculate other scores
    int from = (int)board.at(move.from());
    int to = (int)board.at(move.to());

    // enemy piece captured
    if (board.isCapture(move)) {
        score += abs(params.PVAL[to][1]) - (from % 6);  // MVV/LVA
        score += SEE::goodCapture(move, board, -12) * params.GOOD_CAPTURE_WEIGHT;
    } else {
        // killer move
        if (ply > 0 && killers.isKiller(move, ply)) score += params.KILLER_WEIGHT;
        // history
        score += history.get(move, whiteturn);
    }

    // promotion
    if (move.typeOf() == chess::Move::PROMOTION)
        score += abs(params.PVAL[(int)move.promotionType()][1]);

    move.setScore(score);
}
