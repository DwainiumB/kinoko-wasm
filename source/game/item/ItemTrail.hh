#pragma once

#include "game/item/ItemId.hh"
#include "game/item/ItemObj.hh"

#include "game/kart/KartObjectProxy.hh"

namespace Kinoko::Item {

class KartItem;

/// @brief The items a kart holds behind itself, and the start of every item use (the game's
/// object at KartItem +0xB4, em-eight's `ItemHolderPlayerb4`).
/// @nosubgrouping
class ItemTrail : public Kart::KartObjectProxy {
public:
    /// @brief How an item is used (item behaviour table +0x14).
    enum class Mode {
        Instant = 0, ///< Used at once (mushroom, star, ...).
        Single = 1,  ///< One object held behind the kart.
        Chain = 2,   ///< Triple bananas, trailing in a line.
        Orbit = 3,   ///< Triple shells, circling the kart.
        None = 4,
    };

    ItemTrail();
    ~ItemTrail();

    void init(KartItem *kartItem, size_t playerIdx);
    void use();
    void calc();
    void clear();
    void startDrop();
    void detach(ItemObj *obj);

    [[nodiscard]] f32 calcThrowForward(f32 speed, f32 deltaScale, f32 dirScale, bool arc,
            EGG::Vector3f &dir);
    [[nodiscard]] f32 calcThrowBack(f32 speed, f32 deltaScale, f32 dirScale, bool arc,
            EGG::Vector3f &dir);

    /// @beginGetters
    [[nodiscard]] KartItem *kartItem() const {
        return m_kartItem;
    }

    [[nodiscard]] const Kart::KartObject *kartObject() const;

    [[nodiscard]] ItemObjKind objKind() const {
        return m_objKind;
    }

    [[nodiscard]] ItemId itemId() const {
        return m_itemId;
    }

    [[nodiscard]] Mode mode() const {
        return m_mode;
    }

    [[nodiscard]] s32 count() const {
        return m_count;
    }

    [[nodiscard]] s32 dropTimer() const {
        return m_dropTimer;
    }

    [[nodiscard]] bool forcedRelease() const {
        return m_forcedRelease;
    }

    [[nodiscard]] const EGG::Vector3f &right() const {
        return m_pose[0];
    }

    [[nodiscard]] const EGG::Vector3f &up() const {
        return m_pose[1];
    }

    [[nodiscard]] const EGG::Vector3f &forward() const {
        return m_pose[2];
    }

    [[nodiscard]] const EGG::Vector3f &posDelta() const {
        return m_posDelta;
    }

    [[nodiscard]] f32 kartSpeed() const {
        return m_speed;
    }

    [[nodiscard]] f32 hopHeight() const {
        return m_hopHeight;
    }
    /// @endGetters

private:
    void reset();
    [[nodiscard]] bool calcKartPose();
    void initSingle();
    void calcSingle();
    void initChain();
    void calcChain();
    void initOrbit();
    void calcOrbit();
    void calcOrbitPositions();
    void growHeld(ItemObj *obj, f32 step);
    void calcTrailPos();
    void calcCollision();
    void releaseLast();
    void releaseObj(ItemObj *obj);
    void detachChain(s32 idx, ItemObj *obj);
    void detachOrbit(s32 idx, ItemObj *obj);
    void finishThrowDir(bool arc, EGG::Vector3f &dir, EGG::Vector3f &out);

    KartItem *m_kartItem;            ///< +0x0C
    u8 m_playerIdx;                  ///< +0x10
    ItemObjKind m_objKind;           ///< +0x14
    ItemId m_itemId;                 ///< +0x18
    Mode m_mode;                     ///< +0x1C
    std::array<ItemObj *, 3> m_objs; ///< +0x20
    s32 m_count;                     ///< +0x50
    s32 m_dropTimer;                 ///< +0x54 Drops one held item at 8, 4 and 0.
    std::array<EGG::Vector3f, 4>
            m_pose; ///< +0x58 Right, up, forward, and the kart position shifted forward by the item
                    ///< offset (not the kart position itself).
    EGG::Vector3f m_velocity;                  ///< +0x88
    f32 m_speed;                               ///< +0x94
    f32 m_speedRatio;                          ///< +0x98
    EGG::Vector3f m_posDelta;                  ///< +0x9C
    f32 m_hopHeight;                           ///< +0xA8 KartMove +0x22C.
    bool m_movingForward;                      ///< +0xAC
    EGG::Vector3f m_scale;                     ///< +0xB0
    f32 m_bob;                                 ///< +0xCC
    f32 m_trailDist;                           ///< +0xD0 Stats +0x170.
    f32 m_trailHeight;                         ///< +0xD4 Stats +0x174.
    f32 m_trailDistShort;                      ///< +0xD8
    f32 m_trailHeightOfs;                      ///< +0xDC Stats +0x178.
    f32 m_trailUpOfs;                          ///< +0xE0 Stats +0x17C.
    EGG::Vector3f m_smoothedBack;              ///< +0xE4
    EGG::Vector3f m_trailDir;                  ///< +0xF0
    EGG::Vector3f m_trailAnchor;               ///< +0xFC
    std::array<EGG::Vector3f, 2> m_chainLinks; ///< +0x108 Where the 2nd and 3rd banana hang.
    f32 m_chainSlackBase;                      ///< +0x120
    std::array<f32, 2> m_chainSlack; ///< +0x124 How much the links follow the kart's motion.
    EGG::Vector3f m_chainPrevDir;    ///< +0x12C Last frame's trail direction.
    f32 m_lift;                      ///< +0x138
    f32 m_liftDelta;                 ///< +0x13C
    f32 m_followY;                   ///< +0x140
    f32 m_followXZ;                  ///< +0x144
    std::array<std::array<f32, 16>, 2> m_chainLiftRing; ///< +0x148 / +0x14C Delayed lifts.
    u32 m_chainRingIdx;                                 ///< +0x150
    bool m_forcedRelease;                               ///< +0x154
    u16 m_orbitAngle;                                   ///< +0x156
    u16 m_orbitSpeed;                                   ///< +0x158
    f32 m_orbitRadiusSide;                              ///< +0x15C
    f32 m_orbitRadiusFwd;                               ///< +0x160
    f32 m_orbitLift;                                    ///< +0x164
    EGG::Vector3f m_orbitCenter;                        ///< +0x168
    std::array<u16, 3> m_orbitSlots;             ///< +0x174 Each shell's place on the circle.
    std::array<EGG::Vector3f, 3> m_trailOffsets; ///< +0x2C Course push-out of each held item.
};

} // namespace Kinoko::Item
