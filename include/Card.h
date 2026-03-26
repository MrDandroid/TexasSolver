//
// Created by Xuefeng Huang on 2020/1/28.
//

#ifndef TEXASSOLVER_CARD_H
#define TEXASSOLVER_CARD_H

#include <iostream>
#include <string>
#include <vector>
#include "include/tools/tinyformat.h"
#include <QString>

using namespace std;

class Card {
private:
    string card;
    int    card_int;
    int    card_number_in_deck;

public:
    Card();
    explicit Card(string card, int card_number_in_deck);
    Card(string card);

    // ---- 只读访问器一律加 const ----
    bool   empty() const;
    string getCard() const;
    int    getCardInt() const;
    int    getNumberInDeckInt() const;

    // ---- 静态工具函数 ----
    static int    card2int(const Card& card);
    static int    strCard2int(string card);
    static string intCard2Str(int card);

    static uint64_t boardCards2long(vector<string> cards);
    static uint64_t boardCard2long(const Card& card);
    static uint64_t boardCards2long(const vector<Card>& cards);
    static QString  boardCards2html(const vector<Card>& cards);

    static inline bool boardsHasIntercept(uint64_t board1, uint64_t board2) {
        return ((board1 & board2) != 0);
    };

    static uint64_t   boardInts2long(const vector<int>& board);
    static uint64_t   boardInt2long(int board);
    static vector<int>   long2board(uint64_t board_long);
    static vector<Card>  long2boardCards(uint64_t board_long);

    static string suitToString(int suit);
    static string rankToString(int rank);
    static int    rankToInt(char rank);
    static int    suitToInt(char suit);
    static vector<string> getSuits();

    // ---- 字符串化（只读） ----
    string  toString() const;
    string  toFormattedString() const;
    QString toFormattedHtml() const;
};

#endif // TEXASSOLVER_CARD_H
