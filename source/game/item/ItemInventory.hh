#pragma once

#include "game/item/ItemId.hh"
#include "game/item/ItemObj.hh"

namespace Kinoko::Item {

class KartItem;

/// @brief A player's held item (KartItem +0x88 in the game).
/// @nosubgrouping
class ItemInventory {
public:
    /// @brief One row of the game's item behaviour table (0x809C36A0, stride 0x1C), which it
    /// fills at boot (`ItemProperties_init` 0x807BCAE0). Written out by hand.
    struct ItemBehaviour {
        bool usable;         ///< +0x00
        ItemObjKind objKind; ///< +0x04
        s32 count;           ///< +0x08 Uses per pickup.
        bool timed;          ///< +0x10
        s32 mode;            ///< +0x14 ItemTrail::Mode.
    };

    ItemInventory();
    ~ItemInventory();

    void init(KartItem *kartItem);
    void calc();

    /// @beginSetters
    void setItem(ItemId id, bool replaced = false);
    void useItem(int count);
    void clear();
    void clearAndDespawn();
    void startDrop();
    [[nodiscard]] bool startTimer(int frames);
    void setTimerUse(int frames);
    /// @endSetters

    /// @brief Changes only the item (KartItem +0x78), as the bullet bill's end does.
    void setId(ItemId id) {
        m_currentId = id;
    }

    /// @beginGetters
    [[nodiscard]] int currentCount() const {
        return m_currentCount;
    }

    [[nodiscard]] ItemId id() const {
        return m_currentId;
    }

    [[nodiscard]] int timer() const {
        return m_timer;
    }

    [[nodiscard]] int dropTimer() const {
        return m_dropTimer;
    }

    [[nodiscard]] static const ItemBehaviour &Behaviour(ItemId id);
    /// @endGetters

private:
    void spawnDrop();
    void dropOne();

    KartItem *m_kartItem;             ///< Offset 0x00.
    ItemId m_currentId;               ///< Offset 0x04.
    int m_currentCount;               ///< Offset 0x08.
    std::array<ItemObj *, 3> m_drops; ///< Offset 0x0C. Objects waiting to be dropped.
    int m_dropTimer;                  ///< Offset 0x18. Drops one object at 7, 5 and 3.
    bool m_hasTimer;                  ///< Offset 0x1C. Only the golden mushroom.
    int m_timer;                      ///< Offset 0x20. Frames left of a timed item.
    int m_timerUse;                   ///< Offset 0x24. m_timer - 12 at the last use.
    bool m_replaced;                  ///< Offset 0x29. The roulette replaced the item it drew.
};

} // namespace Kinoko::Item
