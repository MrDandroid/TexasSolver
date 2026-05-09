//
// Created by Xuefeng Huang on 2020/1/31.
//

#include "include/ranges/RiverRangeManager.h"
#include "include/tools/HotspotProfiler.h"

#include <algorithm>
#include <array>
#include <utility>

RiverRangeManager::RiverRangeManager() = default;

RiverRangeManager::RiverRangeManager(shared_ptr<Compairer> handEvaluator) {
    this->handEvaluator = std::move(handEvaluator);
    this->maplock = std::make_shared<std::mutex>();
#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
    this->rankCacheLock = std::make_shared<std::mutex>();
#endif
}

static int river_private_key(int card1, int card2) {
    if (card1 > card2) {
        std::swap(card1, card2);
    }
    return card1 * 52 + card2;
}

static int river_permute_card(int card, const std::array<int, 4>& perm) {
    return (card / 4) * 4 + perm[card % 4];
}

static uint64_t river_board_long_with_perm(uint64_t board_long, const std::array<int, 4>& perm) {
    uint64_t mapped = 0;
    for (int card = 0; card < 52; ++card) {
        if ((board_long & (uint64_t(1) << card)) != 0) {
            mapped |= uint64_t(1) << river_permute_card(card, perm);
        }
    }
    return mapped;
}

static int river_board_card_count(uint64_t board_long) {
#if TEXASSOLVER_OPT_FAST_CARD_ACCESSORS
    return Card::boardLongCardCount(board_long);
#else
    int count = 0;
    while (board_long != 0) {
        count += static_cast<int>(board_long & uint64_t(1));
        board_long >>= 1;
    }
    return count;
#endif
}

#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
RiverRangeManager::CanonicalBoard RiverRangeManager::canonicalizeBoard(uint64_t board_long) {
    CanonicalBoard best;
    bool first = true;
    std::array<int, 4> perm{{0, 1, 2, 3}};
    do {
        const uint64_t mapped = river_board_long_with_perm(board_long, perm);
        if (first || mapped < best.board_long) {
            best.board_long = mapped;
            best.perm = perm;
            first = false;
        }
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

int RiverRangeManager::getCachedCanonicalRank(const PrivateCards& combo, const CanonicalBoard& canonical_board) {
    const int mapped_card1 = river_permute_card(combo.card1, canonical_board.perm);
    const int mapped_card2 = river_permute_card(combo.card2, canonical_board.perm);
    const int private_key = river_private_key(mapped_card1, mapped_card2);

    {
        std::lock_guard<std::mutex> lock(*this->rankCacheLock);
        vector<int>& ranks = this->canonicalRankCache[canonical_board.board_long];
        if (ranks.empty()) {
            ranks.assign(52 * 52, -1);
        }
        const int cached_rank = ranks[private_key];
        if (cached_rank >= 0) {
            return cached_rank;
        }
    }

    const uint64_t private_long = (uint64_t(1) << mapped_card1) | (uint64_t(1) << mapped_card2);
    const int rank = this->handEvaluator->get_rank(private_long, canonical_board.board_long);

    std::lock_guard<std::mutex> lock(*this->rankCacheLock);
    vector<int>& ranks = this->canonicalRankCache[canonical_board.board_long];
    if (ranks.empty()) {
        ranks.assign(52 * 52, -1);
    }
    if (ranks[private_key] < 0) {
        ranks[private_key] = rank;
    }
    return ranks[private_key];
}
#endif

const vector<RiverCombs> &
RiverRangeManager::getRiverCombos(int player, const vector<PrivateCards> &riverCombos, const vector<int> &board) {
    uint64_t board_long = Card::boardInts2long(board);
    return this->getRiverCombos(player,riverCombos,board_long);
}

const vector<RiverCombs> &
RiverRangeManager::getRiverCombos(int player, const vector<PrivateCards> &preflopCombos, uint64_t board_long) {
    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::RiverRangeGet);
    unordered_map<uint64_t , vector<RiverCombs>>* riverRanges;

    if (player == 0)
        riverRanges = &p1RiverRanges;
    else if (player == 1)
        riverRanges = &p2RiverRanges;
    else
        throw runtime_error(tfm::format("player %s not found",player));

    uint64_t key = board_long;

#if TEXASSOLVER_OPT_RIVER_LAZY_CACHE
    this->maplock->lock();
    if (riverRanges->find(key) != riverRanges->end()) {
        const vector<RiverCombs> &retval = (*riverRanges)[key];
        this->maplock->unlock();
        return retval;
    }
    this->maplock->unlock();
#else
    {
        std::lock_guard<std::mutex> lock(*this->maplock);
        auto found = riverRanges->find(key);
        if (found != riverRanges->end()) {
            return found->second;
        }
    }
#endif

    TEXASSOLVER_HOTSPOT_SCOPE(HotspotId::RiverRangeBuild);
    int count = 0;

    for (const PrivateCards& one_hand : preflopCombos) {
        if (!Card::boardsHasIntercept(
                one_hand.toBoardLong(), board_long
        ))
            count++;
    }

    int index = 0;
    vector<RiverCombs> riverCombos = vector<RiverCombs>(count);
#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
    const CanonicalBoard canonical_board = canonicalizeBoard(board_long);
#endif

    for (std::size_t hand = 0; hand < preflopCombos.size(); hand++)
    {
        const PrivateCards& preflopCombo = preflopCombos[hand];


        if (Card::boardsHasIntercept(
                preflopCombo.toBoardLong(), board_long
        )){
            continue;
        }

#if TEXASSOLVER_OPT_RIVER_CANONICAL_RANK_CACHE
        int rank = this->getCachedCanonicalRank(preflopCombo, canonical_board);
#else
        int rank = this->handEvaluator->get_rank(preflopCombo.toBoardLong(),board_long);
#endif
#if TEXASSOLVER_OPT_LIGHT_RIVER_COMBS
        RiverCombs riverCombo = RiverCombs(preflopCombo, rank, hand);
#else
        RiverCombs riverCombo = RiverCombs(Card::long2board(board_long),preflopCombo,rank, hand);
#endif
        riverCombos[index++] = riverCombo;
    }

    std::sort(riverCombos.begin(),riverCombos.end(),[ ]( const RiverCombs& lhs, const RiverCombs& rhs )
    {
        return lhs.rank > rhs.rank;
    });

    std::lock_guard<std::mutex> lock(*this->maplock);
    auto inserted = riverRanges->emplace(key, std::move(riverCombos));
    return inserted.first->second;
}

void RiverRangeManager::preloadRiverCombos(const vector<PrivateCards>& player0Combos,
                                           const vector<PrivateCards>& player1Combos,
                                           const vector<Card>& deckCards,
                                           uint64_t initial_board_long) {
    const int board_count = river_board_card_count(initial_board_long);
    if (board_count >= 5) {
        (void)this->getRiverCombos(0, player0Combos, initial_board_long);
        (void)this->getRiverCombos(1, player1Combos, initial_board_long);
        return;
    }

    if (board_count == 4) {
        for (const Card& river_card : deckCards) {
            const uint64_t river_long = uint64_t(1) << river_card.getCardInt();
            if (Card::boardsHasIntercept(initial_board_long, river_long)) {
                continue;
            }
            const uint64_t board_long = initial_board_long | river_long;
            (void)this->getRiverCombos(0, player0Combos, board_long);
            (void)this->getRiverCombos(1, player1Combos, board_long);
        }
        return;
    }

    for (std::size_t i = 0; i < deckCards.size(); ++i) {
        const uint64_t first_long = uint64_t(1) << deckCards[i].getCardInt();
        if (Card::boardsHasIntercept(initial_board_long, first_long)) {
            continue;
        }
        for (std::size_t j = i + 1; j < deckCards.size(); ++j) {
            const uint64_t second_long = uint64_t(1) << deckCards[j].getCardInt();
            if (Card::boardsHasIntercept(initial_board_long, second_long)) {
                continue;
            }
            const uint64_t board_long = initial_board_long | first_long | second_long;
            (void)this->getRiverCombos(0, player0Combos, board_long);
            (void)this->getRiverCombos(1, player1Combos, board_long);
        }
    }
}
