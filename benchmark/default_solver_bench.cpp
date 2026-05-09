#include "include/runtime/PokerSolver.h"
#include "include/tools/HotspotProfiler.h"

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;

struct BenchOptions {
    std::string resource_dir = "resources";
    std::string parameter_file;
    std::string export_bin_path;
    int dump_rounds = 2;
    int max_iteration = 200;
    int print_interval = 10;
    int threads = 8;
    float accuracy = 0.5f;
    bool use_isomorphism = true;
    int use_halffloats = 0;
    int exploitability_interval = -1;
    bool collect_evs = true;
    bool profile_hotspots = false;
    bool iso_potential_only = false;
    bool dump_rounds_set = false;
    bool max_iteration_set = false;
    bool print_interval_set = false;
    bool threads_set = false;
    bool accuracy_set = false;
    bool use_isomorphism_set = false;
    bool use_halffloats_set = false;
    bool exploitability_interval_set = false;
    bool collect_evs_set = false;
};

struct BenchParameters {
    std::string board;
    std::string ip_range;
    std::string oop_range;
    float pot;
    float effective_stack;
    int raise_limit;
    float allin_threshold;
    int dump_rounds;
    int max_iteration;
    int print_interval;
    int threads;
    float accuracy;
    bool use_isomorphism;
    int use_halffloats;
    int exploitability_interval;
    bool collect_evs;
    StreetSetting flop_ip;
    StreetSetting turn_ip;
    StreetSetting river_ip;
    StreetSetting flop_oop;
    StreetSetting turn_oop;
    StreetSetting river_oop;

    BenchParameters()
        : board("Qs,Jh,2h"),
          ip_range("AA,KK,QQ,JJ,TT,99:0.75,88:0.75,77:0.5,66:0.25,55:0.25,AK,AQs,AQo:0.75,AJs,AJo:0.5,ATs:0.75,A6s:0.25,A5s:0.75,A4s:0.75,A3s:0.5,A2s:0.5,KQs,KQo:0.5,KJs,KTs:0.75,K5s:0.25,K4s:0.25,QJs:0.75,QTs:0.75,Q9s:0.5,JTs:0.75,J9s:0.75,J8s:0.75,T9s:0.75,T8s:0.75,T7s:0.75,98s:0.75,97s:0.75,96s:0.5,87s:0.75,86s:0.5,85s:0.5,76s:0.75,75s:0.5,65s:0.75,64s:0.5,54s:0.75,53s:0.5,43s:0.5"),
          oop_range("QQ:0.5,JJ:0.75,TT,99,88,77,66,55,44,33,22,AKo:0.25,AQs,AQo:0.75,AJs,AJo:0.75,ATs,ATo:0.75,A9s,A8s,A7s,A6s,A5s,A4s,A3s,A2s,KQ,KJ,KTs,KTo:0.5,K9s,K8s,K7s,K6s,K5s,K4s:0.5,K3s:0.5,K2s:0.5,QJ,QTs,Q9s,Q8s,Q7s,JTs,JTo:0.5,J9s,J8s,T9s,T8s,T7s,98s,97s,96s,87s,86s,76s,75s,65s,64s,54s,53s,43s"),
          pot(50.0f),
          effective_stack(200.0f),
          raise_limit(3),
          allin_threshold(0.67f),
          dump_rounds(2),
          max_iteration(200),
          print_interval(10),
          threads(8),
          accuracy(0.5f),
          use_isomorphism(true),
          use_halffloats(0),
          exploitability_interval(-1),
          collect_evs(true),
          flop_ip({50}, {60}, {}, true),
          turn_ip({50}, {60}, {}, true),
          river_ip({50}, {60, 100}, {}, true),
          flop_oop({50}, {60}, {}, true),
          turn_oop({50}, {60}, {50}, true),
          river_oop({50}, {60, 100}, {50}, true)
    {}
};

static long long elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

static bool read_bool(const std::string& value) {
    return value == "1" || value == "true" || value == "True" || value == "TRUE";
}

static std::string trim_copy(const std::string& input) {
    std::size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) {
        ++first;
    }
    std::size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) {
        --last;
    }
    return input.substr(first, last - first);
}

static std::vector<std::string> split_string(const std::string& input, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(input);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(trim_copy(part));
    }
    if (!input.empty() && input.back() == delimiter) {
        parts.emplace_back();
    }
    return parts;
}

static std::vector<float> parse_size_list(const std::vector<std::string>& parts, std::size_t start) {
    std::vector<float> sizes;
    for (std::size_t i = start; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            continue;
        }
        std::string value = parts[i];
        if (!value.empty() && value.back() == 'x') {
            value.pop_back();
            sizes.push_back(static_cast<float>(std::atof(value.c_str())) * 100.0f);
        } else {
            sizes.push_back(static_cast<float>(std::atof(value.c_str())));
        }
    }
    return sizes;
}

static StreetSetting& select_street_setting(BenchParameters& params, const std::string& player, const std::string& street) {
    if (player == "ip" && street == "flop") return params.flop_ip;
    if (player == "ip" && street == "turn") return params.turn_ip;
    if (player == "ip" && street == "river") return params.river_ip;
    if (player == "oop" && street == "flop") return params.flop_oop;
    if (player == "oop" && street == "turn") return params.turn_oop;
    if (player == "oop" && street == "river") return params.river_oop;
    throw std::runtime_error("invalid bet size street: " + player + "," + street);
}

static int current_round_from_board(const std::string& board) {
    const std::vector<std::string> cards = split_string(board, ',');
    if (cards.size() == 3) return 1;
    if (cards.size() == 4) return 2;
    if (cards.size() == 5) return 3;
    throw std::runtime_error("board not recognized: " + board);
}

static BenchParameters load_parameters_from_file(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open parameter file: " + path);
    }

    BenchParameters params;
    std::string line;
    while (std::getline(input, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::size_t split = line.find_first_of(" \t");
        const std::string command = split == std::string::npos ? line : line.substr(0, split);
        const std::string value = split == std::string::npos ? "" : trim_copy(line.substr(split + 1));

        if (command == "set_pot") {
            params.pot = static_cast<float>(std::atof(value.c_str()));
        } else if (command == "set_effective_stack") {
            params.effective_stack = static_cast<float>(std::atof(value.c_str()));
        } else if (command == "set_board") {
            params.board = value;
        } else if (command == "set_range_ip") {
            params.ip_range = value;
        } else if (command == "set_range_oop") {
            params.oop_range = value;
        } else if (command == "set_bet_sizes") {
            const std::vector<std::string> parts = split_string(value, ',');
            if (parts.size() < 3) {
                throw std::runtime_error("invalid set_bet_sizes line: " + line);
            }
            StreetSetting& setting = select_street_setting(params, parts[0], parts[1]);
            if (parts[2] == "allin") {
                setting.allin = true;
            } else if (parts[2] == "bet") {
                setting.bet_sizes = parse_size_list(parts, 3);
            } else if (parts[2] == "raise") {
                setting.raise_sizes = parse_size_list(parts, 3);
            } else if (parts[2] == "donk") {
                setting.donk_sizes = parse_size_list(parts, 3);
            } else {
                throw std::runtime_error("invalid bet size type: " + parts[2]);
            }
        } else if (command == "set_allin_threshold") {
            params.allin_threshold = static_cast<float>(std::atof(value.c_str()));
        } else if (command == "set_raise_limit") {
            params.raise_limit = std::atoi(value.c_str());
        } else if (command == "set_thread_num") {
            params.threads = std::atoi(value.c_str());
        } else if (command == "set_accuracy") {
            params.accuracy = static_cast<float>(std::atof(value.c_str()));
        } else if (command == "set_max_iteration") {
            params.max_iteration = std::atoi(value.c_str());
        } else if (command == "set_print_interval") {
            params.print_interval = std::atoi(value.c_str());
        } else if (command == "set_use_isomorphism") {
            params.use_isomorphism = read_bool(value);
        } else if (command == "set_exploitability_interval") {
            params.exploitability_interval = std::atoi(value.c_str());
        } else if (command == "set_collect_evs") {
            params.collect_evs = read_bool(value);
        } else if (command == "set_dump_rounds") {
            params.dump_rounds = std::atoi(value.c_str());
        }
    }
    return params;
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
        } else if (arg == "--parameter-file") {
            options.parameter_file = need_value("--parameter-file");
        } else if (arg == "--export-bin") {
            options.export_bin_path = need_value("--export-bin");
        } else if (arg == "--dump-rounds") {
            options.dump_rounds = std::atoi(need_value("--dump-rounds").c_str());
            options.dump_rounds_set = true;
        } else if (arg == "--max-iteration") {
            options.max_iteration = std::atoi(need_value("--max-iteration").c_str());
            options.max_iteration_set = true;
        } else if (arg == "--print-interval") {
            options.print_interval = std::atoi(need_value("--print-interval").c_str());
            options.print_interval_set = true;
        } else if (arg == "--threads") {
            options.threads = std::atoi(need_value("--threads").c_str());
            options.threads_set = true;
        } else if (arg == "--accuracy") {
            options.accuracy = static_cast<float>(std::atof(need_value("--accuracy").c_str()));
            options.accuracy_set = true;
        } else if (arg == "--use-isomorphism") {
            options.use_isomorphism = read_bool(need_value("--use-isomorphism"));
            options.use_isomorphism_set = true;
        } else if (arg == "--use-halffloats") {
            options.use_halffloats = std::atoi(need_value("--use-halffloats").c_str());
            options.use_halffloats_set = true;
        } else if (arg == "--exploitability-interval") {
            options.exploitability_interval = std::atoi(need_value("--exploitability-interval").c_str());
            options.exploitability_interval_set = true;
        } else if (arg == "--collect-evs") {
            options.collect_evs = read_bool(need_value("--collect-evs"));
            options.collect_evs_set = true;
        } else if (arg == "--profile-hotspots") {
            options.profile_hotspots = true;
        } else if (arg == "--iso-potential-only") {
            options.iso_potential_only = true;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return options;
}

using SuitPerm = std::array<int, 4>;

static std::vector<int> parse_board_cards(const std::string& board_text) {
    std::vector<int> board;
    std::stringstream stream(board_text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (!token.empty()) {
            board.push_back(Card::strCard2int(token));
        }
    }
    return board;
}

static bool has_card(const std::vector<int>& cards, int card) {
    return std::find(cards.begin(), cards.end(), card) != cards.end();
}

static int permute_card_int(int card, const SuitPerm& perm) {
    return (card / 4) * 4 + perm[card % 4];
}

static std::vector<SuitPerm> all_suit_permutations() {
    std::vector<SuitPerm> perms;
    SuitPerm perm{{0, 1, 2, 3}};
    do {
        perms.push_back(perm);
    } while (std::next_permutation(perm.begin(), perm.end()));
    return perms;
}

static std::string sorted_card_key(std::vector<int> cards) {
    std::sort(cards.begin(), cards.end());
    std::ostringstream out;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << cards[i];
    }
    return out.str();
}

static std::string permuted_sorted_card_key(const std::vector<int>& cards, const SuitPerm& perm) {
    std::vector<int> mapped;
    mapped.reserve(cards.size());
    for (int card : cards) {
        mapped.push_back(permute_card_int(card, perm));
    }
    return sorted_card_key(std::move(mapped));
}

static std::string canonical_board_key(const std::vector<int>& cards, const std::vector<SuitPerm>& perms) {
    std::string best;
    bool first = true;
    for (const SuitPerm& perm : perms) {
        std::string key = permuted_sorted_card_key(cards, perm);
        if (first || key < best) {
            best = std::move(key);
            first = false;
        }
    }
    return best;
}

static std::string canonical_sequence_key(
    const std::vector<int>& board,
    int turn_card,
    int river_card,
    const std::vector<SuitPerm>& perms) {
    std::string best;
    bool first = true;
    for (const SuitPerm& perm : perms) {
        std::vector<int> turn_board = board;
        turn_board.push_back(turn_card);
        std::vector<int> river_board = turn_board;
        river_board.push_back(river_card);
        std::string key = permuted_sorted_card_key(turn_board, perm) + "|" +
                          permuted_sorted_card_key(river_board, perm);
        if (first || key < best) {
            best = std::move(key);
            first = false;
        }
    }
    return best;
}

static std::array<int, 4> current_iso_offsets_for_board(const std::vector<int>& board) {
    std::array<uint16_t, 4> suit_rank_masks{{0, 0, 0, 0}};
    for (int card : board) {
        suit_rank_masks[card % 4] |= static_cast<uint16_t>(1u << (card / 4));
    }

    std::array<int, 4> offsets{{0, 0, 0, 0}};
    for (int suit = 0; suit < 4; ++suit) {
        for (int previous = 0; previous < suit; ++previous) {
            if (suit_rank_masks[suit] == suit_rank_masks[previous]) {
                offsets[suit] = previous - suit;
                break;
            }
        }
    }
    return offsets;
}

static int private_hand_key(int card1, int card2) {
    if (card1 > card2) {
        std::swap(card1, card2);
    }
    return card1 * 52 + card2;
}

static std::vector<PrivateCards> filter_private_range(
    const std::vector<PrivateCards>& private_range,
    uint64_t board_long) {
    std::vector<PrivateCards> filtered;
    filtered.reserve(private_range.size());
    std::set<int> seen;
    for (const PrivateCards& combo : private_range) {
        const int key = private_hand_key(combo.card1, combo.card2);
        if (!seen.insert(key).second) {
            throw std::runtime_error("duplicated private combo in range");
        }
        if (!Card::boardsHasIntercept(combo.toBoardLong(), board_long)) {
            filtered.push_back(combo);
        }
    }
    return filtered;
}

static std::unordered_map<int, float> make_range_weight_map(const std::vector<PrivateCards>& range) {
    std::unordered_map<int, float> weights;
    weights.reserve(range.size() * 2 + 1);
    for (const PrivateCards& combo : range) {
        weights[private_hand_key(combo.card1, combo.card2)] = combo.weight;
    }
    return weights;
}

static bool range_symmetric_under_perm(
    const std::vector<PrivateCards>& range,
    const std::unordered_map<int, float>& weights,
    const SuitPerm& perm) {
    constexpr float weight_eps = 0.00001f;
    for (const PrivateCards& combo : range) {
        const int mapped_key = private_hand_key(
            permute_card_int(combo.card1, perm),
            permute_card_int(combo.card2, perm));
        auto found = weights.find(mapped_key);
        if (found == weights.end() || std::fabs(found->second - combo.weight) > weight_eps) {
            return false;
        }
    }
    return true;
}

static bool board_preserved_by_perm(const std::vector<int>& board, const SuitPerm& perm) {
    std::set<int> original(board.begin(), board.end());
    std::set<int> mapped;
    for (int card : board) {
        mapped.insert(permute_card_int(card, perm));
    }
    return original == mapped;
}

static double reduction_pct(long long before, long long after) {
    if (before <= 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(before - after) / static_cast<double>(before);
}

static void print_iso_potential_stats(
    const std::string& board_text,
    const std::string& ip_range_text,
    const std::string& oop_range_text,
    const Deck& deck) {
    const std::vector<int> board = parse_board_cards(board_text);
    const uint64_t board_long = Card::boardInts2long(board);
    const std::vector<SuitPerm> perms = all_suit_permutations();

    const std::vector<PrivateCards> ip_range = filter_private_range(
        PrivateRangeConverter::rangeStr2Cards(ip_range_text, board),
        board_long);
    const std::vector<PrivateCards> oop_range = filter_private_range(
        PrivateRangeConverter::rangeStr2Cards(oop_range_text, board),
        board_long);
    const auto ip_weights = make_range_weight_map(ip_range);
    const auto oop_weights = make_range_weight_map(oop_range);

    std::vector<SuitPerm> board_stabilizers;
    for (const SuitPerm& perm : perms) {
        if (board_preserved_by_perm(board, perm)) {
            board_stabilizers.push_back(perm);
        }
    }

    bool ip_board_symmetric = true;
    bool oop_board_symmetric = true;
    for (const SuitPerm& perm : board_stabilizers) {
        ip_board_symmetric = ip_board_symmetric && range_symmetric_under_perm(ip_range, ip_weights, perm);
        oop_board_symmetric = oop_board_symmetric && range_symmetric_under_perm(oop_range, oop_weights, perm);
    }

    const auto initial_offsets = current_iso_offsets_for_board(board);
    std::vector<int> all_turn_cards;
    std::vector<int> current_turn_cards;
    std::set<std::string> strict_turn_keys;
    for (const Card& card : deck.getCards()) {
        const int card_int = card.getCardInt();
        if (has_card(board, card_int)) {
            continue;
        }
        all_turn_cards.push_back(card_int);
        std::vector<int> turn_board = board;
        turn_board.push_back(card_int);
        strict_turn_keys.insert(canonical_board_key(turn_board, perms));
        if (initial_offsets[card_int % 4] >= 0) {
            current_turn_cards.push_back(card_int);
        }
    }

    long long ordered_river_raw = 0;
    std::set<std::string> strict_transition_keys;
    std::set<std::string> strict_river_board_keys;
    std::set<std::string> raw_river_board_keys;
    for (int turn_card : all_turn_cards) {
        std::vector<int> turn_board = board;
        turn_board.push_back(turn_card);
        for (const Card& river : deck.getCards()) {
            const int river_card = river.getCardInt();
            if (has_card(turn_board, river_card)) {
                continue;
            }
            ++ordered_river_raw;
            std::vector<int> river_board = turn_board;
            river_board.push_back(river_card);
            raw_river_board_keys.insert(sorted_card_key(river_board));
            strict_river_board_keys.insert(canonical_board_key(river_board, perms));
            strict_transition_keys.insert(canonical_sequence_key(board, turn_card, river_card, perms));
        }
    }

    long long current_river_transitions = 0;
    std::set<std::string> current_river_board_keys;
    for (int turn_card : current_turn_cards) {
        std::vector<int> turn_board = board;
        turn_board.push_back(turn_card);
        const auto river_offsets = current_iso_offsets_for_board(turn_board);
        for (const Card& river : deck.getCards()) {
            const int river_card = river.getCardInt();
            if (has_card(turn_board, river_card) || river_offsets[river_card % 4] < 0) {
                continue;
            }
            ++current_river_transitions;
            std::vector<int> river_board = turn_board;
            river_board.push_back(river_card);
            current_river_board_keys.insert(sorted_card_key(river_board));
        }
    }

    std::cout << "BENCH_ISO_POTENTIAL"
              << " board=" << board_text
              << " deck_cards=" << deck.getCards().size()
              << " initial_board_cards=" << board.size()
              << " suit_perms=" << perms.size()
              << std::endl;
    std::cout << "BENCH_ISO_RANGE"
              << " ip_combos=" << ip_range.size()
              << " oop_combos=" << oop_range.size()
              << " board_stabilizer_perms=" << board_stabilizers.size()
              << " ip_board_symmetric=" << (ip_board_symmetric ? 1 : 0)
              << " oop_board_symmetric=" << (oop_board_symmetric ? 1 : 0)
              << " board_stabilizer_exact_safe=" << ((ip_board_symmetric && oop_board_symmetric) ? 1 : 0)
              << std::endl;
    std::cout << "BENCH_ISO_CHANCE_TURN"
              << " raw=" << all_turn_cards.size()
              << " current_iso=" << current_turn_cards.size()
              << " strict_canonical=" << strict_turn_keys.size()
              << " current_reduction_pct=" << reduction_pct(static_cast<long long>(all_turn_cards.size()), static_cast<long long>(current_turn_cards.size()))
              << " strict_extra_pct=" << reduction_pct(static_cast<long long>(current_turn_cards.size()), static_cast<long long>(strict_turn_keys.size()))
              << std::endl;
    std::cout << "BENCH_ISO_CHANCE_RIVER"
              << " ordered_raw=" << ordered_river_raw
              << " current_iso=" << current_river_transitions
              << " strict_canonical=" << strict_transition_keys.size()
              << " current_reduction_pct=" << reduction_pct(ordered_river_raw, current_river_transitions)
              << " strict_extra_pct=" << reduction_pct(current_river_transitions, static_cast<long long>(strict_transition_keys.size()))
              << std::endl;
    std::cout << "BENCH_ISO_RIVER_CACHE"
              << " raw_unordered=" << raw_river_board_keys.size()
              << " current_iso_boards=" << current_river_board_keys.size()
              << " strict_canonical_boards=" << strict_river_board_keys.size()
              << " strict_extra_pct=" << reduction_pct(static_cast<long long>(current_river_board_keys.size()), static_cast<long long>(strict_river_board_keys.size()))
              << std::endl;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    try {
        const BenchOptions options = parse_args(argc, argv);
        BenchParameters params = options.parameter_file.empty()
            ? BenchParameters()
            : load_parameters_from_file(options.parameter_file);

        if (options.max_iteration_set) params.max_iteration = options.max_iteration;
        if (options.print_interval_set) params.print_interval = options.print_interval;
        if (options.threads_set) params.threads = options.threads;
        if (options.accuracy_set) params.accuracy = options.accuracy;
        if (options.dump_rounds_set) params.dump_rounds = options.dump_rounds;
        if (options.use_isomorphism_set) params.use_isomorphism = options.use_isomorphism;
        if (options.use_halffloats_set) params.use_halffloats = options.use_halffloats;
        if (options.exploitability_interval_set) params.exploitability_interval = options.exploitability_interval;
        if (options.collect_evs_set) params.collect_evs = options.collect_evs;

        std::cout << "BENCH_DEFAULT_CONFIG"
                  << " parameter_file=" << (options.parameter_file.empty() ? "builtin" : options.parameter_file)
                  << " board=" << params.board
                  << " pot=" << params.pot
                  << " effective_stack=" << params.effective_stack
                  << " max_iteration=" << params.max_iteration
                  << " print_interval=" << params.print_interval
                  << " accuracy=" << params.accuracy
                  << " threads=" << params.threads
                  << " dump_rounds=" << params.dump_rounds
                  << " exploitability_interval=" << params.exploitability_interval
                  << " collect_evs=" << (params.collect_evs ? 1 : 0)
                  << " export_bin=" << (options.export_bin_path.empty() ? 0 : 1)
                  << " profile_hotspots=" << (options.profile_hotspots ? 1 : 0)
                  << " iso_potential_only=" << (options.iso_potential_only ? 1 : 0)
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

        if (options.iso_potential_only) {
            print_iso_potential_stats(params.board, params.ip_range, params.oop_range, *solver.get_deck());
            auto inspected = Clock::now();
            std::cout << "BENCH_DEFAULT_RESULT"
                      << " load_ms=" << elapsed_ms(total_start, loaded)
                      << " build_ms=0"
                      << " solve_ms=0"
                      << " export_bin_ms=0"
                      << " total_ms=" << elapsed_ms(total_start, inspected)
                      << std::endl;
            HotspotProfiler::setEnabled(false);
            return 0;
        }

        GameTreeBuildingSettings settings(
            params.flop_ip,
            params.turn_ip,
            params.river_ip,
            params.flop_oop,
            params.turn_oop,
            params.river_oop);
        const float committed = params.pot / 2.0f;

        solver.build_game_tree(
            committed,
            committed,
            current_round_from_board(params.board),
            params.raise_limit,
            0.5f,
            1.0f,
            params.effective_stack + committed,
            settings,
            params.allin_threshold,
            0.0f,
            0.0f,
            true);
        auto built = Clock::now();

        solver.train(
            params.ip_range,
            params.oop_range,
            params.board,
            "",
            params.max_iteration,
            params.print_interval,
            "discounted_cfr",
            -1,
            params.accuracy,
            params.use_isomorphism,
            params.use_halffloats,
            params.threads,
            params.exploitability_interval,
            params.collect_evs);
        auto solved = Clock::now();

        Clock::time_point exported = solved;
        if (!options.export_bin_path.empty()) {
            solver.dump_strategy_bin_with_index(QString::fromStdString(options.export_bin_path), params.dump_rounds);
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
