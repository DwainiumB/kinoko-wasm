#pragma once

#include "game/item/ItemInventory.hh"
#include "game/item/ItemRoulette.hh"
#include "game/item/ItemTrail.hh"

#include "game/kart/KartObjectProxy.hh"

#include <egg/core/BitFlag.hh>

namespace Kinoko::Item {

/// @brief State management for item usage
/// @nosubgrouping
class KartItem : Kart::KartObjectProxy {
public:
    KartItem();
    ~KartItem();

    void init(size_t playerIdx);
    void calc();
    void clear();

    void activateMushroom();
    void activateStar();
    void activateMega();
    [[nodiscard]] bool hitByLightning();
    void strikeByThunderCloud();

    /// @brief The bullet bill ended at this item point (0x807B99D4 from 0x8059C118).
    void setBulletItemPoint(u8 idx) {
        setItemPoint(idx, true);
    }

    /// @brief The kart was put back on the track at a respawn point linked to this item point
    /// (0x807B99D4 from 0x80584334).
    void setRespawnItemPoint(u8 idx) {
        setItemPoint(idx, true);
    }

    void clearBulletFromInventory();
    void loseItem();
    void dropTrail(bool singleOnly);

    void startRoulette(u16 humanSetting, u16 cpuSetting, bool isShort);
    [[nodiscard]] bool hasTripleItem(bool ignoreRoulette) const;

    void useItem();
    void useMushroom();
    void useGolden();
    void useStar();
    void useMega();
    void useLightning();
    void useThunderCloud();
    void useBlooper();
    void useBullet();
    void usePow();

    /// @brief The trail asks for the button release of the held item to be watched.
    void armRelease() {
        m_flags.setBit(eFlags::ReleaseArmed);
    }

    /// @beginGetters
    [[nodiscard]] ItemInventory &inventory() {
        return m_inventory;
    }

    [[nodiscard]] const ItemInventory &inventory() const {
        return m_inventory;
    }

    [[nodiscard]] ItemTrail &trail() {
        return m_trail;
    }

    [[nodiscard]] const ItemRoulette &roulette() const {
        return m_roulette;
    }

    [[nodiscard]] ItemRoulette &roulette() {
        return m_roulette;
    }

    /// @brief The nearest point of the item route (0xFF on courses without one).
    [[nodiscard]] u8 itemPoint() const {
        return m_itemPoint.idx;
    }

    /// @brief The item button was pressed while the roulette spins (flags 0, 6 and 9).
    [[nodiscard]] bool rouletteStopRequested() const {
        return m_flags.onAllBit(eFlags::RouletteHeld, eFlags::PressAccepted, eFlags::Unlocked);
    }

    [[nodiscard]] const ItemTrail &trail() const {
        return m_trail;
    }

    [[nodiscard]] size_t playerIdx() const {
        return m_playerIdx;
    }

    [[nodiscard]] const Kart::KartObject *kartObject() const;

    [[nodiscard]] bool stickUp() const {
        return m_flags.onBit(eFlags::StickUp);
    }

    [[nodiscard]] bool stickDown() const {
        return m_flags.onBit(eFlags::StickDown);
    }

    /// @brief The triple items' throw: pressed this frame, unlocked, items held behind (0x244).
    [[nodiscard]] bool pressRequested() const {
        return m_flags.onAllBit(eFlags::Unlocked, eFlags::PressAccepted, eFlags::HasTrail);
    }

    /// @brief The trail's release condition: released this frame, unlocked, item held behind.
    [[nodiscard]] bool releaseRequested() const {
        return m_flags.onAllBit(eFlags::Unlocked, eFlags::ReleaseAccepted, eFlags::HasTrail);
    }

    [[nodiscard]] const EGG::Vector3f &posDelta() const {
        return m_posDelta;
    }

    [[nodiscard]] f32 throwPitch() const {
        return m_throwPitch;
    }

    [[nodiscard]] f32 smoothedStickX() const {
        return m_smoothedStickX;
    }

    [[nodiscard]] f32 speedRatio() const;
    /// @endGetters

    /// @brief The kart's place on the item route (+0x44, 0x10 bytes). The shells copy it as their
    /// route state, where +0x01 is the travel direction (1: along the next links).
    struct ItemPointTracker {
        u8 idx;            ///< +0x00
        bool moved;        ///< +0x01 Set when idx changes.
        EGG::Vector3f pos; ///< +0x04 The point's position.
    };

    [[nodiscard]] const ItemPointTracker &itemPointTracker() const {
        return m_itemPoint;
    }

private:
    [[nodiscard]] bool canStartUse();
    void setItemPoint(u8 idx, bool moved);
    bool calcItemPoint(const EGG::Vector3f &kartPos);
    bool calcItemPointFar(const EGG::Vector3f &kartPos);

    size_t m_playerIdx; ///< +0x18
    bool m_isHuman;     ///< +0x19 Not a CPU: the player odds.
    bool m_isRemote;    ///< +0x1A An online racer simulated elsewhere. Always false in Kinoko.

    /// @brief +0x0C.
    enum class eFlags {
        RouletteHeld = 0, ///< Pressed while the roulette spins.
        HasItem = 1,      ///< An item in the inventory.
        HasTrail = 2,     ///< An item held behind the kart.
        StickUp = 4,
        StickDown = 5,
        PressAccepted = 6,   ///< Pressed this frame while allowed to use an item.
        ReleaseAccepted = 7, ///< Released this frame while allowed to throw.
        ReleaseArmed = 8,    ///< A held item waits for the button release.
        Unlocked = 9,        ///< Not locked out this frame.
        Lockout = 10,
        ItemButtonHold = 12,
        ItemButtonActivation = 13,
        ItemButtonRelease = 14,
    };
    typedef EGG::TBitFlag<u16, eFlags> Flags;

    Flags m_flags;                ///< +0x0C
    EGG::Vector3f m_prevPos;      ///< +0x24
    EGG::Vector3f m_posDelta;     ///< +0x34
    f32 m_throwPitch;             ///< +0x40 Smoothed pitch of a throw.
    ItemPointTracker m_itemPoint; ///< +0x44
    ItemRoulette m_roulette;      ///< +0x54
    ItemInventory m_inventory;    ///< +0x88
    ItemTrail m_trail;            ///< +0xB4
    f32 m_smoothedStickX;         ///< +0x23C
};

} // namespace Kinoko::Item
