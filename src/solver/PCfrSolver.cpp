//
// Created by Xuefeng Huang on 2020/1/31.
//

#include <include/solver/BestResponse.h>
#include "include/solver/PCfrSolver.h"
#include <QtCore>
#include <QObject>
#include <QTranslator>
#include <cmath>        // roundf
#include <limits>
#include <cmath>
#include <iomanip>
#include <utility>
#include <zlib.h>
#include "include/tools/HotspotProfiler.h"
#include "include/tools/OptimizationSwitches.h"
static inline float calc_rake_amount(float pot, float rake_pct, float rake_cap, bool no_flop_no_drop, GameTreeNode::GameRound round) {
    if (rake_pct <= 0.0f) return 0.0f;
    if (no_flop_no_drop && round == GameTreeNode::GameRound::PREFLOP) return 0.0f;

    float rake = pot * rake_pct;
    if (rake_cap > 0.0f && rake > rake_cap) rake = rake_cap;
    if (rake < 0.0f) rake = 0.0f;
    return rake;
}
// 四舍五入到 3 位小数；把极小的 -0.000、1.000 规整一下，避免打印出奇怪尾数
static inline double r3(double x) {
    double y = std::floor(x * 1000.0 + 0.5) / 1000.0;  // 四舍五入到千分位
    if (std::fabs(y) < 0.0005) y = 0.0;
    if (std::fabs(1.0 - y) < 0.0005) y = 1.0;
    return y;
}
std::array<int,4> PCfrSolver::get_color_iso_offset_row(int deal) const {
    std::array<int,4> row{0,0,0,0};
    if (deal < 0 || deal >= 52 * 52 * 2) return row;
    for (int i = 0; i < 4; ++i) row[i] = this->color_iso_offset[deal][i];
    return row;
}
static inline void round_json_float_array3(nlohmann::json& arr) {
    if (!arr.is_array()) return;
    for (auto& v : arr) {
        if (v.is_number()) {
            // 用 double 存回去（nlohmann::json 输出会选用短格式）
            v = r3(v.get<double>());
        }
    }
}

// 针对 dump_strategy(false) 结构：obj["strategy"]["strategy"][hand] 是一个数组
static inline void round_strategy_block3(nlohmann::json& block,
                                         const char* key_level1,
                                         const char* key_level2) {
    if (!block.contains(key_level1)) return;
    nlohmann::json& lv1 = block[key_level1];
    if (!lv1.contains(key_level2)) return;
    nlohmann::json& mp = lv1[key_level2];   // hand -> [p1, p2, ...]
    if (!mp.is_object()) return;
    for (auto it = mp.begin(); it != mp.end(); ++it) {
        round_json_float_array3(it.value());
        // （可选）若和为 1 附近，归一化再保留三位
        // double s = 0.0; for (auto& x : it.value()) s += x.get<double>();
        // if (s > 0.0 && std::fabs(s - 1.0) < 0.005)
        //   for (auto& x : it.value()) x = r3(x.get<double>() / s);
    }
}
//#define DEBUG;

PCfrSolver::~PCfrSolver(){
    //cout << "Pcfr destroyed" << endl;
}

PCfrSolver::PCfrSolver(shared_ptr<GameTree> tree, vector<PrivateCards> range1, vector<PrivateCards> range2,
                     vector<int> initial_board, shared_ptr<Compairer> compairer, Deck deck, int iteration_number, bool debug,
                     int print_interval, string logfile, string trainer, Solver::MonteCarolAlg monteCarolAlg,int warmup,float accuracy,bool use_isomorphism,int use_halffloats,int num_threads,int exploitability_interval,bool collect_evs) :Solver(tree){
    this->initial_board = initial_board;
    this->initial_board_long = Card::boardInts2long(initial_board);
    this->logfile = logfile;
    this->trainer = trainer;
    this->warmup = warmup;
    this->rake_pct_ = 0.0f;
    this->rake_cap_ = 0.0f;
    this->no_flop_no_drop_ = true;
    if (this->tree != nullptr) {
        this->rake_pct_ = this->tree->get_rake_pct();
        this->rake_cap_ = this->tree->get_rake_cap();
        this->no_flop_no_drop_ = this->tree->get_no_flop_no_drop();
    }

    range1 = this->noDuplicateRange(range1,initial_board_long);
    range2 = this->noDuplicateRange(range2,initial_board_long);

    this->range1 = range1;
    this->range2 = range2;
    this->player_number = 2;
    this->ranges = vector<vector<PrivateCards>>(this->player_number);
    this->ranges[0] = range1;
    this->ranges[1] = range2;

    this->compairer = compairer;

    this->deck = deck;
    this->use_isomorphism = use_isomorphism;
    this->use_halffloats = use_halffloats;

    this->rrm = RiverRangeManager(compairer);
#if !TEXASSOLVER_OPT_RIVER_LAZY_CACHE
    this->rrm.preloadRiverCombos(this->ranges[0], this->ranges[1], this->deck.getCards(), this->initial_board_long);
#endif
    this->iteration_number = iteration_number;

    vector<vector<PrivateCards>> private_cards(this->player_number);
    private_cards[0] = range1;
    private_cards[1] = range2;
    pcm = PrivateCardsManager(private_cards,this->player_number,Card::boardInts2long(this->initial_board));
#if TEXASSOLVER_OPT_TERMINAL_SAME_CARD_CACHE
    this->same_card_index = vector<vector<vector<int>>>(this->player_number, vector<vector<int>>(this->player_number));
    for (int from_player = 0; from_player < this->player_number; ++from_player) {
        for (int to_player = 0; to_player < this->player_number; ++to_player) {
            vector<int>& index_map = this->same_card_index[from_player][to_player];
            index_map.resize(this->ranges[from_player].size());
            for (std::size_t hand_id = 0; hand_id < this->ranges[from_player].size(); ++hand_id) {
                index_map[hand_id] = this->pcm.indPlayer2Player(from_player, to_player, (int)hand_id);
            }
        }
    }
#endif
#if TEXASSOLVER_OPT_COLOR_EXCHANGE_CACHE
    this->buildColorExchangeCache();
#endif
    this->debug = debug;
    this->print_interval = print_interval;
    this->exploitability_interval = exploitability_interval < 0 ? print_interval : exploitability_interval;
    this->collect_evs = collect_evs;
    this->monteCarolAlg = monteCarolAlg;
    this->accuracy = accuracy;
    if(num_threads == -1){
        num_threads = omp_get_num_procs();
    }
    qDebug().noquote() << QString::fromStdString(tfm::format(QObject::tr("Using %s threads").toStdString().c_str(),num_threads));
    this->num_threads = num_threads;
    this->distributing_task = false;
    omp_set_num_threads(this->num_threads);
    setTrainable(this->tree->getRoot());
    this->root_round = this->tree->getRoot()->getRound();
    if(this->root_round == GameTreeNode::GameRound::PREFLOP){
        this->split_round = GameTreeNode::GameRound::FLOP;
    }else if(this->root_round == GameTreeNode::GameRound::FLOP){
        this->split_round = GameTreeNode::GameRound::TURN;
    }else if(this->root_round == GameTreeNode::GameRound::TURN){
        this->split_round = GameTreeNode::GameRound::RIVER;
    }else{
        // do not use multithread in river, really not necessary
        this->split_round = GameTreeNode::GameRound::PREFLOP;
    }
}

#if TEXASSOLVER_OPT_COLOR_EXCHANGE_CACHE
void PCfrSolver::buildColorExchangeCache() {
    this->color_exchange_pairs.clear();
    this->color_exchange_pairs.resize(this->player_number);

    for (int player = 0; player < this->player_number; ++player) {
        const vector<PrivateCards>& range = this->pcm.getPreflopCards(player);
        for (int rank1 = 0; rank1 < 4; ++rank1) {
            for (int rank2 = 0; rank2 < 4; ++rank2) {
                vector<pair<int, int>>& pairs = this->color_exchange_pairs[player][rank1][rank2];
                pairs.clear();
                if (rank1 == rank2 || range.empty()) {
                    continue;
                }

                vector<int> self_ind(range.size());
                int privateint2ind[52 * 52 * 2] = {0};

                for (std::size_t i = 0; i < range.size(); i++) {
                    const PrivateCards& pc = range[i];
                    int card1 = pc.card1;
                    int card2 = pc.card2;
                    if(card1 > card2){
                        int tmp = card1;
                        card1 = card2;
                        card2 = tmp;
                    }
                    self_ind[i] = card1 * 52 + card2;

                    if(card1 % 4 == rank1) card1 = card1 - rank1 + rank2;
                    else if(card1 % 4 == rank2) card1 = card1 - rank2 + rank1;

                    if(card2 % 4 == rank1) card2 = card2 - rank1 + rank2;
                    else if(card2 % 4 == rank2) card2 = card2 - rank2 + rank1;

                    if(card1 > card2){
                        int tmp = card1;
                        card1 = card2;
                        card2 = tmp;
                    }
                    privateint2ind[card1 * 52 + card2] = static_cast<int>(i);
                }

                for (std::size_t i = 0; i < range.size(); i++) {
                    if(self_ind[i] == -1) continue;
                    std::size_t ind = privateint2ind[self_ind[i]];
                    if(ind != i){
                        self_ind[ind] = -1;
                        pairs.emplace_back(static_cast<int>(i), static_cast<int>(ind));
                    }
                }
            }
        }
    }
}

void PCfrSolver::exchangeColorWithPairs(vector<float>& value, const vector<pair<int, int>>& pairs) {
    if(value.empty()) return;
    for (const pair<int, int>& one_pair : pairs) {
        std::swap(value[one_pair.first], value[one_pair.second]);
    }
}
#endif

const vector<PrivateCards> &PCfrSolver::playerHands(int player) {
    if(player == 0){
        return range1;
    }else if (player == 1){
        return range2;
    }else{
        throw runtime_error("player not found");
    }
}

vector<vector<float>> PCfrSolver::getReachProbs() {
    vector<vector<float>> retval(this->player_number);
    for(int player = 0;player < this->player_number;player ++){
        vector<PrivateCards> player_cards = this->playerHands(player);
        vector<float> reach_prob(player_cards.size());
        for(std::size_t i = 0;i < player_cards.size();i ++){
            reach_prob[i] = player_cards[i].weight;
        }
        retval[player] = reach_prob;
    }
    return retval;
}

vector<PrivateCards>
PCfrSolver::noDuplicateRange(const vector<PrivateCards> &private_range, uint64_t board_long) {
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

void PCfrSolver::setTrainable(shared_ptr<GameTreeNode> root) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrSetTrainable);
    if(root->getType() == GameTreeNode::ACTION){
        shared_ptr<ActionNode> action_node = std::dynamic_pointer_cast<ActionNode>(root);

        int player = action_node->getPlayer();

        if(this->trainer == "cfr_plus"){
            //vector<PrivateCards> player_privates = this->ranges[player];
            //action_node->setTrainable(make_shared<CfrPlusTrainable>(action_node,player_privates));
            throw runtime_error(tfm::format("trainer %s not supported",this->trainer));
        }else if(this->trainer == "discounted_cfr"){
            vector<PrivateCards>* player_privates = &this->ranges[player];
            //action_node->setTrainable(make_shared<DiscountedCfrTrainable>(action_node,player_privates));
            int num;
            GameTreeNode::GameRound gr = this->tree->getRoot()->getRound();
            int root_round = GameTreeNode::gameRound2int(gr);
            int current_round = GameTreeNode::gameRound2int(root->getRound());
            int gap = current_round - root_round;

            if(gap == 2) {
                num = this->deck.getCards().size() * this->deck.getCards().size() +
                          this->deck.getCards().size() + 1;
            }else if(gap == 1) {
                num = this->deck.getCards().size() + 1;
            }else if(gap == 0) {
                num =  1;
            }else{
                throw runtime_error("gap not understand");
            }
            action_node->setTrainable(vector<shared_ptr<Trainable>>(num),player_privates);
        }else{
            throw runtime_error(tfm::format("trainer %s not found",this->trainer));
        }

        vector<shared_ptr<GameTreeNode>> childrens =  action_node->getChildrens();
        for(shared_ptr<GameTreeNode> one_child:childrens) setTrainable(one_child);
    }else if(root->getType() == GameTreeNode::CHANCE) {
        shared_ptr<ChanceNode> chance_node = std::dynamic_pointer_cast<ChanceNode>(root);
        shared_ptr<GameTreeNode> children = chance_node->getChildren();
        setTrainable(children);
    }
    else if(root->getType() == GameTreeNode::TERMINAL){

    }else if(root->getType() == GameTreeNode::SHOWDOWN){

    }
}

vector<int> PCfrSolver::getAllAbstractionDeal(int deal){
    vector<int> all_deal;
    int card_num = this->deck.getCards().size();
    if(deal == 0){
        all_deal.push_back(deal);
    } else if (deal > 0 && deal <= card_num){
        int origin_deal = int((deal - 1) / 4) * 4;
        for(int i = 0;i < 4;i ++){
            int one_card = origin_deal + i + 1;

            Card *first_card = const_cast<Card *>(&(this->deck.getCards()[origin_deal + i]));
            uint64_t first_long = Card::boardInt2long(
                    first_card->getCardInt());
            if (Card::boardsHasIntercept(first_long, this->initial_board_long))continue;
            all_deal.push_back(one_card);
        }
    } else{
        //cout << "______________________" << endl;
        int c_deal = deal - (1 + card_num);
        int first_deal = int((c_deal / card_num) / 4) * 4;
        int second_deal = int((c_deal % card_num) / 4) * 4;

        for(int i = 0;i < 4;i ++) {
            for(int j = 0;j < 4;j ++) {
                if(first_deal == second_deal && i == j) continue;

                Card *first_card = const_cast<Card *>(&(this->deck.getCards()[first_deal + i]));
                uint64_t first_long = Card::boardInt2long(
                        first_card->getCardInt());
                if (Card::boardsHasIntercept(first_long, this->initial_board_long))continue;

                Card *second_card = const_cast<Card *>(&(this->deck.getCards()[second_deal + j]));
                uint64_t second_long = Card::boardInt2long(
                        second_card->getCardInt());
                if (Card::boardsHasIntercept(second_long, this->initial_board_long))continue;

                int one_card = card_num * (first_deal + i) + (second_deal + j) + 1 + card_num;
                //cout << ";" << this->deck.getCards()[first_deal + i].toString() << "," << this->deck.getCards()[second_deal + j].toString();
                all_deal.push_back(one_card);
            }
        }
        //cout << endl;
    }
    return all_deal;
}

vector<float> PCfrSolver::cfr(int player, shared_ptr<GameTreeNode> node, const vector<float> &reach_probs, int iter,
                                    uint64_t current_board,int deal) {
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
    vector<float> out;
    this->cfrInto(player, node, reach_probs, iter, current_board, deal, out);
    return out;
#else
    switch(node->getType()) {
        case GameTreeNode::ACTION: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ActionNode> action_node = std::static_pointer_cast<ActionNode>(node);
#else
            shared_ptr<ActionNode> action_node = std::dynamic_pointer_cast<ActionNode>(node);
#endif
            return actionUtility(player, action_node, reach_probs, iter, current_board,deal);
        }case GameTreeNode::SHOWDOWN: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ShowdownNode> showdown_node = std::static_pointer_cast<ShowdownNode>(node);
#else
            shared_ptr<ShowdownNode> showdown_node = std::dynamic_pointer_cast<ShowdownNode>(node);
#endif
            return showdownUtility(player, showdown_node, reach_probs, iter, current_board,deal);
        }case GameTreeNode::TERMINAL: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<TerminalNode> terminal_node = std::static_pointer_cast<TerminalNode>(node);
#else
            shared_ptr<TerminalNode> terminal_node = std::dynamic_pointer_cast<TerminalNode>(node);
#endif
            return terminalUtility(player, terminal_node, reach_probs, iter, current_board,deal);
        }case GameTreeNode::CHANCE: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ChanceNode> chance_node = std::static_pointer_cast<ChanceNode>(node);
#else
            shared_ptr<ChanceNode> chance_node = std::dynamic_pointer_cast<ChanceNode>(node);
#endif
            return chanceUtility(player, chance_node, reach_probs, iter, current_board,deal);
        }default:
            throw runtime_error("node type unknown");
    }
#endif
}

void PCfrSolver::cfrInto(int player, const shared_ptr<GameTreeNode>& node, const vector<float> &reach_probs, int iter,
                                    uint64_t current_board,int deal, vector<float>& out) {
    switch(node->getType()) {
        case GameTreeNode::ACTION: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ActionNode> action_node = std::static_pointer_cast<ActionNode>(node);
#else
            shared_ptr<ActionNode> action_node = std::dynamic_pointer_cast<ActionNode>(node);
#endif
            this->actionUtilityInto(player, action_node, reach_probs, iter, current_board,deal, out);
            return;
        }case GameTreeNode::SHOWDOWN: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ShowdownNode> showdown_node = std::static_pointer_cast<ShowdownNode>(node);
#else
            shared_ptr<ShowdownNode> showdown_node = std::dynamic_pointer_cast<ShowdownNode>(node);
#endif
            this->showdownUtilityInto(player, showdown_node, reach_probs, iter, current_board,deal, out);
            return;
        }case GameTreeNode::TERMINAL: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<TerminalNode> terminal_node = std::static_pointer_cast<TerminalNode>(node);
#else
            shared_ptr<TerminalNode> terminal_node = std::dynamic_pointer_cast<TerminalNode>(node);
#endif
            this->terminalUtilityInto(player, terminal_node, reach_probs, iter, current_board,deal, out);
            return;
        }case GameTreeNode::CHANCE: {
#if TEXASSOLVER_OPT_STATIC_NODE_CAST
            shared_ptr<ChanceNode> chance_node = std::static_pointer_cast<ChanceNode>(node);
#else
            shared_ptr<ChanceNode> chance_node = std::dynamic_pointer_cast<ChanceNode>(node);
#endif
            this->chanceUtilityInto(player, chance_node, reach_probs, iter, current_board,deal, out);
            return;
        }default:
            throw runtime_error("node type unknown");
    }
}

vector<float>
PCfrSolver::chanceUtility(int player, shared_ptr<ChanceNode> node, const vector<float> &reach_probs, int iter,
                         uint64_t current_board,int deal) {
    vector<float> out;
    this->chanceUtilityInto(player, node, reach_probs, iter, current_board, deal, out);
    return out;
}

void PCfrSolver::chanceUtilityInto(int player, const shared_ptr<ChanceNode>& node, const vector<float> &reach_probs, int iter,
                         uint64_t current_board,int deal, vector<float>& chance_utility) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrChanceUtility);
    vector<Card>& cards = this->deck.getCards();
    //float[] cardWeights = getCardsWeights(player,reach_probs[1 - player],current_board);

    int card_num = node->getCards().size();
    if(card_num % 4 != 0) throw runtime_error("card num cannot round 4");
    // 可能的发牌情况,2代表每个人的holecard是两张
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
    int possible_deals = (int)node->getCards().size() - Card::boardLongCardCount(current_board) - 2;
#else
    int possible_deals = node->getCards().size() - Card::long2board(current_board).size() - 2;
#endif
    int oppo = 1 - player;

    //vector<float> chance_utility(reach_probs[player].size());
    chance_utility.assign(this->ranges[player].size(), 0.0f);

    int random_deal = 0;
    if(this->monteCarolAlg==MonteCarolAlg::PUBLIC) {
        if (this->round_deal[GameTreeNode::gameRound2int(node->getRound())] == -1) {
            random_deal = random(1, possible_deals + 1 + 2);
            this->round_deal[GameTreeNode::gameRound2int(node->getRound())] = random_deal;
        } else {
            random_deal = this->round_deal[GameTreeNode::gameRound2int(node->getRound())];
        }
    }
    //vector<vector<vector<float>>> arr_new_reach_probs = vector<vector<vector<float>>>(node->getCards().size());

    vector<vector<float>> results(node->getCards().size());
    //fill(results.begin(),results.end(),nullptr);

    vector<float> multiplier;
    if(iter <= this->warmup){
        multiplier = vector<float>(card_num);
        fill(multiplier.begin(), multiplier.end(), 0);
        for (int card_base = 0; card_base < card_num / 4; card_base++) {
            int cardr = std::rand() % 4;
            int card_target = card_base * 4 + cardr;
            int multiplier_num = 0;
            for (int i = 0; i < 4; i++) {
                int i_card = card_base * 4 + i;
                if (i == cardr) {
                    Card *one_card = const_cast<Card *>(&(node->getCards()[i_card]));
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
                    uint64_t card_long = Card::boardInt2longUnchecked(one_card->getCardInt());
#else
                    uint64_t card_long = Card::boardInt2long(
                            one_card->getCardInt());
#endif
                    if (!Card::boardsHasIntercept(card_long, current_board)) {
                        multiplier_num += 1;
                    }
                } else {
                    Card *one_card = const_cast<Card *>(&(node->getCards()[i_card]));
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
                    uint64_t card_long = Card::boardInt2longUnchecked(one_card->getCardInt());
#else
                    uint64_t card_long = Card::boardInt2long(
                            one_card->getCardInt());
#endif
                    if (!Card::boardsHasIntercept(card_long, current_board)) {
                        multiplier_num += 1;
                    }
                }
            }
            multiplier[card_target] = multiplier_num;
        }
    }

    vector<int> valid_cards;
    valid_cards.reserve(node->getCards().size());

    for(std::size_t card = 0;card < node->getCards().size();card ++) {
        shared_ptr<GameTreeNode> one_child = node->getChildren();
        Card *one_card = const_cast<Card *>(&(node->getCards()[card]));
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
        uint64_t card_long = Card::boardInt2longUnchecked(one_card->getCardInt());
#else
        uint64_t card_long = Card::boardInt2long(one_card->getCardInt());//Card::boardCards2long(new Card[]{one_card});
#endif
        if (Card::boardsHasIntercept(card_long, current_board)) continue;
        if (iter <= this->warmup && multiplier[card] == 0) continue;
        if (this->color_iso_offset[deal][one_card->getCardInt() % 4] < 0) continue;
        valid_cards.push_back(card);
    }

#if TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
    #pragma omp parallel
    {
        vector<float> new_reach_probs(this->ranges[oppo].size());
        #pragma omp for schedule(static)
#else
    #pragma omp parallel for schedule(static)
#endif
    for(std::size_t valid_ind = 0;valid_ind < valid_cards.size();valid_ind++) {
        int card = valid_cards[valid_ind];
        shared_ptr<GameTreeNode> one_child = node->getChildren();
        Card *one_card = const_cast<Card *>(&(node->getCards()[card]));
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
        uint64_t card_long = Card::boardInt2longUnchecked(one_card->getCardInt());
#else
        uint64_t card_long = Card::boardInt2long(one_card->getCardInt());//Card::boardCards2long(new Card[]{one_card});
#endif

        uint64_t new_board_long = current_board | card_long;
        if (this->monteCarolAlg == MonteCarolAlg::PUBLIC) {
            throw runtime_error("parallel solver don't support public monte carol");
        }

        //cout << "Card deal:" << one_card->toString() << endl;

        vector<PrivateCards> &playerPrivateCard = (this->ranges[player]);
        vector<PrivateCards> &oppoPrivateCards = (this->ranges[1 - player]);

#if !TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
        vector<float> new_reach_probs = vector<float>(oppoPrivateCards.size());
#endif

#ifdef DEBUG
        if (playerPrivateCard.size() != this->ranges[player].size()) throw runtime_error("length not match");
        if (oppoPrivateCards.size() != this->ranges[1 - player].size()) throw runtime_error("length not match");
#endif

        int player_hand_len = this->ranges[oppo].size();
        for (int player_hand = 0; player_hand < player_hand_len; player_hand++) {
            PrivateCards &one_private = this->ranges[oppo][player_hand];
            uint64_t privateBoardLong = one_private.toBoardLong();
            if (Card::boardsHasIntercept(card_long, privateBoardLong)) {
                new_reach_probs[player_hand] = 0;
                continue;
            }
            new_reach_probs[player_hand] = reach_probs[player_hand] / possible_deals;
        }
#ifdef DEBUG
        if (Card::boardsHasIntercept(current_board, card_long))
            throw runtime_error("board has intercept with dealt card");
#endif

        int new_deal;
        if(deal == 0){
            new_deal = card + 1;
        } else if (deal > 0 && deal <= card_num){
            int origin_deal = deal - 1;

#ifdef DEBUG
            if(origin_deal == card) throw runtime_error("deal should not be equal");
#endif
            new_deal = card_num * origin_deal + card;
            new_deal += (1 + card_num);
        } else{
            throw runtime_error(tfm::format("deal out of range : %s ",deal));
        }
        if(this->distributing_task && node->getRound() == this->split_round) {
            results[one_card->getNumberInDeckInt()] = vector<float>(this->ranges[player].size());
            //TaskParams taskParams = TaskParams();
        }else {
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
            vector<float>& child_utility = results[one_card->getNumberInDeckInt()];
            this->cfrInto(player, one_child, new_reach_probs, iter, new_board_long, new_deal, child_utility);
#else
            vector<float> child_utility = this->cfr(player, one_child, new_reach_probs, iter, new_board_long, new_deal);
#if TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
            results[one_card->getNumberInDeckInt()] = std::move(child_utility);
#else
            results[one_card->getNumberInDeckInt()] = child_utility;
#endif
#endif
        }
    }
#if TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
    }
#endif

#if TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
    vector<float> exchanged_child_utility;
#endif
    for(std::size_t card = 0;card < node->getCards().size();card ++) {
        Card *one_card = const_cast<Card *>(&(node->getCards()[card]));
#if !TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE
        vector<float> exchanged_child_utility;
#endif
        const vector<float>* child_utility_ptr;
        int offset = this->color_iso_offset[deal][one_card->getCardInt() % 4];
        if(offset < 0) {
            int rank1 = one_card->getCardInt() % 4;
            int rank2 = rank1 + offset;
#ifdef DEBUG
            if(rank2 < 0) throw runtime_error("rank error");
#endif
            exchanged_child_utility = results[one_card->getNumberInDeckInt() + offset];
#if TEXASSOLVER_OPT_COLOR_EXCHANGE_CACHE
            exchangeColorWithPairs(exchanged_child_utility, this->color_exchange_pairs[player][rank1][rank2]);
#else
            exchange_color(exchanged_child_utility,this->pcm.getPreflopCards(player),rank1,rank2);
#endif
            child_utility_ptr = &exchanged_child_utility;
        }else{
            child_utility_ptr = &results[one_card->getNumberInDeckInt()];
        }
        const vector<float>& child_utility = *child_utility_ptr;
        if(child_utility.empty())
            continue;

#ifdef DEBUG
        if(child_utility.size() != chance_utility.size()) throw runtime_error("length not match");
#endif
        if(iter > this->warmup) {
            for (std::size_t i = 0; i < child_utility.size(); i++)
                chance_utility[i] += child_utility[i];
        }else{
            for (std::size_t i = 0; i < child_utility.size(); i++)
                chance_utility[i] += child_utility[i] * multiplier[card];
        }
    }

#ifdef DEBUG
    if(this->monteCarolAlg == MonteCarolAlg::PUBLIC) {
        throw runtime_error("not possible");
    }
    if(chance_utility.size() != this->ranges[player].size()) {
        throw runtime_error("size problems");
    }
#endif
    return;
}

vector<float>
PCfrSolver::actionUtility(int player, shared_ptr<ActionNode> node, const vector<float> &reach_probs, int iter,
                         uint64_t current_board,int deal) {
    vector<float> out;
    this->actionUtilityInto(player, node, reach_probs, iter, current_board, deal, out);
    return out;
}

void PCfrSolver::actionUtilityInto(int player, const shared_ptr<ActionNode>& node, const vector<float> &reach_probs, int iter,
                         uint64_t current_board,int deal, vector<float>& payoffs) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionUtility);
    int oppo = 1 - player;
    const vector<PrivateCards>& node_player_private_cards = this->ranges[node->getPlayer()];

    payoffs.assign(this->ranges[player].size(), 0.0f);
    vector<shared_ptr<GameTreeNode>>& children =  node->getChildrens();
    vector<GameActions>& actions =  node->getActions();

    shared_ptr<Trainable> trainable;

    /*
    if(iter <= this->warmup){
        vector<int> deals = this->getAllAbstractionDeal(deal);
        trainable = node->getTrainable(deals[0]);
    }else{
        trainable = node->getTrainable(deal);
    }
     */
    trainable = node->getTrainable(deal,true,this->use_halffloats);

#ifdef DEBUG
    if(trainable == nullptr){
        throw runtime_error("null trainable");
    }
#endif

    int node_player = node->getPlayer();

    if (node_player != player) {
        vector<float> new_reach_prob(reach_probs.size());
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER && !TEXASSOLVER_OPT_ACTION_STRATEGY_FUSED_LOOPS
        vector<float> action_strategy(reach_probs.size());
#endif
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
        vector<float> action_utilities;
#endif
        for (std::size_t action_id = 0; action_id < actions.size(); action_id++) {
            {
                TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionOpponentReach);
#if TEXASSOLVER_OPT_ACTION_STRATEGY_FUSED_LOOPS
                trainable->fillReachProbsForAction((int)action_id, reach_probs, new_reach_prob);
#else
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER
                trainable->fillCurrentStrategyForAction((int)action_id, action_strategy);
#endif
                for (std::size_t hand_id = 0; hand_id < new_reach_prob.size(); hand_id++) {
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER
                    float strategy_prob = action_strategy[hand_id];
#else
                    float strategy_prob = trainable->getCurrentStrategy(action_id, hand_id);
#endif
                    new_reach_prob[hand_id] = reach_probs[hand_id] * strategy_prob;
                }
#endif
            }
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
            {
                TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionOpponentChild);
                this->cfrInto(player, children[action_id], new_reach_prob, iter, current_board, deal, action_utilities);
            }
#else
            vector<float> action_utilities;
            {
                TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionOpponentChild);
                action_utilities = this->cfr(player, children[action_id], new_reach_prob, iter,
                                             current_board,deal);
            }
#endif
            if(action_utilities.empty()){
                continue;
            }

            {
                TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionOpponentPayoff);
            // cfr缁撴灉鏄瘡鎵嬬墝鐨勬敹鐩婏紝payoffs浠ｈ〃鐨勪篃鏄瘡鎵嬬墝鐨勬敹鐩婏紝浠栦滑鐨勯暱搴︾悊搴旂浉绛?#ifdef DEBUG
#ifdef DEBUG
            if (action_utilities.size() != payoffs.size()) {
                cout << ("errmsg") << endl;
                cout << (tfm::format("node player %s ", node->getPlayer())) << endl;
                node->printHistory();
                throw runtime_error(
                        tfm::format(
                                "action and payoff length not match %s - %s", action_utilities.size(),
                                payoffs.size()
                        )
                );
            }
#endif

            for (std::size_t hand_id = 0; hand_id < action_utilities.size(); hand_id++) {
                payoffs[hand_id] += action_utilities[hand_id];
            }
            }
        }
        return;
    }

    vector<vector<float>> results(actions.size());
    {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionPlayerChildren);
        for (std::size_t action_id = 0; action_id < actions.size(); action_id++) {
            //#pragma omp task shared(results,action_id)
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
            this->cfrInto(player, children[action_id], reach_probs, iter, current_board, deal, results[action_id]);
#else
            results[action_id] = this->cfr(player, children[action_id], reach_probs, iter,
                                           current_board,deal);
#endif
        }
    }

    //#pragma omp taskwait
    {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionPlayerPayoff);
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER && !TEXASSOLVER_OPT_ACTION_STRATEGY_FUSED_LOOPS
        vector<float> action_strategy(node_player_private_cards.size());
#endif
        for (std::size_t action_id = 0; action_id < actions.size(); action_id++) {
            const vector<float>& action_utilities = results[action_id];
            if(action_utilities.empty()){
                continue;
            }
#if !TEXASSOLVER_OPT_ACTION_STRATEGY_FUSED_LOOPS
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER
            trainable->fillCurrentStrategyForAction((int)action_id, action_strategy);
#endif
#endif

        // cfr结果是每手牌的收益，payoffs代表的也是每手牌的收益，他们的长度理应相等
#ifdef DEBUG
            if (action_utilities.size() != payoffs.size()) {
                cout << ("errmsg") << endl;
                cout << (tfm::format("node player %s ", node->getPlayer())) << endl;
                node->printHistory();
                throw runtime_error(
                        tfm::format(
                                "action and payoff length not match %s - %s", action_utilities.size(),
                                payoffs.size()
                        )
                );
            }
#endif

#if TEXASSOLVER_OPT_ACTION_STRATEGY_FUSED_LOOPS
            trainable->accumulateStrategyWeightedActionUtility((int)action_id, action_utilities, payoffs);
#else
            for (std::size_t hand_id = 0; hand_id < action_utilities.size(); hand_id++) {
#if TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER
                float strategy_prob = action_strategy[hand_id];
#else
                float strategy_prob = trainable->getCurrentStrategy(action_id, hand_id);
#endif
                payoffs[hand_id] += strategy_prob * action_utilities[hand_id];
            }
#endif
        }
    }


#if TEXASSOLVER_OPT_ACTION_REGRET_DIRECT_UPDATE
    {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionPlayerRegret);
        if(!this->distributing_task && !this->collecting_statics) {
            if (iter > this->warmup) {
                trainable->updateRegretsFromActionUtilities(results, payoffs, iter + 1, reach_probs);
            } else {
                // iter == this->warmup
                vector<int> deals = this->getAllAbstractionDeal(deal);
                shared_ptr<Trainable> standard_trainable = nullptr;
                for (int one_deal : deals) {
                    shared_ptr<Trainable> one_trainable = node->getTrainable(one_deal,true,this->use_halffloats);
                    if (standard_trainable == nullptr) {
                        one_trainable->updateRegretsFromActionUtilities(results, payoffs, iter + 1, reach_probs);
                        standard_trainable = one_trainable;
                    } else {
                        one_trainable->copyStrategy(standard_trainable);
                    }
                }
            }
        }
    }
#else
    {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionPlayerRegret);
    vector<float> regrets(actions.size() * node_player_private_cards.size());
    for (std::size_t i = 0; i < node_player_private_cards.size(); i++) {
        //boolean regrets_all_negative = true;
        for (std::size_t action_id = 0; action_id < actions.size(); action_id++) {
            // 下面是regret计算的伪代码
            // regret[action_id * player_hc: (action_id + 1) * player_hc]
            //     = all_action_utilitiy[action_id] - payoff[action_id]
            regrets[action_id * node_player_private_cards.size() + i] =
                    results[action_id][i] - payoffs[i];
        }
    }

    if(!this->distributing_task && !this->collecting_statics) {
        if (iter > this->warmup) {
            trainable->updateRegrets(regrets, iter + 1, reach_probs);
        }/*else if(iter < this->warmup){
            vector<int> deals = this->getAllAbstractionDeal(deal);
            shared_ptr<Trainable> one_trainable = node->getTrainable(deals[0]);
            one_trainable->updateRegrets(regrets, iter + 1, reach_probs[player]);
            }*/
        else {
            // iter == this->warmup
            vector<int> deals = this->getAllAbstractionDeal(deal);
            shared_ptr<Trainable> standard_trainable = nullptr;
            for (int one_deal : deals) {
                shared_ptr<Trainable> one_trainable = node->getTrainable(one_deal,true,this->use_halffloats);
                if (standard_trainable == nullptr) {
                    one_trainable->updateRegrets(regrets, iter + 1, reach_probs);
                    standard_trainable = one_trainable;
                } else {
                    one_trainable->copyStrategy(standard_trainable);
                }
            }
        }
    }
    }
#endif

        if(this->collect_evs && (this->collecting_statics || (iter % this->print_interval == 0))){
            TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrActionCollectEvs);
            float oppo_sum = 0;
            vector<float> oppo_card_sum = vector<float> (52);
            fill(oppo_card_sum.begin(),oppo_card_sum.end(),0);

            const vector<PrivateCards>& oppo_hand = playerHands(oppo);
            for(std::size_t i = 0;i < oppo_hand.size();i ++){
                oppo_card_sum[oppo_hand[i].card1] += reach_probs[i];
                oppo_card_sum[oppo_hand[i].card2] += reach_probs[i];
                oppo_sum += reach_probs[i];
            }

            const vector<PrivateCards>& player_hand = playerHands(player);
            vector<float> evs(actions.size() * node_player_private_cards.size(),0.0);
            for (std::size_t action_id = 0; action_id < actions.size(); action_id++) {
                for (std::size_t hand_id = 0; hand_id < node_player_private_cards.size(); hand_id++) {
                    float one_ev = results[action_id][hand_id];//current_strategy; //[hand_id + action_id * node_player_private_cards.size()];

                    int oppo_same_card_ind = this->pcm.indPlayer2Player(player,oppo,hand_id);
                    float plus_reach_prob;

                    const PrivateCards& one_player_hand = player_hand[hand_id];
                    if(oppo_same_card_ind == -1){
                        plus_reach_prob = 0;
                    }else{
                        plus_reach_prob = reach_probs[oppo_same_card_ind];
                    }

                    float rp_sum = (
                        oppo_sum - oppo_card_sum[one_player_hand.card1]
                        - oppo_card_sum[one_player_hand.card2]
                        + plus_reach_prob);

                    evs[hand_id + action_id * node_player_private_cards.size()] = one_ev / rp_sum;
                }
            }
            // TODO if evs contains nan what should we do?
            trainable->setEv(evs);
        }

    return;

}

vector<float>
PCfrSolver::showdownUtility(int player, shared_ptr<ShowdownNode> node, const vector<float> &reach_probs,
                                    int iter, uint64_t current_board,int deal) {
    vector<float> out;
    this->showdownUtilityInto(player, node, reach_probs, iter, current_board, deal, out);
    return out;
}

void PCfrSolver::showdownUtilityInto(int player, const shared_ptr<ShowdownNode>& node, const vector<float> &reach_probs,
                                    int iter, uint64_t current_board,int deal, vector<float>& payoffs) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrShowdownUtility);

    int oppo = 1 - player;

    // 原有 payoff（无抽水）
    float win_payoff  = node->get_payoffs(ShowdownNode::ShowDownResult::NOTTIE, player, player);
    float lose_payoff = node->get_payoffs(ShowdownNode::ShowDownResult::NOTTIE, oppo,  player);

    // === rake adjust ===
    const float pot = (float)node->getPot();
    const float rake = calc_rake_amount(pot, this->rake_pct_, this->rake_cap_, this->no_flop_no_drop_, node->getRound());

    // winner 扣 rake；loser 不额外扣（rake 从底池抽走）
    const float win_payoff_adj = win_payoff - rake;

    // tie：原来 tie payoff 基本是 0（因为双方 commit 相等），抽水后应为 -rake/2
    const float tie_payoff_adj = -0.5f * rake;

    const vector<PrivateCards>& player_private_cards = this->ranges[player];
    const vector<PrivateCards>& oppo_private_cards   = this->ranges[oppo];

    const vector<RiverCombs>& player_combs = this->rrm.getRiverCombos(player,player_private_cards,current_board);
    const vector<RiverCombs>& oppo_combs   = this->rrm.getRiverCombos(oppo,oppo_private_cards,current_board);

    payoffs.assign(player_private_cards.size(), 0.0f);

    // ===== win part（保持你原有逻辑，但乘 win_payoff_adj）=====
    float winsum = 0.0f;
    std::array<float, 52> card_winsum{};

#if TEXASSOLVER_OPT_SHOWDOWN_LOSS_FROM_TOTAL
    float oppo_total_sum = 0.0f;
    std::array<float, 52> oppo_total_card_sum{};
    for (const RiverCombs& one_oppo_comb : oppo_combs) {
#if TEXASSOLVER_OPT_SHOWDOWN_FAST_FIELDS
        const int oppo_reach_index = one_oppo_comb.reach_prob_index;
        const float oppo_reach = reach_probs[oppo_reach_index];
        oppo_total_sum += oppo_reach;
        oppo_total_card_sum[one_oppo_comb.card1] += oppo_reach;
        oppo_total_card_sum[one_oppo_comb.card2] += oppo_reach;
#else
        oppo_total_sum += reach_probs[one_oppo_comb.reach_prob_index];
        oppo_total_card_sum[one_oppo_comb.card1] += reach_probs[one_oppo_comb.reach_prob_index];
        oppo_total_card_sum[one_oppo_comb.card2] += reach_probs[one_oppo_comb.reach_prob_index];
#endif
    }
#endif

    // ===== tie cache（新加）=====
    int cached_rank = INT32_MIN;
    float tiesum = 0.0f;
    std::array<float, 52> card_tiesum{};
    int j_tie_end = 0;

    int j = 0;
    for (std::size_t i = 0; i < player_combs.size(); i++) {
        const RiverCombs& one_player_comb = player_combs[i];

        while (j < (int)oppo_combs.size() && one_player_comb.rank < oppo_combs[j].rank) {
            const RiverCombs& one_oppo_comb = oppo_combs[j];
#if TEXASSOLVER_OPT_SHOWDOWN_FAST_FIELDS
            const int oppo_reach_index = one_oppo_comb.reach_prob_index;
            const float oppo_reach = reach_probs[oppo_reach_index];
            winsum += oppo_reach;
            card_winsum[one_oppo_comb.card1] += oppo_reach;
            card_winsum[one_oppo_comb.card2] += oppo_reach;
#else
            winsum += reach_probs[one_oppo_comb.reach_prob_index];
            card_winsum[one_oppo_comb.card1] += reach_probs[one_oppo_comb.reach_prob_index];
            card_winsum[one_oppo_comb.card2] += reach_probs[one_oppo_comb.reach_prob_index];
#endif
            j++;
        }

        // === tie segment for this rank（只在 rank 变化时重算一次）===
        if (cached_rank != one_player_comb.rank) {
            cached_rank = one_player_comb.rank;
            tiesum = 0.0f;
            std::fill(card_tiesum.begin(), card_tiesum.end(), 0.0f);
            j_tie_end = j;
            while (j_tie_end < (int)oppo_combs.size() && oppo_combs[j_tie_end].rank == cached_rank) {
                const RiverCombs& one_oppo_comb = oppo_combs[j_tie_end];
#if TEXASSOLVER_OPT_SHOWDOWN_FAST_FIELDS
                const int oppo_reach_index = one_oppo_comb.reach_prob_index;
                const float oppo_reach = reach_probs[oppo_reach_index];
                tiesum += oppo_reach;
                card_tiesum[one_oppo_comb.card1] += oppo_reach;
                card_tiesum[one_oppo_comb.card2] += oppo_reach;
#else
                tiesum += reach_probs[one_oppo_comb.reach_prob_index];
                card_tiesum[one_oppo_comb.card1] += reach_probs[one_oppo_comb.reach_prob_index];
                card_tiesum[one_oppo_comb.card2] += reach_probs[one_oppo_comb.reach_prob_index];
#endif
                j_tie_end++;
            }
        }

        const int c1 = one_player_comb.card1;
        const int c2 = one_player_comb.card2;

        const float win_prob = (winsum - card_winsum[c1] - card_winsum[c2]);
        const float tie_prob = (tiesum - card_tiesum[c1] - card_tiesum[c2]);

        payoffs[one_player_comb.reach_prob_index] =
#if TEXASSOLVER_OPT_SHOWDOWN_LOSS_FROM_TOTAL
            win_prob * win_payoff_adj
            + tie_prob * tie_payoff_adj
            + (oppo_total_sum - oppo_total_card_sum[c1] - oppo_total_card_sum[c2] - win_prob - tie_prob) * lose_payoff;
#else
            win_prob * win_payoff_adj
            + tie_prob * tie_payoff_adj;
#endif
    }

    // ===== lose part（你原有逻辑保持，但不需要改，仍乘 lose_payoff）=====
#if !TEXASSOLVER_OPT_SHOWDOWN_LOSS_FROM_TOTAL
    float losssum = 0.0f;
    std::array<float, 52>& card_losssum = card_winsum;
    std::fill(card_losssum.begin(), card_losssum.end(), 0.0f);

    j = (int)oppo_combs.size() - 1;
    for (int i = (int)player_combs.size() - 1; i >= 0; i--) {
        const RiverCombs& one_player_comb = player_combs[i];
        while (j >= 0 && one_player_comb.rank > oppo_combs[j].rank) {
            const RiverCombs& one_oppo_comb = oppo_combs[j];
#if TEXASSOLVER_OPT_SHOWDOWN_FAST_FIELDS
            const int oppo_reach_index = one_oppo_comb.reach_prob_index;
            const float oppo_reach = reach_probs[oppo_reach_index];
            losssum += oppo_reach;
            card_losssum[one_oppo_comb.card1] += oppo_reach;
            card_losssum[one_oppo_comb.card2] += oppo_reach;
#else
            losssum += reach_probs[one_oppo_comb.reach_prob_index];
            card_losssum[one_oppo_comb.card1] += reach_probs[one_oppo_comb.reach_prob_index];
            card_losssum[one_oppo_comb.card2] += reach_probs[one_oppo_comb.reach_prob_index];
#endif
            j--;
        }
        payoffs[one_player_comb.reach_prob_index] += (losssum
                                                      - card_losssum[one_player_comb.card1]
                                                       - card_losssum[one_player_comb.card2]
                                                       ) * lose_payoff;
    }
#endif

    return;
}


#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE
const vector<int>& PCfrSolver::getRiverValidComboIndices(int player, uint64_t current_board) {
    std::lock_guard<std::mutex> lock(this->river_result_cache_lock);
    vector<vector<int>>& by_player = this->river_valid_combo_indices[current_board];
    if (by_player.empty()) {
        by_player.resize(this->player_number);
    }

    vector<int>& valid_indices = by_player[player];
    if (!valid_indices.empty()) {
        return valid_indices;
    }

    const vector<PrivateCards>& hands = this->playerHands(player);
    valid_indices.reserve(hands.size());
    for (std::size_t i = 0; i < hands.size(); ++i) {
        if (!Card::boardsHasIntercept(current_board, hands[i].toBoardLong())) {
            valid_indices.push_back(static_cast<int>(i));
        }
    }
    return valid_indices;
}

#if TEXASSOLVER_OPT_RIVER_RESULT_BOARD_CACHE
const vector<vector<int>>& PCfrSolver::getRiverValidComboIndicesForBoard(uint64_t current_board) {
    std::lock_guard<std::mutex> lock(this->river_result_cache_lock);
    vector<vector<int>>& by_player = this->river_valid_combo_indices[current_board];
    if (by_player.empty()) {
        by_player.resize(this->player_number);
    }

    for (int player = 0; player < this->player_number; ++player) {
        vector<int>& valid_indices = by_player[player];
        if (!valid_indices.empty()) {
            continue;
        }

        const vector<PrivateCards>& hands = this->playerHands(player);
        valid_indices.reserve(hands.size());
        for (std::size_t i = 0; i < hands.size(); ++i) {
            if (!Card::boardsHasIntercept(current_board, hands[i].toBoardLong())) {
                valid_indices.push_back(static_cast<int>(i));
            }
        }
    }

    return by_player;
}
#endif
#endif

vector<float>
PCfrSolver::terminalUtility(int player, shared_ptr<TerminalNode> node, const vector<float> &reach_prob, int iter,
                           uint64_t current_board,int deal) {
    vector<float> out;
    this->terminalUtilityInto(player, node, reach_prob, iter, current_board, deal, out);
    return out;
}

void PCfrSolver::terminalUtilityInto(int player, const shared_ptr<TerminalNode>& node, const vector<float> &reach_prob, int iter,
                           uint64_t current_board,int deal, vector<float>& payoffs) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrTerminalUtility);
    float player_payoff = node->get_payoffs()[player];

    // === rake adjust for terminal (fold) ===
    // 只对赢家扣：谁的 payoff > 0 谁是赢家
    {
        const float pot = (float)node->getPot();
        const float rake = calc_rake_amount(pot, this->rake_pct_, this->rake_cap_, this->no_flop_no_drop_, node->getRound());
        if (player_payoff > 0.0f) {
            player_payoff -= rake;
        }
    }
    int oppo = 1 - player;
    const vector<PrivateCards>& player_hand = playerHands(player);
    const vector<PrivateCards>& oppo_hand = playerHands(oppo);

    payoffs.assign(this->playerHands(player).size(), 0.0f);

    float oppo_sum = 0;
    std::array<float, 52> oppo_card_sum{};

#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE && TEXASSOLVER_OPT_RIVER_RESULT_BOARD_CACHE
    const vector<vector<int>>& valid_indices_by_player = this->getRiverValidComboIndicesForBoard(current_board);
    const vector<int>& oppo_valid_indices = valid_indices_by_player[oppo];
#elif TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    const vector<int>& oppo_valid_indices = this->getRiverValidComboIndices(oppo, current_board);
#endif
#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    for (int oppo_index : oppo_valid_indices) {
        const PrivateCards& oppo_cards = oppo_hand[oppo_index];
        oppo_card_sum[oppo_cards.card1] += reach_prob[oppo_index];
        oppo_card_sum[oppo_cards.card2] += reach_prob[oppo_index];
        oppo_sum += reach_prob[oppo_index];
    }
#else
    for(std::size_t i = 0;i < oppo_hand.size();i ++){
        oppo_card_sum[oppo_hand[i].card1] += reach_prob[i];
        oppo_card_sum[oppo_hand[i].card2] += reach_prob[i];
        oppo_sum += reach_prob[i];
    }
#endif

#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE && TEXASSOLVER_OPT_RIVER_RESULT_BOARD_CACHE
    const vector<int>& player_valid_indices = valid_indices_by_player[player];
#elif TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    const vector<int>& player_valid_indices = this->getRiverValidComboIndices(player, current_board);
#endif
#if TEXASSOLVER_OPT_RIVER_RESULT_CACHE
    for (int player_index : player_valid_indices) {
        const int player_hand_index = player_index;
        const PrivateCards& one_player_hand = player_hand[player_index];
#else
    for(std::size_t i = 0;i < player_hand.size();i ++){
        const int player_hand_index = static_cast<int>(i);
        const PrivateCards& one_player_hand = player_hand[i];
        if(Card::boardsHasIntercept(current_board,one_player_hand.toBoardLong())){
            continue;
        }
#endif
#if TEXASSOLVER_OPT_TERMINAL_SAME_CARD_CACHE
        int oppo_same_card_ind = this->same_card_index[player][oppo][player_hand_index];
#else
        int oppo_same_card_ind = this->pcm.indPlayer2Player(player,oppo,player_hand_index);
#endif
        float plus_reach_prob;
        if(oppo_same_card_ind == -1){
            plus_reach_prob = 0;
        }else{
            plus_reach_prob = reach_prob[oppo_same_card_ind];
        }
        payoffs[player_hand_index] = player_payoff * (
                oppo_sum - oppo_card_sum[one_player_hand.card1]
                - oppo_card_sum[one_player_hand.card2]
                + plus_reach_prob
        );
    }

    return;
}

void PCfrSolver::findGameSpecificIsomorphisms() {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrFindIso);
    // hand isomorphisms
    vector<Card> board_cards = Card::long2boardCards(this->initial_board_long);
    for(int i = 0;i <= 1;i ++){
        vector<PrivateCards>& range = i == 0?this->range1:this->range2;
        for(std::size_t i_range = 0;i_range < range.size();i_range ++) {
            PrivateCards one_range = range[i_range];
            uint32_t range_hash[4]; // four colors, hash of the isomorphisms range + hand combos
            for(int i = 0;i < 4;i ++)range_hash[i] = 0;
            for (int color = 0; color < 4; color++) {
                for (Card one_card:board_cards) {
                    if (one_card.getCardInt() % 4 == color) {
                        range_hash[color] = range_hash[color] | (1 << (one_card.getCardInt() / 4));
                    }
                }
            }
            for (int color = 0; color < 4; color++) {
                for (int one_card_int:{one_range.card1,one_range.card2}) {
                    if (one_card_int % 4 == color) {
                        range_hash[color] = range_hash[color] | (1 << (one_card_int / 4 + 16));
                    }
                }
            }
            // TODO check whethe hash is equal with others
        }
    }

    // chance node isomorphisms
    uint16_t color_hash[4];
    for(int i = 0;i < 4;i ++)color_hash[i] = 0;
    for (Card one_card:board_cards) {
        int rankind = one_card.getCardInt() % 4;
        int suitind = one_card.getCardInt() / 4;
        color_hash[rankind] = color_hash[rankind] | (1 << suitind);
    }
    for(int i = 0;i < 4;i ++){
        this->color_iso_offset[0][i] = 0;
        for(int j = 0;j < i;j ++){
            if(color_hash[i] == color_hash[j]){
                this->color_iso_offset[0][i] = j - i;
                continue;
            }
        }
    }
    for(std::size_t deal = 0;deal < this->deck.getCards().size();deal ++) {
        uint16_t color_hash[4];
        for(int i = 0;i < 4;i ++)color_hash[i] = 0;
        // chance node isomorphisms
        for (Card one_card:board_cards) {
            int rankind = one_card.getCardInt() % 4;
            int suitind = one_card.getCardInt() / 4;
            color_hash[rankind] = color_hash[rankind] | (1 << suitind);
        }
        Card one_card = this->deck.getCards()[deal];
        int rankind = one_card.getCardInt() % 4;
        int suitind = one_card.getCardInt() / 4;
        color_hash[rankind] = color_hash[rankind] | (1 << suitind);
        for (int i = 0; i < 4; i++) {
            this->color_iso_offset[deal + 1][i] = 0;
            for (int j = 0; j < i; j++) {
                if (color_hash[i] == color_hash[j]) {
                    this->color_iso_offset[deal + 1][i] = j - i;
                    continue;
                }
            }
        }
    }
}

void PCfrSolver::purnTree() {
    // TODO how to purn the tree, use wramup to start training in memory-save mode, and switch to purn tree directly to both save memory and speedup
}

void PCfrSolver::stop() {
    this->nowstop = true;
}

void PCfrSolver::train() {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrTrain);

    vector<vector<PrivateCards>> player_privates(this->player_number);
    player_privates[0] = pcm.getPreflopCards(0);
    player_privates[1] = pcm.getPreflopCards(1);
    if(this->use_isomorphism){
        this->findGameSpecificIsomorphisms();
    }

    const bool exploitability_checks_enabled = this->exploitability_interval > 0;
    BestResponse br = BestResponse(player_privates,this->player_number,this->pcm,this->rrm,this->deck,this->debug,this->color_iso_offset,this->split_round,this->num_threads,this->use_halffloats);

    if(exploitability_checks_enabled) {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrInitialExploitability);
        br.printExploitability(tree->getRoot(), 0, tree->getRoot()->getPot(), initial_board_long);
    }

    vector<vector<float>> reach_probs = this->getReachProbs();
    ofstream fileWriter;
    if(!this->logfile.empty())fileWriter.open(this->logfile);

    uint64_t begintime = timeSinceEpochMillisec();
    uint64_t endtime = timeSinceEpochMillisec();

    {
        TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrTrainingLoop);
        for(int i = 0;i < this->iteration_number;i++){
            for(int player_id = 0;player_id < this->player_number;player_id ++) {
                this->round_deal = vector<int>{-1,-1,-1,-1};
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
                vector<float> root_utility;
#endif
                //#pragma omp parallel
                {
                    //#pragma omp single
                    {
                        //this->distributing_task = true;
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
                        this->cfrInto(player_id, this->tree->getRoot(), reach_probs[1 - player_id], i, this->initial_board_long,0, root_utility);
#else
                        cfr(player_id, this->tree->getRoot(), reach_probs[1 - player_id], i, this->initial_board_long,0);
#endif
                        //throw runtime_error("returning...");
                    }
                }
            }
            const bool should_check_exploitability = exploitability_checks_enabled &&
                (i % this->exploitability_interval == 0 && i != 0 && i >= this->warmup);
            if(should_check_exploitability || this->nowstop) {
                endtime = timeSinceEpochMillisec();
                long time_ms = endtime - begintime;
                qDebug().noquote() << "-------------------";
                float expliotibility = 0.0f;
                if(should_check_exploitability) {
                    expliotibility = br.printExploitability(tree->getRoot(), i + 1, tree->getRoot()->getPot(), initial_board_long);
                }
                qDebug().noquote() << QObject::tr("time used: ") << float(time_ms) / 1000 << QObject::tr(" second.");
                if(should_check_exploitability && !this->logfile.empty()){
                    json jo;
                    jo["iteration"] = i;
                    jo["exploitibility"] = expliotibility;
                    jo["time_ms"] = time_ms;
                    fileWriter << jo << endl;
                }
                if(should_check_exploitability && expliotibility <= this->accuracy){
                    break;
                }
                if(this->nowstop){
                    this->nowstop = false;
                    break;
                }
                //begintime = timeSinceEpochMillisec();
            }
        }
    }

    if(this->collect_evs) {
        qDebug().noquote() << QObject::tr("collecting statics");
        this->collecting_statics = true;
        {
            TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::PcfrCollectStatics);
            for(int player_id = 0;player_id < this->player_number;player_id ++) {
                this->round_deal = vector<int>{-1,-1,-1,-1};
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
                vector<float> root_utility;
#endif
                //#pragma omp parallel
                {
                    //#pragma omp single
                    {
                        //this->distributing_task = true;
#if TEXASSOLVER_OPT_CFR_OUT_BUFFER
                        this->cfrInto(player_id, this->tree->getRoot(), reach_probs[1 - player_id], this->iteration_number, this->initial_board_long,0, root_utility);
#else
                        cfr(player_id, this->tree->getRoot(), reach_probs[1 - player_id], this->iteration_number, this->initial_board_long,0);
#endif
                    }
                }
            }
        }
        this->collecting_statics = false;
        this->statics_collected = true;
        qDebug().noquote() << QObject::tr("statics collected");
    } else {
        this->collecting_statics = false;
        this->statics_collected = false;
        qDebug().noquote() << QObject::tr("EV statics skipped");
    }

    if(!this->logfile.empty()) {
        fileWriter.flush();
        fileWriter.close();
    }

}

void PCfrSolver::exchangeRange(json& strategy,int rank1,int rank2,shared_ptr<ActionNode> one_node){
    if(rank1 == rank2)return;
    int player = one_node->getPlayer();
    vector<string> range_strs;
    vector<vector<float>> strategies;

    for(std::size_t i = 0;i < this->ranges[player].size();i ++){
        string one_range_str = this->ranges[player][i].toString();
        if(!strategy.contains(one_range_str)){
            for(auto one_key:strategy.items()){
                cout << one_key.key() << endl;
            }
            cout << "strategy: " << strategy  << endl;
            throw runtime_error(tfm::format("%s not exist in strategy",one_range_str));
        }
        vector<float> one_strategy = strategy[one_range_str];
        range_strs.push_back(one_range_str);
        strategies.push_back(one_strategy);
    }
    exchange_color(strategies,this->ranges[player],rank1,rank2);

    for(std::size_t i = 0;i < this->ranges[player].size();i ++) {
        string one_range_str = this->ranges[player][i].toString();
        vector<float> one_strategy = strategies[i];
        strategy[one_range_str] = one_strategy;
    }
}

void PCfrSolver::reConvertJson(const shared_ptr<GameTreeNode>& node,json& strategy,string key,int depth,int max_depth,vector<string> prefix,int deal,vector<vector<int>> exchange_color_list) {
    if(depth >= max_depth) return;
    if(node->getType() == GameTreeNode::GameTreeNodeType::ACTION) {
        json* retval;
        if(key != ""){
            strategy[key] = json();
            retval = &(strategy[key]);
        }else{
            retval = &strategy;
        }

        shared_ptr<ActionNode> one_node = std::dynamic_pointer_cast<ActionNode>(node);

        vector<string> actions_str;
        for(GameActions one_action:one_node->getActions()) actions_str.push_back(one_action.toString());

        (*retval)["actions"] = actions_str;
        (*retval)["player"] = one_node->getPlayer();

        (*retval)["childrens"] = json();
        json& childrens = (*retval)["childrens"];

        for(std::size_t i = 0;i < one_node->getActions().size();i ++){
            GameActions& one_action = one_node->getActions()[i];
            shared_ptr<GameTreeNode> one_child = one_node->getChildrens()[i];
            vector<string> new_prefix(prefix);
            new_prefix.push_back(one_action.toString());
            this->reConvertJson(one_child,childrens,one_action.toString(),depth,max_depth,new_prefix,deal,exchange_color_list);
        }
        if((*retval)["childrens"].empty()){
            (*retval).erase("childrens");
        }
        // shared_ptr<Trainable> trainable = one_node->getTrainable(deal,false);
        // if(trainable != nullptr) {
        //     (*retval)["strategy"] = trainable->dump_strategy(false);
        //     for(vector<int> one_exchange:exchange_color_list){
        //         int rank1 = one_exchange[0];
        //         int rank2 = one_exchange[1];
        //         this->exchangeRange((*retval)["strategy"]["strategy"],rank1,rank2,one_node);

        //     }
        // }
        shared_ptr<Trainable> trainable = one_node->getTrainable(deal,false);
        if (trainable != nullptr) {
            (*retval)["strategy"] = trainable->dump_strategy(false);

            // ★ 先三位小数化
            round_strategy_block3((*retval)["strategy"], "strategy", "strategy");

            // ★ 再做花色同构交换（顺序不反）
            for (vector<int> one_exchange : exchange_color_list) {
                int rank1 = one_exchange[0];
                int rank2 = one_exchange[1];
                this->exchangeRange((*retval)["strategy"]["strategy"], rank1, rank2, one_node);
            }

            // （可选）交换后再扫一遍，避免交换过程中出现超微小尾差
            round_strategy_block3((*retval)["strategy"], "strategy", "strategy");
        }
        (*retval)["node_type"] = "action_node";

    }else if(node->getType() == GameTreeNode::GameTreeNodeType::SHOWDOWN) {
    }else if(node->getType() == GameTreeNode::GameTreeNodeType::TERMINAL) {
    }else if(node->getType() == GameTreeNode::GameTreeNodeType::CHANCE) {
        json* retval;
        if(key != ""){
            strategy[key] = json();
            retval = &(strategy[key]);
        }else{
            retval = &strategy;
        }

        shared_ptr<ChanceNode> chanceNode = std::dynamic_pointer_cast<ChanceNode>(node);
        const vector<Card>& cards = chanceNode->getCards();
        shared_ptr<GameTreeNode> childerns = chanceNode->getChildren();
        vector<string> card_strs;
        for(Card card:cards)
            card_strs.push_back(card.toString());

        json& dealcards = (*retval)["dealcards"];
        for(std::size_t i = 0;i < cards.size();i ++){
            vector<vector<int>> new_exchange_color_list(exchange_color_list);
            Card& one_card = const_cast<Card &>(cards[i]);
            vector<string> new_prefix(prefix);
            new_prefix.push_back("Chance:" + one_card.toString());

            std::size_t card = i;

            int offset = this->color_iso_offset[deal][one_card.getCardInt() % 4];
            if(offset < 0) {
                for(std::size_t x = 0;x < cards.size();x ++){
                    if(
                            Card::card2int(cards[x]) ==
                            (Card::card2int(cards[card]) + offset)
                    ){
                        card = x;
                        break;
                    }
                }
                if(card == i){
                    throw runtime_error("isomorphism not found while dump strategy");
                }
                vector<int> one_exchange{one_card.getCardInt() % 4,one_card.getCardInt() % 4 + offset};
                new_exchange_color_list.push_back(one_exchange);
            }

            int card_num = this->deck.getCards().size();
            int new_deal;
            if(deal == 0){
                new_deal = card + 1;
            } else if (deal > 0 && deal <= card_num){
                int origin_deal = deal - 1;

#ifdef DEBUG
                if(origin_deal == card) throw runtime_error("deal should not be equal");
#endif
                new_deal = card_num * origin_deal + card;
                new_deal += (1 + card_num);
            } else{
                throw runtime_error(tfm::format("deal out of range : %s ",deal));
            }

            if(exchange_color_list.size() > 1){
                throw runtime_error("exchange color list shouldn't be exceed size 1 here");
            }

            string one_card_str = one_card.toString();
            if(exchange_color_list.size() == 1) {
                int rank1 = exchange_color_list[0][0];
                int rank2 = exchange_color_list[0][1];
                if(one_card.getCardInt() % 4 == rank1){
                    one_card_str = Card::intCard2Str(one_card.getCardInt() - rank1 + rank2);
                }else if(one_card.getCardInt() % 4 == rank2){
                    one_card_str = Card::intCard2Str(one_card.getCardInt() - rank2 + rank1);
                }

            }

            this->reConvertJson(childerns,dealcards,one_card_str,depth + 1,max_depth,new_prefix,new_deal,new_exchange_color_list);
        }
        if((*retval)["dealcards"].empty()){
            (*retval).erase("dealcards");
        }

        (*retval)["deal_number"] = dealcards.size();
        (*retval)["node_type"] = "chance_node";
    }else{
        throw runtime_error("node type unknown!!");
    }
}

vector<vector<vector<float>>> PCfrSolver::get_strategy(shared_ptr<ActionNode> node,vector<Card> chance_cards){
    int deal = 0;
    int card_num = this->deck.getCards().size();
    vector<vector<int>> exchange_color_list;

    vector<vector<vector<float>>> ret_strategy = vector<vector<vector<float>>>(52);
    for(int i = 0;i < 52;i ++){
        ret_strategy[i] = vector<vector<float>>(52);
        for(int j = 0;j < 52;j ++){
            ret_strategy[i][j] = vector<float>();
        }
    }

    vector<Card>& cards = this->deck.getCards();

    for(Card one_card: chance_cards){
        int card = one_card.getNumberInDeckInt();
        int offset = this->color_iso_offset[deal][one_card.getCardInt() % 4];
        if(offset < 0) {
            for(std::size_t x = 0;x < cards.size();x ++){
                if(
                    Card::card2int(cards[x]) ==
                    (Card::card2int(cards[card]) + offset)
                ){
                    card = x;
                    break;
                }
            }
            if(card == one_card.getNumberInDeckInt()){
                throw runtime_error("isomorphism not found while dump strategy");
            }
            vector<int> one_exchange{one_card.getCardInt() % 4,one_card.getCardInt() % 4 + offset};
            exchange_color_list.push_back(one_exchange);
        }

        int new_deal;
        if(deal == 0){
            new_deal = card + 1;
        } else if (deal > 0 && deal <= card_num){
            int origin_deal = deal - 1;
            new_deal = card_num * origin_deal + card;
            new_deal += (1 + card_num);
        } else{
            throw runtime_error(tfm::format("deal out of range : %s ",deal));
        }
        deal = new_deal;
    }
    shared_ptr<Trainable> trainable = node->getTrainable(deal,true,this->use_halffloats);
    json retjson = trainable->dump_strategy(false);;

    for(vector<int> one_exchange:exchange_color_list){
        int rank1 = one_exchange[0];
        int rank2 = one_exchange[1];
        this->exchangeRange((retjson["strategy"]),rank1,rank2,node);
    }

    int player = node->getPlayer();

    json& strategy = retjson["strategy"];
    for(std::size_t i = 0;i < this->ranges[player].size();i ++){
        PrivateCards pc = this->ranges[player][i];
        string one_range_str = pc.toString();
        if(!strategy.contains(one_range_str)){
            for(auto one_key:strategy.items()){
                cout << one_key.key() << endl;
            }
            cout << "strategy: " << strategy  << endl;
            cout << "Eror when get_strategy in PCfrSolver" << endl;
            throw runtime_error(tfm::format("%s not exist in strategy",one_range_str));
        }
        vector<float> one_strategy = strategy[one_range_str];
        bool intercept = false;
        for(auto one_card:chance_cards){
            if(one_card.getCardInt() == pc.card1 || one_card.getCardInt() == pc.card2){
                intercept = true;
            }
        }
        if(intercept) continue;
        ret_strategy[pc.card1][pc.card2] = one_strategy;
    }
    return ret_strategy;
}

vector<vector<vector<float>>> PCfrSolver::get_evs(shared_ptr<ActionNode> node,vector<Card> chance_cards){
    // If solving process has not finished, then no evs is set, therefore we shouldn't return anything
    int deal = 0;
    int card_num = this->deck.getCards().size();
    vector<vector<int>> exchange_color_list;

    vector<vector<vector<float>>> ret_evs = vector<vector<vector<float>>>(52);
    for(int i = 0;i < 52;i ++){
        ret_evs[i] = vector<vector<float>>(52);
        for(int j = 0;j < 52;j ++){
            ret_evs[i][j] = vector<float>();
        }
    }

    vector<Card>& cards = this->deck.getCards();

    for(Card one_card: chance_cards){
        int card = one_card.getNumberInDeckInt();
        int offset = this->color_iso_offset[deal][one_card.getCardInt() % 4];
        if(offset < 0) {
            for(std::size_t x = 0;x < cards.size();x ++){
                if(
                    Card::card2int(cards[x]) ==
                    (Card::card2int(cards[card]) + offset)
                ){
                    card = x;
                    break;
                }
            }
            if(card == one_card.getNumberInDeckInt()){
                throw runtime_error("isomorphism not found while dump evs");
            }
            vector<int> one_exchange{one_card.getCardInt() % 4,one_card.getCardInt() % 4 + offset};
            exchange_color_list.push_back(one_exchange);
        }

        int new_deal;
        if(deal == 0){
            new_deal = card + 1;
        } else if (deal > 0 && deal <= card_num){
            int origin_deal = deal - 1;
            new_deal = card_num * origin_deal + card;
            new_deal += (1 + card_num);
        } else{
            throw runtime_error(tfm::format("deal out of range : %s ",deal));
        }
        deal = new_deal;
    }
    shared_ptr<Trainable> trainable = node->getTrainable(deal,true,this->use_halffloats);
    json retjson = trainable->dump_evs();

    for(vector<int> one_exchange:exchange_color_list){
        int rank1 = one_exchange[0];
        int rank2 = one_exchange[1];
        this->exchangeRange((retjson["evs"]),rank1,rank2,node);
    }

    int player = node->getPlayer();

    json& evs = retjson["evs"];
    for(std::size_t i = 0;i < this->ranges[player].size();i ++){
        PrivateCards pc = this->ranges[player][i];
        string one_range_str = pc.toString();
        if(!evs.contains(one_range_str)){
            for(auto one_key:evs.items()){
                cout << one_key.key() << endl;
            }
            cout << "evs: " << evs  << endl;
            cout << "Eror when get_evs in PCfrSolver" << endl;
            throw runtime_error(tfm::format("%s not exist in evs",one_range_str));
        }
        vector<float> one_evs = evs[one_range_str];
        bool intercept = false;
        for(auto one_card:chance_cards){
            if(one_card.getCardInt() == pc.card1 || one_card.getCardInt() == pc.card2){
                intercept = true;
            }
        }
        if(intercept) continue;
        ret_evs[pc.card1][pc.card2] = one_evs;
    }
    return ret_evs;
}

json PCfrSolver::dumps(bool with_status,int depth) {
    if(with_status == true){
        throw runtime_error("");
    }
    json retjson;
    this->reConvertJson(this->tree->getRoot(),retjson,"",0,depth,vector<string>({"begin"}),0,vector<vector<int>>());
    return std::move(retjson);
}

static inline double round_to(double x, int precision) {
    double scale = std::pow(10.0, precision);
    double y = std::floor(x * scale + 0.5) / scale;
    if (std::fabs(y) < 1.0/scale) y = 0.0;
    if (std::fabs(1.0 - y) < 1.0/scale) y = 1.0;
    return y;
}

int PCfrSolver::getColorIsoOffset(int deal, int suit) const {
    if (deal < 0 || suit < 0 || suit >= 4) return 0;
    return this->color_iso_offset[deal][suit];
}
