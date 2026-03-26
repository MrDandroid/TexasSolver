#include "include/solver/PCfrSolver.h"
#include "include/nodes/GameTreeNode.h"
#include "include/nodes/ActionNode.h"
#include "include/nodes/ChanceNode.h"
#include "include/trainable/Trainable.h"
#include <queue>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <chrono>
#include <algorithm>

// --- little helpers ---
static inline uint16_t q16(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    // 0..65535
    return (uint16_t) (x * 65535.0f + 0.5f);
}

static inline uint32_t fourcc(const char a, const char b, const char c, const char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b<<8) | ((uint32_t)(uint8_t)c<<16) | ((uint32_t)(uint8_t)d<<24);
}

void PCfrSolver::dump_strategy_bin(const std::string& bin_path,
                                   const std::string& idx_path,
                                   int max_depth,
                                   bool /*use_fp16*/,
                                   bool /*align8*/) {
    auto t_begin = std::chrono::steady_clock::now();

    FILE* f = std::fopen(bin_path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open bin for write: " + bin_path);

    std::ofstream idx(idx_path, std::ios::binary);
    if (!idx) {
        std::fclose(f);
        throw std::runtime_error("cannot open idx for write: " + idx_path);
    }

    const auto root = this->tree->getRoot();
    const int root_round = GameTreeNode::gameRound2int(root->getRound());
    const int deck_size = (int)this->deck.getCards().size();

    // ===== File header =====
    // magic + version
    uint32_t magic = fourcc('T','S','B','1');
    uint32_t version = 1;
    std::fwrite(&magic,   4, 1, f);
    std::fwrite(&version, 4, 1, f);

    // deck_size + root_round + max_depth
    uint32_t u_deck = (uint32_t)deck_size;
    uint32_t u_root_round = (uint32_t)root_round;
    uint32_t u_max_depth = (uint32_t)max_depth;
    std::fwrite(&u_deck, 4, 1, f);
    std::fwrite(&u_root_round, 4, 1, f);
    std::fwrite(&u_max_depth, 4, 1, f);

    // ===== ranges (hands) for both players: write as (card1, card2) uint8 pairs =====
    // player 0
    {
        uint32_t n = (uint32_t)this->ranges[0].size();
        std::fwrite(&n, 4, 1, f);
        for (auto &hc : this->ranges[0]) {
            uint8_t c1 = (uint8_t)hc.card1;
            uint8_t c2 = (uint8_t)hc.card2;
            std::fwrite(&c1, 1, 1, f);
            std::fwrite(&c2, 1, 1, f);
        }
    }
    // player 1
    {
        uint32_t n = (uint32_t)this->ranges[1].size();
        std::fwrite(&n, 4, 1, f);
        for (auto &hc : this->ranges[1]) {
            uint8_t c1 = (uint8_t)hc.card1;
            uint8_t c2 = (uint8_t)hc.card2;
            std::fwrite(&c1, 1, 1, f);
            std::fwrite(&c2, 1, 1, f);
        }
    }

    // ===== BFS traverse tree and dump action nodes =====
    std::queue<std::shared_ptr<GameTreeNode>> q;
    q.push(root);

    uint32_t node_id = 0;
    uint64_t rec_count = 0;

    double ms_getavg = 0.0;
    double ms_quant_write = 0.0;

    while (!q.empty()) {
        auto n = q.front(); q.pop();
        const int cur_round = GameTreeNode::gameRound2int(n->getRound());
        const int gap = cur_round - root_round;
        if (gap > max_depth) continue;

        if (n->getType() == GameTreeNode::ACTION) {
            auto an = std::dynamic_pointer_cast<ActionNode>(n);
            const int player = an->getPlayer();

            const uint16_t action_cnt = (uint16_t)an->getActions().size();
            const uint32_t hand_cnt = (uint32_t)this->ranges[player].size();
            const int deal_cnt = an->getTrainablesSize();// 注意：trainables 在 ActionNode 里是 public 吗？如果不是，用 an->getTrainable(i,...) 直接探测即可

            for (int deal = 0; deal < deal_cnt; ++deal) {
                std::shared_ptr<Trainable> tr;
                try {
                    tr = an->getTrainable(deal, /*create_on_site*/false, /*use_halffloats*/0);
                } catch (...) {
                    tr.reset();
                }
                if (!tr) continue;

                auto t0 = std::chrono::steady_clock::now();
                std::vector<float> avg = tr->getAverageStrategy(); // 会分配，但比 JSON 轻得多
                auto t1 = std::chrono::steady_clock::now();

                const uint32_t expect = (uint32_t)action_cnt * hand_cnt;
                if ((uint32_t)avg.size() != expect) {
                    // 数据不一致就跳过（验证版）
                    continue;
                }

                // record header offset
                long off = std::ftell(f);
                if (off < 0) off = 0;

                // --- Record header ---
                // tag 'NODE'
                uint32_t tag = fourcc('N','O','D','E');
                std::fwrite(&tag, 4, 1, f);

                uint8_t u_player = (uint8_t)player;
                uint8_t u_round = (uint8_t)cur_round;
                uint16_t u_actions = action_cnt;
                uint32_t u_hands = hand_cnt;
                uint32_t u_deal = (uint32_t)deal;
                uint32_t u_node_id = node_id++;

                std::fwrite(&u_player, 1, 1, f);
                std::fwrite(&u_round,  1, 1, f);
                std::fwrite(&u_actions,2, 1, f);
                std::fwrite(&u_hands,  4, 1, f);
                std::fwrite(&u_deal,   4, 1, f);
                std::fwrite(&u_node_id,4, 1, f);

                // --- quantize & write ---
                // SoA layout: avg[action*hand + hand_id]
                std::vector<uint16_t> qbuf;
                qbuf.resize(expect);
                for (uint32_t i = 0; i < expect; ++i) qbuf[i] = q16(avg[i]);
                std::fwrite(qbuf.data(), sizeof(uint16_t), expect, f);

                auto t2 = std::chrono::steady_clock::now();

                ms_getavg += std::chrono::duration<double, std::milli>(t1 - t0).count();
                ms_quant_write += std::chrono::duration<double, std::milli>(t2 - t1).count();

                // idx line: node_id deal player round offset bytes
                const uint64_t bytes = 4 + 1 + 1 + 2 + 4 + 4 + 4 + (uint64_t)expect * 2;
                idx << u_node_id << " "
                    << (int)u_round << " "
                    << (int)u_player << " "
                    << u_deal << " "
                    << (uint64_t)off << " "
                    << bytes << "\n";
                rec_count++;
            }

            // enqueue children
            const auto& ch = an->getChildrens();
            for (auto &c : ch) if (c) q.push(c);

        } else if (n->getType() == GameTreeNode::CHANCE) {
            auto cn = std::dynamic_pointer_cast<ChanceNode>(n);
            auto c = cn->getChildren();
            if (c) q.push(c);
        } else {
            // TERMINAL / SHOWDOWN: ignore
        }
    }

    std::fflush(f);
    std::fclose(f);
    idx.flush();
    idx.close();

    auto t_end = std::chrono::steady_clock::now();
    const double ms_total = std::chrono::duration<double, std::milli>(t_end - t_begin).count();

    // 你会在日志里看到：到底是 getAverageStrategy 慢，还是写文件慢
    std::fprintf(stderr,
                 "[dump_strategy_bin] records=%llu total=%.1fms  getAvg=%.1fms  quant+write=%.1fms  out=%s  idx=%s\n",
                 (unsigned long long)rec_count, ms_total, ms_getavg, ms_quant_write,
                 bin_path.c_str(), idx_path.c_str());
}
