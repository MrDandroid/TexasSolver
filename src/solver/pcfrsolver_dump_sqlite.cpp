#include "include/solver/PCfrSolver.h"
#include "include/nodes/GameTreeNode.h"
#include "include/nodes/ActionNode.h"
#include "include/nodes/ChanceNode.h"
#include "include/nodes/TerminalNode.h"
#include "include/nodes/ShowdownNode.h"
#include "include/trainable/Trainable.h"

#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cmath>
#include <sstream>
#include <type_traits>

// 优先匹配：存在 dump_strategy(bool) 的版本
template <typename T>
auto ts_call_dump_strategy(T* t, int) -> decltype(t->dump_strategy(false)) {
    return t->dump_strategy(false);
}

// 备选匹配：只有 dump_strategy() 的版本
template <typename T>
auto ts_call_dump_strategy(T* t, long) -> decltype(t->dump_strategy()) {
    return t->dump_strategy();
}

// 同理给 EVs 做个保险（多数实现都是无参，这里也做双形态兼容）
template <typename T>
auto ts_call_dump_evs(T* t, int) -> decltype(t->dump_evs(false)) {
    return t->dump_evs(false);
}
template <typename T>
auto ts_call_dump_evs(T* t, long) -> decltype(t->dump_evs()) {
    return t->dump_evs();
}
// ============== 小工具：执行 SQL/Step 的统一封装（修正返回码判断） ==============
static inline void sql_exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errmsg(db);
        if (err) sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg);
    }
}

static inline void sql_step_done_or_throw(sqlite3_stmt* stmt, sqlite3* db, const char* what) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        throw std::runtime_error(std::string("sqlite step failed on ") + what + ": " + msg);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

// ============== 导出配置：量化 & 稀疏 ==============
static void TS_quantize_and_prune(std::vector<float>& v, double eps, int precision) {
    double s = 0.0;
    if (eps > 0.0) {
        for (auto &x : v) {
            if (x < eps) x = 0.0f;
            s += x;
        }
        if (s > 0.0) {
            for (auto &x : v) x = static_cast<float>(x / s);
        }
    } else {
        for (auto &x : v) s += x;
    }
    if (precision >= 0) {
        float base = 1.f;
        for (int i = 0; i < precision; ++i) base *= 10.f;
        for (auto &x : v) x = std::round(x * base) / base;
    }
    if (s > 0.0) {
        double sum2 = 0.0; for (auto x : v) sum2 += x;
        double diff = 1.0 - sum2;
        if (std::fabs(diff) < 1e-6) {
            for (int i = (int)v.size()-1; i >= 0; --i) {
                if (v[i] > 0.f) { v[i] = static_cast<float>(v[i] + diff); break; }
            }
        }
    }
}

// chance_cards（按 deck 索引）折成 deal 索引（与 get_strategy 一致）
static int TS_make_deal_from_chance_cards(const std::vector<int>& chance_cards, int card_num) {
    int deal = 0;
    for (std::size_t k = 0; k < chance_cards.size(); ++k) {
        int card = chance_cards[k];
        if (k == 0) {
            deal = card + 1;
        } else {
            int origin = deal - 1;
            deal = card_num * origin + card + (1 + card_num);
        }
    }
    return deal;
}

// 牌面标签（初始牌面是 card int，chance_cards 是 deck 索引）
static void TS_make_board_labels(const std::vector<int>& initial_board,
                                 const std::vector<int>& chance_cards,
                                 const std::vector<Card>& deck_cards,
                                 std::string& flop, std::string& turn, std::string& river) {
    auto toStrByCardInt = [](int card_int)->std::string {
        return Card::intCard2Str(card_int);
    };
    auto toStrByDeckIdx = [&](int deck_idx)->std::string {
        return Card::intCard2Str(deck_cards[deck_idx].getCardInt());
    };

    flop.clear(); turn = "*"; river = "*";

    if (initial_board.size() >= 3) {
        flop  = toStrByCardInt(initial_board[0])
        + toStrByCardInt(initial_board[1])
            + toStrByCardInt(initial_board[2]);
    }
    if (!chance_cards.empty()) {
        turn  = toStrByDeckIdx(chance_cards[0]);
        if (chance_cards.size() >= 2) river = toStrByDeckIdx(chance_cards[1]);
    }
}

static const char* round_to_cstr(GameTreeNode::GameRound r) {
    switch (r) {
    case GameTreeNode::GameRound::FLOP:  return "flop";
    case GameTreeNode::GameRound::TURN:  return "turn";
    case GameTreeNode::GameRound::RIVER: return "river";
    default: return "unknown";
    }
}
static const char* type_to_cstr(GameTreeNode::GameTreeNodeType t) {
    switch (t) {
    case GameTreeNode::GameTreeNodeType::ACTION:   return "ACTION";
    case GameTreeNode::GameTreeNodeType::CHANCE:   return "CHANCE";
    case GameTreeNode::GameTreeNodeType::TERMINAL: return "TERMINAL";
    case GameTreeNode::GameTreeNodeType::SHOWDOWN: return "SHOWDOWN";
    default: return "UNKNOWN";
    }
}

// ============== SQLite 导出主体 ==============
void PCfrSolver::dump_strategy_sqlite(const std::string& path,
                                      int max_depth,
                                      double prune_eps,
                                      int precision)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("cannot open sqlite db: " + msg);
    }

    try {
        // 提速 PRAGMA
        sql_exec_or_throw(db, "PRAGMA journal_mode=WAL;");
        sql_exec_or_throw(db, "PRAGMA synchronous=OFF;");
        sql_exec_or_throw(db, "PRAGMA temp_store=MEMORY;");
        sql_exec_or_throw(db, "PRAGMA mmap_size=30000000000;"); // 可按需调整

        // 表结构（尽量简单通用）
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS meta ("
                          "  key TEXT PRIMARY KEY,"
                          "  value TEXT"
                          ");"
                          );
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS nodes ("
                          "  id        INTEGER PRIMARY KEY,"
                          "  parent_id INTEGER,"
                          "  type      TEXT,"
                          "  round     TEXT,"
                          "  street    TEXT,"
                          "  pos       INTEGER,"
                          "  board     TEXT"
                          ");"
                          );
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS edges ("
                          "  src_id INTEGER,"
                          "  dst_id INTEGER,"
                          "  action TEXT,"
                          "  ord    INTEGER,"
                          "  PRIMARY KEY(src_id, ord)"
                          ");"
                          );
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS strategies ("
                          "  node_id INTEGER,"
                          "  deal    INTEGER,"
                          "  json    TEXT,"
                          "  PRIMARY KEY(node_id, deal)"
                          ");"
                          );
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS evs ("
                          "  node_id INTEGER,"
                          "  deal    INTEGER,"
                          "  json    TEXT,"
                          "  PRIMARY KEY(node_id, deal)"
                          ");"
                          );
        sql_exec_or_throw(db,
                          "CREATE TABLE IF NOT EXISTS ranges ("
                          "  player INTEGER,"
                          "  json   TEXT,"
                          "  PRIMARY KEY(player)"
                          ");"
                          );

        // 索引
        sql_exec_or_throw(db, "CREATE INDEX IF NOT EXISTS idx_nodes_parent ON nodes(parent_id);");
        sql_exec_or_throw(db, "CREATE INDEX IF NOT EXISTS idx_edges_src    ON edges(src_id);");
        sql_exec_or_throw(db, "CREATE INDEX IF NOT EXISTS idx_strat_node   ON strategies(node_id);");
        sql_exec_or_throw(db, "CREATE INDEX IF NOT EXISTS idx_evs_node     ON evs(node_id);");

        // 预编译语句
        sqlite3_stmt* ins_node = nullptr;
        sqlite3_stmt* ins_edge = nullptr;
        sqlite3_stmt* ins_str  = nullptr;
        sqlite3_stmt* ins_evs  = nullptr;
        sqlite3_stmt* ins_rng  = nullptr;

        sql_exec_or_throw(db, "BEGIN IMMEDIATE TRANSACTION;");

        const char* SQL_INS_NODE =
            "INSERT INTO nodes(id,parent_id,type,round,street,pos,board) "
            "VALUES(?,?,?,?,?,?,?);";
        const char* SQL_INS_EDGE =
            "INSERT OR REPLACE INTO edges(src_id,dst_id,action,ord) VALUES(?,?,?,?);";
        const char* SQL_INS_STR =
            "INSERT OR REPLACE INTO strategies(node_id,deal,json) VALUES(?,?,?);";
        const char* SQL_INS_EVS =
            "INSERT OR REPLACE INTO evs(node_id,deal,json) VALUES(?,?,?);";
        const char* SQL_INS_RNG =
            "INSERT OR REPLACE INTO ranges(player,json) VALUES(?,?);";

        if (sqlite3_prepare_v2(db, SQL_INS_NODE, -1, &ins_node, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare insert node failed: " + std::string(sqlite3_errmsg(db)));
        if (sqlite3_prepare_v2(db, SQL_INS_EDGE, -1, &ins_edge, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare insert edge failed: " + std::string(sqlite3_errmsg(db)));
        if (sqlite3_prepare_v2(db, SQL_INS_STR, -1, &ins_str, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare insert strategies failed: " + std::string(sqlite3_errmsg(db)));
        if (sqlite3_prepare_v2(db, SQL_INS_EVS, -1, &ins_evs, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare insert evs failed: " + std::string(sqlite3_errmsg(db)));
        if (sqlite3_prepare_v2(db, SQL_INS_RNG, -1, &ins_rng, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare insert ranges failed: " + std::string(sqlite3_errmsg(db)));

        // 保存 ranges（玩家 0/1）
        {
            // 导出 range1 / range2 -> 简单序列化成 "AhKd:1.0, ..." 的文本（你可以换成 JSON）
            auto serialize_range = [&](const std::vector<PrivateCards>& r){
                std::ostringstream oss;
                for (std::size_t i=0;i<r.size();++i) {
                    if (i) oss << ",";
                    oss << Card::intCard2Str(r[i].card1) << Card::intCard2Str(r[i].card2)
                        << ":" << r[i].weight;
                }
                return oss.str();
            };
            const std::string r0 = serialize_range(this->range1);
            const std::string r1 = serialize_range(this->range2);

            sqlite3_bind_int  (ins_rng, 1, 0);
            sqlite3_bind_text (ins_rng, 2, r0.c_str(), (int)r0.size(), SQLITE_TRANSIENT);
            sql_step_done_or_throw(ins_rng, db, "insert range p0");

            sqlite3_bind_int  (ins_rng, 1, 1);
            sqlite3_bind_text (ins_rng, 2, r1.c_str(), (int)r1.size(), SQLITE_TRANSIENT);
            sql_step_done_or_throw(ins_rng, db, "insert range p1");
        }

        // DFS 遍历树并插入节点/边/策略
        std::unordered_map<const GameTreeNode*, int64_t> idmap;
        int64_t next_id = 1;

        std::vector<int> chance_cards; // 按 deck 索引
        auto boardLabels = [&](std::string& flop, std::string& turn, std::string& river){
            TS_make_board_labels(this->initial_board, chance_cards, this->deck.getCards(), flop, turn, river);
        };
        auto street_name = [&]()->const char*{
            return chance_cards.empty() ? "flop" : (chance_cards.size()==1 ? "turn" : "river");
        };

        std::function<void(std::shared_ptr<GameTreeNode>, std::shared_ptr<GameTreeNode>, int)> dfs;
        dfs = [&](std::shared_ptr<GameTreeNode> node,
                  std::shared_ptr<GameTreeNode> parent,
                  int depth)
        {
            if (!node) return;
            if (depth > max_depth) return;

            // 为当前节点分配/获取 id
            int64_t my_id;
            auto it = idmap.find(node.get());
            if (it == idmap.end()) {
                my_id = next_id++;
                idmap.emplace(node.get(), my_id);

                // 插入 nodes
                std::string flop, turn, river;
                boardLabels(flop, turn, river);
                std::string board_str = flop + "|" + turn + "|" + river;

                const char* tstr = type_to_cstr(node->getType());
                const char* rstr = round_to_cstr(node->getRound());
                const char* sstr = street_name();

                sqlite3_bind_int64(ins_node, 1, my_id);
                if (parent) {
                    auto pit = idmap.find(parent.get());
                    sqlite3_bind_int64(ins_node, 2, (pit==idmap.end()? 0 : pit->second));
                } else {
                    sqlite3_bind_null(ins_node, 2);
                }
                sqlite3_bind_text (ins_node, 3, tstr, -1, SQLITE_STATIC);
                sqlite3_bind_text (ins_node, 4, rstr, -1, SQLITE_STATIC);
                sqlite3_bind_text (ins_node, 5, sstr, -1, SQLITE_STATIC);

                int pos = -1;
                if (node->getType() == GameTreeNode::GameTreeNodeType::ACTION)
                    pos = std::dynamic_pointer_cast<ActionNode>(node)->getPlayer();
                sqlite3_bind_int (ins_node, 6, pos);

                sqlite3_bind_text (ins_node, 7, board_str.c_str(), (int)board_str.size(), SQLITE_TRANSIENT);

                sql_step_done_or_throw(ins_node, db, "insert node");
            } else {
                my_id = it->second;
            }

            // 策略/EV（仅行动节点）
            if (node->getType() == GameTreeNode::GameTreeNodeType::ACTION) {
                auto an = std::dynamic_pointer_cast<ActionNode>(node);

                // 计算 deal、并通过 Trainable 导出稀疏策略（与 NDJSON 导出一致）
                int deal = TS_make_deal_from_chance_cards(chance_cards,
                                                          static_cast<int>(this->deck.getCards().size()));

                // 策略
                {
                    std::shared_ptr<Trainable> tr = an->getTrainable(deal, true, /*use_halffloats*/false);
                    if (tr) {
                        // {"actions":[...], "strategy":{ "AsKs":[...], ... } }
                        json st = tr->dump_strategy(false);
                        // 量化/剪枝（只动 strategy 的数组）
                        if (st.contains("strategy") && st["strategy"].is_object()) {
                            for (auto it = st["strategy"].begin(); it != st["strategy"].end(); ++it) {
                                std::vector<float> vec = it.value().get<std::vector<float>>();
                                TS_quantize_and_prune(vec, prune_eps, precision);
                                it.value() = vec;
                            }
                        }
                        const std::string j = st.dump();
                        sqlite3_bind_int64(ins_str, 1, my_id);
                        sqlite3_bind_int  (ins_str, 2, deal);
                        sqlite3_bind_text (ins_str, 3, j.c_str(), (int)j.size(), SQLITE_TRANSIENT);
                        sql_step_done_or_throw(ins_str, db, "insert strategies");
                    }
                }

                // EV：用 solver 的 get_evs（缺点：是 52x52 网格，体积较大；你也可以转为 map<string,float>）
                {
                    // 构建 chance_cards -> Card 的列表以便 get_evs
                    std::vector<Card> deal_cards;
                    for (int di : chance_cards) deal_cards.push_back(this->deck.getCards()[di]);

                    auto evs = this->get_evs(an, deal_cards);
                    // 为了节省体积，这里不压扁 52x52，直接存为 JSON 文本
                    json jevs = evs;
                    const std::string j = jevs.dump();
                    sqlite3_bind_int64(ins_evs, 1, my_id);
                    sqlite3_bind_int  (ins_evs, 2, deal);
                    sqlite3_bind_text (ins_evs, 3, j.c_str(), (int)j.size(), SQLITE_TRANSIENT);
                    sql_step_done_or_throw(ins_evs, db, "insert evs");
                }

                // 插入边 + 递归孩子
                const auto& children = an->getChildrens();
                const auto& actions  = an->getActions();
                for (std::size_t i = 0; i < actions.size(); ++i) {
                    auto child = children[i];
                    // 先给 child 分配 id（如果还没分配，会在下一次 dfs 插入节点表）
                    int64_t child_id;
                    auto it2 = idmap.find(child.get());
                    if (it2 == idmap.end()) {
                        child_id = next_id++;
                        idmap.emplace(child.get(), child_id);
                    } else child_id = it2->second;

                    // edges
                    const std::string act = actions[i].toString();
                    sqlite3_bind_int64(ins_edge, 1, my_id);
                    sqlite3_bind_int64(ins_edge, 2, child_id);
                    sqlite3_bind_text (ins_edge, 3, act.c_str(), (int)act.size(), SQLITE_TRANSIENT);
                    sqlite3_bind_int  (ins_edge, 4, (int)i);
                    sql_step_done_or_throw(ins_edge, db, "insert edge");

                    // 递归（不变更 chance_cards）
                    dfs(child, node, depth);
                }
                return;
            }

            // 发牌节点
            if (node->getType() == GameTreeNode::GameTreeNodeType::CHANCE) {
                if (depth == max_depth) return;
                auto cn = std::dynamic_pointer_cast<ChanceNode>(node);
                const auto& cards = cn->getCards();
                auto child = cn->getChildren();

                for (std::size_t i = 0; i < cards.size(); ++i) {
                    int idx = cards[i].getNumberInDeckInt();
                    chance_cards.push_back(idx);
                    // 这里是“从 chance 发牌到唯一 child”
                    dfs(child, node, depth + 1);
                    chance_cards.pop_back();
                }
                return;
            }

            // 终局节点：不插策略/EV
            // 同时插入从 parent 到该节点的边（若 parent 是 ACTION，会在上面处理；
            // 若 parent 是 CHANCE，这里没有具体 action 名称，不插 edges）
        };

        // 根
        dfs(this->tree->getRoot(), nullptr, 0);

        // meta
        {
            const std::string kv1 = "game";  const std::string vv1 = "holdem";
            const std::string kv2 = "max_depth"; std::string vv2 = std::to_string(max_depth);
            const std::string kv3 = "prune_eps"; std::string vv3 = std::to_string(prune_eps);
            const std::string kv4 = "precision"; std::string vv4 = std::to_string(precision);

            const char* SQL_META = "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?);";
            sqlite3_stmt* sm = nullptr;
            if (sqlite3_prepare_v2(db, SQL_META, -1, &sm, nullptr) != SQLITE_OK)
                throw std::runtime_error("prepare meta failed: " + std::string(sqlite3_errmsg(db)));

            sqlite3_bind_text(sm, 1, kv1.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sm, 2, vv1.c_str(), -1, SQLITE_TRANSIENT);
            sql_step_done_or_throw(sm, db, "insert meta1");

            sqlite3_bind_text(sm, 1, kv2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sm, 2, vv2.c_str(), -1, SQLITE_TRANSIENT);
            sql_step_done_or_throw(sm, db, "insert meta2");

            sqlite3_bind_text(sm, 1, kv3.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sm, 2, vv3.c_str(), -1, SQLITE_TRANSIENT);
            sql_step_done_or_throw(sm, db, "insert meta3");

            sqlite3_bind_text(sm, 1, kv4.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sm, 2, vv4.c_str(), -1, SQLITE_TRANSIENT);
            sql_step_done_or_throw(sm, db, "insert meta4");

            sqlite3_finalize(sm);
        }

        sql_exec_or_throw(db, "COMMIT;");

        sqlite3_finalize(ins_node);
        sqlite3_finalize(ins_edge);
        sqlite3_finalize(ins_str);
        sqlite3_finalize(ins_evs);
        sqlite3_finalize(ins_rng);

        sqlite3_close(db);
    } catch (...) {
        // 回滚并关闭
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        throw; // 交给上层 QSolverJob::saving() 打印
    }
}
