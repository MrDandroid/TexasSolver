#include "include/solver/PCfrSolver.h"
#include "include/json.hpp"
#include <fstream>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <zlib.h>

using nlohmann::json;

// --------- 简易行写入器：自动识别 .gz 与普通文件 ----------
namespace {
struct LineWriter {
    bool use_gz = false;
    std::ofstream ofs;
    gzFile gz = nullptr;

    explicit LineWriter(const std::string& path) {
        auto ends_with = [](const std::string& s, const std::string& suf){
            if (s.size() < suf.size()) return false;
            return std::equal(suf.rbegin(), suf.rend(), s.rbegin());
        };
        use_gz = ends_with(path, ".gz");
        if (use_gz) {
            gz = gzopen(path.c_str(), "wb6");
            gzbuffer(gz, 4<<20); // 1MB
            if (!gz) throw std::runtime_error("cannot open gzip for write: " + path);
        } else {
            ofs.open(path, std::ios::binary);
            if (!ofs) throw std::runtime_error("cannot open output file: " + path);
        }
    }

    void write_json_line(const json& j) {
        std::string s = j.dump();
        s.push_back('\n');
        if (use_gz) {
            int w = gzwrite(gz, s.data(), (unsigned int)s.size());
            if (w != (int)s.size()) {
                int errnum = 0;
                const char* msg = gzerror(gz, &errnum);
                throw std::runtime_error(std::string("gzwrite failed: ") + (msg ? msg : ""));
            }
        } else {
            ofs.write(s.data(), (std::streamsize)s.size());
        }
    }

    void close() {
        if (use_gz && gz) {
            int rc = gzclose(gz);
            gz = nullptr;
            if (rc != Z_OK) throw std::runtime_error("gzclose failed");
        } else if (ofs.is_open()) {
            ofs.close();
        }
    }
    ~LineWriter() {
        try { if (use_gz && gz) gzclose(gz); else if (ofs.is_open()) ofs.close(); }
        catch (...) {} // 析构期不抛
    }
};

// 稀疏 + 量化（可选）
inline void quantize_and_prune(std::vector<float>& v, double eps, int precision) {
    double s = 0.0;
    if (eps > 0.0) {
        for (auto &x : v) { if (x < eps) x = 0.0f; s += x; }
        if (s > 0.0) for (auto &x : v) x = static_cast<float>(x / s);
    } else {
        for (auto &x : v) s += x;
    }
    if (precision >= 0) {
        float base = 1.f; for (int i=0;i<precision;++i) base *= 10.f;
        for (auto &x : v) x = std::round(x*base)/base;
    }
    if (!v.empty()) {
        double sum2=0.0; for (auto x:v) sum2+=x;
        if (sum2>0.0) {
            double d = 1.0 - sum2;
            if (std::fabs(d) < 1e-6) {
                for (int i=(int)v.size()-1; i>=0; --i) {
                    if (v[i]>0.f) { v[i] = (float)(v[i]+d); break; }
                }
            }
        }
    }
}

// 由 turn/river 的 deck 索引路径计算 deal
inline int make_deal_from_path(const std::vector<int>& chance_cards, int deck_size) {
    int deal = 0;
    for (size_t k=0;k<chance_cards.size();++k) {
        int c = chance_cards[k];
        if (k==0) deal = c + 1;
        else {
            int origin = deal - 1;
            deal = deck_size * origin + c + (1 + deck_size);
        }
    }
    return deal;
}

// flop/turn/river 标签（ASCII）
inline void board_labels(const std::vector<int>& init_board,
                         const std::vector<int>& chance_cards,
                         const Deck& deck,
                         std::string& flop, std::string& turn, std::string& river) {
    auto sCardInt = [](int ci){ return Card::intCard2Str(ci); };
    auto sDeckIdx = [&](int di){ return Card::intCard2Str(deck.getCards()[di].getCardInt()); };

    flop.clear(); turn="*"; river="*";
    if (init_board.size() >= 3) {
        flop = sCardInt(init_board[0]) + sCardInt(init_board[1]) + sCardInt(init_board[2]);
    }
    if (chance_cards.size() >= 1) turn  = sDeckIdx(chance_cards[0]);
    if (chance_cards.size() >= 2) river = sDeckIdx(chance_cards[1]);
}
} // namespace

// ------------------- 主函数：流式导出 -------------------
void PCfrSolver::dump_strategy_stream(const std::string& out_path,
                                      int max_depth,
                                      double prune_eps,
                                      int precision)
{
    LineWriter lw(out_path);

    // ===== meta 行 =====
    {
        json meta;
        meta["mode"] = (this->deck.getCards().size() < 52 ? "shortdeck" : "holdem");

        json jr = json::array();
        for (const auto& r : this->deck.getRanks()) jr.push_back(r);
        meta["ranks"] = std::move(jr);

        meta["suits"] = json::array({"c","d","h","s"});
        meta["deck_size"] = (int)this->deck.getCards().size();

        {
            auto root = this->tree->getRoot();
            std::string rr = "FLOP";
            if (root->getRound() == GameTreeNode::GameRound::TURN)   rr = "TURN";
            else if (root->getRound() == GameTreeNode::GameRound::RIVER) rr = "RIVER";
            meta["root_round"] = rr;
        }
        {
            json jb = json::array();
            for (int ci : this->initial_board) jb.push_back(Card::intCard2Str(ci));
            meta["initial_board"] = std::move(jb);
        }
        {
            json r1 = json::object();
            for (const auto& pc : this->range1) {
                std::string hs = const_cast<PrivateCards&>(pc).toString();
                r1[hs] = pc.weight;
            }
            meta["player1Range"] = std::move(r1);

            json r2 = json::object();
            for (const auto& pc : this->range2) {
                std::string hs = const_cast<PrivateCards&>(pc).toString();
                r2[hs] = pc.weight;
            }
            meta["player2Range"] = std::move(r2);
        }
        meta["policy_type"] = "current";
        meta["id_map"] = { {"OOP", 1}, {"IP", 0} };
        json line; line["meta"] = std::move(meta);
        lw.write_json_line(line);
    }

    // ===== DFS：只写 ACTION 节点；CHANCE 跳过但传递“可见父” =====
    std::vector<int> chance_cards; // turn/river 的 deck 索引
    std::unordered_map<const GameTreeNode*, int> id_of;
    id_of.reserve(1<<16);
    auto id_for = [&](const std::shared_ptr<GameTreeNode>& n)->int {
        auto it = id_of.find(n.get());
        if (it != id_of.end()) return it->second;
        int nid = (int)id_of.size() + 1;
        id_of.emplace(n.get(), nid);
        return nid;
    };

    // parent_visible_id：上一个“可见的 ACTION 节点 id”
    std::function<void(std::shared_ptr<GameTreeNode>, int depth, int parent_visible_id)> dfs;
    dfs = [&](std::shared_ptr<GameTreeNode> node, int depth, int parent_visible_id)
    {
        using GT = GameTreeNode::GameTreeNodeType;
        if (depth > max_depth) return;

        if (node->getType() == GT::ACTION) {
            auto an = std::dynamic_pointer_cast<ActionNode>(node);
            const int my_id = id_for(node);

            json jo;
            jo["node_id"]   = my_id;
            jo["parent_id"] = (parent_visible_id >= 0 ? json(parent_visible_id) : json(nullptr));
            jo["type"]      = "ACTION";

            std::string flop, turn, river;
            board_labels(this->initial_board, chance_cards, this->deck, flop, turn, river);
            jo["flop"]  = flop;
            jo["turn"]  = turn;
            jo["river"] = river;

            std::string street = "flop";
            if (!chance_cards.empty()) street = (chance_cards.size()==1 ? "turn" : "river");
            jo["street"] = street;

            const int pos = an->getPlayer();
            jo["pos"] = pos;
            jo["player"] = pos;
            jo["deal"] = make_deal_from_path(chance_cards, (int)this->deck.getCards().size());

            const auto& actions = an->getActions();
            {
                json a = json::array();
                for (const auto& act : actions) a.push_back(act.toString());
                jo["actions"] = std::move(a);
            }
            {
                json am = json::array();
                for (const auto& act : actions) { json x; x["name"]=act.toString(); am.push_back(x); }
                jo["actions_meta"] = std::move(am);
            }

            if (auto tr = an->getTrainable(jo["deal"].get<int>(), true, false)) {
                json st = tr->dump_strategy(false);      // 无状态版
                if (st.contains("strategy")) {
                    json outmap = json::object();
                    auto& smap = st["strategy"];
                    for (auto it = smap.begin(); it != smap.end(); ++it) {
                        std::vector<float> vec = it.value().get<std::vector<float>>();
                        //quantize_and_prune(vec, prune_eps, precision);
                        outmap[it.key()] = vec;
                    }
                    if (!outmap.empty()) jo["strategy"] = std::move(outmap);
                }
                try {
                    json evj = tr->dump_evs();           // EV（不量化）
                    if (evj.contains("evs")) jo["evs"] = evj["evs"];
                } catch (...) {}
            }

            lw.write_json_line(jo);

            // 递归：这里传入 my_id 作为“可见父”
            const auto& children = an->getChildrens();
            for (const auto& ch : children) {
                dfs(ch, depth, my_id);
            }
            return;
        }

        if (node->getType() == GT::CHANCE) {
            if (depth == max_depth) return;
            auto cn = std::dynamic_pointer_cast<ChanceNode>(node);
            auto child = cn->getChildren();
            const auto& cards = cn->getCards();
            // 注意：这里不改变 parent_visible_id（跳过 CHANCE）
            for (size_t i=0;i<cards.size();++i) {
                int deck_idx = cards[i].getNumberInDeckInt();
                chance_cards.push_back(deck_idx);
                dfs(child, depth + 1, parent_visible_id);
                chance_cards.pop_back();
            }
            return;
        }

        // SHOWDOWN / TERMINAL：不导出
    };

    dfs(this->tree->getRoot(), /*depth=*/0, /*parent_visible_id=*/-1);
}
