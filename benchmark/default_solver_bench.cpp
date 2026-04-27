#include "include/runtime/PokerSolver.h"
#include "include/tools/HotspotProfiler.h"

#include <QCoreApplication>
#include <QString>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using Clock = std::chrono::steady_clock;

struct BenchOptions {
    std::string resource_dir = "resources";
    std::string export_bin_path;
    int dump_rounds = 2;
    int max_iteration = 200;
    int print_interval = 10;
    int threads = 8;
    float accuracy = 0.5f;
    bool use_isomorphism = true;
    int use_halffloats = 0;
    bool profile_hotspots = false;
};

static long long elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

static bool read_bool(const std::string& value) {
    return value == "1" || value == "true" || value == "True" || value == "TRUE";
}

static BenchOptions parse_args(int argc, char** argv) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--resource-dir") {
            options.resource_dir = need_value("--resource-dir");
        } else if (arg == "--export-bin") {
            options.export_bin_path = need_value("--export-bin");
        } else if (arg == "--dump-rounds") {
            options.dump_rounds = std::atoi(need_value("--dump-rounds").c_str());
        } else if (arg == "--max-iteration") {
            options.max_iteration = std::atoi(need_value("--max-iteration").c_str());
        } else if (arg == "--print-interval") {
            options.print_interval = std::atoi(need_value("--print-interval").c_str());
        } else if (arg == "--threads") {
            options.threads = std::atoi(need_value("--threads").c_str());
        } else if (arg == "--accuracy") {
            options.accuracy = static_cast<float>(std::atof(need_value("--accuracy").c_str()));
        } else if (arg == "--use-isomorphism") {
            options.use_isomorphism = read_bool(need_value("--use-isomorphism"));
        } else if (arg == "--use-halffloats") {
            options.use_halffloats = std::atoi(need_value("--use-halffloats").c_str());
        } else if (arg == "--profile-hotspots") {
            options.profile_hotspots = true;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return options;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    try {
        const BenchOptions options = parse_args(argc, argv);
        const std::string ip_range =
            "AA,KK,QQ,JJ,TT,99:0.75,88:0.75,77:0.5,66:0.25,55:0.25,AK,AQs,AQo:0.75,AJs,AJo:0.5,ATs:0.75,A6s:0.25,A5s:0.75,A4s:0.75,A3s:0.5,A2s:0.5,KQs,KQo:0.5,KJs,KTs:0.75,K5s:0.25,K4s:0.25,QJs:0.75,QTs:0.75,Q9s:0.5,JTs:0.75,J9s:0.75,J8s:0.75,T9s:0.75,T8s:0.75,T7s:0.75,98s:0.75,97s:0.75,96s:0.5,87s:0.75,86s:0.5,85s:0.5,76s:0.75,75s:0.5,65s:0.75,64s:0.5,54s:0.75,53s:0.5,43s:0.5";
        const std::string oop_range =
            "QQ:0.5,JJ:0.75,TT,99,88,77,66,55,44,33,22,AKo:0.25,AQs,AQo:0.75,AJs,AJo:0.75,ATs,ATo:0.75,A9s,A8s,A7s,A6s,A5s,A4s,A3s,A2s,KQ,KJ,KTs,KTo:0.5,K9s,K8s,K7s,K6s,K5s,K4s:0.5,K3s:0.5,K2s:0.5,QJ,QTs,Q9s,Q8s,Q7s,JTs,JTo:0.5,J9s,J8s,T9s,T8s,T7s,98s,97s,96s,87s,86s,76s,75s,65s,64s,54s,53s,43s";

        std::cout << "BENCH_DEFAULT_CONFIG"
                  << " board=Qs,Jh,2h"
                  << " max_iteration=" << options.max_iteration
                  << " print_interval=" << options.print_interval
                  << " accuracy=" << options.accuracy
                  << " threads=" << options.threads
                  << " dump_rounds=" << options.dump_rounds
                  << " export_bin=" << (options.export_bin_path.empty() ? 0 : 1)
                  << " profile_hotspots=" << (options.profile_hotspots ? 1 : 0)
                  << std::endl;

        HotspotProfiler::reset();
        HotspotProfiler::setEnabled(options.profile_hotspots);

        auto total_start = Clock::now();
        PokerSolver solver(
            "2,3,4,5,6,7,8,9,T,J,Q,K,A",
            "c,d,h,s",
            options.resource_dir + "/compairer/card5_dic_sorted.txt",
            2598961,
            options.resource_dir + "/compairer/card5_dic_zipped.bin");
        auto loaded = Clock::now();

        StreetSetting flop_ip({50}, {60}, {}, true);
        StreetSetting turn_ip({50}, {60}, {}, true);
        StreetSetting river_ip({50}, {60, 100}, {}, true);
        StreetSetting flop_oop({50}, {60}, {}, true);
        StreetSetting turn_oop({50}, {60}, {50}, true);
        StreetSetting river_oop({50}, {60, 100}, {50}, true);
        GameTreeBuildingSettings settings(flop_ip, turn_ip, river_ip, flop_oop, turn_oop, river_oop);

        solver.build_game_tree(
            25.0f,
            25.0f,
            1,
            3,
            0.5f,
            1.0f,
            225.0f,
            settings,
            0.67f,
            0.0f,
            0.0f,
            true);
        auto built = Clock::now();

        solver.train(
            ip_range,
            oop_range,
            "Qs,Jh,2h",
            "",
            options.max_iteration,
            options.print_interval,
            "discounted_cfr",
            -1,
            options.accuracy,
            options.use_isomorphism,
            options.use_halffloats,
            options.threads);
        auto solved = Clock::now();

        Clock::time_point exported = solved;
        if (!options.export_bin_path.empty()) {
            solver.dump_strategy_bin_with_index(QString::fromStdString(options.export_bin_path), options.dump_rounds);
            exported = Clock::now();
        }

        std::cout << "BENCH_DEFAULT_RESULT"
                  << " load_ms=" << elapsed_ms(total_start, loaded)
                  << " build_ms=" << elapsed_ms(loaded, built)
                  << " solve_ms=" << elapsed_ms(built, solved)
                  << " export_bin_ms=" << elapsed_ms(solved, exported)
                  << " total_ms=" << elapsed_ms(total_start, exported)
                  << std::endl;
        if (options.profile_hotspots) {
            HotspotProfiler::print(std::cout);
        }
        HotspotProfiler::setEnabled(false);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BENCH_DEFAULT_ERROR " << error.what() << std::endl;
        return 1;
    }
}
