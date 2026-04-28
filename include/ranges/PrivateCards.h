//
// Created by Xuefeng Huang on 2020/1/31.
//

#ifndef TEXASSOLVER_PRIVATECARDS_H
#define TEXASSOLVER_PRIVATECARDS_H
#include "include/Card.h"
#include "include/tools/OptimizationSwitches.h"

class PrivateCards {
public:
    int card1{};
    int card2{};
    float weight{};
    float relative_prob{};
    PrivateCards();
    PrivateCards(int card1, int card2, float weight);
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
    uint64_t toBoardLong() const { return this->board_long; }
#else
    uint64_t toBoardLong() const;
#endif
    int hashCode();
    string toString();
    const vector<int> & get_hands() const;
private:
    vector<int> card_vec;
    int hash_code{};
    uint64_t board_long;
};


#endif //TEXASSOLVER_PRIVATECARDS_H
