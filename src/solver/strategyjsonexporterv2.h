#pragma once

#include "include/json.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <fstream>
#include <unordered_map>

class PCfrSolver;
class GameTreeNode;
class ActionNode;
class ChanceNode;
class Trainable;

#include "GzRawWriter.h"
#include "xidxv2writer.h"
#include "fnv1a.h"

class StrategyJsonExporterV2 {
public:
    StrategyJsonExporterV2(PCfrSolver* pcfr,
                           const std::string& json_gz_path,
                           const std::string& xidx_path,
                           int deck_size);

    void run();

private:
    void write_node_(std::shared_ptr<GameTreeNode> n, int depth);
    void write_action_node_(std::shared_ptr<ActionNode> an, int depth);
    void write_chance_node_(std::shared_ptr<ChanceNode> cn, int depth);
    // 修改：write_strategy_block_and_index_ 接口，传 shared_ptr<ActionNode>，并告知是否需要在前面加逗号
    void write_strategy_block_and_index_(std::shared_ptr<ActionNode> an, bool prepend_comma);

    // deal 计算（与你的 PCfr 一致：1-based）
    int make_deal_global_(const std::vector<int>& cc) const;

    // JSON 帮助
    void        write_key_nloh_(const std::string& k);
    std::string rfc6901_escape_(const std::string& s);
    std::string pointer_from_stack_() const;
    std::vector<std::pair<int,int>> exch_stack_;
    // 新增：把已记录换色对应用到“显示牌面标签”
    std::string apply_exchanges_to_label_(int card_int) const;
#ifdef SJEV2_ENABLE_DIAG
    std::string current_street_() const;
#endif
#ifdef SJEV2_ENABLE_DIAG
    void diag_open_(const std::string& json_gz_path);
    void diag_close_();
    void diag_log_action_(const std::string& ptr,
                          const std::string& street,
                          int deal_used,
                          bool has_tr,
                          size_t actions_cnt,
                          size_t hands_cnt,
                          uint64_t hash_stream,
                          bool used_abstraction);
#else
    inline void diag_open_(const std::string&) {}
    inline void diag_close_() {}
    inline void diag_log_action_(const std::string&, const std::string&, int, bool, size_t, size_t, uint64_t, bool) {}
#endif

    // 用于快速比对策略内容是否一致
    uint64_t hash_strategy_map_(const nlohmann::json& smap) const;

private:
    PCfrSolver*              pcfr_;
    GzRawWriter              gz_;
    XidxV2Writer             xw_;
    const int                deck_size_;
    uint64_t hash_pointer_(const std::string& s) const; // FNV-1a 64 on RFC6901 pointer
    uint8_t  fp8_(uint64_t h) const;
    std::vector<int>         chance_cards_; // turn/river 的“牌堆索引”路径（全局 0..D-1）
    std::vector<std::string> key_stack_;    // 构造 RFC6901 指针
    // ===== [#1] handsTables：将每个不同公共牌对应的手牌顺序去重并编号 =====
    struct HandsTable { int hid; std::vector<std::string> keys; };
    std::vector<HandsTable> hands_tables_;           // 全局表：hid -> keys[]
    std::unordered_map<uint64_t,int> keys_sig_to_hid_; // keys 列表的签名 -> hid

    // 注册并返回 hid（若同样的 keys 已出现，则复用原 hid）
    int register_hands_table_(const std::vector<std::string>& keys);
    // 结束导出前一次性把所有 handsTables 写出去（单独一条 JSON 对象）
    void write_all_hands_tables_();
    // 对 keys[] 做稳定签名（FNV-1a）
    uint64_t hash_keys_(const std::vector<std::string>& keys) const;

    // ===== 修改 #2：全局动作字典 =====
    std::vector<std::string>              action_dict_;    // 索引 -> 动作名
    std::unordered_map<std::string, int>  action_to_idx_;  // 动作名 -> 索引
    void write_action_dict_global_();                       // 在导出末尾写出 {"type":"actionDict",...}

    // ===== 修改 #3：全局牌面字典 =====
    std::vector<std::string>              card_dict_;    // 索引 -> 牌名（如 "As"）
    std::unordered_map<std::string, int>  card_to_idx_;  // 牌名 -> 索引（按 AKQJT... × shdc）

    // 初始化牌面字典（首次使用前保证已填充）
    void ensure_card_dict_();

    // 在导出末尾写出 {"type":"cardDict","cards":[...]}
    void write_card_dict_global_();

    uint64_t                 nodes_done_ = 0;
    uint64_t                 log_step_   = 10000;

#ifdef SJEV2_ENABLE_DIAG
    bool                     diag_on_    = false;
    std::ofstream            diag_os_;
#endif
};
