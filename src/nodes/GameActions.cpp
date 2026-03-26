//
// Created by Xuefeng Huang on 2020/1/29.
//
#include "include/nodes/GameActions.h"
#include <cmath>

GameActions::GameActions() = default;

GameActions::GameActions(GameTreeNode::PokerActions action, double amount) {
    this->action = action;
    if (action == GameTreeNode::PokerActions::RAISE || action == GameTreeNode::PokerActions::BET ) {
        if (amount == -1) throw runtime_error(tfm::format("raise/bet amount should not be -1, find %s",amount));
    } else {
        if (amount != -1) throw runtime_error(tfm::format("check/fold/call amount should be -1, find %s",amount));
    }
    this->amount = amount;
}

GameTreeNode::PokerActions GameActions::getAction() const {
    return this->action;
}

double GameActions::getAmount() const {
    return this->amount;
}

std::string GameActions::pokerActionToString(GameTreeNode::PokerActions pokerActions) {
    switch (pokerActions)
    {
    case GameTreeNode::PokerActions::BEGIN:       return "BEGIN";
    case GameTreeNode::PokerActions::ROUNDBEGIN:  return "ROUNDBEGIN";
    case GameTreeNode::PokerActions::BET:         return "BET";
    case GameTreeNode::PokerActions::RAISE:       return "RAISE";
    case GameTreeNode::PokerActions::CHECK:       return "CHECK";
    case GameTreeNode::PokerActions::FOLD:        return "FOLD";
    case GameTreeNode::PokerActions::CALL:        return "CALL";
    default: throw runtime_error("PokerActions not found");
    }
}

std::string GameActions::toString() const {
    if(this->amount == -1) {
        return this->pokerActionToString(this->action);
    } else {
        //以前的是固定保留6位小数
        // return this->pokerActionToString(this->action) + " " + std::to_string(amount);

        // 有金额动作（BET / RAISE）：改为无小数点的整数显示
        const std::string head = this->pokerActionToString(this->action);

        // 方案一：四舍五入到最近整数（推荐）
        long long iv = llround(this->amount);
        return head + " " + std::to_string(iv);
    }
}

