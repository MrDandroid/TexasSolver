//
// Created by Xuefeng Huang on 2020/2/6.
//

#ifndef TEXASSOLVER_POKERSOLVER_H
#define TEXASSOLVER_POKERSOLVER_H

#include <string>
#include <vector>
#include <memory>
#include "include/compairer/Dic5Compairer.h"
#include "include/tools/PrivateRangeConverter.h"
#include "include/solver/CfrSolver.h"
#include "include/solver/PCfrSolver.h"
#include "include/library.h"
#include <QDebug>
#include <QFile>

using namespace std;

class PokerSolver {
public:
    PokerSolver();
    PokerSolver(string ranks,string suits,string compairer_file,int compairer_file_lines,string compairer_file_bin);

    void load_game_tree(string game_tree_file);
    void build_game_tree(
        float oop_commit,
        float ip_commit,
        int current_round,
        int raise_limit,
        float small_blind,
        float big_blind,
        float stack,
        GameTreeBuildingSettings buildingSettings,
        float allin_threshold
        );

    //添加的抽水
    void build_game_tree(
        float oop_commit,
        float ip_commit,
        int current_round,
        int raise_limit,
        float small_blind,
        float big_blind,
        float stack,
        GameTreeBuildingSettings buildingSettings,
        float allin_threshold,
        // === rake settings ===
        float rake_pct,
        float rake_cap,          // cap in chips（由 rake_cap_bb * big_blind 换算）
        bool no_flop_no_drop
        );

    void train(
        string p1_range,
        string p2_range,
        string boards,
        string log_file,
        int iteration_number,
        int print_interval,
        string algorithm,
        int warmup,
        float accuracy,
        bool use_isomorphism,
        int use_halffloats,
        int threads
        );

    void stop();
    long long estimate_tree_memory(QString range1,QString range2,QString board);

    /**
     * 一次性导出完整 JSON（容易内存爆炸，文件可能数 GB）
     * @param dump_file 输出文件路径（建议 .json）
     * @param dump_rounds 导出深度（1 = flop, 2 = turn, 3 = river）
     */
    void dump_strategy(QString dump_file,int dump_rounds);

    /**
     * 新增：流式导出（按节点逐行写出 NDJSON，极大降低内存占用）
     * - 支持 .ndjson 或 .ndjson.gz 后缀
     * - 导出参数从 QSettings("TexasSolver","Setting")/solver 读取：
     *   - dump_round（默认 2，即到 turn）
     *   - export_prune_eps（默认 0.005，小于此概率的动作会被剪枝）
     *   - export_precision（默认 3，小数点后三位）
     *
     * @param dump_file 输出文件路径（建议 .ndjson 或 .ndjson.gz）
     * @param dump_rounds 导出深度，若 <=0 则使用 settings 中的 dump_round
     */
    void dump_strategy_stream(QString dump_file, int dump_rounds);
    void dump_strategy_sqlite(QString dump_file, int dump_rounds);
    // 与 JSON 同构的二进制导出（CBOR）+ 生成 SQLite 索引（.idx）
    void dump_strategy_bin_with_index(QString dump_file, int dump_rounds);
    void compress_and_write_chunk(const std::string& chunk, std::ofstream& bin_writer);
    shared_ptr<GameTree> get_game_tree(){return this->game_tree;};
    Deck* get_deck(){return &this->deck;}
    shared_ptr<Solver> get_solver(){return this->solver;}

    const shared_ptr<GameTree> &getGameTree() const;

    vector<PrivateCards> player1Range;
    vector<PrivateCards> player2Range;

private:
    shared_ptr<Dic5Compairer> compairer;
    Deck deck;
    shared_ptr<GameTree> game_tree;
    shared_ptr<Solver> solver;
};

#endif //TEXASSOLVER_POKERSOLVER_H
