//
// Created by Xuefeng Huang on 2020/1/30.
//

#ifndef TEXASSOLVER_DECK_H
#define TEXASSOLVER_DECK_H

#include "Card.h"

class Deck {
public:
    Deck();
    Deck(const vector<string>& ranks, const vector<string>& suits);
    vector<Card>& getCards();              // 非const版本
    const vector<Card>& getCards() const;  // const版本

    vector<string>& getRanks();
    const vector<string>& getRanks() const;
private:
    vector<string> ranks;
    vector<string> suits;
    vector<string> cards_str;
    vector<Card> cards;

};


#endif //TEXASSOLVER_DECK_H
