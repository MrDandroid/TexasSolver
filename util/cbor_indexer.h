#pragma once
#include <vector>
#include <string>
#include <memory>
#include <map>

#include <QtCore/QString>
#include <QtCore/QDebug>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

// ========= 外部工具（若你工程里没有，请在 .cpp 里给出实现） =========
bool ensureDir(const QString& filePath);

struct QSqlDbHolder {
    QString connName;
    QSqlDatabase db;
    explicit QSqlDbHolder(const QString& name): connName(name) {
        db = QSqlDatabase::addDatabase("QSQLITE", connName);
    }
    ~QSqlDbHolder() {}
};

// ========= CBOR 游标/扫描上下文 =========
struct Cursor {
    const std::uint8_t* p;
    const std::uint8_t* end;
    const std::uint8_t* base;
};

struct ScanCtx {
    std::vector<std::string> stack;
    int depth = 0; // 根=0；根下一层=1
};

// ========= 类型标签（仅供调试/排查）=========
enum {
    T_OBJECT = 7, T_ARRAY = 6, T_STRING = 3, T_BYTES = 2, T_INT = 0,
    T_BOOL = 20, T_NULL = 22, T_FLOAT = 26, T_OTHER = 99
};

// ========= 配置项 =========
struct IndexOptions {
    bool containersOnly = true;        // 只对容器插索引（推荐 true）
    int  maxDepth = 5;                 // 最大索引深度（根=0；<0 不限）
    std::vector<std::string> skipPrefixes; // 命中这些前缀则不递归

    // 进度与估算
    size_t expectedRows = 0;           // 可填 0（不打印百分比）
    size_t progressStep = 100000;

    // 分片：0=不分片；1=按根下一层分片（推荐先用 1）
    int shardLevel = 1;
};

// ========= 索引器 =========
class CBORIndexer {
public:
    class SQLiteSink {
    public:
        explicit SQLiteSink(const QString& path, size_t expectedRows = 0, size_t logStep = 100000);
        ~SQLiteSink();
        bool valid() const { return ok; }
        bool begin();
        bool commit();

        // 基本写入
        void insert(const std::string& pointer, size_t offset, size_t length, int type);

        // 根索引专用：登记分片映射
        void declareShard(const std::string& pointer, const QString& file);

        size_t rows = 0;        // 已写入条数
        size_t expected = 0;    // 预估行数（用于打印百分比）
        size_t step = 100000;   // 进度步长

        // 根索引：是否具备 shard 表
        bool hasShardTable = false;

    private:
        bool ok = false;
        void* db = nullptr; // QSqlDatabase*
    };

    explicit CBORIndexer(IndexOptions opt): opts(std::move(opt)) {}

    // 主入口：从 CBOR buffer 构建索引（支持分片）
    bool buildIndexFromBuffer(const std::vector<std::uint8_t>& buf,
                              const QString& baseIdxPath);
    // util/cbor_indexer.h 里 CBORIndexer 类的 public: 区域，现有声明旁边加这一行
    bool buildIndexFromFile(const QString& binPath, const QString& baseIdxPath);

private:
    IndexOptions opts;
    QString baseIdxPath_;

    // === 扫描 ===
    size_t scanItem(Cursor& c, ScanCtx& ctx, int& outType, SQLiteSink& where);
    size_t scanArray(Cursor& c, ScanCtx& ctx, SQLiteSink& where);
    size_t scanMap  (Cursor& c, ScanCtx& ctx, SQLiteSink& where);

    // === 跳过（不写入）===
    size_t skipItem(Cursor& c);
    size_t skipArray(Cursor& c);
    size_t skipMap  (Cursor& c);

    // 工具
    static uint64_t readUInt(const std::uint8_t* p, const std::uint8_t* end, int ai, size_t& hdr);
    static std::string joinPointer(const std::vector<std::string>& parts);
    static bool starts_with(const std::string& s, const std::string& pfx);
    bool shouldPrune(const std::string& curPtr, int depth) const;

    // ====== 分片支持 ======
    // 根索引 sink（始终存在）
    std::unique_ptr<SQLiteSink> rootSink;

    // 每个 shard 指向一个 sink；键是“分片根指针”，例如 "/childrens/BET 100.000000"
    std::map<std::string, std::unique_ptr<SQLiteSink>> shardSinks;

    // 创建或获取分片 sink；shardId从1开始递增
    SQLiteSink& ensureShard(const std::string& shardRootPtr);
};
