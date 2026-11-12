#pragma once

#include "game/field/KColData.hh"
#include "game/field/ObjectCollisionPoint.hh"

#include <egg/core/BitFlag.hh>
#include <egg/math/Quat.hh>

namespace Kinoko {

namespace Field {

struct BoxColUnit;

} // namespace Field

namespace Kart {

class KartObject;

} // namespace Kart

namespace Item {

using Field::KCLTypeMask;

class ItemTrail;

/// @brief The game's item object kinds: one ItemObjHolder each (ItemDirector +0x48).
enum class ItemObjKind {
    GreenShell = 0,
    RedShell = 1,
    Banana = 2,
    Mushroom = 3,
    Star = 4,
    BlueShell = 5,
    Lightning = 6,
    FakeItemBox = 7,
    MegaMushroom = 8,
    BobOmb = 9,
    Blooper = 10,
    PowBlock = 11,
    GoldenMushroom = 12,
    BulletBill = 13,
    ThunderCloud = 14,
    Count = 15,
    None = 16,
};

/// @brief Per-kind item object properties. The game copies these from its static data into
/// 0x809C2F48 (stride 0x74) at boot (`initItemObjProperties` 0x80790AFC). The values Kinoko uses
/// are written out by hand in ItemObj.cc.
struct ItemObjProperties {
    s32 limit;            ///< +0x04 Most objects of this kind on the course at once.
    s32 capacity;         ///< +0x08 Not the pool size: that is +0x0C (ItemObjHolder::room).
    bool noVanishState;   ///< +0x14 Keeps its own state when it vanishes.
    bool noBreakState;    ///< +0x15 Keeps its own state when it breaks.
    bool noLandState;     ///< +0x16 Keeps its own state when it lands.
    bool noBoxCol;        ///< +0x2C Never in the box collision manager.
    f32 kclRadius;        ///< +0x38 Course collision radius at scale 1.
    f32 trailHitRadius;   ///< +0x3C Hit radius at scale 1 while held behind a kart.
    f32 hitRadius;        ///< +0x40 Hit radius at scale 1.
    f32 trailScale;       ///< +0x44 Scale while held behind a kart.
    f32 scale;            ///< +0x48 Full scale.
    f32 throwScaleStep;   ///< +0x4C Scale growth per frame after a throw.
    f32 scaleStep;        ///< +0x50 Scale growth per frame otherwise.
    f32 landBounce;       ///< +0x54 Scale bounce per unit of landing speed.
    bool survivesHits;    ///< +0x58 Not destroyed by other items while dropped.
    bool breaksOnKartHit; ///< +0x59
    bool reactsToKartHit; ///< +0x5A
    s32 weight;           ///< +0x5C Counts towards the total item limit with this weight.
};

/// @brief Base class of an item on the course (the game's `ItemObj`, constructor 0x8079D8BC).
/// @details Offsets in the comments are the game's. Model, effect and sound members are not
/// ported. States are member function pointers, as in the game (+0x168 and +0x174).
/// @nosubgrouping
class ItemObj {
    friend class Host::Context;
    friend class ItemDirector;
    friend class ItemObjHolder;
    friend class ItemTrail;

public:
    typedef void (ItemObj::*StateFunc)();
    typedef void (ItemObj::*WallFunc)(const EGG::Vector3f &nrm);

    /// @brief +0x74, cleared at the start of every calc.
    enum class eFrameFlags {
        Breaking = 0,     ///< Set when it starts breaking.
        Vanishing = 1,    ///< Set when it starts vanishing.
        Despawned = 2,    ///< Taken off the course this frame.
        Landed = 3,       ///< Touched the floor this frame (throwables).
        Bounced = 4,      ///< Bounced off the floor this frame.
        WallHit = 5,      ///< Hit a wall this frame.
        ScaleChanged = 6, ///< The scale was changed this frame.
        UpSet = 7,        ///< The up vector was taken from the floor this frame.
    };
    typedef EGG::TBitFlag<u32, eFrameFlags> FrameFlags;

    /// @brief +0x78.
    enum class eFlags {
        Trailing = 0,       ///< Held behind its owner.
        OnCourse = 1,       ///< In the holder's active list.
        Dropped = 2,        ///< Dropped (not thrown).
        Landed = 3,         ///< Lying on the floor.
        Settled = 4,        ///< Resting on a moving road object.
        Airborne = 5,       ///< No floor contact for 5 frames.
        Breaking = 6,       ///< Breaking (+0x74 bit 0 the frame it starts).
        Vanishing = 7,      ///< Vanishing.
        RedForward = 8,     ///< A red shell thrown forward.
        ScaleFrozen = 10,   ///< The scale is no longer updated (breaking banana, vanished below 0).
        WallSlide = 11,     ///< Sliding along a wall.
        Contact = 12,       ///< Bounced off another item this frame.
        WallSet = 13,       ///< Touching a wall.
        Unk14 = 14,         ///< Set on a blue shell's throw.
        NoOwner = 15,       ///< Dropped by a hit: counts as nobody's.
        Direct = 16,        ///< Placed by the course (`ItemDirect` objects).
        OwnerImmune = 17,   ///< Cannot hit its owner yet.
        OwnerImmune15 = 18, ///< Owner immunity lasts 15 grounded frames.
        OwnerImmune7 = 19,  ///< Owner immunity lasts 7 frames.
        Unk20 = 20,
        BreakPending = 21,  ///< Break requested online (not ported).
        VanishPending = 22, ///< Vanish requested online (not ported).
        Unk23 = 23,
        Released = 24,    ///< Thrown or dropped by a kart (shell direction set).
        FromKart = 25,    ///< Thrown forward from the kart (not dropped behind).
        DroppedBack = 26, ///< Set by the banana's release.
        DetachedFirst = 27,
        DetachedSecond = 28,
        DetachedThird = 29,
    };
    typedef EGG::TBitFlag<u32, eFlags> Flags;

    /// @brief +0x7C.
    enum class eFlags2 {
        Online = 0,    ///< Online-replicated object (not ported).
        NoHit = 2,     ///< Cannot hit karts.
        Releasing = 3, ///< Blending from the kart's hand into its own motion.
        Unk6 = 6,      ///< Tested by the item-item response; never set in Kinoko.
        InBoxCol = 12,
    };
    typedef EGG::TBitFlag<u32, eFlags2> Flags2;

    ItemObj();
    virtual ~ItemObj();

    void init(u16 globalIdx, u16 idx, ItemObjKind kind);
    void spawn(ItemObjKind kind, u8 owner, const EGG::Vector3f &pos);
    [[nodiscard]] bool calc();

    [[nodiscard]] bool checkKartCollision(const Kart::KartObject *kart, bool preciseCheck);
    void onKartHit(const Kart::KartObject *kart, bool skip);

    void startBreak(bool notify);
    void startVanish(bool notify);
    void setDropVelocity(const EGG::Vector3f &vel, f32 scale);
    void dropFromKart(const EGG::Vector3f &kartPos, const EGG::Vector3f &kartVel,
            const EGG::Vector3f &up, f32 velScale);

    /// @beginGetters
    [[nodiscard]] ItemObjKind kind() const {
        return m_kind;
    }

    [[nodiscard]] u8 owner() const {
        return m_owner;
    }

    /// @brief The player this item's hits are credited to: its owner, or 12 (nobody) once a hit
    /// made its owner drop it.
    [[nodiscard]] u8 hitOwner() const {
        constexpr u8 NO_PLAYER = 12;
        return m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;
    }

    [[nodiscard]] const EGG::Vector3f &pos() const {
        return m_pos;
    }

    [[nodiscard]] const EGG::Vector3f &vel() const {
        return m_vel;
    }

    [[nodiscard]] f32 hitRadius() const {
        return m_hitRadius;
    }

    [[nodiscard]] f32 scale() const {
        return m_scale;
    }

    [[nodiscard]] const Flags &flags() const {
        return m_flags;
    }

    [[nodiscard]] bool isTrailing() const {
        return m_flags.onBit(eFlags::Trailing);
    }

    [[nodiscard]] bool isBreaking() const {
        return m_flags.onBit(eFlags::Breaking, eFlags::Vanishing);
    }

    [[nodiscard]] bool isInBoxCol() const {
        return m_boxColUnit != nullptr;
    }

    /// @addr{0x8079FF14}
    /// @brief A dropped item that cannot survive hits: any hit makes it vanish.
    [[nodiscard]] bool vanishesOnHit() const {
        return m_flags2.offBit(eFlags2::Unk6) && m_flags.onBit(eFlags::Dropped) &&
                !properties().survivesHits;
    }

    [[nodiscard]] const ItemObjProperties &properties() const;
    [[nodiscard]] static const ItemObjProperties &Properties(ItemObjKind kind);
    /// @endGetters

protected:
    // Game vtable, in slot order (base 0x808D19F0).
    virtual void onSpawn() {}                                 ///< +0x18
    [[nodiscard]] virtual bool onRelease(ItemTrail &trail);   ///< +0x20
    [[nodiscard]] virtual s32 onBreak();                      ///< +0x24
    virtual void onDrop() {}                                  ///< +0x28
    virtual void onDespawn() {}                               ///< +0x2C
    virtual void initThrown();                                ///< +0x30
    virtual void initDropped();                               ///< +0x34
    [[nodiscard]] virtual StateFunc initialState() const = 0; ///< Properties +0x18

    void resetForSpawn(ItemObjKind kind);
    void activate();
    void insertBoxCol(bool trailing);
    void removeBoxCol();
    void setScale(f32 scale);
    void setScaleTrailVec(const EGG::Vector3f &kartScale);
    [[nodiscard]] bool growScale(f32 max, f32 step);
    void startRelease(const EGG::Vector3f &kartPos);
    void placeFromObject(const EGG::Vector3f &pos, const EGG::Vector3f &vel);
    void calcRelease();
    void placeInDriverHand(bool forward);
    void calcItemCollision();
    void calcObjectCollision();
    void calcItemPairFree(ItemObj *other);
    void calcItemPair(ItemObj *other);
    [[nodiscard]] s32 itemPairResponse(const ItemObj *other) const;
    void calcItemContact(const EGG::Vector3f &otherPos, EGG::Vector3f delta);
    void calcItemPairResponse(s32 response, const EGG::Vector3f &otherPos,
            const EGG::Vector3f &otherVel);
    void calcKnockedAway(const EGG::Vector3f &pos, const EGG::Vector3f &vel,
            const EGG::Vector3f &dir, bool spin);
    void placeDropped(u32 dropIdx, const EGG::Vector3f &kartPos, const EGG::Vector3f &kartVel,
            const EGG::Vector3f &up);
    [[nodiscard]] bool checkBreakOnWall(const EGG::Vector3f &nrm);
    void slideOnWall(const EGG::Vector3f &nrm);
    [[nodiscard]] bool checkBreakOnContact(const EGG::Vector3f &nrm);
    void checkLanding(const EGG::Vector3f &nrm);
    void mergeWallInfo(const EGG::Vector3f &tangentOff, f32 maxDot);
    void landOnFloor();
    [[nodiscard]] bool checkSphereFull(f32 radius, KCLTypeMask mask);

    void calcBreaking();
    void calcVanishing();
    void startPickup();
    void calcPickedUp();
    void calcDropped();
    void calcLanded();
    void onDroppedWall(const EGG::Vector3f &nrm);

    void setState(StateFunc state) {
        if (!m_flags.onBit(eFlags::Breaking, eFlags::Vanishing)) {
            m_state = state;
        }
    }

    // Offsets are the game's.
    ItemObjKind m_kind;                   ///< +0x04
    u16 m_globalIdx;                      ///< +0x08
    u16 m_idx;                            ///< +0x0A Index in the holder's pool.
    EGG::Quatf m_rot;                     ///< +0x10
    std::array<EGG::Vector3f, 3> m_basis; ///< +0x20 Right, up, forward.
    EGG::Vector3f m_pos;                  ///< +0x44
    EGG::Vector3f m_vel;                  ///< +0x50
    EGG::Vector3f m_scaleVec;             ///< +0x5C
    f32 m_scale;                          ///< +0x68
    u8 m_owner;                           ///< +0x6C
    u8 m_lastHitPlayer;                   ///< +0x6D
    FrameFlags m_frameFlags;              ///< +0x74
    Flags m_flags;                        ///< +0x78
    Flags2 m_flags2;                      ///< +0x7C
    EGG::Vector3f m_releasePos;           ///< +0x8C Where the object is drawn while it is released.
    Field::BoxColUnit *m_boxColUnit;      ///< +0xB0
    f32 m_kclRadius;                      ///< +0xB4
    f32 m_hitRadius;                      ///< +0xB8
    EGG::Vector3f m_prevPos;              ///< +0xBC
    EGG::Vector3f m_up;                   ///< +0xC8
    KCLTypeMask m_kclMask;                ///< +0xD4
    Field::CollisionInfo m_colInfo;       ///< +0xD8
    KCLTypeMask m_landMask;               ///< +0x138
    f32 m_scaleBounce;                    ///< +0x140
    EGG::Vector3f m_landPos;              ///< +0x144
    EGG::Vector3f m_contactNrm;           ///< +0x150
    s16 m_airFrames;                      ///< +0x15C Frames without floor contact, up to 6.
    s16 m_wallTimer;                      ///< +0x15E Vanishes when it runs out off the floor.
    s32 m_frame;                          ///< +0x160 Frames since it was put on the course.
    s32 m_trailFrame;                     ///< +0x164 Frames held behind the kart.
    StateFunc m_state;                    ///< +0x168
    WallFunc m_onWall;                    ///< +0x174
    bool m_vanishBelow;                   ///< +0x185
    s32 m_vanishTimer;                    ///< +0x188
    EGG::Vector3f m_spinVel;              ///< +0x18C
    bool m_spinDir;                       ///< +0x198
    s32 m_breakTimer;                     ///< +0x19C

    /// @brief The point collider for the item-vs-kart convex hull test (0x809C3628).
    static Field::ObjectCollisionPoint s_pointCollision;
};

} // namespace Item

} // namespace Kinoko
