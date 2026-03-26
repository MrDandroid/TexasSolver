#include "cbor_indexer.h"

#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QStringBuilder>
#include <QtCore/QFileDevice>
#include "lix_writer.h"   // ★ 新增：.lix 写入器

// ★ 新增：线程局部的 LixBuilder 指针（无需修改类定义/头文件）
static thread_local LixBuilder* g_tls_lix = nullptr;

// ================== 外部工具（若你工程中无，实现于此） ==================
bool ensureDir(const QString& filePath) {
    const QString dirPath = QFileInfo(filePath).absolutePath();
    if (dirPath.isEmpty()) return true;
    QDir dir(dirPath);
    if (dir.exists()) return true;
    return QDir().mkpath(dirPath);
}

// ================== SQLiteSink 实现 ==================

CBORIndexer::SQLiteSink::SQLiteSink(const QString& path, size_t expectedRows, size_t logStep) {
    expected = expectedRows;
    step = (logStep ? logStep : 100000);

    if (!ensureDir(path)) {
        qWarning() << "mkdir failed for" << path;
        return;
    }
    auto tag = QString("cboridx_%1").arg(reinterpret_cast<quintptr>(this));
    QSqlDbHolder* holder = new QSqlDbHolder(tag);
    db = holder;

    QSqlDatabase& dbh = holder->db;
    dbh.setDatabaseName(path);
    if (!dbh.open()) {
        qWarning() << "open idx sqlite failed:" << dbh.lastError().text();
        return;
    }

    QSqlQuery q(dbh);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA synchronous=NORMAL");

    // 清表重建
    if (!q.exec("DROP TABLE IF EXISTS idx")) {
        qWarning() << "drop table failed:" << q.lastError().text(); return;
    }
    if (!q.exec("CREATE TABLE idx("
                "pointer TEXT NOT NULL,"
                "offset  INTEGER NOT NULL,"
                "length  INTEGER NOT NULL,"
                "type    INTEGER NOT NULL)")) {
        qWarning() << "create table failed:" << q.lastError().text(); return;
    }

    // 根索引可能需要 shard 映射表（默认不需要；由外层创建后把 hasShardTable=true）
    ok = true;
}

CBORIndexer::SQLiteSink::~SQLiteSink() {
    if (!db) return;
    QSqlDbHolder* holder = reinterpret_cast<QSqlDbHolder*>(db);
    const QString conn = holder->connName;

    if (holder->db.isOpen()) holder->db.close();
    holder->db = QSqlDatabase();                 // 先切断对象引用
    { QSqlDatabase temp = QSqlDatabase::database(conn, false);
        if (temp.isValid() && temp.isOpen()) temp.close(); }
    QSqlDatabase::removeDatabase(conn);

    delete holder;
    db = nullptr;
}


bool CBORIndexer::SQLiteSink::begin() {
    if (!ok) return false;
    QSqlDbHolder* holder = reinterpret_cast<QSqlDbHolder*>(db);
    QSqlQuery q(holder->db);
    if (!q.exec("BEGIN IMMEDIATE")) {
        qWarning() << "BEGIN failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool CBORIndexer::SQLiteSink::commit() {
    if (!ok) return false;
    QSqlDbHolder* holder = reinterpret_cast<QSqlDbHolder*>(db);
    QSqlQuery q(holder->db);

    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_pointer ON idx(pointer)")) {
        qWarning() << "create index failed:" << q.lastError().text();
    }
    if (!q.exec("COMMIT")) {
        qWarning() << "COMMIT failed:" << q.lastError().text();
        return false;
    }
    q.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    return true;
}

void CBORIndexer::SQLiteSink::insert(const std::string& pointer, size_t offset, size_t length, int type) {
    if (!ok) return;
    QSqlDbHolder* holder = reinterpret_cast<QSqlDbHolder*>(db);
    QSqlQuery q(holder->db);
    q.prepare("INSERT INTO idx(pointer,offset,length,type) VALUES(?,?,?,?)");
    q.addBindValue(QString::fromUtf8(pointer.c_str(), int(pointer.size())));
    q.addBindValue(static_cast<qint64>(offset));
    q.addBindValue(static_cast<qint64>(length));
    q.addBindValue(type);
    if (!q.exec()) {
        qWarning() << "insert idx failed:" << q.lastError().text();
        return;
    }
    rows++;
    if (expected && step && (rows % step == 0)) {
        const double pct = (expected ? (100.0 * double(rows) / double(expected)) : 0.0);
        qDebug().noquote() << "Index progress: " << quint64(rows) << "/" << quint64(expected)
                           << QString(" (%1%)").arg(pct, 0, 'f', 1);
    }
}

void CBORIndexer::SQLiteSink::declareShard(const std::string& pointer, const QString& file) {
    if (!ok) return;
    QSqlDbHolder* holder = reinterpret_cast<QSqlDbHolder*>(db);

    // 1) 确保表存在（每次都跑 IF NOT EXISTS，成本很低且最稳）
    {
        QSqlQuery q(holder->db);
        if (!q.exec("CREATE TABLE IF NOT EXISTS shard("
                    "pointer TEXT PRIMARY KEY,"
                    "file    TEXT NOT NULL)")) {
            qWarning() << "create shard table failed:" << q.lastError().text();
            return;
        }
    }

    // 2) 插入映射（使用位置占位符，避免命名参数计数差异）
    {
        QSqlQuery q(holder->db);
        q.prepare("INSERT OR REPLACE INTO shard(pointer, file) VALUES(?, ?)");
        q.addBindValue(QString::fromUtf8(pointer.c_str(), int(pointer.size())));
        q.addBindValue(file);
        if (!q.exec()) {
            qWarning() << "insert shard mapping failed:" << q.lastError().text();
            return;
        }
    }
}

// ================== 工具 ==================
bool CBORIndexer::starts_with(const std::string& s, const std::string& pfx) {
    return s.size() >= pfx.size() && std::equal(pfx.begin(), pfx.end(), s.begin());
}

std::string CBORIndexer::joinPointer(const std::vector<std::string>& parts) {
    if (parts.empty()) return std::string("/");
    std::string out; out.reserve(64);
    for (size_t i=0;i<parts.size();++i) {
        out.push_back('/');
        const std::string& seg = parts[i];
        for (char ch: seg) {
            if (ch=='~') { out.push_back('~'); out.push_back('0'); }
            else if (ch=='/') { out.push_back('~'); out.push_back('1'); }
            else out.push_back(ch);
        }
    }
    return out;
}

uint64_t CBORIndexer::readUInt(const std::uint8_t* p, const std::uint8_t* end, int ai, size_t& hdr) {
    if (ai < 24) { hdr=1; return ai; }
    if (ai == 24) { hdr=2; return p[1]; }
    if (ai == 25) { hdr=3; return (uint64_t(p[1])<<8) | p[2]; }
    if (ai == 26) { hdr=5; return (uint64_t(p[1])<<24)|(uint64_t(p[2])<<16)|(uint64_t(p[3])<<8)|p[4]; }
    if (ai == 27) { hdr=9; uint64_t v=0; for(int i=1;i<=8;i++) v=(v<<8)|p[i]; return v; }
    hdr=1; return 0;
}

bool CBORIndexer::shouldPrune(const std::string& curPtr, int depth) const {
    if (opts.maxDepth >= 0 && depth >= opts.maxDepth) return true;
    for (const auto& pfx : opts.skipPrefixes) {
        if (!pfx.empty() && starts_with(curPtr, pfx)) return true;
    }
    return false;
}

// ================== 跳过（无写入） ==================
size_t CBORIndexer::skipItem(Cursor& c) {
    const std::uint8_t* start = c.p;
    int major = (*c.p) >> 5;
    int ai    = (*c.p) & 0x1f;
    size_t hdr=0; uint64_t n=0;
    switch (major) {
    case 0: case 1: readUInt(c.p, c.end, ai, hdr); c.p += hdr; break;
    case 2: case 3: n = readUInt(c.p, c.end, ai, hdr); c.p += hdr + n; break;
    case 4: n = readUInt(c.p, c.end, ai, hdr); c.p += hdr; for (uint64_t i=0;i<n;i++) skipItem(c); break;
    case 5: n = readUInt(c.p, c.end, ai, hdr); c.p += hdr;
        for (uint64_t i=0;i<n;i++) { int kai=c.p[0]&0x1f; size_t kh=0; uint64_t klen=readUInt(c.p,c.end,kai,kh); c.p+=kh+klen; skipItem(c); }
        break;
    case 6: readUInt(c.p, c.end, ai, hdr); c.p += hdr; skipItem(c); break;
    case 7: if (ai<24) c.p+=1; else if(ai==24)c.p+=2; else if(ai==25)c.p+=3; else if(ai==26)c.p+=5; else if(ai==27)c.p+=9; else c.p+=1; break;
    default: break;
    }
    return size_t(c.p - start);
}

size_t CBORIndexer::skipArray(Cursor& c) { return skipItem(c); }
size_t CBORIndexer::skipMap  (Cursor& c) { return skipItem(c); }

// ================== 分片：获取/创建 shard sink ==================
CBORIndexer::SQLiteSink& CBORIndexer::ensureShard(const std::string& shardRootPtr) {
    // 已有就直接返回
    auto it = shardSinks.find(shardRootPtr);
    if (it != shardSinks.end()) return *it->second;

    // 生成分片文件名：<baseIdxPath_>.sNNNNN
    const int shardId = int(shardSinks.size()) + 1;
    const QString shardPath = baseIdxPath_ + QString(".s%1").arg(shardId, 5, 10, QLatin1Char('0'));

    // 创建并开启单事务
    auto sink = std::make_unique<SQLiteSink>(shardPath, /*expected*/0, opts.progressStep);
    if (!sink->valid()) {
        qWarning() << "create shard sink failed for" << shardPath << ", fallback to root sink";
        return *rootSink;
    }
    if (!sink->begin()) {
        qWarning() << "shard begin failed for" << shardPath << ", fallback to root sink";
        return *rootSink;
    }

    // 在根索引登记 “分片根指针 -> 分片文件”
    rootSink->declareShard(shardRootPtr, shardPath);

    auto* raw = sink.get();
    shardSinks.emplace(shardRootPtr, std::move(sink));
    return *raw; // 返回引用，避免 “taking the address of a temporary” 报错
}


// ================== 扫描（容器 only，带剪枝 & 分片） ==================

size_t CBORIndexer::scanArray(Cursor& c, ScanCtx& ctx, SQLiteSink& where) {
    const std::uint8_t* header = c.p;
    int ai = c.p[0] & 0x1f; size_t hdr=0; uint64_t n = readUInt(c.p, c.end, ai, hdr);
    c.p += hdr;

    const std::string curPtr = joinPointer(ctx.stack);
    const bool pruneChildren = shouldPrune(curPtr, ctx.depth);

    // 分片：若启用 shardLevel==1 且当前容器在根下一层（depth==1），
    // 其“子节点递归部分”写入对应分片
    SQLiteSink* childSink = &where;
    if (opts.shardLevel == 1 && ctx.depth == 1) {
        childSink = &ensureShard(curPtr);  // ★ 只有 1 个参数
    }

    if (pruneChildren) {
        for (uint64_t i=0;i<n;i++) skipItem(c);
    } else {
        for (uint64_t i=0;i<n;i++) {
            ctx.stack.push_back(std::to_string(i));
            ctx.depth++;
            int t=0; (void)scanItem(c, ctx, t, (childSink ? *childSink : where));
            ctx.depth--;
            ctx.stack.pop_back();
        }
    }

    const size_t total = size_t(c.p - header);
    // 当前容器自身记录在“where”（根或父分片）
    where.insert(curPtr, size_t(header - c.base), total, T_ARRAY);

    // ★补充：如果正在分片（childSink 指向分片），也把“当前容器”写进分片
    if (childSink && childSink != &where) {
        childSink->insert(curPtr, size_t(header - c.base), total, T_ARRAY);
    }
    return total;
}

size_t CBORIndexer::scanMap(Cursor& c, ScanCtx& ctx, SQLiteSink& where) {
    const std::uint8_t* header = c.p;
    int ai = c.p[0] & 0x1f;
    size_t hdr = 0;
    uint64_t n = readUInt(c.p, c.end, ai, hdr);
    c.p += hdr;

    const std::string curPtr = joinPointer(ctx.stack);
    const bool pruneChildren = shouldPrune(curPtr, ctx.depth);

    // 与现有分片策略保持一致：根下一层的容器，子节点写入分片
    SQLiteSink* childSink = &where;
    if (opts.shardLevel == 1 && ctx.depth == 1) {
        childSink = &ensureShard(curPtr);
    }
    SQLiteSink& sink = (childSink ? *childSink : where);

    // 小工具：不改动全局 Cursor 的 skip（用一份临时 Cursor 来跳过）
    auto skipAnyRaw = [&](const std::uint8_t* p) -> const std::uint8_t* {
        Cursor tmp{ p, c.end, c.base };
        (void)skipItem(tmp);
        return tmp.p;
    };
    // 小工具：JSON Pointer 段转义
    auto escSeg = [](const std::string& s)->std::string {
        std::string out; out.reserve(s.size()+8);
        for (char ch : s) {
            if (ch == '~') { out.push_back('~'); out.push_back('0'); }
            else if (ch == '/') { out.push_back('~'); out.push_back('1'); }
            else out.push_back(ch);
        }
        return out;
    };
    // ★ 新增：安全拼接，避免根 "/" 变成 "//xxx"
    auto joinPtrSafe = [](const std::string& base, const std::string& tail)->std::string {
        if (base == "/") return "/" + tail;
        return base + "/" + tail;
    };

    for (uint64_t i = 0; i < n; ++i) {
        // 读取 key（text）
        const std::uint8_t* kstart = c.p;
        int kai = kstart[0] & 0x1f;
        size_t khdr = 0;
        uint64_t klen = readUInt(kstart, c.end, kai, khdr);
        std::string key(reinterpret_cast<const char*>(kstart + khdr), size_t(klen));
        c.p = kstart + khdr + klen;

        // value 起点
        const std::uint8_t* vstart = c.p;

        // ---------- 特例 1：dealcards（仅做一层“子牌面 → 容器切片”的浅索引） ----------
        if (key == "dealcards" && vstart < c.end && ((vstart[0] >> 5) == 5)) {
            // 这是一个 map；枚举其子键（牌面）并为每个 value 记一条行
            const std::uint8_t* it = vstart;
            size_t vh = 0;
            uint64_t nchild = readUInt(it, c.end, it[0] & 0x1f, vh);
            it += vh;

            for (uint64_t k = 0; k < nchild; ++k) {
                // 子键（牌面，text）
                int ck_ai = it[0] & 0x1f;
                size_t ck_hdr = 0;
                uint64_t ck_len = readUInt(it, c.end, ck_ai, ck_hdr);
                std::string card(reinterpret_cast<const char*>(it + ck_hdr), size_t(ck_len));
                it += ck_hdr + ck_len;

                // 子值切片
                const std::uint8_t* vs = it;
                it = skipAnyRaw(it);

                // 写入一条：/…/dealcards/<card>
                const size_t off = size_t(vs - c.base);
                const size_t len = size_t(it - vs);
                std::string ptr = curPtr + "/dealcards/" + escSeg(card);
                sink.insert(ptr, off, len, T_OBJECT);
                // ★ 可选：若将来需要整段拿这里的 map/容器，也可 lix.add(ptr, off, len, 5);
            }
            // 整个 value 已处理完，把主游标跳到 it，然后继续下一个键（不递归下钻）
            c.p = it;
            continue;
        }

        // ---------- 特例 2：strategy（容器 + hand 叶子 + actions） ----------
        if (key == "strategy" && vstart < c.end && ((vstart[0] >> 5) == 5)) {
            const std::uint8_t* it = vstart;
            size_t vh = 0;
            uint64_t nchild = readUInt(it, c.end, it[0] & 0x1f, vh);
            it += vh;

            for (uint64_t k = 0; k < nchild; ++k) {
                // 子键（可能是 "strategy" 或 "actions"）
                int sk_ai = it[0] & 0x1f;
                size_t sk_hdr = 0;
                uint64_t sk_len = readUInt(it, c.end, sk_ai, sk_hdr);
                std::string subkey(reinterpret_cast<const char*>(it + sk_hdr), size_t(sk_len));
                it += sk_hdr + sk_len;

                // 子值切片
                const std::uint8_t* vs = it;
                const int sub_major = (vs[0] >> 5);
                it = skipAnyRaw(it);

                if (subkey == "strategy" && sub_major == 5) {
                    // 1) 整个容器
                    const size_t off = size_t(vs - c.base);
                    const size_t len = size_t(it - vs);
                    std::string contPtr = joinPtrSafe(curPtr, "strategy/strategy");
                    sink.insert(contPtr, off, len, T_OBJECT);
                    if (g_tls_lix) g_tls_lix->add(contPtr, (uint64_t)off, (uint32_t)len, /*major*/5);

                    // 2) 枚举 hand -> value（叶子）并同时入 sqlite + .lix
                    const std::uint8_t* m = vs;
                    size_t mh = 0; uint64_t nent = readUInt(m, c.end, m[0] & 0x1f, mh);
                    m += mh;
                    for (uint64_t iHand = 0; iHand < nent; ++iHand) {
                        // hand（text key）
                        int hk_ai = m[0] & 0x1f; size_t hk_hdr = 0;
                        uint64_t hk_len = readUInt(m, c.end, hk_ai, hk_hdr);
                        std::string hand(reinterpret_cast<const char*>(m + hk_hdr), size_t(hk_len));
                        m += hk_hdr + hk_len;

                        // value 切片
                        const std::uint8_t* hv = m;
                        int hv_major = (hv[0] >> 5);
                        m = skipAnyRaw(m);
                        size_t off2 = size_t(hv - c.base), len2 = size_t(m - hv);

                        std::string leaf = joinPtrSafe(curPtr, std::string("strategy/strategy/") + escSeg(hand));
                        sink.insert(leaf, off2, len2,
                                    (hv_major == 4 ? T_ARRAY : (hv_major == 5 ? T_OBJECT : T_OTHER)));
                        if (g_tls_lix) g_tls_lix->add(leaf, (uint64_t)off2, (uint32_t)len2, (uint8_t)hv_major);
                    }
                } else if (subkey == "actions" && sub_major == 4) {
                    const size_t off = size_t(vs - c.base);
                    const size_t len = size_t(it - vs);
                    std::string actPtr = joinPtrSafe(curPtr, "strategy/actions");
                    sink.insert(actPtr, off, len, T_ARRAY);
                    if (g_tls_lix) g_tls_lix->add(actPtr, (uint64_t)off, (uint32_t)len, /*major*/4);
                }
            }
            // 不递归进入 strategy 子树
            c.p = it;
            continue;
        }

        // ---------- 常规路径：按原逻辑递归或跳过 ----------
        if (pruneChildren) {
            (void)skipItem(c);
        } else {
            ctx.stack.push_back(key);
            ctx.depth++;
            int t = 0;
            (void)scanItem(c, ctx, t, sink);
            ctx.depth--;
            ctx.stack.pop_back();
        }
    }

    const size_t total = size_t(c.p - header);

    // 记录当前容器自身（放在 where）
    where.insert(curPtr, size_t(header - c.base), total, T_OBJECT);
    // 若正在分片（childSink 指向分片），也把“当前容器”写进分片，保持一致
    if (childSink && childSink != &where) {
        childSink->insert(curPtr, size_t(header - c.base), total, T_OBJECT);
    }
    return total;
}


// 注意：为了让 ensureShard 能拿到 baseIdxPath，我们稍后在 buildIndexFromBuffer 里
// 给它设置一个捕获用的“当前 basePath”。这里先用占位的方式实现 scanItem。
size_t CBORIndexer::scanItem(Cursor& c, ScanCtx& ctx, int& outType, SQLiteSink& where) {
    const std::uint8_t* start = c.p;
    int major = (*c.p) >> 5;
    switch (major) {
    case 0: { size_t hdr=0; (void)readUInt(c.p,c.end,(*c.p)&0x1f,hdr); c.p+=hdr; outType=T_INT;    return size_t(c.p - start); }
    case 1: { size_t hdr=0; (void)readUInt(c.p,c.end,(*c.p)&0x1f,hdr); c.p+=hdr; outType=T_INT;    return size_t(c.p - start); }
    case 2: { size_t hdr=0; uint64_t n=readUInt(c.p,c.end,(*c.p)&0x1f,hdr); c.p+=hdr+n; outType=T_BYTES;  return size_t(c.p - start); }
    case 3: { size_t hdr=0; uint64_t n=readUInt(c.p,c.end,(*c.p)&0x1f,hdr); c.p+=hdr+n; outType=T_STRING; return size_t(c.p - start); }
    case 4: { size_t len=scanArray(c, ctx, where); outType=T_ARRAY;  return len; }
    case 5: { size_t len=scanMap  (c, ctx, where); outType=T_OBJECT; return len; }
    case 6: { size_t hdr=0; (void)readUInt(c.p,c.end,(*c.p)&0x1f,hdr); c.p+=hdr; int t=0; size_t len=scanItem(c,ctx,t,where); outType=T_OTHER; return size_t(c.p - start); }
    case 7: { int ai=(*c.p)&0x1f; if(ai<24)c.p+=1; else if(ai==24)c.p+=2; else if(ai==25)c.p+=3; else if(ai==26)c.p+=5; else if(ai==27)c.p+=9; else c.p+=1;
        if(ai==20||ai==21) outType=T_BOOL; else if(ai==22) outType=T_NULL; else if(ai==26||ai==27) outType=T_FLOAT; else outType=T_OTHER;
        return size_t(c.p - start); }
    default: qWarning()<<"unknown CBOR major"; return 0;
    }
}

// ================== 主入口（含分片组装） ==================
static inline std::string withExt(const std::string& path, const char* newExt) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return path + newExt;
    return path.substr(0, pos) + newExt;
}

bool CBORIndexer::buildIndexFromBuffer(const std::vector<std::uint8_t>& buf,
                                       const QString& baseIdxPath) {
    if (buf.empty()) return false;

    baseIdxPath_ = baseIdxPath;

    // ★ 新增：构造线程局部 lix 收集器
    LixBuilder lix_local;
    g_tls_lix = &lix_local;

    // 根索引
    rootSink = std::make_unique<SQLiteSink>(baseIdxPath_, opts.expectedRows, opts.progressStep);
    if (!rootSink->valid()) { g_tls_lix = nullptr; return false; }
    if (!rootSink->begin()) { g_tls_lix = nullptr; return false; }

    Cursor c{buf.data(), buf.data()+buf.size(), buf.data()};
    ScanCtx ctx; ctx.stack.clear(); ctx.depth = 0;

    int outT = 0;
    size_t consumed = scanItem(c, ctx, outT, *rootSink);
    if (consumed != buf.size()) {
        qWarning() << "warning: cbor consumed" << consumed << "but buf size" << buf.size();
    }

    // 根 '/' 兜底
    rootSink->insert("/", 0, buf.size(), T_OBJECT);

    // 提交
    bool okRoot = rootSink->commit();
    for (auto& kv : shardSinks) {
        if (kv.second) kv.second->commit();
    }

    // ★ 新增：写出 .lix 到 idx 同目录（将 .idx 改后缀为 .lix）
    try {
        const std::string lix_path = withExt(baseIdxPath_.toStdString(), ".lix");
        lix_local.write(lix_path);
        qDebug() << "[lix] wrote" << quint64(lix_local.size()) << "entries to" << QString::fromStdString(lix_path);
    } catch (const std::exception& e) {
        qWarning() << "[lix] write failed:" << e.what();
    }

    g_tls_lix = nullptr;
    qDebug() << "CBOR index rows (root):" << rootSink->rows;
    size_t sumShardRows = 0;
    for (auto& kv : shardSinks) sumShardRows += kv.second->rows;
    if (!shardSinks.empty()) {
        qDebug() << "CBOR index rows (shards sum):" << quint64(sumShardRows)
        << " shards:" << int(shardSinks.size());
    }
    return okRoot;
}

bool CBORIndexer::buildIndexFromFile(const QString& binPath,
                                     const QString& baseIdxPath)
{
    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "open bin failed:" << binPath << f.errorString();
        return false;
    }
    const qint64 sz = f.size();
    if (sz <= 0) {
        qWarning() << "empty bin file:" << binPath;
        return false;
    }

    // 直接 mmap 全文件（零拷贝）。如果某些平台不支持，再回退 readAll。
    uchar* mapped = f.map(0, sz);
    std::unique_ptr<std::uint8_t[]> fallback; // 仅在 map 失败时使用
    const std::uint8_t* base = nullptr;

    if (mapped) {
        base = reinterpret_cast<const std::uint8_t*>(mapped);
    } else {
        qWarning() << "QFile::map failed, fallback to readAll (will use RAM)";
        fallback.reset(new std::uint8_t[size_t(sz)]);
        if (f.read(reinterpret_cast<char*>(fallback.get()), sz) != sz) {
            qWarning() << "readAll fallback failed:" << binPath;
            return false;
        }
        base = fallback.get();
    }

    // —— 以下逻辑与 buildIndexFromBuffer 基本一致 —— //
    baseIdxPath_ = baseIdxPath;

    // ★ 新增：构造线程局部 lix 收集器
    LixBuilder lix_local;
    g_tls_lix = &lix_local;

    rootSink = std::make_unique<SQLiteSink>(baseIdxPath_, opts.expectedRows, opts.progressStep);
    if (!rootSink->valid()) {
        if (mapped) f.unmap(mapped);
        g_tls_lix = nullptr;
        return false;
    }
    if (!rootSink->begin()) {
        if (mapped) f.unmap(mapped);
        g_tls_lix = nullptr;
        return false;
    }

    Cursor c{ base, base + sz, base };
    ScanCtx ctx; ctx.stack.clear(); ctx.depth = 0;

    int outT = 0;
    size_t consumed = scanItem(c, ctx, outT, *rootSink);
    if (consumed != size_t(sz)) {
        qWarning() << "warning: cbor consumed" << quint64(consumed) << "but file size" << quint64(sz);
    }

    // 根 '/' 兜底
    rootSink->insert("/", 0, size_t(sz), T_OBJECT);

    // 提交
    bool okRoot = rootSink->commit();
    for (auto& kv : shardSinks) {
        if (kv.second) kv.second->commit();
    }

    // ★ 新增：写出 .lix 到 bin 同目录（把 .bin 改后缀为 .lix）
    try {
        const std::string lix_path = withExt(binPath.toStdString(), ".lix");
        lix_local.write(lix_path);
        qDebug() << "[lix] wrote" << quint64(lix_local.size()) << "entries to" << QString::fromStdString(lix_path);
    } catch (const std::exception& e) {
        qWarning() << "[lix] write failed:" << e.what();
    }

    if (mapped) f.unmap(mapped);
    g_tls_lix = nullptr;
    return okRoot;
}
