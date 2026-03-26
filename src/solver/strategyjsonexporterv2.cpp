#include "strategyjsonexporterv2.h"

#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cmath>

#include "include/solver/PCfrSolver.h"
#include "include/nodes/GameTreeNode.h"
#include "include/nodes/ActionNode.h"
#include "include/nodes/ChanceNode.h"
#include "include/trainable/Trainable.h"
#include "include/Card.h"
#include "include/json.hpp"

#include <QDebug>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <queue>


namespace {
// 无等号的 Base64（标准表，末尾去 '='）
static const char kB64Tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode_unpadded(const std::string& bin) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(bin.data());
    const size_t n = bin.size();
    std::string out; out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        const size_t r = n - i;
        const uint32_t tri = (uint32_t(s[i]) << 16)
                             | (uint32_t(r > 1 ? s[i+1] : 0) << 8)
                             |  uint32_t(r > 2 ? s[i+2] : 0);
        out.push_back(kB64Tab[(tri >> 18) & 0x3F]);
        out.push_back(kB64Tab[(tri >> 12) & 0x3F]);
        out.push_back(r > 1 ? kB64Tab[(tri >> 6) & 0x3F] : '=');
        out.push_back(r > 2 ? kB64Tab[(tri      ) & 0x3F] : '=');
    }
    while (!out.empty() && out.back()=='=') out.pop_back();
    return out;
}

// LSB-first 按 bits 打包到位流（这里 bits=7）
inline std::string pack_bits_lsb(const std::vector<uint16_t>& vals, int bits) {
    const size_t total_bits = vals.size() * (size_t)bits;
    const size_t out_bytes  = (total_bits + 7) / 8;
    std::string bin(out_bytes, '\0');
    size_t bitpos = 0;
    for (uint16_t v : vals) {
        uint32_t x = v;
        for (int i = 0; i < bits; ++i) {
            const size_t byte_idx = bitpos >> 3;
            const int    bit_in   = int(bitpos & 7);
            const uint8_t bit = (x >> i) & 1u; // LSB-first
            unsigned char cur = static_cast<unsigned char>(bin[byte_idx]);
            cur = static_cast<unsigned char>(cur | (bit << bit_in));
            bin[byte_idx] = static_cast<char>(cur);
            ++bitpos;
        }
    }
    return bin;
}
// ===== ANS 工具：规范化频率（sum = 1<<ans_log）=====
inline void ans_normalize_freq(const std::vector<uint64_t>& freq_raw,
                               int ans_log,
                               std::vector<uint16_t>& norm_out) {
    const uint32_t M = 1u << ans_log;
    const int K = (int)freq_raw.size();
    norm_out.assign(K, 0);

    long double total = 0;
    for (auto f : freq_raw) total += (long double)f;
    if (total == 0) { norm_out[0] = (uint16_t)M; return; }

    struct Part { int s; long double frac; };
    std::vector<Part> parts; parts.reserve(K);
    uint32_t used = 0;
    for (int s=0;s<K;++s){
        long double x = (long double)freq_raw[s] * (long double)M / total;
        uint32_t q = (uint32_t) std::floor(x);
        if (q > 0xFFFFu) q = 0xFFFFu;
        norm_out[s] = (uint16_t)q;
        used += q;
        parts.push_back({s, x - (long double)q});
    }

    // 先把剩余按小数部排序分配，凑到 M
    if (used < M) {
        std::stable_sort(parts.begin(), parts.end(),
                         [](const Part& a, const Part& b){ return a.frac > b.frac; });
        uint32_t need = M - used;
        for (uint32_t i=0;i<need && i<parts.size(); ++i){
            ++norm_out[parts[i].s];
        }
    } else if (used > M) {
        std::stable_sort(parts.begin(), parts.end(),
                         [](const Part& a, const Part& b){ return a.frac < b.frac; });
        uint32_t cut = used - M;
        for (uint32_t i=0;i<cut && i<parts.size(); ++i){
            if (norm_out[parts[i].s] > 0) --norm_out[parts[i].s];
        }
    }

    // 【新增】保证：凡是 freq_raw[s]>0 的符号，norm_out[s] 至少为 1
    uint32_t add = 0;
    for (int s=0;s<K;++s){
        if (freq_raw[s] > 0 && norm_out[s] == 0){
            norm_out[s] = 1;
            ++add;
        }
    }
    if (add){
        // 减去 add，从最大的 norm_out 里扣（保持和=M）
        // 构造索引按 norm_out 从大到小
        std::vector<int> order(K); std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a,int b){ return norm_out[a] > norm_out[b]; });
        uint32_t left = add;
        for (int idx=0; idx<K && left; ++idx){
            int s = order[idx];
            if (norm_out[s] > 1){
                --norm_out[s];
                --left;
            }
        }
    }

    uint32_t sum2 = 0;
    for (int s=0;s<K;++s) sum2 += norm_out[s];
    if (sum2 == 0) norm_out[0] = (uint16_t)M;
}

// 依据规范化频率计算累积
inline void ans_build_cum(const std::vector<uint16_t>& norm,
                          std::vector<uint32_t>& cum) {
    const int K = (int)norm.size();
    cum.assign(K+1, 0);
    for (int s=0;s<K;++s) cum[s+1] = cum[s] + (uint32_t)norm[s];
}

// ===== rANS 编码：把 vals (0..K-1) 熵编码成字节串 =====
// 输出字节布局： [final_state:u32 little-endian] + [2字节块序列...（编码过程中按顺序产生）]
inline std::string rans32_encode_vals(const std::vector<uint16_t>& vals,
                                      const std::vector<uint16_t>& norm,
                                      const std::vector<uint32_t>& cum,
                                      int ans_log) {
    const uint32_t M = 1u << ans_log;
    std::string out; out.reserve(4 + vals.size()/2);

    // 写入最终状态占位
    uint32_t x = 1u << 23; // 初始状态
    out.push_back('\0'); out.push_back('\0');
    out.push_back('\0'); out.push_back('\0');

    auto put16 = [&](uint16_t w){
        out.push_back((char)(w & 0xFF));
        out.push_back((char)((w >> 8) & 0xFF));
    };

    // 从后往前编码
    for (size_t idx = vals.size(); idx-- > 0; ){
        const uint32_t s = (uint32_t)vals[idx];
        const uint32_t f = (uint32_t)norm[(size_t)s];
        const uint32_t c = (uint32_t)cum[(size_t)s];

        // 规范化后出现 f==0 的符号会导致不可编码，这里直接把它当“退化符号”，写原始16bit避免死锁
        if (f == 0) { put16((uint16_t)s); continue; }

        // 关键修复点：用 64 位计算阈值，避免 32 位溢出导致 TH==0
        const uint64_t TH = (uint64_t)f << (32 - ans_log);
        while ((uint64_t)x >= TH) {
            put16((uint16_t)(x & 0xFFFFu));
            x >>= 16;
        }

        // rANS 更新：x' = floor(x/f)*M + (x % f) + c
        x = (x / f) * M + (x % f) + c;
    }

    // 回填最终状态
    out[0] = (char)( x        & 0xFF);
    out[1] = (char)((x >> 8 ) & 0xFF);
    out[2] = (char)((x >> 16) & 0xFF);
    out[3] = (char)((x >> 24) & 0xFF);

    return out;
}

// ===== Huffman: 计算码长（针对 0..K-1 的字母表）=====
struct HuffNode {
    uint64_t freq;
    int id;     // 叶子: 0..K-1；内部: K..(K+M-1)
    int left;   // -1 表示叶子
    int right;
};

inline void huffman_build_code_lengths(const std::vector<uint64_t>& freq,
                                       std::vector<uint8_t>& lens_out) {
    const int K = (int)freq.size();
    lens_out.assign(K, 0);
    // 收集非零频率的符号
    struct QItem {
        uint64_t f; int id;
        bool operator<(const QItem& o) const { return f > o.f; } // 小根堆
    };
    std::priority_queue<QItem> pq;
    int nonzero = 0;
    for (int s=0;s<K;++s){
        if (freq[s] > 0){ pq.push({freq[s], s}); ++nonzero; }
    }
    if (nonzero == 0){
        // 空流：保持 lens 为 0
        return;
    }
    if (nonzero == 1){
        // 只有一个符号：给它码长 1（避免空码）
        for (int s=0;s<K;++s) if (freq[s]>0) { lens_out[s] = 1; break; }
        return;
    }
    // 建树
    std::vector<HuffNode> nodes;
    nodes.reserve(2*K);
    // 初始化叶子节点（只为非零频率建节点）
    // 用映射：符号 id -> 节点索引
    std::vector<int> leaf_idx(K, -1);
    for (int s=0;s<K;++s){
        if (freq[s]==0) continue;
        int idx = (int)nodes.size();
        nodes.push_back({freq[s], s, -1, -1});
        leaf_idx[s] = idx;
        pq.push({freq[s], idx + K}); // 队列里用 id 区分：K 起算
    }
    // 注意：pq 现在同时放了“叶子的符号 s”和“节点索引 K+idx”，我们只用 K+idx 这类
    // 为避免混淆，重新构造：只把节点放 pq
    while(!pq.empty()) pq.pop();
    for (int i=0;i<(int)nodes.size();++i) pq.push({nodes[i].freq, K + i});

    auto pop_one = [&]() -> int {
        auto q = pq.top(); pq.pop();
        return q.id - K; // 返回节点索引
    };

    while (pq.size() >= 2){
        int a = pop_one();
        int b = pop_one();
        HuffNode na = nodes[a], nb = nodes[b];
        int idx = (int)nodes.size();
        nodes.push_back({na.freq + nb.freq, -1, a, b});
        pq.push({nodes[idx].freq, K + idx});
    }
    int root = pop_one();

    // DFS 计算码长
    std::vector<int> stack; stack.reserve(nodes.size());
    std::vector<int> depth(nodes.size(), 0);
    stack.push_back(root);
    while(!stack.empty()){
        int u = stack.back(); stack.pop_back();
        const HuffNode& nd = nodes[u];
        if (nd.left == -1 && nd.right == -1){
            // 叶子：映射到符号
            int s = nodes[u].id;
            // 找回符号 s 对应的节点索引
            // 我们最初把叶子的 id 设成符号 s
            lens_out[s] = (uint8_t)depth[u];
        }else{
            if (nd.left != -1){ depth[nd.left] = depth[u] + 1; stack.push_back(nd.left); }
            if (nd.right!= -1){ depth[nd.right]= depth[u]+ 1; stack.push_back(nd.right); }
        }
    }
    // 边界：若出现某些符号 freq=0，len=0 保持不变
    // 特例：若最短码长为 0（不应发生），上面已处理 nonzero==1 的情况
}

// === 依据码长生成“规范 Huffman”码字；可选择 LSB-first 反转便于位流写入 ===
inline void huffman_build_canonical_codes(const std::vector<uint8_t>& lens,
                                          std::vector<uint32_t>& codes,
                                          std::vector<uint8_t>& clen,
                                          bool emit_lsb_first = true) {
    const int K = (int)lens.size();
    clen = lens; codes.assign(K, 0);
    int Lmax = 0;
    for (int s=0;s<K;++s) if ((int)clen[s] > Lmax) Lmax = clen[s];
    if (Lmax == 0) return;

    std::vector<int> bl_count(Lmax+1, 0);
    for (int s=0;s<K;++s) if (clen[s]>0) ++bl_count[clen[s]];

    std::vector<uint32_t> next_code(Lmax+1, 0);
    uint32_t code = 0;
    for (int l=1;l<=Lmax;++l){
        code = (code + bl_count[l-1]) << 1;
        next_code[l] = code;
    }
    // 规范分配：长度相同按符号序
    std::vector<int> order; order.reserve(K);
    for (int s=0;s<K;++s) if (clen[s]>0) order.push_back(s);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b){
        if (clen[a] != clen[b]) return clen[a] < clen[b];
        return a < b;
    });
    for (int s : order){
        uint32_t c = next_code[clen[s]]++;
        if (emit_lsb_first){
            // 反转 bit 序，方便按 LSB-first 写出
            uint32_t r = 0;
            for (int i=0;i<clen[s];++i) r = (r<<1) | ((c>>i)&1u);
            codes[s] = r;
        }else{
            codes[s] = c;
        }
    }
}

// === 把符号流按 Huffman 码字写成字节串（LSB-first）===
inline std::string huffman_encode_LSB(const std::vector<uint16_t>& syms,
                                      const std::vector<uint32_t>& codes,
                                      const std::vector<uint8_t>& clen) {
    std::string out;
    out.reserve((syms.size()*3)/2); // 预估
    uint8_t cur = 0;
    int bitpos = 0; // 0..7
    auto put_bit = [&](uint8_t b){
        cur |= (b & 1u) << bitpos;
        if (++bitpos == 8){
            out.push_back((char)cur);
            cur = 0; bitpos = 0;
        }
    };
    for (uint16_t v : syms){
        uint8_t L = clen[(size_t)v];
        uint32_t C = codes[(size_t)v];
        // 这里 codes 已经按 LSB-first 反转过，顺序写 L 位
        for (int i=0;i<L;++i){
            put_bit((uint8_t)((C>>i)&1u));
        }
    }
    if (bitpos != 0){
        out.push_back((char)cur);
    }
    return out;
}


// === 新增：保和量化到“给定 QMAX”（用于 0..1000 无损整数域） ===
// 说明：把 p[i]（0..1）按比例分配到整数，使 sum(q)=QMAX；算法同 Hamilton 最大余数法。
inline void quantize_sum_to_QMAX(const std::vector<double>& p, int QMAX, std::vector<uint16_t>& q_out){
    const int N = (int)p.size();
    q_out.assign(N, 0);
    struct Item { int idx; double frac; };
    std::vector<Item> frac; frac.reserve(N);

    long long s = 0;
    for (int i=0;i<N;++i){
        double x = p[i];
        if (x < 0.0) x = 0.0;
        if (x > 1.0) x = 1.0;
        const double y = x * QMAX;
        long long f = (long long)std::floor(y);
        if (f < 0) f = 0;
        if (f > QMAX) f = QMAX;
        q_out[i] = (uint16_t)f;
        s += f;
        frac.push_back({i, y - (double)f});
    }
    long long need = QMAX - s;
    if (need > 0){
        std::stable_sort(frac.begin(), frac.end(), [](const Item& a, const Item& b){ return a.frac > b.frac; });
        for (long long k=0; k<need && k<(long long)frac.size(); ++k){
            const int i = frac[(size_t)k].idx;
            if (q_out[i] < (uint16_t)QMAX) ++q_out[i];
        }
    }else if (need < 0){
        std::stable_sort(frac.begin(), frac.end(), [](const Item& a, const Item& b){ return a.frac < b.frac; });
        for (long long k=0; k<-need && k<(long long)frac.size(); ++k){
            const int i = frac[(size_t)k].idx;
            if (q_out[i] > 0) --q_out[i];
        }
    }
    // 保证 sum(q_out) == QMAX
}

// === 兼容原有：按 kBits 的 QMAX = (1<<kBits)-1 的保和量化 ===
inline void quantize_sum_preserving(const std::vector<double>& p, int bits, std::vector<uint16_t>& q_out){
    const int QMAX = (1<<bits) - 1;
    quantize_sum_to_QMAX(p, QMAX, q_out);
}


// === 新增：量化后“等分行”判定（严格按 a/a+1 网格） ===
inline bool is_equal_quantized_row(const std::vector<uint16_t>& q){
    const int N = (int)q.size();
    if (N <= 0) return false;
    int sum = 0;
    for (int i=0;i<N;++i) sum += (int)q[i];
    if (sum <= 0) return false;
    const int a = sum / N;
    const int r = sum % N; // 前 r 个 a+1，其余 a
    for (int i=0;i<N;++i){
        const int expect = (i < r) ? (a+1) : a;
        if ((int)q[i] != expect) return false;
    }
    return true;
}


// 四舍五入到 2 位小数（×100），并 clamp 到 [0,100]
inline uint16_t quantize_p2(double x) {
    long long t = llround(x * 100.0);
    if (t < 0)   t = 0;
    if (t > 100) t = 100;
    return static_cast<uint16_t>(t);
}
// 按 bits 量化到 [0 .. (1<<bits)-1]
inline uint16_t quantize_ubits(double x, int bits) {
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    const int qmax = (1 << bits) - 1;
    long long t = llround(x * qmax);
    if (t < 0)   t = 0;
    if (t > qmax) t = qmax;
    return static_cast<uint16_t>(t);
}

} // namespace
using nlohmann::json;
#ifndef SJE_WRITE_SCALE_COMPAT
#define SJE_WRITE_SCALE_COMPAT 0  // 1=过渡期仍写 "scale":100；0=不再写 scale（推荐）
#endif

#ifndef SJE_WRITE_DEALCARDS_COMPAT
#define SJE_WRITE_DEALCARDS_COMPAT 0  // 1 = 过渡期同时保留旧的 "dealcards":{牌名:子节点}
#endif
#ifndef SJE_WRITE_CHILDRENS_COMPAT
#define SJE_WRITE_CHILDRENS_COMPAT 0  // 1 = 过渡期同时保留旧的 "childrens" 对象（体积更大）
#endif
#ifndef SJE_EXPORT_STRATEGY
#define SJE_EXPORT_STRATEGY 1  // 置 0 可关闭策略导出
#endif
// ================== 构造 / 入口 ==================
StrategyJsonExporterV2::StrategyJsonExporterV2(PCfrSolver* pcfr,
                                               const std::string& json_gz_path,
                                               const std::string& xidx_path,
                                               int deck_size)
    : pcfr_(pcfr)
    , gz_(json_gz_path, /*gzip level*/6)
    , xw_(xidx_path)                 // <— 无条件启用索引写出
    , deck_size_(deck_size)
{
#ifdef SJEV2_ENABLE_DIAG
    diag_open_(json_gz_path);
#endif
}

void StrategyJsonExporterV2::run(){
    auto root = pcfr_->getTree()->getRoot();

    ensure_card_dict_();

    write_node_(root, /*depth=*/0);

    // 修改 #2：写出全局动作字典（基于写节点过程中收集到的动作名）
    write_action_dict_global_();

    // 优化#1：在关闭前一次性写出所有 handsTables（hid -> keys[]）
    write_all_hands_tables_();

    // 修改 #3：写出全局牌面字典
    write_card_dict_global_();
    gz_.close();
    xw_.finish(); // <— 永远写目录收尾
#ifdef SJEV2_ENABLE_DIAG
    diag_close_();
#endif
}

// ================== 递归分派 ==================
void StrategyJsonExporterV2::write_node_(std::shared_ptr<GameTreeNode> n, int depth){
    using GT = GameTreeNode::GameTreeNodeType;

    switch (n->getType()){
    case GT::ACTION:
        write_action_node_(std::dynamic_pointer_cast<ActionNode>(n), depth);
        break;
    case GT::CHANCE:
        write_chance_node_(std::dynamic_pointer_cast<ChanceNode>(n), depth);
        break;
    default: // SHOWDOWN / TERMINAL
        gz_.write("{}");
        break;
    }

    if (++nodes_done_ % log_step_ == 0) {
        qDebug().noquote() << "[export]" << nodes_done_ << "nodes written";
    }
}

// ================== ACTION 节点 ==================
void StrategyJsonExporterV2::write_action_node_(std::shared_ptr<ActionNode> an, int /*depth*/){
    gz_.put('{');

    // actions：写“动作索引数组”（与全局 action_dict_ 对齐；边写边收集）
    write_key_nloh_("actions");
    {
        nlohmann::json arr = nlohmann::json::array();
        const auto& acts = an->getActions();
        for (const auto& a : acts){
            const std::string name = a.toString();
            auto it = action_to_idx_.find(name);
            int idx;
            if (it == action_to_idx_.end()){
                idx = static_cast<int>(action_dict_.size());
                action_to_idx_[name] = idx;
                action_dict_.push_back(name);
            }else{
                idx = it->second;
            }
            arr.push_back(idx);
        }
        gz_.write(arr.dump());
        gz_.flush();  // 强制刷新流，确保数据写入
    }

    // children：写“数组”，顺序与 actions 一一对应
    const auto& children = an->getChildrens();
    if (!children.empty()){
        const auto& acts = an->getActions();
        const size_t n = std::min(children.size(), acts.size());

        gz_.put(',');
        write_key_nloh_("children");
        gz_.put('[');
        for (size_t i=0; i<n; ++i){
            if (i) gz_.put(',');
            // 这里 key_stack_ 只用于生成指针/诊断，不影响导出结构
            key_stack_.push_back("children"); key_stack_.push_back(std::to_string(i));
            write_node_(children[i], /*depth*/0);
            key_stack_.pop_back(); key_stack_.pop_back();
        }
        gz_.put(']');
        gz_.flush();  // 强制刷新流，确保数据写入
    }

#if SJE_WRITE_CHILDRENS_COMPAT
    // （可选兼容）同时输出旧的 "childrens": { 动作名: 子节点 } —— 会增大体积
    if (!children.empty()){
        const auto& acts = an->getActions();
        const size_t n = std::min(children.size(), acts.size());

        gz_.put(',');
        write_key_nloh_("childrens");
        gz_.put('{');
        for (size_t i=0; i<n; ++i){
            if (i) gz_.put(',');
            const std::string name = acts[i].toString();
            std::string ke = nlohmann::json(name).dump(); ke.push_back(':'); gz_.write(ke);
            key_stack_.push_back("childrens"); key_stack_.push_back(name);
            write_node_(children[i], /*depth*/0);
            key_stack_.pop_back(); key_stack_.pop_back();
        }
        gz_.put('}');
        gz_.flush();  // 强制刷新流，确保数据写入
    }
#endif

#if SJE_EXPORT_STRATEGY
    // strategy（只有命中 trainable 时才写；与大 JSON 相同）
    write_strategy_block_and_index_(an, /*prepend_comma=*/true);
#endif

    gz_.put('}');
    gz_.flush();  // 强制刷新流，确保数据写入
}


// ================== CHANCE 节点 ==================
void StrategyJsonExporterV2::write_chance_node_(std::shared_ptr<ChanceNode> cn, int /*depth*/){
    gz_.put('{');

    const auto& cards = cn->getCards();
    if (!cards.empty()){

        // -------- 先写 dealcards：牌索引数组（对齐全局 cardDict）--------
        write_key_nloh_("dealcards");
        gz_.put('[');
        for (size_t i = 0; i < cards.size(); ++i){
            if (i) gz_.put(',');

            const Card c = cards[i];
            const int card_int  = c.getCardInt();  // 0..51（按你们的 Card）
            // 牌名（考虑了路径上的花色交换，仅用于映射到 card_to_idx_）
            std::string label = apply_exchanges_to_label_(card_int);

            int idx = -1;
            auto it = card_to_idx_.find(label);
            if (it != card_to_idx_.end()){
                idx = it->second;
            } else {
                // 理论不该发生；兜底：写 -1
                idx = -1;
            }
            gz_.write(std::to_string(idx));
        }
        gz_.put(']');

#if SJE_WRITE_DEALCARDS_COMPAT
        // （可选兼容）同时输出旧结构：dealcards 对象（键=牌面字符串）
        gz_.put(',');
        write_key_nloh_("dealcards_compat");
        gz_.put('{');
        for (size_t i = 0; i < cards.size(); ++i){
            if (i) gz_.put(',');
            const Card c = cards[i];
            const int card_int  = c.getCardInt();
            std::string keylbl = apply_exchanges_to_label_(card_int);
            std::string ke = nlohmann::json(keylbl).dump(); ke.push_back(':'); gz_.write(ke);
            gz_.write("{}"); // 这里只占位，真实子树见下方 children；需要完全旧格式时可改回递归（体积会更大）
        }
        gz_.put('}');
#endif

        // -------- 再写 children：数组，与 dealcards 顺序一一对应 --------
        auto child = cn->getChildren(); // 你们模型里 chance 后只有一个子节点入口
        gz_.put(',');
        write_key_nloh_("children");
        gz_.put('[');

        for (size_t i = 0; i < cards.size(); ++i){
            if (i) gz_.put(',');

            const Card c = cards[i];
            const int card_int  = c.getCardInt();           // 0..51
            const int suit      = card_int % 4;             // 0..3
            const int deck_idx  = c.getNumberInDeckInt();   // 0..(D-1)

            const int deal_now = make_deal_global_(chance_cards_);

            int off = 0;
            try { off = pcfr_->getColorIsoOffset(deal_now, suit); } catch(...) { off = 0; }

            size_t rep_idx_in_layer = i;
            int    rep_deck_idx     = deck_idx;
            if (off < 0){
                const int want = Card::card2int(c) + off;
                bool found = false;
                for (size_t x=0; x<cards.size(); ++x){
                    if (Card::card2int(cards[x]) == want){
                        rep_idx_in_layer = x;
                        rep_deck_idx = cards[x].getNumberInDeckInt();
                        found = true;
                        break;
                    }
                }
                if (!found){
                    throw std::runtime_error("isomorphism representative not found");
                }
            }

            // —— 路径推进 ——：与旧版一致（只是指针路径改为 children/i）
            chance_cards_.push_back(rep_deck_idx);
            if (off < 0){ exch_stack_.push_back({ suit, suit + off }); }

            key_stack_.push_back("children"); key_stack_.push_back(std::to_string(i));
            write_node_(child, /*depth*/0);
            key_stack_.pop_back(); key_stack_.pop_back();

            // 回溯
            if (off < 0) exch_stack_.pop_back();
            chance_cards_.pop_back();
        }

        gz_.put(']'); // 结束 children 数组
    }

    gz_.put('}');
}

// ================== strategy（命中才写；不做回退/不现场创建/不量化） ==================
void StrategyJsonExporterV2::write_strategy_block_and_index_(std::shared_ptr<ActionNode> an,
                                                             bool prepend_comma){
    // exact deal（1-based）
    const int deal_exact = make_deal_global_(chance_cards_);

    std::shared_ptr<Trainable> tr;
    try {
        tr = an->getTrainable(deal_exact, /*create_on_site*/true, /*use_halffloats*/0);
    } catch (...) {
        tr.reset();
    }
    if (!tr) {
#ifdef SJEV2_ENABLE_DIAG
        const std::string ptr = pointer_from_stack_();
        diag_log_action_(ptr, current_street_(), deal_exact, /*has_tr=*/false,
                         an->getActions().size(), /*hands_cnt=*/0, /*hash_stream=*/0, /*used_abstraction=*/false);
#endif
        return;
    }

    nlohmann::json st;
    try { st = tr->dump_strategy(false); } catch (...) { st = nlohmann::json(); }
    if (!st.is_object()){
        return;
    }

    // 先按路径上的换色对顺序，依次应用到 hand->向量
    if (st.contains("strategy") && st["strategy"].is_object()){
        nlohmann::json& smap = st["strategy"];
        for (const auto& pr : exch_stack_){
            pcfr_->exchangeRange(smap, pr.first, pr.second, an);
        }
    }

    // ===== 只为整个 "/.../strategy" 的 value 建索引 =====
    if (prepend_comma) gz_.put(',');
    write_key_nloh_("strategy"); // 写 key: "strategy":

    // 记录 value 对象（以 '{' 开始、以 '}' 结束）的未压缩起始偏移
    const uint64_t off_strategy_value_begin = gz_.tell_uncompressed();

    gz_.put('{'); // 打开外层 "strategy" 的 value 对象 { ... }

    bool out_first = true;

    // 1) 写 "actions": <array>（不再单独入索引）
    if (st.contains("actions")){
        if (!out_first) gz_.put(','); out_first = false;
        write_key_nloh_("actions");
        std::string val = st["actions"].dump();
        gz_.write(val);
        gz_.flush();  // 强制刷新流，确保数据写入
    }

    // 2) 处理并写出元数据 + 压缩后的手牌映射
    // ... 前面 actions 已写完，仍在外层 { ... } 里
    if (st.contains("strategy") && st["strategy"].is_object()){
        const nlohmann::json& smap = st["strategy"];

// 开关：1 = 无损整数百分比（0..1000）；0 = 仍按 kBits 量化
#ifndef SJE_PERCENT_MODE
#define SJE_PERCENT_MODE 0
#endif
// === 新增：在量化前是否进行“千分位四舍五入”（round(x*1000)/1000） ===
#ifndef SJE_PRE_ROUND_MILLI
#define SJE_PRE_ROUND_MILLI 1   // 置 0 可关闭预四舍五入
#endif
#if SJE_PERCENT_MODE
        static constexpr int kBits  = 10;     // RAW 打包时使用的位宽（0..1000 需要 10bit），熵编码不要求 2^n
        const int QMAX_USE = 1000;
#else
        static constexpr int kBits  = 6;      // 兼容旧行为
        const int QMAX_USE = (1<<kBits) - 1;
#endif

        const int N = (!smap.empty() ? (int)smap.begin().value().size() : 0);

        // 2.1 收集 hand 顺序 + 量化为整数行（保和量化，sum=QMAX）
        std::vector<std::string> keys; keys.reserve(smap.size());
        std::vector< std::vector<uint16_t> > q_rows; q_rows.reserve(smap.size());
        for (auto it = smap.begin(); it != smap.end(); ++it){
            const std::string hand = it.key();
            const auto& arr = it.value();
            keys.push_back(hand);

            std::vector<double> p; p.reserve(arr.size());
            for (const auto& v : arr){
                const double x = v.is_number() ? v.get<double>() : 0.0;
                p.push_back(x);
            }

#if SJE_PRE_ROUND_MILLI
            // === 新增：量化前，先把概率四舍五入到千分位，并夹到 [0,1] ===
            for (double &xi : p){
                double t = std::round(xi * 1000.0) / 1000.0;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                xi = t;
            }
#endif

            std::vector<uint16_t> q;
#if SJE_PERCENT_MODE
            quantize_sum_to_QMAX(p, QMAX_USE, q);   // sum(q)=1000（无损整数百分比）
#else
            quantize_sum_preserving(p, kBits, q);   // sum(q)=(1<<kBits)-1（旧）
#endif
            q_rows.push_back(std::move(q));
        }


        // 2.2 写 hid / bits / mask_bits（替代 len/offsets）
        const int hid = register_hands_table_(keys);
        gz_.write(",\"hid\":");       gz_.write(std::to_string(hid));
#if SJE_WRITE_SCALE_COMPAT
        gz_.write(",\"scale\":");     gz_.write(std::to_string(100));
#endif
        gz_.write(",\"bits\":");      gz_.write(std::to_string(kBits));
        gz_.write(",\"mask_bits\":"); gz_.write(std::to_string(N));   // 每手 mask 的位数=动作数

        // 2.3 生成三段位流：mask(1bit)、等分标志(1bit，可选)、值流(kBits)
        std::vector<uint16_t> mask_bits_stream; mask_bits_stream.reserve(smap.size() * (size_t)N);
        std::vector<uint16_t> eq_bits_stream;   eq_bits_stream.reserve(smap.size());
        std::vector<uint16_t> vals_stream;      vals_stream.reserve(smap.size() * (size_t)std::max(0,N-1));

        const int QMAX = (1<<kBits) - 1;

        for (const auto& q : q_rows){
            // mask：标非零位置
            int m = 0;
            for (int j=0;j<N;++j){
                const uint16_t nz = (q[j] > 0) ? 1u : 0u;
                mask_bits_stream.push_back(nz);
                if (nz) ++m;
            }

            // 等分：只有当 m==N 且 q 是量化网格等分，才视作等分行
            const bool is_eq = (m==N) && is_equal_quantized_row(q);
            eq_bits_stream.push_back(is_eq ? 1u : 0u);
            if (is_eq){
                // 等分行不写任何值（值段为 0）
                continue;
            }

            // 写非零中的 (m-1) 个值（最后一个非零用整数域补齐，mask=0 的为 0）
            if (m == 0){
                // 理论不应发生（空行）；保守起见，什么都不写（或记录告警）
                continue;
            }
            // —— 写 (m−1) 个非零值，最后一个非零用“整数域补齐”
            int left =
#if SJE_PERCENT_MODE
                QMAX_USE;           // 1000
#else
                ((1<<kBits) - 1);   // 旧：2^k−1
#endif
            int written = 0;
            for (int j=0;j<N;++j){
                if (q[j] == 0) continue;
                if (written < m-1){
                    vals_stream.push_back(q[j]); // 写入 0..QMAX 的整数
                    left -= q[j];
                    ++written;
                }else{
                    break; // 第 m 个非零由 left 补齐
                }
            }
        }

        // 2.4 按位打包并写出三个字段
        {
            const std::string bin = pack_bits_lsb(mask_bits_stream, /*bits=*/1);
            const std::string b64 = base64_encode_unpadded(bin);
            gz_.write(",\"mask_b64\":\""); gz_.write(b64); gz_.put('\"');
        }
        {
            const std::string bin = pack_bits_lsb(eq_bits_stream, /*bits=*/1);
            const std::string b64 = base64_encode_unpadded(bin);
            gz_.write(",\"eq_b64\":\""); gz_.write(b64); gz_.put('\"');
        }
        // 4.3 值位流：ANS（rANS 32）熵编码（再 base64）
        {
            // 4.3 值位流：RAW / Huffman / ANS 三选一（谁小用谁），并含小块直接回退
            {
                const size_t SYM_CNT = vals_stream.size();

                // *K* 为符号表规模；RAW_BITS 为 RAW 打包位数
#if SJE_PERCENT_MODE
                const int K        = 1001;   // 符号 0..1000
                const int RAW_BITS = 10;     // RAW 打包用 10 bit
#else
                const int K        = (1<<kBits);
                const int RAW_BITS = kBits;
#endif

                if (SYM_CNT == 0){
                    gz_.write(",\"vals_codec\":\"raw\"");
                    gz_.write(",\"b64\":\"\"");
                }else{
                    // 0) 先计算 RAW 的上线大小
                    const std::string raw_bin = pack_bits_lsb(vals_stream, RAW_BITS);
                    const std::string raw_b64 = base64_encode_unpadded(raw_bin);
                    const size_t      raw_wire= raw_b64.size();

                    gz_.write(",\"vals_codec\":\"raw\"");
                    gz_.write(",\"b64\":\""); gz_.write(raw_b64); gz_.put('\"');
                    // 小块直接 RAW
                    // if (SYM_CNT < 256){
                    //     gz_.write(",\"vals_codec\":\"raw\"");
                    //     gz_.write(",\"b64\":\""); gz_.write(raw_b64); gz_.put('\"');
                    // }else {
                    //     // ===== 1) Huffman 尝试 =====
                    //     // 频率
                    //     std::vector<uint64_t> freq(K, 0);
                    //     for (uint16_t v : vals_stream) ++freq[(size_t)v];
                    //     // 码长
                    //     std::vector<uint8_t> lens;
                    //     huffman_build_code_lengths(freq, lens);
                    //     std::vector<uint32_t> hcodes;
                    //     std::vector<uint8_t>  hclen;
                    //     huffman_build_canonical_codes(lens, hcodes, hclen, /*emit_lsb_first=*/true);
                    //     // 编码
                    //     const std::string huff_bin = huffman_encode_LSB(vals_stream, hcodes, hclen);
                    //     // 码表（K 字节）
                    //     std::string hlens_bytes; hlens_bytes.resize(K);
                    //     for (int s=0;s<K;++s) hlens_bytes[(size_t)s] = (char)hclen[(size_t)s];
                    //     const std::string hlens_b64 = base64_encode_unpadded(hlens_bytes);
                    //     const std::string huff_b64  = base64_encode_unpadded(huff_bin);
                    //     const size_t huff_wire = hlens_b64.size() + huff_b64.size()
                    //                              + sizeof(",\"vals_codec\":\"huff\",\"huff_lens_b64\":\"\",\"b64\":\"") - 1;

                    //     // ===== 2) ANS 尝试 =====
                    //     const int ans_log = 12; // 4096 状态
                    //     std::vector<uint16_t> norm;
                    //     ans_normalize_freq(freq, ans_log, norm);
                    //     std::vector<uint32_t> cum;
                    //     ans_build_cum(norm, cum);
                    //     const std::string ans_bin = rans32_encode_vals(vals_stream, norm, cum, ans_log);
                    //     // 码表（K*2 字节，小端）
                    //     std::string norm_bytes; norm_bytes.resize(K*2);
                    //     for (int s=0; s<K; ++s){
                    //         uint16_t w = norm[(size_t)s];
                    //         norm_bytes[s*2+0] = (char)(w & 0xFF);
                    //         norm_bytes[s*2+1] = (char)((w >> 8) & 0xFF);
                    //     }
                    //     const std::string norm_b64 = base64_encode_unpadded(norm_bytes);
                    //     const std::string ans_b64  = base64_encode_unpadded(ans_bin);
                    //     const size_t ans_wire = norm_b64.size() + ans_b64.size()
                    //                             + sizeof(",\"vals_codec\":\"ans\",\"ans_log\":####,\"ans_norm_b64\":\"\",\"b64\":\"") - 1;

                    //     // ===== 3) 选择“线上体积”最小的方案 =====
                    //     // 注意：用 b64 后的长度直接比较，更贴近最终 .gz 的大小排名
                    //     enum {USE_RAW, USE_HUF, USE_ANS} best = USE_RAW;
                    //     size_t best_wire = raw_wire;
                    //     if (huff_wire < best_wire) { best = USE_HUF; best_wire = huff_wire; }
                    //     if (ans_wire  < best_wire) { best = USE_ANS; best_wire = ans_wire; }

                    //     if (best == USE_RAW) {
                    //         gz_.write(",\"vals_codec\":\"raw\"");
                    //         gz_.write(",\"b64\":\""); gz_.write(raw_b64); gz_.put('\"');
                    //     } else if (best == USE_HUF) {
                    //         gz_.write(",\"vals_codec\":\"huff\"");
                    //         gz_.write(",\"huff_lens_b64\":\""); gz_.write(hlens_b64); gz_.put('\"');
                    //         gz_.write(",\"b64\":\""); gz_.write(huff_b64); gz_.put('\"');
                    //     } else {
                    //         gz_.write(",\"vals_codec\":\"ans\"");
                    //         gz_.write(",\"ans_log\":"); gz_.write(std::to_string(ans_log));
                    //         gz_.write(",\"ans_norm_b64\":\""); gz_.write(norm_b64); gz_.put('\"');
                    //         gz_.write(",\"b64\":\""); gz_.write(ans_b64); gz_.put('\"');
                    //     }
                    // }
                }
            }

        }


        // 兼容可选：如需保留每节点 keys（会变大很多），仍用原有宏控制
#if SJE_WRITE_KEYS
        gz_.write(",\"keys\":[");
        for (size_t i=0;i<keys.size();++i){
            if (i) gz_.put(',');
            std::string k = nlohmann::json(keys[i]).dump();
            gz_.write(k);
        }
        gz_.put(']');
#endif

    }

    gz_.put('}'); // 关闭外层 "strategy" 的 value 对象
    gz_.flush();  // 强制刷新流，确保数据写入

    // ===== 现在一次性为 "/.../strategy" 入索引 =====
    const uint64_t off_after = gz_.tell_uncompressed();
    const uint32_t len = static_cast<uint32_t>(off_after - off_strategy_value_begin);

    key_stack_.push_back("strategy");
    const std::string ptr = pointer_from_stack_(); // /.../strategy
    key_stack_.pop_back();

    const uint64_t h  = hash_pointer_(ptr);   // FNV-1a 64
    const uint8_t  fp = fp8_(h);              // 8-bit 指纹
    xw_.add(h, off_strategy_value_begin, len, fp);

#ifdef SJEV2_ENABLE_DIAG
    {
        size_t hands_cnt = 0;
        uint64_t hash_stream = 0;
        if (st.contains("strategy") && st["strategy"].is_object()){
            hands_cnt = st["strategy"].size();
            hash_stream = hash_stream_of_strategy_value_(st); // 若你原来就有这个，保留；没有可去掉
        }
        const std::string ptr2 = pointer_from_stack_();
        diag_log_action_(ptr2, current_street_(), deal_exact, /*has_tr=*/true,
                         an->getActions().size(), hands_cnt, hash_stream, /*used_abstraction=*/false);
    }
#endif
}


uint64_t StrategyJsonExporterV2::hash_pointer_(const std::string& s) const {
    uint64_t h = Fnv1a::kOffset;
    if (!s.empty()){
        h = Fnv1a::update_bytes(h, s.data(), s.size());
    }
    return h;
}

uint8_t StrategyJsonExporterV2::fp8_(uint64_t h) const {
    return static_cast<uint8_t>((h ^ (h >> 32)) & 0xFF);
}

// ================== deal（1-based，逐层进位；基数 = deck_size_） ==================
int StrategyJsonExporterV2::make_deal_global_(const std::vector<int>& cc) const {
    int deal = 0; // flop
    for (size_t k = 0; k < cc.size(); ++k) {
        const int deck_idx = cc[k]; // 牌堆全局索引 0..D-1（代表分支）
        if (k == 0) {
            deal = deck_idx + 1; // turn: 1..D
        } else {
            const int origin = deal - 1; // 0..D-1
            deal = deck_size_ * origin + deck_idx + (1 + deck_size_); // river: 1+D..1+D+D^2
        }
    }
    return deal;
}

// ================== 工具：输出 JSON key ==================
void StrategyJsonExporterV2::write_key_nloh_(const std::string& k){
    std::string out = json(k).dump();
    out.push_back(':');
    gz_.write(out);
}

// ================== 工具：RFC6901 转义/指针 ==================
std::string StrategyJsonExporterV2::rfc6901_escape_(const std::string& s){
    std::string o; o.reserve(s.size()+8);
    for (char c : s) {
        if (c=='~') o += "~0";
        else if (c=='/') o += "~1";
        else o.push_back(c);
    }
    return o;
}

std::string StrategyJsonExporterV2::pointer_from_stack_() const{
    std::string out; out.reserve(256);
    for (const auto& seg : key_stack_) {
        out.push_back('/');
        out += const_cast<StrategyJsonExporterV2*>(this)->rfc6901_escape_(seg);
    }
    return out;
}

// ================== 工具：把已记录换色对应用到“显示牌面标签” ==================
std::string StrategyJsonExporterV2::apply_exchanges_to_label_(int card_int) const {
    int r = card_int % 4;
    int base = card_int - r;
    // 依次应用路径上的 (r1, r2) 交换
    for (const auto& pr : exch_stack_){
        if      (r == pr.first)  r = pr.second;
        else if (r == pr.second) r = pr.first;
    }
    return Card::intCard2Str(base + r);
}
// 对 keys[] 做稳定哈希（FNV-1a）：按顺序更新字符串与分隔符
uint64_t StrategyJsonExporterV2::hash_keys_(const std::vector<std::string>& keys) const {
    uint64_t h = Fnv1a::kOffset;
    static const char sep = '\x1F'; // 不会出现在手牌字符串里的分隔符
    for (const auto& k : keys){
        if (!k.empty()) h = Fnv1a::update_bytes(h, k.data(), k.size());
        h = Fnv1a::update_bytes(h, &sep, 1);
    }
    return h;
}

// 注册 keys → hid（去重）；返回 hid
int StrategyJsonExporterV2::register_hands_table_(const std::vector<std::string>& keys){
    const uint64_t sig = hash_keys_(keys);
    auto it = keys_sig_to_hid_.find(sig);
    if (it != keys_sig_to_hid_.end()) return it->second;

    const int hid = static_cast<int>(hands_tables_.size());
    keys_sig_to_hid_[sig] = hid;
    hands_tables_.push_back(HandsTable{hid, keys});
    return hid;
}

// 在导出末尾把所有 handsTables 写出去（单条 JSON 对象）
// 形如：{"type":"handsTables","tables":[{"hid":0,"keys":[...]}, ...]}
void StrategyJsonExporterV2::write_all_hands_tables_(){
    if (hands_tables_.empty()) return;
    nlohmann::json g;
    g["type"] = "handsTables";
    nlohmann::json arr = nlohmann::json::array();
    arr.get_ref<nlohmann::json::array_t&>().reserve(hands_tables_.size());
    for (const auto& t : hands_tables_){
        nlohmann::json x;
        x["hid"]  = t.hid;
        x["keys"] = t.keys;
        arr.push_back(std::move(x));
    }
    g["tables"] = std::move(arr);

    // === 新增：记录未压缩起止偏移 ===
    const uint64_t off_begin = gz_.tell_uncompressed();

    const std::string s = g.dump();
    gz_.write(s);

    const uint64_t off_end = gz_.tell_uncompressed();
    const uint32_t len = static_cast<uint32_t>(off_end - off_begin);

    // === 新增：写入 xidx 记录 ===
    {
        const std::string ptr = "/globals/handsTables";
        const uint64_t h  = hash_pointer_(ptr);
        const uint8_t  fp = fp8_(h);
        xw_.add(h, off_begin, len, fp);
    }

    // 如需换行，可加：gz_.put('\n');
}


// 在导出末尾写出全局动作字典：{"type":"actionDict","actions":[...]}
void StrategyJsonExporterV2::write_action_dict_global_(){
    if (action_dict_.empty()) return;
    nlohmann::json g;
    g["type"]    = "actionDict";
    g["actions"] = action_dict_;

    // === 新增：记录未压缩起止偏移 ===
    const uint64_t off_begin = gz_.tell_uncompressed();

    const std::string s = g.dump();
    gz_.write(s);

    const uint64_t off_end = gz_.tell_uncompressed();
    const uint32_t len = static_cast<uint32_t>(off_end - off_begin);

    // === 新增：写入 xidx 记录 ===
    {
        const std::string ptr = "/globals/actionDict";
        const uint64_t h  = hash_pointer_(ptr);  // 你类里现成的方法
        const uint8_t  fp = fp8_(h);
        xw_.add(h, off_begin, len, fp);
    }

    // 如需换行：gz_.put('\n');
}

// ===== 修改 #3：牌面字典 =====
void StrategyJsonExporterV2::ensure_card_dict_(){
    if (!card_dict_.empty()) return;

    // 直接用 Card::intCard2Str(i) 枚举 0..51，保证与全工程一致
    card_dict_.clear();
    card_to_idx_.clear();
    card_dict_.reserve(52);
    for (int i = 0; i < 52; ++i){
        std::string name = Card::intCard2Str(i); // 例如 "2c","2d",...,"Ac","Ad","Ah","As"
        card_to_idx_[name] = static_cast<int>(card_dict_.size());
        card_dict_.push_back(std::move(name));
    }
}


// {"type":"cardDict","cards":[...]}
void StrategyJsonExporterV2::write_card_dict_global_(){
    if (card_dict_.empty()) return;
    nlohmann::json g;
    g["type"]  = "cardDict";
    g["cards"] = card_dict_;

    // === 新增：记录未压缩起止偏移 ===
    const uint64_t off_begin = gz_.tell_uncompressed();

    const std::string s = g.dump();
    gz_.write(s);

    const uint64_t off_end = gz_.tell_uncompressed();
    const uint32_t len = static_cast<uint32_t>(off_end - off_begin);

    // === 新增：写入 xidx 记录 ===
    {
        const std::string ptr = "/globals/cardDict";
        const uint64_t h  = hash_pointer_(ptr);
        const uint8_t  fp = fp8_(h);
        xw_.add(h, off_begin, len, fp);
    }

    // 如需换行：gz_.put('\n');
}



#ifdef SJEV2_ENABLE_DIAG
// ================== 诊断输出（可选） ==================
std::string StrategyJsonExporterV2::current_street_() const {
    if (chance_cards_.empty()) return "flop";
    if (chance_cards_.size()==1) return "turn";
    return "river";
}

void StrategyJsonExporterV2::diag_open_(const std::string& json_gz_path){
    if (!diag_on_){
        std::string p = json_gz_path;
        const auto dot = p.rfind('.');
        if (dot != std::string::npos) p = p.substr(0, dot);
        p += ".diag.ndjson";
        diag_os_.open(p, std::ios::out | std::ios::trunc);
        diag_on_ = diag_os_.is_open();
        if (diag_on_) {
            qDebug().noquote() << "[export] diag ->" << QString::fromStdString(p);
        } else {
            qDebug().noquote() << "[export] diag open failed";
        }
    }
}

void StrategyJsonExporterV2::diag_close_(){
    if (diag_on_) { diag_os_.flush(); diag_os_.close(); diag_on_ = false; }
}

void StrategyJsonExporterV2::diag_log_action_(const std::string& ptr,
                                              const std::string& street,
                                              int deal_used,
                                              bool has_tr,
                                              size_t actions_cnt,
                                              size_t hands_cnt,
                                              uint64_t hash_stream,
                                              bool used_abstraction){
    if (!diag_on_) return;
    json j;
    j["ptr"]              = ptr;
    j["street"]           = street;
    j["deal_used"]        = deal_used;
    j["has_tr"]           = has_tr ? 1 : 0;
    j["actions_cnt"]      = actions_cnt;
    j["hands_cnt"]        = hands_cnt;
    j["used_abstraction"] = used_abstraction ? 1 : 0;

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << (unsigned long long)hash_stream;
    j["hash_stream"]  = oss.str();

    diag_os_ << j.dump() << '\n';
}

uint64_t StrategyJsonExporterV2::hash_strategy_map_(const nlohmann::json& smap) const{
    uint64_t h = Fnv1a::kOffset;
    if (!smap.is_object()) return h;

    std::vector<std::string> keys; keys.reserve(smap.size());
    for (auto it = smap.begin(); it != smap.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());

    auto upd = [&](const void* p, size_t n){ h = Fnv1a::update_bytes(h, p, n); };

    for (const auto& k : keys){
        upd(k.data(), k.size());
        const auto& val = smap.at(k);
        if (val.is_array()){
            for (const auto& x : val){
                double v = 0.0;
                if (x.is_number()) v = x.get<double>();
                long long q = (long long) llround(v * 1000000.0);
                upd(&q, sizeof(q));
            }
        } else if (val.is_object()){
            std::vector<std::string> kk; kk.reserve(val.size());
            for (auto it = val.begin(); it != val.end(); ++it) kk.push_back(it.key());
            std::sort(kk.begin(), kk.end());
            for (const auto& sk : kk){
                upd(sk.data(), sk.size());
                const auto& inner = val.at(sk);
                if (inner.is_number()){
                    long long q = (long long) llround(inner.get<double>() * 1000000.0);
                    upd(&q, sizeof(q));
                }
            }
        }
    }
    return h;
}
#endif
