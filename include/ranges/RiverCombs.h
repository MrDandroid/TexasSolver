//
// Created by Xuefeng Huang on 2020/1/31.
//

#ifndef TEXASSOLVER_RIVERCOMBS_H
#define TEXASSOLVER_RIVERCOMBS_H


#include <include/ranges/PrivateCards.h>
#include <include/tools/OptimizationSwitches.h>

class RiverCombs {
public:
    int rank{};
    int reach_prob_index{};
    int card1{};
    int card2{};
    uint64_t private_board_long{};
#if !TEXASSOLVER_OPT_LIGHT_RIVER_COMBS
    PrivateCards private_cards;
#endif
    RiverCombs();
    RiverCombs(const vector<int>& board, const PrivateCards& private_cards, int rank, int reach_prob_index);
    RiverCombs(const PrivateCards& private_cards, int rank, int reach_prob_index);
    uint64_t toBoardLong() const;
private:
#if !TEXASSOLVER_OPT_LIGHT_RIVER_COMBS
    vector<int> board;
#endif
};


#endif //TEXASSOLVER_RIVERCOMBS_H
