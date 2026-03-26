#include "include/solver/PCfrSolver.h"
#include "include/nodes/GameTreeNode.h"
#include "include/nodes/ActionNode.h"
#include "include/nodes/ChanceNode.h"
#include "include/nodes/GameActions.h"
#include "include/tools/utils.h"   // exchange_color

#include <fstream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <zstd.h>
#include <cstring>

// 稀疏块头：16 bytes
struct Tsb2SparseHdr {
    uint32_t magic;    // 'SPU1' = 0x31555053
    uint16_t A;        // 动作数（校验用）
    uint16_t flags;    // 预留=0
    uint32_t vals_cnt; // vals 的 uint16 个数
    uint32_t qsum;     // 65535
};

static inline std::vector<uint8_t>
encode_sparse_maskvals_u16(const uint16_t* qbuf, int A, int H) {
    const uint32_t QSUM = 65535u;

    const size_t mask_bits  = (size_t)A * (size_t)H;
    const size_t mask_bytes = (mask_bits + 7) / 8;

    std::vector<uint8_t> mask(mask_bytes, 0);
    std::vector<uint16_t> vals;
    vals.reserve((size_t)H * (size_t)std::max(0, A - 1)); // 上界

    for (int h = 0; h < H; ++h) {
        int m = 0;

        // 1) 生成 mask（按 hand-major：bit_index = h*A + a）
        for (int a = 0; a < A; ++a) {
            uint16_t v = qbuf[(size_t)a * (size_t)H + (size_t)h]; // action-major 存储
            if (v) {
                ++m;
                size_t bi = (size_t)h * (size_t)A + (size_t)a;
                mask[bi >> 3] |= (uint8_t)(1u << (bi & 7));
            }
        }

        // 2) 写入 (m-1) 个非零值
        if (m <= 1) continue; // 纯策略行：不写 vals，最后一个非零=QSUM

        uint32_t left = QSUM;
        int written = 0;
        for (int a = 0; a < A; ++a) {
            uint16_t v = qbuf[(size_t)a * (size_t)H + (size_t)h];
            if (!v) continue;

            if (written < m - 1) {
                vals.push_back(v);
                left -= (uint32_t)v;
                ++written;
            } else {
                break; // 最后一个非零由 left 补齐
            }
        }
    }

    Tsb2SparseHdr hdr;
    hdr.magic    = 0x31555053u;     // 'SPU1'
    hdr.A        = (uint16_t)A;
    hdr.flags    = 0;
    hdr.vals_cnt = (uint32_t)vals.size();
    hdr.qsum     = 65535u;

    std::vector<uint8_t> out;
    out.resize(sizeof(Tsb2SparseHdr) + mask_bytes + (size_t)vals.size() * 2);

    std::memcpy(out.data(), &hdr, sizeof(hdr));
    std::memcpy(out.data() + sizeof(hdr), mask.data(), mask_bytes);
    std::memcpy(out.data() + sizeof(hdr) + mask_bytes, vals.data(), (size_t)vals.size() * 2);
    return out;
}

static inline bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf) == 0;
}
static inline std::string strip_ext(const std::string& p) {
    auto pos = p.find_last_of('.');
    if (pos == std::string::npos) return p;
    return p.substr(0, pos);
}

template<typename T>
static inline void write_pod(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
static inline void write_bytes(std::ofstream& f, const void* p, size_t n) {
    f.write(reinterpret_cast<const char*>(p), (std::streamsize)n);
}
// ---------------- block-level Zstd compression for TSB ----------------
// Each strategy block is stored as: [u32 raw_bytes][u32 comp_bytes][comp_payload]
// .tsx 'offset' points to the beginning of this header, and 'bytes' equals 8 + comp_bytes.
static inline uint32_t write_zstd_block(std::ofstream& f, const void* raw, uint32_t raw_bytes, int clevel) {
    size_t bound = ZSTD_compressBound((size_t)raw_bytes);
    std::vector<char> tmp(bound);
    size_t comp = ZSTD_compress(tmp.data(), bound, raw, (size_t)raw_bytes, clevel);
    if (ZSTD_isError(comp)) {
        const char* err = ZSTD_getErrorName(comp);
        throw std::runtime_error(std::string("Zstd compress failed: ") + (err ? err : "unknown"));
    }
    uint32_t comp_bytes = (uint32_t)comp;
    write_pod(f, raw_bytes);
    write_pod(f, comp_bytes);
    write_bytes(f, tmp.data(), comp_bytes);
    return 8u + comp_bytes;
}

// ---------------- deal encode/decode (matches StrategyJsonExporterV2::make_deal_global_) ----------------
// deck_idx is assumed to be 0..(D-1) where D==52 and equals Card::card2int(...)
static inline int make_deal_turn(int deck_idx, int D) { return deck_idx + 1; }
static inline int make_deal_river(int turn_idx, int river_idx, int D) { return D * turn_idx + river_idx + (1 + D); }

// ---------------- quantize float strategy to u16 with sum=65535 ----------------
static inline void quantize_col_u16(const float* src, uint16_t* dst, int A) {
    // src: A probabilities, assume >=0
    double sum = 0.0;
    for (int i = 0; i < A; ++i) sum += (src[i] > 0.f ? (double)src[i] : 0.0);
    if (sum <= 0.0) {
        // all zero -> uniform
        uint16_t q = (uint16_t)(65535 / A);
        uint32_t acc = 0;
        for (int i = 0; i < A; ++i) { dst[i] = q; acc += q; }
        // fix remainder
        if (acc != 65535) dst[0] = (uint16_t)(dst[0] + (65535 - acc));
        return;
    }
    // first pass
    uint32_t acc = 0;
    int best = 0;
    double best_frac = -1.0;
    for (int i = 0; i < A; ++i) {
        double p = (src[i] > 0.f ? (double)src[i] : 0.0) / sum;
        double x = p * 65535.0;
        uint32_t q = (uint32_t)(x + 0.5); // round
        if (q > 65535u) q = 65535u;
        dst[i] = (uint16_t)q;
        acc += q;
        double frac = x - (double)(uint32_t)x;
        if (frac > best_frac) { best_frac = frac; best = i; }
    }
    // fix to exact 65535
    if (acc != 65535u) {
        int32_t diff = (int32_t)65535 - (int32_t)acc;
        int32_t v = (int32_t)dst[best] + diff;
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
        dst[best] = (uint16_t)v;
    }
}

// Compute representative deal + exch_stack pairs for a given actual deal (turn/river), using PCfrSolver::getColorIsoOffset.
// Returns rep_deal, and fills exch_pairs with pairs (suit_a, suit_b) in the same semantics as StrategyJsonExporterV2 (off<0 => push {suit, suit+off}).
static inline int compute_rep_deal_and_swaps(const PCfrSolver* pcfr,
                                             int deal_actual,
                                             int D,
                                             std::vector<std::pair<int,int>>& exch_pairs) {
    exch_pairs.clear();
    std::vector<int> rep_cards; rep_cards.reserve(2);

    auto deal_prefix = [&](const std::vector<int>& cc) -> int {
        int deal = 0;
        for (size_t k=0;k<cc.size();++k){
            int deck_idx = cc[k];
            if (k==0) deal = make_deal_turn(deck_idx, D);
            else { int origin = deal - 1; deal = make_deal_river(origin, deck_idx, D); }
        }
        return deal;
    };

    auto process_card = [&](int card_int) {
        int deal_now = deal_prefix(rep_cards); // rep prefix deal (0 / turn rep deal)
        int suit = card_int % 4;
        int off = 0;
        try { off = pcfr->getColorIsoOffset(deal_now, suit); } catch(...) { off = 0; }
        int rep_card = card_int;
        if (off < 0) {
            rep_card = card_int + off;
            exch_pairs.push_back({suit, suit + off});
        }
        rep_cards.push_back(rep_card);
    };

    if (deal_actual == 0) {
        return 0;
    } else if (deal_actual >= 1 && deal_actual <= D) {
        int turn_idx = deal_actual - 1;
        process_card(turn_idx);
        return deal_prefix(rep_cards);
    } else {
        int tmp = deal_actual - (1 + D);
        int turn_idx = tmp / D;
        int river_idx = tmp % D;
        process_card(turn_idx);
        process_card(river_idx);
        return deal_prefix(rep_cards);
    }
}

// Build a suit-permutation signature from exch_pairs (order-independent final mapping).
static inline std::array<uint8_t,4> suit_signature(const std::vector<std::pair<int,int>>& exch_pairs) {
    std::array<uint8_t,4> m = {0,1,2,3}; // image mapping
    for (auto& pr : exch_pairs){
        int a = pr.first, b = pr.second;
        // apply swap(a,b) to image m
        for (int i=0;i<4;++i){
            if (m[i]==(uint8_t)a) m[i]=(uint8_t)b;
            else if (m[i]==(uint8_t)b) m[i]=(uint8_t)a;
        }
    }
    return m;
}

void PCfrSolver::dump_strategy_tsb2(const std::string& out_path, int max_depth) {
    if (!this->tree) throw std::runtime_error("tree is null");
    auto root = this->tree->getRoot();
    if (!root) throw std::runtime_error("tree root is null");

    // 1) base path
    std::string base = out_path;
    if (ends_with(base, ".tsb") || ends_with(base, ".bin") || ends_with(base, ".gz") || ends_with(base, ".tsx") || ends_with(base, ".tsm"))
        base = strip_ext(base);

    const std::string tsb_path = base + ".tsb";
    const std::string tsx_path = base + ".tsx";
    const std::string tsm_path = base + ".tsm";

    // deck size: we assume 52 (card2int space). If your Deck isn't 52, keep it in sync with Card::strCard2int mapping.
    const int D = 52;

    // 2) BFS assign node_id (compress chance: only keep 1 child)
    std::unordered_map<GameTreeNode*, uint32_t> id;
    std::vector<std::shared_ptr<GameTreeNode>> nodes; nodes.reserve(4096);

    auto add_node = [&](const std::shared_ptr<GameTreeNode>& n) -> uint32_t {
        auto it = id.find(n.get());
        if (it != id.end()) return it->second;
        uint32_t nid = (uint32_t)nodes.size();
        id.emplace(n.get(), nid);
        nodes.push_back(n);
        return nid;
    };

    std::queue<std::shared_ptr<GameTreeNode>> q;
    add_node(root);
    q.push(root);

    while (!q.empty()) {
        auto n = q.front(); q.pop();
        if (!n) continue;

        // depth limiter (optional): if max_depth>0, stop expanding deeper action branches
        if (max_depth > 0 && (int)n->depth >= max_depth) continue;


        switch (n->getType()) {
        case GameTreeNode::ACTION: {
            auto an = std::static_pointer_cast<ActionNode>(n);
            auto& chs = an->getChildrens();
            for (auto& c : chs) { add_node(c); q.push(c); }
        } break;
        case GameTreeNode::CHANCE: {
            auto cn = std::static_pointer_cast<ChanceNode>(n);
            auto c = cn->getChildren();
            if (c) { add_node(c); q.push(c); }
        } break;
        case GameTreeNode::SHOWDOWN:
        case GameTreeNode::TERMINAL:
        default:
            break;
        }
    }

    const uint32_t N = (uint32_t)nodes.size();

    // 3) actionDict (only ACTION node actions; chance compressed)
    std::unordered_map<std::string, uint32_t> action_id;
    std::vector<std::string> action_dict; action_dict.reserve(512);

    auto get_action_id = [&](const std::string& s) -> uint32_t {
        auto it = action_id.find(s);
        if (it != action_id.end()) return it->second;
        uint32_t aid = (uint32_t)action_dict.size();
        action_dict.push_back(s);
        action_id.emplace(s, aid);
        return aid;
    };

    // 4) meta nodes SoA + blobs
    std::vector<uint8_t>  node_type(N, 0);
    std::vector<uint8_t>  node_round(N, 0);
    std::vector<int8_t>   node_player(N, -1);
    std::vector<uint16_t> node_deg(N, 0);
    std::vector<uint32_t> node_lab_off(N, 0);
    std::vector<uint32_t> node_chi_off(N, 0);

    std::vector<uint32_t> labels_blob;   labels_blob.reserve(8192);
    std::vector<uint32_t> children_blob; children_blob.reserve(8192);

    for (uint32_t nid = 0; nid < N; ++nid) {
        auto n = nodes[nid];
        node_type[nid]  = (uint8_t)n->getType();
        node_round[nid] = (uint8_t)GameTreeNode::gameRound2int(n->getRound());

        if (n->getType() == GameTreeNode::ACTION) {
            auto an = std::static_pointer_cast<ActionNode>(n);
            node_player[nid] = (int8_t)an->getPlayer();

            auto& acts = an->getActions();
            auto& chs  = an->getChildrens();
            uint16_t A = (uint16_t)acts.size();

            node_deg[nid]     = A;
            node_lab_off[nid] = (uint32_t)labels_blob.size();
            node_chi_off[nid] = (uint32_t)children_blob.size();

            for (size_t i = 0; i < acts.size(); ++i) {
                uint32_t aid = get_action_id(acts[i].toString());
                labels_blob.push_back(aid);
            }
            for (size_t i = 0; i < chs.size(); ++i) {
                uint32_t cid = id[chs[i].get()];
                children_blob.push_back(cid);
            }
        } else if (n->getType() == GameTreeNode::CHANCE) {
            auto cn = std::static_pointer_cast<ChanceNode>(n);
            node_player[nid] = (int8_t)cn->getPlayer();
            auto c = cn->getChildren();
            node_deg[nid]     = (c ? 1 : 0);
            node_lab_off[nid] = (uint32_t)labels_blob.size();
            node_chi_off[nid] = (uint32_t)children_blob.size();
            if (c) {
                labels_blob.push_back(0); // unused
                children_blob.push_back(id[c.get()]);
            }
        } else {
            node_player[nid] = -1;
            node_deg[nid] = 0;
            node_lab_off[nid] = (uint32_t)labels_blob.size();
            node_chi_off[nid] = (uint32_t)children_blob.size();
        }
    }

    // 5) write .tsm (TSM4): meta + base ranges + iso offsets + hand permutation tables
    // action blob + offsets
    std::vector<uint32_t> aoff; aoff.reserve(action_dict.size() + 1);
    std::string ablob; ablob.reserve(4096);
    uint32_t cur = 0;
    aoff.push_back(0);
    for (auto& s : action_dict) { ablob.append(s); cur += (uint32_t)s.size(); aoff.push_back(cur); }

    // Prepare hand permutation tables cache: (player, suitSig[4]) -> hid
    struct HTKey {
        uint8_t player;
        std::array<uint8_t,4> sig;
        bool operator==(const HTKey& o) const { return player==o.player && sig==o.sig; }
    };
    struct HTKeyHash {
        size_t operator()(const HTKey& k) const noexcept {
            uint32_t x = (uint32_t)k.player;
            x = x*131u + k.sig[0];
            x = x*131u + k.sig[1];
            x = x*131u + k.sig[2];
            x = x*131u + k.sig[3];
            return (size_t)x;
        }
    };

    std::unordered_map<HTKey, uint32_t, HTKeyHash> ht_id;
    std::vector<uint8_t>  ht_player;
    std::vector<std::vector<uint16_t>> ht_perm;

    auto get_hid = [&](int player, const std::vector<std::pair<int,int>>& exch_pairs) -> uint32_t {
        HTKey k;
        k.player = (uint8_t)player;
        k.sig = suit_signature(exch_pairs);
        auto it = ht_id.find(k);
        if (it != ht_id.end()) return it->second;

        // build perm for this player
        const auto& rng = this->ranges[player];
        const int H = (int)rng.size();
        std::vector<uint16_t> perm(H);
        for (int i=0;i<H;++i) perm[i] = (uint16_t)i;

        for (auto& pr : exch_pairs) {
            int a = pr.first, b = pr.second;
            if (a == b) continue;
            if (a > b) std::swap(a,b);
            exchange_color<uint16_t>(perm, rng, a, b);
        }

        uint32_t hid = (uint32_t)ht_perm.size();
        ht_id.emplace(k, hid);
        ht_player.push_back((uint8_t)player);
        ht_perm.push_back(std::move(perm));
        return hid;
    };

    // For convenience, reserve identity tables for each player (no swaps)
    for (int p=0;p<(int)this->ranges.size();++p){
        std::vector<std::pair<int,int>> none;
        get_hid(p, none);
    }

    // write tsm first (we already have perm tables cache but will grow while exporting tsx)
    // We'll write later after building all ht_perm (need final count). So keep buffers for now.

    // 6) write .tsb and build .tsx2 entries (key=(nid<<32)|deal_actual, with hid)
    struct IdxEnt { uint64_t key; uint64_t offset; uint32_t bytes; uint32_t hid; };
    std::vector<IdxEnt> idx;
    idx.reserve(1<<20);

    // dedupe strategy blocks per (nid, rep_deal)
    struct RepKey { uint32_t nid; uint32_t rep_deal; bool operator==(const RepKey& o) const { return nid==o.nid && rep_deal==o.rep_deal; } };
    struct RepKeyHash { size_t operator()(const RepKey& k) const noexcept { return ((size_t)k.nid<<32) ^ (size_t)k.rep_deal; } };
    std::unordered_map<RepKey, std::pair<uint64_t,uint32_t>, RepKeyHash> rep_written;

    std::ofstream fb(tsb_path, std::ios::binary);
    if (!fb) throw std::runtime_error("open .tsb failed");
    {
        char magic[4] = {'T','S','B','2'};
        fb.write(magic, 4);
        uint32_t ver = 4;
        uint32_t reserved = 2;
        write_pod(fb, ver);
        write_pod(fb, reserved);
    }
    uint64_t cur_off = (uint64_t)fb.tellp();

    // precompute used cards from initial_board (flop) if present
    std::array<uint8_t,52> used = {0};
    for (int c : this->initial_board) {
        if (c >= 0 && c < 52) used[(size_t)c] = 1;
    }

    std::vector<std::pair<int,int>> exch_pairs;

    for (uint32_t nid = 0; nid < N; ++nid) {
        if (node_type[nid] != (uint8_t)GameTreeNode::ACTION) continue;

        auto an = std::static_pointer_cast<ActionNode>(nodes[nid]);
        const int player = (int)an->getPlayer();
        const int H = (int)this->ranges[player].size();
        const int A = (int)an->getActions().size();
        if (A <= 0 || H <= 0) continue;

        int gr = (int)node_round[nid];

        auto emit_one = [&](int deal_actual) {
            // skip impossible combos quickly
            if (deal_actual == 0) {
                // ok
            } else if (deal_actual >= 1 && deal_actual <= D) {
                int t = deal_actual - 1;
                if (t < 0 || t >= 52) return;
                if (used[(size_t)t]) return;
            } else {
                int tmp = deal_actual - (1 + D);
                int t = tmp / D;
                int r = tmp % D;
                if (t < 0 || t >= 52 || r < 0 || r >= 52) return;
                if (t == r) return;
                if (used[(size_t)t] || used[(size_t)r]) return;
            }

            int rep_deal = compute_rep_deal_and_swaps(this, deal_actual, D, exch_pairs);
            // compute hid for this player + suit permutation
            uint32_t hid = get_hid(player, exch_pairs);

            RepKey rk{nid, (uint32_t)rep_deal};
            auto itw = rep_written.find(rk);
            uint64_t off;
            uint32_t bytes;
            if (itw != rep_written.end()) {
                off = itw->second.first;
                bytes = itw->second.second;
            } else {
                std::shared_ptr<Trainable> tr;
                try { tr = an->getTrainable(rep_deal, /*create_on_site*/false, /*use_halffloats*/0); } catch (...) { tr.reset(); }
                if (!tr) return;

                // strategy float: size A*H (row-major by action then hand)
                auto strat = tr->getAverageStrategy();
                if ((int)strat.size() != A*H) return;

                std::vector<uint16_t> qbuf((size_t)A*(size_t)H);
                // quantize per hand column (ensure sum 65535)
                std::vector<float> col((size_t)A);
                std::vector<uint16_t> qcol((size_t)A);

                for (int h=0; h<H; ++h) {
                    for (int a=0; a<A; ++a) col[a] = strat[(size_t)a*(size_t)H + (size_t)h];
                    quantize_col_u16(col.data(), qcol.data(), A);
                    for (int a=0; a<A; ++a) qbuf[(size_t)a*(size_t)H + (size_t)h] = qcol[a];
                }

                auto sblob = encode_sparse_maskvals_u16(qbuf.data(), A, H);

                off = cur_off;
                bytes = write_zstd_block(fb, sblob.data(), (uint32_t)sblob.size(), /*level=*/12);
                cur_off += bytes;

                rep_written.emplace(rk, std::make_pair(off, bytes));
            }

            uint64_t key = (uint64_t(nid) << 32) | (uint32_t)deal_actual;
            idx.push_back({key, off, bytes, hid});
        };

        if (gr <= 1) { // preflop/flop
            emit_one(0);
        } else if (gr == 2) { // turn: 1..D
            for (int t=0; t<52; ++t) emit_one(make_deal_turn(t, D));
        } else { // river: all (turn,river)
            for (int t=0; t<52; ++t) {
                for (int r=0; r<52; ++r) {
                    emit_one(make_deal_river(t, r, D));
                }
            }
        }
    }

    fb.flush();
    fb.close();

    // sort index by key
    std::sort(idx.begin(), idx.end(), [](const IdxEnt& a, const IdxEnt& b){ return a.key < b.key; });

    // write .tsx (TSX2 with hid)
    {
        std::ofstream fx(tsx_path, std::ios::binary);
        if (!fx) throw std::runtime_error("open .tsx failed");

        char magic[4] = {'T','S','X','2'};
        fx.write(magic, 4);
        uint32_t ver = 2;
        uint32_t reserved = 0;
        write_pod(fx, ver);
        write_pod(fx, reserved);

        uint64_t M = (uint64_t)idx.size();
        write_pod(fx, M);

        for (auto& e : idx) write_pod(fx, e.key);
        for (auto& e : idx) write_pod(fx, e.offset);
        for (auto& e : idx) write_pod(fx, e.bytes);
        for (auto& e : idx) write_pod(fx, e.hid);
    }

    // finally write .tsm with final ht tables
    {
        std::ofstream f(tsm_path, std::ios::binary);
        if (!f) throw std::runtime_error("open .tsm failed");

        char magic[4] = {'T','S','M','4'};
        f.write(magic, 4);
        uint32_t ver = 4;
        write_pod(f, ver);

        uint32_t deck_size = (uint32_t)D;
        write_pod(f, deck_size);

        // initial board (flop) cards as uint8 list
        uint32_t bn = (uint32_t)this->initial_board.size();
        write_pod(f, bn);
        for (uint32_t i=0;i<bn;++i){
            uint8_t c = (uint8_t)this->initial_board[i];
            write_pod(f, c);
        }

        // color iso table (full 52*52*2 rows is tiny and keeps parity with getColorIsoOffset index space)
        uint32_t iso_rows = 52*52*2;
        write_pod(f, iso_rows);
        for (uint32_t d=0; d<iso_rows; ++d) {
            auto row = this->get_color_iso_offset_row((int)d);
            int8_t s0 = (int8_t)row[0];
            int8_t s1 = (int8_t)row[1];
            int8_t s2 = (int8_t)row[2];
            int8_t s3 = (int8_t)row[3];
            write_pod(f, s0); write_pod(f, s1); write_pod(f, s2); write_pod(f, s3);
        }

        // action dict
        uint32_t action_cnt = (uint32_t)action_dict.size();
        uint32_t ablob_bytes = (uint32_t)ablob.size();
        write_pod(f, action_cnt);
        write_pod(f, ablob_bytes);
        for (uint32_t v : aoff) write_pod(f, v);
        write_bytes(f, ablob.data(), ablob.size());

        // base ranges: for each player store pairs (uint8,uint8)
        uint32_t P = (uint32_t)this->ranges.size();
        write_pod(f, P);
        for (uint32_t p=0;p<P;++p){
            const auto& rng = this->ranges[p];
            uint32_t H = (uint32_t)rng.size();
            write_pod(f, H);
            for (uint32_t i=0;i<H;++i){
                uint8_t c1 = (uint8_t)rng[i].card1;
                uint8_t c2 = (uint8_t)rng[i].card2;
                write_pod(f, c1); write_pod(f, c2);
            }
        }

        // nodes SoA
        write_pod(f, N);
        write_bytes(f, node_type.data(), node_type.size());
        write_bytes(f, node_round.data(), node_round.size());
        write_bytes(f, node_player.data(), node_player.size()*sizeof(int8_t));
        write_bytes(f, node_deg.data(), node_deg.size()*sizeof(uint16_t));
        write_bytes(f, node_lab_off.data(), node_lab_off.size()*sizeof(uint32_t));
        write_bytes(f, node_chi_off.data(), node_chi_off.size()*sizeof(uint32_t));

        uint32_t L = (uint32_t)labels_blob.size();
        uint32_t C = (uint32_t)children_blob.size();
        write_pod(f, L);
        write_pod(f, C);
        write_bytes(f, labels_blob.data(), labels_blob.size()*sizeof(uint32_t));
        write_bytes(f, children_blob.data(), children_blob.size()*sizeof(uint32_t));

        // hand permutation tables
        uint32_t HT = (uint32_t)ht_perm.size();
        write_pod(f, HT);
        for (uint32_t hid=0; hid<HT; ++hid){
            uint8_t p = ht_player[hid];
            write_pod(f, p);
            uint32_t H = (uint32_t)ht_perm[hid].size();
            write_pod(f, H);
            write_bytes(f, ht_perm[hid].data(), ht_perm[hid].size()*sizeof(uint16_t));
        }
    }
}
