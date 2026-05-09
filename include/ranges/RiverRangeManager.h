//
// Created by Xuefeng Huang on 2020/1/31.
//

#ifndef TEXASSOLVER_RIVERRANGEMANAGER_H
#define TEXASSOLVER_RIVERRANGEMANAGER_H

#include "RiverCombs.h"
#include <unordered_map>
#include <include/compairer/Compairer.h>
#include <include/compairer/Dic5Compairer.h>
#include <mutex>
#include <memory>
#include <array>

class RiverRangeManager {
public:
    RiverRangeManager();
    RiverRangeManager(shared_ptr<Compairer> handEvaluator);
    const vector<RiverCombs>& getRiverCombos(int player, const vector<PrivateCards>& riverCombos, const vector<int>& board);
    const vector<RiverCombs>& getRiverCombos(int player, const vector<PrivateCards>& riverCombos, uint64_t board_long);
    void preloadRiverCombos(const vector<PrivateCards>& player0Combos,
                            const vector<PrivateCards>& player1Combos,
                            const vector<Card>& deckCards,
                            uint64_t initial_board_long);
private:
    struct CanonicalBoard {
        uint64_t board_long = 0;
        std::array<int, 4> perm{{0, 1, 2, 3}};
    };

    unordered_map<uint64_t , vector<RiverCombs>> p1RiverRanges;
    unordered_map<uint64_t , vector<RiverCombs>> p2RiverRanges;
#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
    unordered_map<uint64_t, vector<int>> canonicalRankCache;
#endif
    shared_ptr<Compairer> handEvaluator;
    shared_ptr<mutex> maplock;
#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
    shared_ptr<mutex> rankCacheLock;
    static CanonicalBoard canonicalizeBoard(uint64_t board_long);
    int getCachedCanonicalRank(const PrivateCards& combo, const CanonicalBoard& canonical_board);
#endif
};


#endif //TEXASSOLVER_RIVERRANGEMANAGER_H
