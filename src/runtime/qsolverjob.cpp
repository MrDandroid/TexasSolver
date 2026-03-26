#include "include/runtime/qsolverjob.h"
#include <QFileInfo>
#include <QElapsedTimer>

using namespace std;

void QSolverJob::setContext(QSTextEdit * textEdit){
    this->textEdit = textEdit;
}

PokerSolver* QSolverJob::get_solver(){
    if(this->mode == Mode::HOLDEM){
        return &this->ps_holdem;
    }else if(this->mode == Mode::SHORTDECK){
        return &this->ps_shortdeck;
    }else{
        throw runtime_error("unknown mode in get_solver");
    }
}

void QSolverJob::run(){
    try{
        if(this->current_mission == MissionType::SOLVING){
            this->solving();
        }else if(this->current_mission == MissionType::LOADING){
            this->loading();
        }else if(this->current_mission == MissionType::BUILDTREE){
            this->build_tree();
        }else if(this->current_mission == MissionType::SAVING){
            this->saving();
        }else{
            throw runtime_error("unsupported mission type");
        }
    }
    catch (const runtime_error& error){
        qDebug().noquote() << tr("Encountering error:");
        qDebug().noquote() << error.what() << "\n";
    }
}

void QSolverJob::loading(){
    string suits = "c,d,h,s";
    string ranks;
    this->resource_dir =  ":/resources";
    string compairer_file, compairer_file_bin;
    int lines;

    qDebug().noquote() << tr("Loading holdem compairing file");
    ranks = "2,3,4,5,6,7,8,9,T,J,Q,K,A";
    compairer_file = this->resource_dir + "/compairer/card5_dic_sorted.txt";
    compairer_file_bin = this->resource_dir + "/compairer/card5_dic_zipped.bin";
    lines = 2598961;
    this->ps_holdem = PokerSolver(ranks,suits,compairer_file,lines,compairer_file_bin);

    qDebug().noquote() << tr("Loading shortdeck compairing file");
    ranks = "6,7,8,9,T,J,Q,K,A";
    compairer_file = this->resource_dir + "/compairer/card5_dic_sorted_shortdeck.txt";
    compairer_file_bin = this->resource_dir + "/compairer/card5_dic_zipped_shortdeck.bin";
    lines = 376993;
    this->ps_shortdeck = PokerSolver(ranks,suits,compairer_file,lines,compairer_file_bin);

    qDebug().noquote() << tr("Loading finished. Good to go.");
}

void QSolverJob::saving(){
    QElapsedTimer t;
    t.start();

    qDebug().noquote() << tr("Saving..");

    QSettings setting("TexasSolver", "Setting");
    setting.beginGroup("solver");
    this->dump_rounds = setting.value("dump_round", 2).toInt(); // 默认到 turn

    QFileInfo fi(this->savefile);
    const QString suffix = fi.completeSuffix().toLower(); // 支持多段后缀（如 ndjson.gz）

    try {
        if (suffix == "json") {
            // 传统一次性 JSON
            this->get_solver()->dump_strategy(this->savefile, this->dump_rounds);
        } else if (suffix == "ndjson" || suffix == "ndjson.gz") {
            // 流式 NDJSON（内部已处理 .gz 与否）
            this->get_solver()->dump_strategy_stream(this->savefile, this->dump_rounds);
        } else if (suffix == "sqlite") {
            // 注意：PokerSolver 现在是两个参数版本
            this->get_solver()->dump_strategy_sqlite(this->savefile, this->dump_rounds);
        } else if (suffix == "bin") {
            // 导出 CBOR .bin + 生成 .idx（SQLite 索引）
            this->get_solver()->dump_strategy_bin_with_index(this->savefile, this->dump_rounds);
        } else if (suffix == "json.gz"){
            this->get_solver()->dump_strategy_stream(this->savefile, this->dump_rounds);
        } else {
            qDebug().noquote() << tr("Unknown extension: ") << suffix << tr(", fallback to NDJSON.");
            this->get_solver()->dump_strategy_stream(this->savefile, this->dump_rounds);
        }

        const double seconds = t.elapsed() / 1000.0;
        qDebug().noquote() << tr("保存成功.");
        qDebug().noquote() << tr("耗时：") << QString::number(seconds, 'f', 1) << tr(" 秒");
    } catch (const std::exception& e) {
        const double seconds = t.elapsed() / 1000.0;
        qDebug().noquote() << tr("保存失败: ") << e.what();
        qDebug().noquote() << tr("耗时：") << QString::number(seconds, 'f', 1) << tr(" 秒");
    }
}

void QSolverJob::stop(){
    qDebug().noquote() << tr("Trying to stop solver.");
    if(this->mode == Mode::HOLDEM){
        this->ps_holdem.stop();
    }else if(this->mode == Mode::SHORTDECK){
        this->ps_shortdeck.stop();
    }
}

void QSolverJob::solving(){
    qDebug().noquote() << tr("Start Solving..");
    int warmup = std::min(500, std::max(50, max_iteration / 5));
    if(this->mode == Mode::HOLDEM){
        this->ps_holdem.train(
            this->range_ip,
            this->range_oop,
            this->board,
            "",
            max_iteration,
            this->print_interval,
            "discounted_cfr",
            -1,
            this->accuracy,
            this->use_isomorphism,
            this->use_halffloats,
            this->thread_number
            );
    }else{
        this->ps_shortdeck.train(
            this->range_ip,
            this->range_oop,
            this->board,
            "",
            max_iteration,
            this->print_interval,
            "discounted_cfr",
            -1,
            this->accuracy,
            this->use_isomorphism,
            this->use_halffloats,
            this->thread_number
            );
    }
    qDebug().noquote() << tr("Solving done.");
}

long long QSolverJob::estimate_tree_memory(QString range1,QString range2,QString board){
    qDebug().noquote() << tr("Estimating tree memory..");
    if(this->mode == Mode::HOLDEM){
        return ps_holdem.estimate_tree_memory(range1,range2,board);
    }else{
        return ps_shortdeck.estimate_tree_memory(range1,range2,board);
    }
}

void QSolverJob::build_tree(){
    qDebug().noquote() << tr("building tree..");

    // === read rake settings ===
    QSettings setting("TexasSolver", "Setting");
    setting.beginGroup("solver");
    const double rake_pct = 0.00;             // 0.05
    const double rake_cap_bb = 0.0;       // 3.0
    const bool no_flop_no_drop = true;
    setting.endGroup();

    const float rake_cap = (rake_cap_bb > 0.0 ? float(rake_cap_bb * double(big_blind)) : 0.0f);

    if(this->mode == Mode::HOLDEM){
        ps_holdem.build_game_tree(
            oop_commit, ip_commit, current_round, raise_limit,
            small_blind, big_blind, stack, *gtbs.get(), allin_threshold,
            // === rake settings ===
            (float)rake_pct, rake_cap, no_flop_no_drop
            );
    }else{
        ps_shortdeck.build_game_tree(
            oop_commit, ip_commit, current_round, raise_limit,
            small_blind, big_blind, stack, *gtbs.get(), allin_threshold,
            // === rake settings ===
            (float)rake_pct, rake_cap, no_flop_no_drop
            );
    }
    qDebug() << "[RAKE] pct=" << rake_pct
             << " cap_bb=" << rake_cap_bb
             << " cap_chip=" << rake_cap
             << " no_flop_no_drop=" << no_flop_no_drop;
    qDebug().noquote() << tr("build tree finished");
}
