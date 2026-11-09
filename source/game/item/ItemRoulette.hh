#pragma once

#include "game/item/ItemId.hh"

#include <Common.hh>

#include <array>

namespace Kinoko::Item {

class KartItem;

/// @brief The item roulette of one racer (KartItem +0x54 in the game, 0x34 bytes). An item box
/// decides the item at once; the roulette then shows random items, slowing down, for 184 frames
/// (45 for some boxes) before it hands the item over.
/// @nosubgrouping
class ItemRoulette {
public:
    enum class State {
        Idle = 0,
        Spinning = 1,
        Stopping = 2, ///< Shows the decided item, then hands it over.
    };

    ItemRoulette();

    void init(KartItem *kartItem);
    void start(u16 boxSetting, u8 rank, bool human, bool isShort);
    [[nodiscard]] bool calc();
    void finish();
    void cancel();

    /// @addr{0x807BA5D0}
    void setPending(ItemId id) {
        m_pending = id;
    }

    /// @beginGetters
    [[nodiscard]] State state() const {
        return m_state;
    }

    [[nodiscard]] s32 frame() const {
        return m_frame;
    }

    [[nodiscard]] ItemId shownItem() const {
        return m_shown;
    }

    [[nodiscard]] ItemId decidedItem() const {
        return m_decided;
    }

    [[nodiscard]] ItemId pending() const {
        return m_pending;
    }

    /// @brief The decided item was replaced by a mushroom because its kind had no room.
    [[nodiscard]] bool replaced() const {
        return m_replaced;
    }
    /// @endGetters

    /// @brief Test hook, off by default: every item box gives this player `id`. Mirrors the
    /// Gecko code used for the Dolphin test recordings, which overwrites the item that
    /// `decideItem` returned where `start` stores it (0x807BA164). The draw itself still happens.
    /// @param id ItemId::NONE turns it off.
    static void SetForcedItem(size_t playerIdx, ItemId id);

private:
    void showNext();

    static constexpr size_t FORCED_ITEM_PLAYERS = 12;
    static std::array<ItemId, FORCED_ITEM_PLAYERS> s_forcedItem; ///< Not in the game.

    KartItem *m_kartItem; ///< +0x00
    State m_state;        ///< +0x04
    s32 m_frame;          ///< +0x08
    s32 m_duration;       ///< +0x0C Frames before the roulette may stop by itself.
    f32 m_speed;          ///< +0x10 Items shown per frame.
    f32 m_progress;       ///< +0x14 Shows the next item when it reaches 1.
    ItemId m_prevShown;   ///< +0x18
    ItemId m_shown;       ///< +0x1C
    ItemId m_decided;     ///< +0x20
    ItemId m_pending;     ///< +0x24 An item still held behind the kart blocks the roulette.
    u16 m_boxSetting;     ///< +0x28
    u8 m_rank;            ///< +0x2A
    bool m_byRank;        ///< +0x2B The box had setting 0xFF.
    bool m_isShort;       ///< +0x2C
    bool m_waitingForNet; ///< +0x2D Online only.
    bool m_replaced;      ///< +0x2E
    s32 m_rollCount;      ///< +0x30 Online only: roulettes started this race.
};

} // namespace Kinoko::Item
