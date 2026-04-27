#ifndef TEXASSOLVER_HOTSPOTPROFILER_H
#define TEXASSOLVER_HOTSPOTPROFILER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

enum class HotspotId {
    PokerTrain,
    PokerTrainParseRanges,
    PokerTrainConstructSolver,
    PokerTrainSolve,
    PcfrConstruct,
    PcfrSetTrainable,
    PcfrFindIso,
    PcfrTrain,
    PcfrInitialExploitability,
    PcfrTrainingLoop,
    PcfrCollectStatics,
    PcfrActionUtility,
    PcfrChanceUtility,
    PcfrShowdownUtility,
    PcfrTerminalUtility,
    BestResponsePrintExploitability,
    BestResponseGetEv,
    BestResponseAction,
    BestResponseChance,
    BestResponseShowdown,
    BestResponseTerminal,
    RiverRangeGet,
    RiverRangeBuild,
    DiscountedFillAverage,
    DiscountedFillCurrent,
    DiscountedUpdateRegrets,
    ExportTsb2,
    ExportTsb2CollectNodes,
    ExportTsb2BuildMeta,
    ExportTsb2EmitStrategies,
    ExportTsb2WriteIndexMeta,
    Count
};

#define TEXASSOLVER_HOTSPOT_CONCAT_IMPL(a, b) a##b
#define TEXASSOLVER_HOTSPOT_CONCAT(a, b) TEXASSOLVER_HOTSPOT_CONCAT_IMPL(a, b)

#ifdef TEXASSOLVER_HOTSPOT_PROFILING
#define TEXASSOLVER_HOTSPOT_SCOPE(id) \
    HotspotProfiler::Scope TEXASSOLVER_HOTSPOT_CONCAT(hotspot_scope_, __LINE__)(id)
#else
#define TEXASSOLVER_HOTSPOT_SCOPE(id) ((void)0)
#endif

class HotspotProfiler {
public:
    struct Snapshot {
        std::string name;
        long long count;
        long long inclusive_ns;
        long long self_ns;
    };

    class Scope {
    public:
        explicit Scope(HotspotId id)
            : id_(id), active_(HotspotProfiler::enabled()) {
            if (!active_) return;
            start_ = Clock::now();
            stack().push_back(this);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        ~Scope() {
            if (!active_) return;

            const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start_).count();
            const auto self_ns = elapsed_ns > child_ns_ ? elapsed_ns - child_ns_ : 0;

            const int index = static_cast<int>(id_);
            inclusiveNs()[index].fetch_add(elapsed_ns, std::memory_order_relaxed);
            selfNs()[index].fetch_add(self_ns, std::memory_order_relaxed);
            counts()[index].fetch_add(1, std::memory_order_relaxed);

            std::vector<Scope*>& frames = stack();
            if (!frames.empty() && frames.back() == this) {
                frames.pop_back();
                if (!frames.empty()) {
                    frames.back()->child_ns_ += elapsed_ns;
                }
                return;
            }

            auto it = std::find(frames.begin(), frames.end(), this);
            if (it != frames.end()) {
                frames.erase(it);
            }
        }

    private:
        using Clock = std::chrono::steady_clock;

        static std::vector<Scope*>& stack() {
            static thread_local std::vector<Scope*> frames;
            return frames;
        }

        HotspotId id_;
        bool active_ = false;
        Clock::time_point start_;
        long long child_ns_ = 0;
    };

    static void setEnabled(bool value) {
        enabledFlag().store(value, std::memory_order_relaxed);
    }

    static bool enabled() {
        return enabledFlag().load(std::memory_order_relaxed);
    }

    static void reset() {
        for (int i = 0; i < static_cast<int>(HotspotId::Count); ++i) {
            inclusiveNs()[i].store(0, std::memory_order_relaxed);
            selfNs()[i].store(0, std::memory_order_relaxed);
            counts()[i].store(0, std::memory_order_relaxed);
        }
    }

    static std::vector<Snapshot> snapshot() {
        std::vector<Snapshot> out;
        for (int i = 0; i < static_cast<int>(HotspotId::Count); ++i) {
            const long long count = counts()[i].load(std::memory_order_relaxed);
            const long long inclusive_ns = inclusiveNs()[i].load(std::memory_order_relaxed);
            const long long self_ns = selfNs()[i].load(std::memory_order_relaxed);
            if (count == 0 && inclusive_ns == 0 && self_ns == 0) continue;
            out.push_back(Snapshot{names()[i], count, inclusive_ns, self_ns});
        }
        std::sort(out.begin(), out.end(), [](const Snapshot& a, const Snapshot& b) {
            if (a.self_ns != b.self_ns) return a.self_ns > b.self_ns;
            return a.inclusive_ns > b.inclusive_ns;
        });
        return out;
    }

    static void print(std::ostream& out) {
        const std::vector<Snapshot> rows = snapshot();
        for (const Snapshot& row : rows) {
            out << "BENCH_HOTSPOT"
                << " name=" << row.name
                << " count=" << row.count
                << " inclusive_ms=" << (row.inclusive_ns / 1000000.0)
                << " self_ms=" << (row.self_ns / 1000000.0)
                << '\n';
        }
    }

private:
    static std::atomic<bool>& enabledFlag() {
        static std::atomic<bool> enabled(false);
        return enabled;
    }

    static const char** names() {
        static const char* data[static_cast<int>(HotspotId::Count)] = {
            "poker.train",
            "poker.train.parse_ranges",
            "poker.train.construct_solver",
            "poker.train.solve",
            "pcfr.construct",
            "pcfr.set_trainable",
            "pcfr.find_iso",
            "pcfr.train",
            "pcfr.initial_exploitability",
            "pcfr.training_loop",
            "pcfr.collect_statics",
            "pcfr.action_utility",
            "pcfr.chance_utility",
            "pcfr.showdown_utility",
            "pcfr.terminal_utility",
            "br.print_exploitability",
            "br.get_ev",
            "br.action",
            "br.chance",
            "br.showdown",
            "br.terminal",
            "river_range.get",
            "river_range.build",
            "trainable.discounted.fill_average",
            "trainable.discounted.fill_current",
            "trainable.discounted.update_regrets",
            "export.tsb2",
            "export.tsb2.collect_nodes",
            "export.tsb2.build_meta",
            "export.tsb2.emit_strategies",
            "export.tsb2.write_index_meta"
        };
        return data;
    }

    static std::atomic<long long>* inclusiveNs() {
        static std::atomic<long long> data[static_cast<int>(HotspotId::Count)];
        return data;
    }

    static std::atomic<long long>* selfNs() {
        static std::atomic<long long> data[static_cast<int>(HotspotId::Count)];
        return data;
    }

    static std::atomic<long long>* counts() {
        static std::atomic<long long> data[static_cast<int>(HotspotId::Count)];
        return data;
    }
};

#endif // TEXASSOLVER_HOTSPOTPROFILER_H
