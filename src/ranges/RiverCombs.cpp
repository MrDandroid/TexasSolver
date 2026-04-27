//
// Created by Xuefeng Huang on 2020/1/31.
//

#include "include/ranges/RiverCombs.h"

RiverCombs::RiverCombs() {

}

RiverCombs::RiverCombs(const vector<int>& board, const PrivateCards& private_cards, int rank, int reach_prob_index) {
#if !TEXASSOLVER_OPT_LIGHT_RIVER_COMBS
    this->board = board;
    this->private_cards = private_cards;
#else
    (void)board;
#endif
    this->rank = rank;
    this->reach_prob_index = reach_prob_index;
    this->card1 = private_cards.card1;
    this->card2 = private_cards.card2;
    this->private_board_long = private_cards.toBoardLong();
}

RiverCombs::RiverCombs(const PrivateCards& private_cards, int rank, int reach_prob_index) {
#if !TEXASSOLVER_OPT_LIGHT_RIVER_COMBS
    this->private_cards = private_cards;
#endif
    this->rank = rank;
    this->reach_prob_index = reach_prob_index;
    this->card1 = private_cards.card1;
    this->card2 = private_cards.card2;
    this->private_board_long = private_cards.toBoardLong();
}

uint64_t RiverCombs::toBoardLong() const {
    return this->private_board_long;
}

