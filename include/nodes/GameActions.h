#ifndef TEXASSOLVER_GAMEACTIONS_H
#define TEXASSOLVER_GAMEACTIONS_H

#include <string>
#include <vector>
#include "include/tools/tinyformat.h"
#include "include/nodes/GameTreeNode.h"

class GameActions {
public:
    GameActions();
    GameActions(GameTreeNode::PokerActions action, double amount);

    GameTreeNode::PokerActions getAction() const;   // 加 const
    double getAmount() const;                       // 加 const
    std::string toString() const;                   // 加 const

    static std::string pokerActionToString(GameTreeNode::PokerActions pokerActions);

private:
    GameTreeNode::PokerActions action;
    double amount{};
};


#endif //TEXASSOLVER_GAMEACTIONS_H
