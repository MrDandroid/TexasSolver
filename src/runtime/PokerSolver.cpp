//
// Created by Xuefeng Huang on 2020/2/6.
//

#include "include/runtime/PokerSolver.h"
#include <fstream>
#include <locale>
#include <QSettings>
#include "include/json.hpp"      // nlohmann::json
#include "util/cbor_indexer.h"   // 新增的索引器
#include <vector>
#include <QtCore/QElapsedTimer>
#include <src/solver/GzRawWriter.h>
#include <src/solver/strategyjsonexporterv2.h>
using nlohmann::json;


PokerSolver::PokerSolver() {}

PokerSolver::PokerSolver(string ranks, string suits, string compairer_file,int compairer_file_lines, string compairer_file_bin) {
    vector<string> ranks_vector = string_split(ranks,',');
    vector<string> suits_vector = string_split(suits,',');
    this->deck = Deck(ranks_vector,suits_vector);
    this->compairer = make_shared<Dic5Compairer>(compairer_file,compairer_file_lines,compairer_file_bin);
}

void PokerSolver::load_game_tree(string game_tree_file) {
    shared_ptr<GameTree> game_tree = make_shared<GameTree>(game_tree_file,this->deck);
    this->game_tree = game_tree;
}

void PokerSolver::build_game_tree(
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
    float rake_cap,
    bool no_flop_no_drop
    ){
    shared_ptr<GameTree> game_tree = make_shared<GameTree>(
        this->deck,
        oop_commit,
        ip_commit,
        current_round,
        raise_limit,
        small_blind,
        big_blind,
        stack,
        buildingSettings,
        allin_threshold,
        // === rake settings ===
        rake_pct,
        rake_cap,
        no_flop_no_drop
        );
    this->game_tree = game_tree;
}

static vector<PrivateCards> noDuplicateRange(const vector<PrivateCards> &private_range, uint64_t board_long) {
    vector<PrivateCards> range_array;
    unordered_map<int,bool> rangekv;
    for(PrivateCards one_range:private_range){
        if(rangekv.find(one_range.hashCode()) != rangekv.end())
            throw runtime_error(tfm::format("duplicated key %s",one_range.toString()));
        rangekv[one_range.hashCode()] = true;
        uint64_t hand_long = Card::boardInts2long(one_range.get_hands());
        if(!Card::boardsHasIntercept(hand_long,board_long)){
            range_array.push_back(one_range);
        }
    }
    return range_array;
}

void PokerSolver::stop(){
    if(this->solver != nullptr){
        this->solver->stop();
    }
}

long long PokerSolver::estimate_tree_memory(QString range1,QString range2,QString board){
    if(this->game_tree == nullptr){
        qDebug().noquote() << QObject::tr("Please build tree first.");
        return 0;
    }
    else{
        string player1RangeStr = range1.toStdString();
        string player2RangeStr = range2.toStdString();

        vector<string> board_str_arr = string_split(board.toStdString(),',');
        vector<int> initialBoard;
        for(string one_board_str:board_str_arr){
            initialBoard.push_back(Card::strCard2int(one_board_str));
        }

        vector<PrivateCards> range1v = PrivateRangeConverter::rangeStr2Cards(player1RangeStr,initialBoard);
        vector<PrivateCards> range2v = PrivateRangeConverter::rangeStr2Cards(player2RangeStr,initialBoard);
        return this->game_tree->estimate_tree_memory(this->deck.getCards().size() - (int)initialBoard.size(),
                                                     (int)range1v.size(),(int)range2v.size());
    }
}

void PokerSolver::train(string p1_range, string p2_range, string boards, string log_file, int iteration_number,
                        int print_interval, string algorithm,int warmup,float accuracy,bool use_isomorphism, int use_halffloats, int threads) {
    string player1RangeStr = p1_range;
    string player2RangeStr = p2_range;

    vector<string> board_str_arr = string_split(boards,',');
    vector<int> initialBoard;
    for(string one_board_str:board_str_arr){
        initialBoard.push_back(Card::strCard2int(one_board_str));
    }

    vector<PrivateCards> range1 = PrivateRangeConverter::rangeStr2Cards(player1RangeStr,initialBoard);
    vector<PrivateCards> range2 = PrivateRangeConverter::rangeStr2Cards(player2RangeStr,initialBoard);
    uint64_t initial_board_long = Card::boardInts2long(initialBoard);

    this->player1Range = noDuplicateRange(range1,initial_board_long);
    this->player2Range = noDuplicateRange(range2,initial_board_long);

    string logfile_name = log_file;
    this->solver = make_shared<PCfrSolver>(
        game_tree,
        range1,
        range2,
        initialBoard,
        compairer,
        deck,
        iteration_number,
        false,
        print_interval,
        logfile_name,
        algorithm,
        Solver::MonteCarolAlg::NONE,
        warmup,
        accuracy,
        use_isomorphism,
        use_halffloats,
        threads
        );
    this->solver->train();
}

// 旧：一次性 DOM 导出（体积大）
// 保存成一个巨大的 JSON，容易几 GB
void PokerSolver::dump_strategy(QString dump_file,int dump_rounds) {
    setlocale(LC_ALL,"");
    json dump_json = this->solver->dumps(false,dump_rounds);
    ofstream fileWriter;
    fileWriter.open(dump_file.toLocal8Bit());
    if(!fileWriter.fail()){
        fileWriter << dump_json;
        fileWriter.flush();
        fileWriter.close();
        qDebug().noquote() << QObject::tr("save success");
    }else{
        qDebug().noquote() << QObject::tr("save failed, file cannot be open");
    }
    setlocale(LC_CTYPE, "C");
}

// 新：流式导出（低内存，建议 .ndjson 或 .ndjson.gz）
void PokerSolver::dump_strategy_stream(QString dump_file, int dump_rounds) {
    setlocale(LC_ALL,"");

    QSettings setting("TexasSolver", "Setting");
    setting.beginGroup("solver");
    int max_depth   = (dump_rounds > 0 ? dump_rounds : setting.value("dump_round", 2).toInt());
    double prune_eps = setting.value("export_prune_eps", 0.005).toDouble();
    int precision    = setting.value("export_precision", 3).toInt();

    auto pcfr = std::dynamic_pointer_cast<PCfrSolver>(this->solver);
    if (!pcfr) {
        qDebug().noquote() << QObject::tr("stream save failed: solver type not supported");
        setlocale(LC_CTYPE, "C");
        return;
    }

    try {
        const std::string path = dump_file.toStdString();
        // 根据后缀自动选择
        if (dump_file.endsWith(".sqlite", Qt::CaseInsensitive)) {
            pcfr->dump_strategy_sqlite(path, max_depth, prune_eps, precision);
            qDebug().noquote() << QObject::tr("SQLite save success");
        } else if (dump_file.endsWith(".ndjson", Qt::CaseInsensitive)||dump_file.endsWith(".ndjson.gz", Qt::CaseInsensitive)){
            pcfr->dump_strategy_stream(path, max_depth, prune_eps, precision);
            qDebug().noquote() << QObject::tr("NDJSON save success");
        } else if (dump_file.endsWith(".bin", Qt::CaseInsensitive)) {
            pcfr->dump_strategy_bin(path, (dump_file + ".idx.txt").toStdString(), max_depth, /*use_fp16=*/false, /*align8=*/true);
            qDebug().noquote() << QObject::tr("BIN save success");
        } else {
            // ✅ 新：流式 .json.gz + 仅 strategy 叶子索引（v2 do=SLEB64）
            // 标准德扑：52；短牌传 36
            StrategyJsonExporterV2 ex(pcfr.get(),
                                      dump_file.toStdString(),
                                      (dump_file + ".xidx").toStdString(),
                                      /*deck_size=*/(int)this->deck.getCards().size());
            ex.run();
            qDebug().noquote() << QObject::tr("JSON.gz + XIDX(v2) 导出完成（流式，无 DOM）");
        }
    } catch (const std::exception& e) {
        qDebug().noquote() << QObject::tr("stream save failed: ") << e.what();
    }

    setlocale(LC_CTYPE, "C");
}
void PokerSolver::dump_strategy_sqlite(QString dump_file, int dump_rounds) {
    setlocale(LC_ALL,"");

    QSettings setting("TexasSolver", "Setting");
    setting.beginGroup("solver");
    int max_depth    = (dump_rounds > 0 ? dump_rounds : setting.value("dump_round", 2).toInt());
    double prune_eps = setting.value("export_prune_eps", 0.005).toDouble();
    int precision    = setting.value("export_precision", 3).toInt();

    auto pcfr = std::dynamic_pointer_cast<PCfrSolver>(this->solver);
    if (!pcfr) {
        qDebug().noquote() << QObject::tr("sqlite save failed: solver type not supported");
        setlocale(LC_CTYPE, "C");
        return;
    }

    try {
        pcfr->dump_strategy_sqlite(dump_file.toStdString(), max_depth, prune_eps, precision);
        qDebug().noquote() << QObject::tr("sqlite save success");
    } catch (const std::exception& e) {
        qDebug().noquote() << QObject::tr("sqlite save failed: ") << e.what();
    }

    setlocale(LC_CTYPE, "C");
}

// 只统计“容器（object/array）”数量，用于预估进度
static size_t count_json_containers(const json& j, int maxDepth, const std::vector<std::string>& skipPrefixes, int depth=0) {
    auto starts_with = [](const std::string& s, const std::string& pfx){
        return s.size() >= pfx.size() && std::equal(pfx.begin(), pfx.end(), s.begin());
    };
    // 简化：这里不按 pointer 前缀判断（因为缺少路径），只按深度估计；精确前缀可在构建时再剪枝
    if (maxDepth >= 0 && depth > maxDepth) return 0;

    if (j.is_object()) {
        size_t n = 1;
        for (auto it = j.begin(); it != j.end(); ++it) {
            n += count_json_containers(it.value(), maxDepth, skipPrefixes, depth+1);
        }
        return n;
    }
    if (j.is_array()) {
        size_t n = 1;
        for (const auto& el : j) {
            n += count_json_containers(el, maxDepth, skipPrefixes, depth+1);
        }
        return n;
    }
    return 0;
}

// 新导出：后缀 .bin 时走本实现
void PokerSolver::dump_strategy_bin_with_index(QString dump_file, int dump_rounds){
    setlocale(LC_ALL,"");

    QSettings setting("TexasSolver", "Setting");
    setting.beginGroup("solver");
    int max_depth = (dump_rounds > 0 ? dump_rounds : setting.value("dump_round", 2).toInt());

    auto pcfr = std::dynamic_pointer_cast<PCfrSolver>(this->solver);
    if (!pcfr) {
        qDebug().noquote() << QObject::tr("BIN save failed: solver type not supported");
        setlocale(LC_CTYPE, "C");
        return;
    }

    try {
        pcfr->dump_strategy_tsb2(dump_file.toStdString(), max_depth);
        qDebug().noquote() << QObject::tr("TSB2 导出完成（.tsb/.tsx/.tsm）");
    } catch (const std::exception& e) {
        qDebug().noquote() << QObject::tr("BIN save failed: ") << e.what();
    }

    setlocale(LC_CTYPE, "C");
}


const shared_ptr<GameTree> &PokerSolver::getGameTree() const {
    return game_tree;
}
