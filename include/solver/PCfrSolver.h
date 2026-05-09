//
// Created by Xuefeng Huang on 2020/1/31.
//

#ifndef TEXASSOLVER_PCFRSOLVER_H
#define TEXASSOLVER_PCFRSOLVER_H
#include <include/ranges/PrivateCards.h>
#include <include/compairer/Compairer.h>
#include <include/Deck.h>
#include <include/ranges/RiverRangeManager.h>
#include <include/ranges/PrivateCardsManager.h>
#include <include/trainable/CfrPlusTrainable.h>
#include <include/trainable/DiscountedCfrTrainable.h>
#include "include/solver/Solver.h"
#include <omp.h>
#include "include/tools/lookup8.h"
#include "include/tools/utils.h"
#include <queue>
#include <optional>
#include <array>
#include <mutex>
#include <unordered_map>
#include <include/tools/OptimizationSwitches.h>
class StrategyJsonExporterV2;
/*
template<typename T>
class ThreadsafeQueue {
    std::queue<T> queue_;
    mutable std::mutex mutex_;

    // Moved out of public interface to prevent races between this
    // and pop().
    bool empty() const {
        return queue_.empty();
    }

public:
    ThreadsafeQueue() = default;
    ThreadsafeQueue(const ThreadsafeQueue<T> &) = delete ;
    ThreadsafeQueue& operator=(const ThreadsafeQueue<T> &) = delete ;

    ThreadsafeQueue(ThreadsafeQueue<T>&& other) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(other.queue_);
    }

    virtual ~ThreadsafeQueue() { }

    unsigned long size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return {};
        }
        T tmp = queue_.front();
        queue_.pop();
        return tmp;
    }

    void push(const T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
    }
};

struct TaskParams{
    int player;
    shared_ptr<GameTreeNode> node;
    const vector<float> &reach_probs;
    int iter;
    uint64_t current_board;
    int deal;
};
*/

class PCfrSolver:public Solver {
    friend class StrategyJsonExporterV2;
public:
    PCfrSolver(shared_ptr<GameTree> tree,
            vector<PrivateCards> range1 ,
            vector<PrivateCards> range2,
            vector<int> initial_board,
            shared_ptr<Compairer> compairer,
            Deck deck,
            int iteration_number,
            bool debug,
            int print_interval,
            string logfile,
            string trainer,
            Solver::MonteCarolAlg monteCarolAlg,
            int warmup,
            float accuracy,
            bool use_isomorphism,
            int use_halffloats,
            int num_threads,
            int exploitability_interval = -1,
            bool collect_evs = true
    );
    ~PCfrSolver();
    void train() override;
    void stop() override;
    json dumps(bool with_status,int depth) override;
    vector<vector<vector<float>>> get_strategy(shared_ptr<ActionNode> node,vector<Card> chance_cards) override;
    vector<vector<vector<float>>> get_evs(shared_ptr<ActionNode> node,vector<Card> chance_cards) override;
    // === 新增：流式导出（三条街全 runout） ===
    // path_gz: 建议传 .ndjson.gz；max_depth: 0=只flop, 1=到turn, 2(或3)=到river
    // prune_eps=0 表示不稀疏；precision<0 表示不量化
    void dump_strategy_stream(const std::string& path_gz,
                              int max_depth,
                              double prune_eps = 0.0,
                              int precision = -1);
    // 新增：导出到 SQLite（策略 JSON 压缩为 BLOB）
    /*void dump_strategy_sqlite(const std::string& db_path,
                              int max_depth,
                              double prune_eps,
                              int precision);*/
    // 导出为 SQLite（全量字段）
    void dump_strategy_sqlite(const std::string& path,
                              int max_depth,
                              double prune_eps,
                              int precision);
    // max_depth: 0=仅flop, 1=到turn, 2/3=到river；use_fp16 预留（当前实现写 fp32）；align8 是否记录 8 字节对齐
    void dump_strategy_bin(const std::string& bin_path,
                           const std::string& idx_path,
                           int max_depth,
                           bool use_fp16 = false,
                           bool align8 = true);
 void   dump_strategy_bin_from_json(const std::string& bin_path,
                                     const std::string& idx_path,
                                     int max_depth = 3,
                                     bool align8 = true);
      std::array<int,4> get_color_iso_offset_row(int deal) const;
    void dump_strategy_tsb2(const std::string& out_path, int max_depth);
 // PCfrSolver.h (public)
 int getColorIsoOffset(int deal, int suit) const;
private:
    float rake_pct_ = 0.0f;     // 0.05 = 5%
    float rake_cap_ = 0.0f;     // in chips, 0=无限
    bool  no_flop_no_drop_ = true;

    vector<vector<PrivateCards>> ranges;
    vector<PrivateCards> range1;
    vector<PrivateCards> range2;
    vector<int> initial_board;
    uint64_t initial_board_long;
    shared_ptr<Compairer> compairer;
    int color_iso_offset[52 * 52 * 2][4] = {0};
    bool collecting_statics = false;
    bool statics_collected = false;
    bool collect_evs = true;

    Deck deck;
    RiverRangeManager rrm;
    int player_number;
    int iteration_number;
    PrivateCardsManager pcm;
#if TEXASSOLVER_OPT_TERMINAL_SAME_CARD_CACHE
    vector<vector<vector<int>>> same_card_index;
#endif
#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    unordered_map<uint64_t, vector<vector<int>>> river_valid_combo_indices;
    std::mutex river_result_cache_lock;
#endif
    bool debug;
    int print_interval;
    int exploitability_interval;
    string trainer;
    string logfile;
    Solver::MonteCarolAlg monteCarolAlg;
    vector<int> round_deal;
    int num_threads;
    int warmup;
    GameTreeNode::GameRound root_round;
    GameTreeNode::GameRound split_round;
    bool distributing_task;
    float accuracy;
    bool use_isomorphism;
    int use_halffloats;
    bool nowstop = false;

    const vector<PrivateCards>& playerHands(int player);
    vector<vector<float>> getReachProbs();
    static vector<PrivateCards> noDuplicateRange(const vector<PrivateCards>& private_range,uint64_t board_long);
    void setTrainable(shared_ptr<GameTreeNode> root);
    vector<float> cfr(int player, shared_ptr<GameTreeNode> node, const vector<float>& reach_probs, int iter, uint64_t current_board,int deal);
    void cfrInto(int player, const shared_ptr<GameTreeNode>& node, const vector<float>& reach_probs, int iter, uint64_t current_board,int deal, vector<float>& out);
    vector<int> getAllAbstractionDeal(int deal);
    vector<float> chanceUtility(int player,shared_ptr<ChanceNode> node,const vector<float>& reach_probs,int iter,uint64_t current_boardi,int deal);
    void chanceUtilityInto(int player,const shared_ptr<ChanceNode>& node,const vector<float>& reach_probs,int iter,uint64_t current_boardi,int deal, vector<float>& out);
    vector<float> showdownUtility(int player,shared_ptr<ShowdownNode> node,const vector<float>& reach_probs,int iter,uint64_t current_board,int deal);
    void showdownUtilityInto(int player,const shared_ptr<ShowdownNode>& node,const vector<float>& reach_probs,int iter,uint64_t current_board,int deal, vector<float>& out);
    vector<float> actionUtility(int player,shared_ptr<ActionNode> node,const vector<float>& reach_probs,int iter,uint64_t current_board,int deal);
    void actionUtilityInto(int player,const shared_ptr<ActionNode>& node,const vector<float>& reach_probs,int iter,uint64_t current_board,int deal, vector<float>& out);
    vector<float> terminalUtility(int player,shared_ptr<TerminalNode> node,const vector<float>& reach_prob,int iter,uint64_t current_board,int deal);
    void terminalUtilityInto(int player,const shared_ptr<TerminalNode>& node,const vector<float>& reach_prob,int iter,uint64_t current_board,int deal, vector<float>& out);
#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    const vector<int>& getRiverValidComboIndices(int player, uint64_t current_board);
#endif
    void findGameSpecificIsomorphisms();
    void purnTree();
    void exchangeRange(json& strategy,int rank1,int rank2,shared_ptr<ActionNode> one_node);
    void reConvertJson(const shared_ptr<GameTreeNode>& node,json& strategy,string key,int depth,int max_depth,vector<string> prefix,int deal,vector<vector<int>> exchange_color_list);

};


#endif //TEXASSOLVER_PCFRSOLVER_H
