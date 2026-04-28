//
// Created by Xuefeng Huang on 2020/1/31.
//

#ifndef TEXASSOLVER_TRAINABLE_H
#define TEXASSOLVER_TRAINABLE_H
#include <cstddef>
#include <memory>
#include <vector>
#include "include/json.hpp"
using namespace std;
using json = nlohmann::json;

class Trainable {
public:
    enum TrainableType {
        CFR_PLUS_TRAINABLE,
        DISCOUNTED_CFR_TRAINABLE
    };
    virtual const vector<float> getAverageStrategy() = 0;
    virtual const vector<float> getcurrentStrategy() = 0;
    virtual void fillAverageStrategy(vector<float>& strategy) = 0;
    virtual void fillCurrentStrategy(vector<float>& strategy) = 0;
    virtual void fillCurrentStrategyForAction(int action_id, vector<float>& strategy) const = 0;
    virtual float getCurrentStrategy(int action_id, int private_id) const = 0;
    virtual void updateRegrets(const vector<float>& regrets,int iteration_number,const vector<float>& reach_probs) = 0;
    virtual void updateRegretsFromActionUtilities(const vector<vector<float>>& action_utilities,
                                                  const vector<float>& payoffs,
                                                  int iteration_number,
                                                  const vector<float>& reach_probs) {
        const std::size_t action_count = action_utilities.size();
        const std::size_t card_count = payoffs.size();
        vector<float> regrets(action_count * card_count);
        for (std::size_t action_id = 0; action_id < action_count; action_id++) {
            const vector<float>& one_action_utilities = action_utilities[action_id];
            for (std::size_t private_id = 0; private_id < card_count; private_id++) {
                regrets[action_id * card_count + private_id] =
                        one_action_utilities[private_id] - payoffs[private_id];
            }
        }
        updateRegrets(regrets, iteration_number, reach_probs);
    }
    virtual void setEv(const vector<float>& evs) = 0;
    virtual void copyStrategy(shared_ptr<Trainable> other_trainable) = 0;
    virtual json dump_strategy(bool with_state) = 0;
    virtual json dump_evs() = 0;
    virtual TrainableType get_type() = 0;
};


#endif //TEXASSOLVER_TRAINABLE_H
