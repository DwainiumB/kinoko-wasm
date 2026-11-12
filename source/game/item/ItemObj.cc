#include "ItemObj.hh"
#include "game/render/KartModel.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemMath.hh"
#include "game/item/ItemObjBlueShell.hh"
#include "game/item/ItemObjBobOmb.hh"
#include "game/item/ItemRaceFlags.hh"
#include "game/item/ItemTrail.hh"

#include "game/field/BoxColManager.hh"
#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectCollisionKart.hh"
#include "game/field/ObjectDirector.hh"

#include "game/kart/KartCollide.hh"
#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"

#include <egg/math/Math.hh>

#include "game/system/RaceManager.hh"

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

/// @brief The game's runtime item object property table (0x809C2F48, stride 0x74), filled at boot
/// from per-kind static data (`initItemObjProperties` 0x80790AFC). Written out by hand from those
/// values; only the members Kinoko uses are kept.
constexpr std::array<ItemObjProperties, static_cast<size_t>(ItemObjKind::Count)> PROPERTIES = {{
        // limit, cap, noVanish, noBreak, noLand, noBoxCol, kclR, trailHitR, hitR, trailScale,
        // scale, throwStep, step, landBounce, survives, breaksOnKart, reactsToKart, weight
        {12, 32, false, false, false, false, 31.0f, 70.0f, 34.0f, 1.0f, 2.1f, 0.03f, 0.03f, 0.002f,
                true, true, true, 1}, // green shell
        {8, 12, false, false, false, false, 31.0f, 70.0f, 34.0f, 1.0f, 2.1f, 0.03f, 0.03f, 0.002f,
                true, true, true, 1}, // red shell
        {16, 48, false, true, false, false, 31.0f, 60.0f, 29.0f, 1.15f, 2.0f, 0.1f, 0.1f, 0.1f,
                true, true, true, 1}, // banana
        {12, 16, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.2f, 0.1f, 0.1f, 0.01f,
                true, false, true, 1}, // mushroom
        {3, 12, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.5f, 0.1f, 0.1f, 0.007f,
                true, false, true, 1}, // star
        {1, 1, false, true, false, false, 25.0f, 50.0f, 29.0f, 1.0f, 1.0f, 0.014f, 0.014f, 0.002f,
                false, true, true, 1}, // blue shell
        {1, 1, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.2f, 0.1f, 0.1f, 0.01f,
                false, false, true, 1}, // lightning
        {6, 6, false, false, false, false, 72.0f, 72.0f, 58.0f, 1.0f, 2.5f, 0.04f, 0.07f, 0.002f,
                true, true, true, 1}, // fake item box
        {2, 2, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.2f, 0.1f, 0.1f, 0.01f,
                false, false, true, 1}, // mega mushroom
        {3, 16, false, true, true, false, 41.0f, 60.0f, 29.0f, 1.3f, 2.2f, 0.1f, 0.1f, 0.002f, true,
                true, true, 1}, // bob-omb
        {1, 1, false, false, false, false, 27.0f, 60.0f, 33.0f, 0.4f, 0.4f, 0.03f, 0.03f, 0.01f,
                false, false, true, 1}, // blooper
        {1, 1, false, false, false, false, 27.0f, 60.0f, 29.0f, 1.0f, 1.0f, 0.07f, 0.07f, 0.01f,
                false, false, true, 1}, // POW
        {2, 6, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.2f, 0.1f, 0.1f, 0.01f,
                false, false, true, 1}, // golden mushroom
        {1, 1, false, false, false, false, 27.0f, 60.0f, 33.0f, 1.0f, 2.1f, 0.3f, 0.3f, 0.01f,
                false, false, true, 1}, // bullet bill
        {1, 1, false, true, false, false, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.04f, 0.04f, 0.01f, false,
                false, false, 1}, // thunder cloud
}};

/// The course KCL types an item rests on or is stopped by (floors, walls, item roads).
constexpr KCLTypeMask KCL_ITEM_SOLID = 0x20E80FFF;
/// The types counted as floor for the airborne timer.
constexpr KCLTypeMask KCL_ITEM_FLOOR_ISH = 0x20E80BFF;
constexpr KCLTypeMask KCL_ITEM_FALL_BOUNDARY = 0x10000;
constexpr KCLTypeMask KCL_ITEM_MOVING_WATER = 0x40000000;

} // namespace

Field::ObjectCollisionPoint ItemObj::s_pointCollision;

/// @addr{0x8079D8BC}
ItemObj::ItemObj()
    : m_kind(ItemObjKind::None), m_globalIdx(0), m_idx(0), m_scale(0.0f), m_owner(0),
      m_lastHitPlayer(12), m_boxColUnit(nullptr), m_kclRadius(0.0f), m_hitRadius(0.0f),
      m_kclMask(KCL_NONE), m_landMask(KCL_NONE), m_scaleBounce(0.0f), m_airFrames(0),
      m_wallTimer(0), m_frame(0), m_trailFrame(0), m_state(nullptr), m_onWall(nullptr),
      m_vanishBelow(false), m_vanishTimer(0), m_spinDir(false), m_breakTimer(0) {
    m_frameFlags.makeAllZero();
    m_flags.makeAllZero();
    m_flags2.makeAllZero();
    m_colInfo.bbox.setZero();
    m_vel.setZero();
}

ItemObj::~ItemObj() = default;

/// @addr{0x8079E224}
void ItemObj::init(u16 globalIdx, u16 idx, ItemObjKind kind) {
    m_kind = kind;
    m_globalIdx = globalIdx;
    m_idx = idx;
    m_frameFlags.makeAllZero();
    m_flags.makeAllZero();
    m_flags2.makeAllZero();
}

/// @addr{0x8079E550}
/// @brief Takes the object out of the pool at a position. It is not on the course (active) until
/// it is released or dropped. Model and online id setup are left out.
void ItemObj::spawn(ItemObjKind kind, u8 owner, const EGG::Vector3f &pos) {
    m_pos = pos;
    m_owner = owner;
    m_lastHitPlayer = 12;
    resetForSpawn(kind);
}

/// @addr{0x8079E5F4}
void ItemObj::resetForSpawn(ItemObjKind kind) {
    constexpr f32 TINY_SCALE = 0.0001f;
    constexpr f32 INITIAL_RADIUS = 0.3f;

    m_kind = kind;
    m_frameFlags.makeAllZero();
    m_flags.makeAllZero();
    m_flags2.makeAllZero();
    m_scaleVec = EGG::Vector3f(TINY_SCALE, TINY_SCALE, TINY_SCALE);
    m_scale = TINY_SCALE;
    m_scaleBounce = 0.0f;
    m_kclRadius = INITIAL_RADIUS;
    m_hitRadius = INITIAL_RADIUS;
    Field::CollisionDirector::Instance()->resetCollisionEntries(&m_kclMask);
    m_colInfo.bbox.setZero();
    m_colInfo.tangentOff.setZero();
    m_colInfo.floorDist = -std::numeric_limits<f32>::min();
    m_colInfo.wallDist = -std::numeric_limits<f32>::min();
    m_colInfo.movingFloorDist = -std::numeric_limits<f32>::min();
    m_colInfo.perpendicularity = 0.0f;
    m_landMask = KCL_NONE;
    m_wallTimer = 0;
    m_airFrames = 5;
    m_state = nullptr;
    m_onWall = nullptr;
    m_boxColUnit = nullptr;
    m_lastHitPlayer = 12;
    m_prevPos = m_pos;
    m_releasePos = m_pos;
    m_basis[0] = EGG::Vector3f::ex;
    m_basis[1] = EGG::Vector3f::ey;
    m_basis[2] = EGG::Vector3f::ez;
    m_up = EGG::Vector3f::ey;
    // The velocity is not reset: a reused object keeps its last one, and an object dropped from a
    // course object or a kart adds its drop velocity to it (seen when a mushroom that fell off the
    // course came back from a wooden box).
    m_frame = 0;
    m_trailFrame = 0;
    m_breakTimer = 0;
    m_flags.setBit(eFlags::OwnerImmune);
    onSpawn();
}

/// @addr{0x8079EFEC}
/// @brief The per-frame update (offline path of the game's holder update). The model, shadow
/// and effect parts are left out.
/// @return Whether the object was taken off the course.
bool ItemObj::calc() {
    constexpr f32 DROP_SCALE_STEP = 0.1f;
    constexpr f32 BOUNCE_SCALE_STEP = 0.18f;
    constexpr f32 BOUNCE_EPSILON = 0.01f;
    constexpr f32 BOUNCE_SPRING = 0.2f;
    constexpr f32 BOUNCE_DAMPING = 0.75f;
    constexpr f32 UP_SMOOTH_LIMIT = 0.98f;
    constexpr f32 UP_SMOOTH_STEP = 0.05f;

    m_frameFlags.makeAllZero();

    if (m_airFrames < 6) {
        ++m_airFrames;
    }

    if (m_flags.offBit(eFlags::Landed)) {
        Field::CollisionDirector::Instance()->resetCollisionEntries(&m_kclMask);
    }

    const auto &props = properties();

    if (m_flags.offBit(eFlags::ScaleFrozen)) {
        bool done = false;
        if (m_scaleBounce != 0.0f) {
            // Still bouncing from a landing: only grow towards the trail scale.
            if (growScale(props.trailScale, BOUNCE_SCALE_STEP)) {
                setScale(m_scale);
                done = true;
            }
        }

        if (!done) {
            if (m_flags.offBit(eFlags::Dropped, eFlags::Landed)) {
                f32 step = m_flags.onBit(eFlags::FromKart) ? props.throwScaleStep : props.scaleStep;
                if (growScale(props.scale, step)) {
                    setScale(m_scale);
                }
            } else {
                bool grew = growScale(props.scale, DROP_SCALE_STEP);
                bool bounced = false;

                if (m_scaleBounce > -BOUNCE_EPSILON && m_scaleBounce < BOUNCE_EPSILON &&
                        m_scaleVec.x - m_scaleVec.y > -BOUNCE_EPSILON &&
                        m_scaleVec.x - m_scaleVec.y < BOUNCE_EPSILON) {
                    m_scaleBounce = 0.0f;
                } else {
                    f32 y = std::max(m_scaleVec.y + m_scaleBounce, BOUNCE_EPSILON);
                    m_frameFlags.setBit(eFrameFlags::ScaleChanged);
                    f32 spring = BOUNCE_SPRING * (m_scale - y);
                    f32 xz = 2.0f * m_scale - y;
                    m_scaleVec = EGG::Vector3f(xz, y, xz);
                    m_scaleBounce = BOUNCE_DAMPING * (m_scaleBounce + spring);
                    m_hitRadius = xz * props.hitRadius;
                    m_kclRadius = y * props.kclRadius;
                    bounced = true;
                }

                if (!bounced && grew) {
                    setScale(m_scale);
                }
            }
        }
    }

    if (m_flags2.offBit(eFlags2::Online) && m_flags2.onBit(eFlags2::Releasing)) {
        calcRelease();
    } else if (m_state) {
        (this->*m_state)();
    }

    if (m_frameFlags.onBit(eFrameFlags::Despawned)) {
        return true;
    }

    if (m_boxColUnit) {
        if (m_flags.offBit(eFlags::Landed)) {
            Field::BoxColFlag flags;
            flags.setBit(Field::eBoxColFlag::Item, Field::eBoxColFlag::Object,
                    Field::eBoxColFlag::Drivable);
            m_boxColUnit->search(flags);
            calcItemCollision();
            calcObjectCollision();
        } else if (m_kind == ItemObjKind::BobOmb) {
            // A bob-omb lying on the floor still meets course objects (the game searches with its
            // two object bits, 0xC).
            Field::BoxColFlag flags;
            flags.setBit(Field::eBoxColFlag::Object);
            m_boxColUnit->search(flags);
            calcObjectCollision();
        }
    }

    if (m_flags.onBit(eFlags::Landed) || (m_kclMask & KCL_ITEM_SOLID)) {
        m_airFrames = 0;

        if (((m_landMask & 0x400) ||
                    ((m_kclMask & 0x400) && m_frameFlags.offBit(eFrameFlags::WallHit))) &&
                m_wallTimer == 0 && m_kind != ItemObjKind::BobOmb) {
            m_wallTimer = m_flags.onBit(eFlags::Landed) ? 180 : 60;
        }

        if (!(m_landMask & 0x20E80DFF) && !(m_kclMask & 0x20E80DFF) && m_wallTimer == 0 &&
                m_kind != ItemObjKind::BobOmb && m_flags.onBit(eFlags::Landed)) {
            m_wallTimer = 180;
        }
    }

    if (m_wallTimer != 0 && m_flags.offBit(eFlags::Breaking, eFlags::Vanishing) &&
            (m_flags.onBit(eFlags::Landed) || !(m_kclMask & KCL_ITEM_FLOOR_ISH))) {
        if (--m_wallTimer == 0) {
            startVanish(true);
        }
    }

    auto *colDir = Field::CollisionDirector::Instance();
    if (m_kclMask & KCL_ITEM_FALL_BOUNDARY) {
        if (colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_FALL_BOUNDARY) &&
                colDir->closestCollisionEntry()->baseType() == 4) {
            startVanish(true);
        }
    }

    if (m_kclMask & KCL_ITEM_MOVING_WATER) {
        if (colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_MOVING_WATER) &&
                colDir->closestCollisionEntry()->baseType() == 6) {
            startVanish(true);
        }
    }

    if (m_pos.y < 0.0f && m_kind != ItemObjKind::ThunderCloud && m_kind != ItemObjKind::Blooper) {
        startVanish(false);
    }

    if (m_airFrames < 5) {
        m_flags.resetBit(eFlags::Airborne);
    } else {
        m_flags.setBit(eFlags::Airborne);
        if (m_up.y < UP_SMOOTH_LIMIT && m_frameFlags.offBit(eFrameFlags::UpSet)) {
            m_up.y += UP_SMOOTH_STEP;
            NormaliseInto(m_up, m_up);
        }
    }

    m_prevPos = m_pos;
    ++m_frame;

    if (m_flags.onBit(eFlags::OwnerImmune)) {
        if (m_flags.onBit(eFlags::OwnerImmune7)) {
            if (m_frame > 7) {
                m_flags.resetBit(eFlags::OwnerImmune);
            }
        } else if (m_flags.onBit(eFlags::OwnerImmune15)) {
            if (m_frame > 15 && m_flags.offBit(eFlags::Airborne)) {
                m_flags.resetBit(eFlags::OwnerImmune);
            }
        } else if (m_frame > 30) {
            m_flags.resetBit(eFlags::OwnerImmune);
        }
    }

    return false;
}

/// @addr{0x807A14D4}
/// @brief Whether this item touches a kart: a sphere test on the kart's bounding radius, then,
/// unless the kart's centre is inside the item, the item's sphere against the kart's convex hull.
/// @param skipPrecise Only the sphere test (the game's third argument).
bool ItemObj::checkKartCollision(const Kart::KartObject *kart, bool skipPrecise) {
    if (!m_boxColUnit) {
        return false;
    }

    if (m_flags.onBit(eFlags::Trailing) && kart->status().onBit(Kart::eStatus::InAction)) {
        return false;
    }

    if (m_flags.onBit(eFlags::OwnerImmune) && m_owner == kart->param()->playerIdx()) {
        return false;
    }

    if (m_flags2.onBit(eFlags2::NoHit)) {
        return false;
    }

    const EGG::Vector3f &kartPos = kart->pos();
    f32 range = kart->collide()->boundingRadius() + m_hitRadius;
    if (!(m_pos.ps_sqDistance(kartPos) < range * range)) {
        return false;
    }

    if (skipPrecise) {
        return true;
    }

    if (m_pos.ps_sqDistance(kartPos) < m_hitRadius * m_hitRadius) {
        return true;
    }

    s_pointCollision.set(m_hitRadius, m_pos);
    return const_cast<Kart::KartObject *>(kart)->objectCollisionKart()->checkCollision(
            s_pointCollision);
}

/// @addr{0x807A3790}
/// @brief This item hit a kart: it is knocked away and breaks.
void ItemObj::onKartHit(const Kart::KartObject *kart, bool skip) {
    const auto &props = properties();
    if (!props.reactsToKartHit || skip) {
        return;
    }

    if (!props.breaksOnKartHit) {
        // 0x807A6EC8: the kart takes it (mushroom, star). The online event is left out.
        startPickup();
        return;
    }

    calcKnockedAway(kart->pos(), kart->velocity(), kart->move()->vel1Dir(), true);
    m_lastHitPlayer = static_cast<u8>(kart->param()->playerIdx());
    startBreak(true);
}

/// @addr{0x807A6738}
/// @brief Sets the velocity of an item knocked away by a kart.
void ItemObj::calcKnockedAway(const EGG::Vector3f &pos, const EGG::Vector3f &vel,
        const EGG::Vector3f &dir, bool spin) {
    constexpr f32 KART_VEL_SCALE = 0.55f;
    constexpr f32 SIDE_SPEED = 15.0f;
    constexpr f32 LANDED_VEL_BLEND = 0.4f;
    constexpr f32 LANDED_SIDE_SPEED = 7.0f;
    constexpr f32 LANDED_SIDE_RANDOM = 2.0f;

    // The sideways direction: the kart's direction turned by 90 degrees, away from the item.
    f32 sideX = -dir.x;
    f32 sideZ = dir.z;
    EGG::Vector3f diff = m_pos - pos;
    if (sideZ * diff.x + sideX * diff.z <= 0.0f) {
        sideZ = -sideZ;
        sideX = -sideX;
    }

    if (m_flags.offBit(eFlags::Landed)) {
        m_spinVel = EGG::Vector3f(sideZ * SIDE_SPEED, 0.0f, sideX * SIDE_SPEED);
        m_vel = vel * KART_VEL_SCALE + m_spinVel;
    } else {
        m_vel += (vel - m_vel) * LANDED_VEL_BLEND;
        f32 side = LANDED_SIDE_SPEED + ItemRaceFlags::Random().getF32(LANDED_SIDE_RANDOM);
        m_spinVel = EGG::Vector3f(sideZ * side, 0.0f, sideX * side);
        m_vel += m_spinVel;
    }

    m_spinDir = spin;
}

/// @addr{0x807A6560}
/// @brief Starts breaking (shell hit, banana hit). Effects and the online event are left out.
void ItemObj::startBreak(bool /*notify*/) {
    constexpr s32 BREAK_FRAMES = 20;

    if (m_flags.onBit(eFlags::Breaking, eFlags::Vanishing)) {
        return;
    }

    // 0x807A6328: the object's own reaction (+0x24) decides whether it gets the break state.
    m_breakTimer = BREAK_FRAMES;
    s32 reaction = onBreak();
    if (reaction != 2) {
        return;
    }

    if (!properties().noBreakState) {
        m_state = &ItemObj::calcBreaking;
    }

    m_onWall = nullptr;
    m_frameFlags.setBit(eFrameFlags::Breaking);
    m_flags.setBit(eFlags::Breaking);
    removeBoxCol();
}

/// @addr{0x807A6C14}
/// @brief Starts vanishing (out of the course, stuck in a wall too long, too many items).
void ItemObj::startVanish(bool /*notify*/) {
    constexpr f32 VANISH_HEIGHT = 0.0f;

    if (m_flags.onBit(eFlags::Breaking, eFlags::Vanishing)) {
        return;
    }

    if (!properties().noVanishState) {
        m_state = &ItemObj::calcVanishing;
    }

    m_frameFlags.setBit(eFrameFlags::Vanishing);
    m_flags.setBit(eFlags::Vanishing);
    removeBoxCol();

    m_vanishBelow = m_pos.y < VANISH_HEIGHT;
    if (m_vanishBelow) {
        m_flags.setBit(eFlags::ScaleFrozen);
    }

    m_vanishTimer = m_vanishBelow ? 30 : 6;
}

/// @addr{0x807A6D88}
/// @brief A kart took this item: it goes away on its next frame. The online path (+0x7C bit 5)
/// is left out.
void ItemObj::startPickup() {
    // 0x8079D920 is visual.
    if (m_flags.onBit(eFlags::Breaking, eFlags::Vanishing)) {
        return;
    }

    m_state = &ItemObj::calcPickedUp;
    m_frameFlags.setBit(eFrameFlags::Vanishing);
    m_flags.setBit(eFlags::Vanishing);
}

/// @addr{0x807A6F44}
void ItemObj::calcPickedUp() {
    ItemDirector::Instance()->holder(m_kind).despawn(this);
}

/// @addr{0x807A6700}
void ItemObj::calcBreaking() {
    if (--m_breakTimer > 0) {
        return;
    }

    ItemDirector::Instance()->holder(m_kind).despawn(this);
}

/// @addr{0x807A6C90}
void ItemObj::calcVanishing() {
    constexpr f32 MIN_SCALE = 0.001f;

    if (m_vanishBelow) {
        m_pos += m_vel;
        f32 timer = static_cast<f32>(m_vanishTimer);
        m_scale = std::max(MIN_SCALE, m_scale * ((timer - 1.0f) / timer));
        setScale(m_scale);
    }

    if (--m_vanishTimer < 1) {
        ItemDirector::Instance()->holder(m_kind).despawn(this);
    }
}

/// @addr{0x807A0FE8}
/// @brief Base reaction to breaking: take the break state (2).
s32 ItemObj::onBreak() {
    return 2;
}

/// @addr{0x8079DCB4}
bool ItemObj::onRelease(ItemTrail & /*trail*/) {
    return true;
}

/// @addr{0x8079DEE4}
void ItemObj::initThrown() {}

/// @addr{0x8079E1F0}
void ItemObj::initDropped() {}

/// @addr{0x8079EE30}
void ItemObj::insertBoxCol(bool trailing) {
    constexpr f32 MAX_SPEED = 100.0f;

    const auto &props = properties();
    f32 radius = props.scale * (trailing ? props.trailHitRadius : props.hitRadius);
    m_boxColUnit =
            Field::BoxColManager::Instance()->insertItem(radius, MAX_SPEED, &m_pos, true, this);
}

/// @addr{0x8079EECC}
void ItemObj::removeBoxCol() {
    if (!m_boxColUnit) {
        return;
    }

    Field::BoxColManager::Instance()->remove(m_boxColUnit);
    m_flags2.resetBit(eFlags2::InBoxCol);
}

/// @addr{0x8079ED4C}
void ItemObj::setScale(f32 scale) {
    const auto &props = properties();
    m_frameFlags.setBit(eFrameFlags::ScaleChanged);
    m_scaleVec = EGG::Vector3f(scale, scale, scale);
    m_hitRadius = scale * props.hitRadius;
    m_kclRadius = scale * props.kclRadius;
}

/// @addr{0x8079EDB4}
/// @brief A held item's scale is the kart's scale times its own growing scale, and its collision
/// follows it: a banana held at its full 1.15 hovers 1.15 times as high, and an item that has just
/// appeared (scale 0.0001) sits almost on the trail anchor.
void ItemObj::setScaleTrailVec(const EGG::Vector3f &kartScale) {
    const auto &props = properties();
    m_scaleVec = kartScale * m_scale;
    m_frameFlags.setBit(eFrameFlags::ScaleChanged);
    m_hitRadius = m_scaleVec.z * props.trailHitRadius;
    m_kclRadius = m_scaleVec.y * props.kclRadius;
}

/// @brief The velocity and scale an object dropped by a hit gets when it is spawned
/// (0x807BC350).
void ItemObj::setDropVelocity(const EGG::Vector3f &vel, f32 scale) {
    m_vel = vel;
    m_scale = scale;
    m_scaleVec = EGG::Vector3f(scale, scale, scale);
}

/// @addr{0x807BC170}
/// @brief The object part of dropping an inventory item: it starts at the kart and falls.
void ItemObj::dropFromKart(const EGG::Vector3f &kartPos, const EGG::Vector3f &kartVel,
        const EGG::Vector3f &up, f32 velScale) {
    startRelease(kartPos);
    m_vel += kartVel * velScale;
    m_pos = kartPos;
    // 0x807A2FBC: the model basis from the kart's facing (visual).
    placeDropped(0, kartPos, kartVel, up);
    onDrop();
}

/// @addr{0x8079ED1C}
bool ItemObj::growScale(f32 max, f32 step) {
    if (!(m_scale < max)) {
        return false;
    }

    m_scale = std::min(m_scale + step, max);
    return true;
}

/// @addr{0x8079E9E8}
void ItemObj::startRelease(const EGG::Vector3f &kartPos) {
    m_prevPos = kartPos;
    m_up = EGG::Vector3f::ey;
    m_flags.setBit(eFlags::FromKart);
}

/// @brief The object part of an item dropped by a course object (0x8079A12C): it starts at pos,
/// small, and is thrown out with vel.
void ItemObj::placeFromObject(const EGG::Vector3f &pos, const EGG::Vector3f &vel) {
    constexpr f32 SCALE = 0.7f;

    startRelease(pos);
    // 0x807A3474: the model basis (visual); it also clears the release flag.
    m_flags.resetBit(eFlags::Released);
    m_scale = SCALE;
    m_scaleVec = EGG::Vector3f(SCALE, SCALE, SCALE);
    placeDropped(0, pos, EGG::Vector3f::zero, EGG::Vector3f::ey);
    m_vel += vel;
    m_flags.setBit(eFlags::OwnerImmune7);
}

/// @addr{0x807A25B0}
/// @brief The first frames after a release. A forward throw sits in the driver's hand
/// (0x807D171C, the "throw_f" anchor) on frames 0 and 1; a drop keeps following the kart on those
/// frames and is placed at the "throw_b" anchor on frame 2, where the state starts. Without
/// anchor data (no Race/Kart/driverItemAnchors.bin) the kart's position is used instead. The
/// drawing position blend (+0x8C) is visual and left out, except for ending the release after
/// 44 frames.
void ItemObj::calcRelease() {
    constexpr s32 RELEASE_FRAMES = 4;
    constexpr s32 RELEASE_END = 0x2B;

    s32 frame = m_frame - RELEASE_FRAMES + 2;
    if (frame < 0) {
        const auto &props = properties();
        f32 t = std::min(static_cast<f32>(m_frame) / 3.0f, 1.0f);
        f32 scale = props.trailScale * (0.5f + t * 0.5f);
        m_scale = scale;
        setScale(scale);

        if (m_flags.offBit(eFlags::FromKart)) {
            const auto &kartItem = ItemDirector::Instance()->kartItem(m_owner);
            m_pos += kartItem.posDelta();
        } else {
            placeInDriverHand(true);
        }

        return;
    }

    if (m_flags.offBit(eFlags::FromKart) && frame == 0) {
        placeInDriverHand(false);
    }

    if (m_state) {
        (this->*m_state)();
    }

    if (frame > RELEASE_END) {
        m_flags2.resetBit(eFlags2::Releasing);
    }
}

/// @brief Puts the item where the driver's hand holds it out (0x807D171C, through
/// `KartModel::itemHandPosition`), or at the kart when there is no anchor data.
void ItemObj::placeInDriverHand(bool forward) {
    const auto &props = properties();
    const auto *kart = ItemDirector::Instance()->kartItem(m_owner).kartObject();
    if (!kart->model()->itemHandPosition(forward, props.scale * props.hitRadius, m_pos,
                m_prevPos)) {
        m_pos = kart->pos();
        m_prevPos = kart->pos();
    }
}

/// @addr{0x807A1C94}
/// @brief Item-vs-item collision: every item found by the box collision search is tested against
/// this one and both get the response the kind table gives (bounce, break or nothing).
void ItemObj::calcItemCollision() {
    auto *boxColMgr = Field::BoxColManager::Instance();

    while (ItemObj *other = boxColMgr->getNextItem()) {
        if (m_flags.offBit(eFlags::Trailing)) {
            if (other != this) {
                calcItemPairFree(other);
            }
        } else {
            // A held item against anything with a hitbox, except its owner's immune items; two held
            // items are handled once, by the lower index.
            if (!other->m_boxColUnit ||
                    (other->m_flags.onBit(eFlags::OwnerImmune) && m_owner == other->m_owner)) {
                continue;
            }

            if (other->m_flags.offBit(eFlags::Trailing) || m_globalIdx <= other->m_globalIdx ||
                    other->m_flags2.onBit(eFlags2::Online)) {
                calcItemPair(other);
            }
        }

        if (!m_boxColUnit) {
            return;
        }
    }
}

/// @addr{0x807A1ED8}
/// @brief Item-vs-course-object collision: the course objects this item touches (found by the box
/// collision search before) react, and the item bounces off them like off a wall or is knocked
/// aside and breaks, as the item hit tables say. The item is then pushed out of every object by the
/// largest overlap in each direction.
void ItemObj::calcObjectCollision() {
    constexpr f32 KNOCK_SIDE_DIST = 200.0f; ///< 0x808A5268
    constexpr f32 KNOCK_VEL_SCALE = -0.25f; ///< 0x808A526C

    if (!m_boxColUnit) {
        return;
    }

    EGG::Vector3f pushMin = EGG::Vector3f::zero;
    EGG::Vector3f pushMax = EGG::Vector3f::zero;

    auto *objDir = Field::ObjectDirector::Instance();
    size_t count = objDir->checkItemObjectCollision(m_hitRadius, this, m_pos);
    if (count != 0 && vanishesOnHit()) {
        startVanish(false);
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        s32 reaction = objDir->itemReaction(i);
        const EGG::Vector3f &depth = objDir->hitDepth(i);

        if (m_flags.offBit(eFlags::Breaking) && reaction != Field::ItemHit::None) {
            // VEC3Minimize / VEC3Maximize (0x800855C0 / 0x80085580): fsel on the difference.
            auto minimum = [](f32 a, f32 b) { return b - a >= 0.0f ? a : b; };
            auto maximum = [](f32 a, f32 b) { return a - b >= 0.0f ? a : b; };
            pushMin = EGG::Vector3f(minimum(pushMin.x, depth.x), minimum(pushMin.y, depth.y),
                    minimum(pushMin.z, depth.z));
            pushMax = EGG::Vector3f(maximum(pushMax.x, depth.x), maximum(pushMax.y, depth.y),
                    maximum(pushMax.z, depth.z));
        }

        if (reaction == Field::ItemHit::Bounce) {
            // Bounce off a point just behind the previous position, as if it were another item.
            EGG::Vector3f objPos(m_prevPos.x - depth.x, m_prevPos.y, m_prevPos.z - depth.z);
            EGG::Vector3f objVel = objDir->collidingObjectTranslation(i);
            calcItemPairResponse(1, objPos, objVel);
            m_flags.resetBit(eFlags::OwnerImmune);
        } else if (reaction == Field::ItemHit::Break) {
            // Knocked aside from a point beside the item, on the side away from the object, and
            // slowed to a quarter, reversed.
            EGG::Vector3f side(m_vel.z, 0.0f, -m_vel.x);
            if (side.ps_dot(depth) > 0.0f) {
                side *= -1.0f;
            }
            side.y = 0.0f;
            NormaliseInto(side, side);
            side = side * KNOCK_SIDE_DIST + m_pos;

            if (m_vel.y > 0.0f) {
                m_vel.y = 0.0f;
            }
            m_vel.x *= KNOCK_VEL_SCALE;
            m_vel.z *= KNOCK_VEL_SCALE;

            calcItemPairResponse(2, side, -m_vel);
        }

        if (!m_boxColUnit) {
            break;
        }
    }

    m_pos += pushMin + pushMax;
}

/// @addr{0x807A0E28}
/// @brief A free item against another free item: the pair is handled once while both fly (by the
/// lower index) and from the flying one's side once the other has landed.
void ItemObj::calcItemPairFree(ItemObj *other) {
    if (!other->m_boxColUnit || other->m_flags.onBit(eFlags::Trailing)) {
        return;
    }

    if (other->m_flags.offBit(eFlags::Landed) && other->m_globalIdx < m_globalIdx) {
        return;
    }

    if (m_flags2.onBit(eFlags2::Online) && other->m_flags2.onBit(eFlags2::Online) &&
            itemPairResponse(other) != 1 && other->itemPairResponse(this) != 1) {
        return;
    }

    calcItemPair(other);
}

/// @brief The hit test and the responses shared by 0x807A1C94 and 0x807A0E28: the two spheres
/// now, or both spheres against the other's previous position (a fast pass through).
void ItemObj::calcItemPair(ItemObj *other) {
    auto overlaps = [](const ItemObj *a, const EGG::Vector3f &aPos, const ItemObj *b,
                            const EGG::Vector3f &bPos) {
        if (a->m_flags2.onBit(eFlags2::NoHit)) {
            return false;
        }
        f32 range = b->m_hitRadius + a->m_hitRadius;
        return aPos.ps_sqDistance(bPos) < range * range;
    };

    bool hit = overlaps(this, m_pos, other, other->m_pos);
    if (!hit) {
        hit = overlaps(this, m_pos, other, other->m_prevPos) &&
                overlaps(other, other->m_pos, this, m_prevPos);
    }

    if (!hit) {
        return;
    }

    EGG::Vector3f velBefore = m_vel;
    s32 response = itemPairResponse(other);
    s32 otherResponse = other->itemPairResponse(this);
    calcItemPairResponse(response, other->m_pos, other->m_vel);
    other->calcItemPairResponse(otherResponse, m_pos, velBefore);
    // 0x807AB388, the red shell's hook for a response, is empty.
}

/// @addr{0x807A139C}
/// @brief What this item does when it meets `other`: 0 nothing, 1 bounce, 2 break, from a table by
/// kind, except against an exploding bob-omb or blue shell.
s32 ItemObj::itemPairResponse(const ItemObj *other) const {
    // 0x808A4DFC, rows = this kind, columns = the other kind, in holder order (green, red, banana,
    // mushroom, star, blue, lightning, fake item box, mega, bob-omb, blooper, POW, golden, bullet,
    // thunder cloud).
    static constexpr s32 RESPONSES[15][15] = {
            {2, 2, 2, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0},
            {2, 2, 2, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0},
            {2, 2, 1, 1, 1, 2, 0, 1, 0, 2, 0, 0, 0, 0, 0},
            {1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0},
            {1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0},
            {2, 2, 2, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {2, 2, 2, 1, 1, 2, 0, 1, 0, 2, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    };

    s32 response = RESPONSES[static_cast<size_t>(m_kind)][static_cast<size_t>(other->m_kind)];

    bool exploding = false;
    bool weakened = false;
    if (other->m_kind == ItemObjKind::BobOmb) {
        const auto *bomb = static_cast<const ItemObjBobOmb *>(other);
        weakened = bomb->explosionWeakened();
        exploding = bomb->isExploding();
    } else if (other->m_kind == ItemObjKind::BlueShell) {
        const auto *blue = static_cast<const ItemObjBlueShell *>(other);
        weakened = blue->explosionWeakened();
        exploding = blue->isExploding();
    }

    if (exploding) {
        response = (!weakened && m_flags.offBit(eFlags::Dropped)) ? 2 : 0;
        // Team races: items of the same team do not break each other. Kinoko has no team mode
        // (ItemRaceFlags::s_teams is never set), so this is left as a note.
    }

    return response;
}

/// @addr{0x807A1680}
/// @brief The bounce between two items in contact: a random sideways kick on the first contact
/// of the frame, then the contact normal follows the item's wall normal.
void ItemObj::calcItemContact(const EGG::Vector3f &otherPos, EGG::Vector3f delta) {
    constexpr f32 ALIGNED = 0.5f;
    constexpr f32 KICK_SPEED = 13.0f;
    constexpr f32 COUNTER_TO_FIDX = 0.00390625f;

    if (m_flags.onBit(eFlags::Contact)) {
        NormaliseInto(delta, delta);
        if (delta.ps_dot(m_contactNrm) > ALIGNED) {
            m_vel.z = KICK_SPEED * delta.z;
            m_vel.x = KICK_SPEED * delta.x;
        }
    }

    bool hit = false;
    if (m_flags2.offBit(eFlags2::NoHit)) {
        f32 range = 0.0f + m_hitRadius;
        hit = m_pos.ps_sqDistance(otherPos) < range * range;
    }

    if (hit) {
        m_flags.setBit(eFlags::WallSet);
        if (m_flags.offBit(eFlags::Contact)) {
            if (m_vel.ps_dot(delta) < 0.0f) {
                u16 turn = static_cast<u16>(ItemRaceFlags::Random().getU32(0x10000));
                auto [sin, cos] = EGG::Mathf::SinCosFIdx(COUNTER_TO_FIDX * static_cast<f32>(turn));
                m_contactNrm.x = sin;
                m_contactNrm.z = cos;
                m_vel.z = KICK_SPEED * m_contactNrm.z;
                m_vel.x = KICK_SPEED * m_contactNrm.x;
                m_flags.setBit(eFlags::Contact);
            }
        }
    }

    if (m_flags.onBit(eFlags::Contact)) {
        if (m_colInfo.wallDist > -std::numeric_limits<f32>::min()) {
            // 0x807A1864: the wall normal flattened, into the contact normal's x and z.
            EGG::Vector3f flat = m_colInfo.wallNrm;
            flat.y = 0.0f;
            f32 length = EGG::Mathf::sqrt(flat.ps_squareMag());
            if (length > 0.0f) {
                f32 inv = 1.0f / length;
                m_contactNrm.x = m_colInfo.wallNrm.x * inv;
                m_contactNrm.z = m_colInfo.wallNrm.z * inv;
            }
        }
    }
}

/// @addr{0x807A6F74}
/// @brief Places an item dropped by a kart that was hit.
void ItemObj::placeDropped(u32 dropIdx, const EGG::Vector3f &kartPos, const EGG::Vector3f &kartVel,
        const EGG::Vector3f &up) {
    constexpr f32 MAX_UP_SPEED = 1.2f;
    constexpr f32 UP_SPEED_SURVIVOR = 25.0f;
    constexpr f32 UP_SPEED = 35.0f;
    constexpr f32 SPREAD_SPEED = 13.0f;

    bool survives = properties().survivesHits;
    m_flags.setBit(eFlags::Dropped, eFlags::NoOwner);
    m_vel = m_vel - up * m_vel.ps_dot(up);

    f32 upSpeed = std::min(std::max(0.0f, kartVel.ps_dot(up)), MAX_UP_SPEED);
    upSpeed += survives ? UP_SPEED_SURVIVOR : UP_SPEED;
    m_vel += up * upSpeed;

    if (dropIdx != 0) {
        EGG::Vector3f away(0.0f, 0.0f, 0.0f);
        NormaliseInto(away, m_pos - kartPos);
        m_vel += away * (SPREAD_SPEED * static_cast<f32>(dropIdx));
    }

    setState(&ItemObj::calcDropped);
    m_onWall = &ItemObj::onDroppedWall;
    m_flags.resetBit(eFlags::WallSlide, eFlags::Contact, eFlags::WallSet);
    if (!survives) {
        m_flags.setBit(eFlags::Released);
    }
}

/// @addr{0x807A7350}
/// @brief A dropped item flies up and falls until it lands. Moving roads are not ported (the
/// game takes the road's velocity there); the model orientation is left out.
void ItemObj::calcDropped() {
    constexpr f32 DRAG = 0.995f;
    constexpr f32 FLOOR_NORMAL_Y = 0.02f;
    constexpr f32 WALL_VANISH_Y = 0.98f;
    constexpr f32 GRAVITY = 2.2f;
    constexpr f32 MAX_FALL = -100.0f;

    bool survives = properties().survivesHits;
    m_pos += m_vel;
    // Only the horizontal speed is slowed.
    m_vel.x *= DRAG;
    m_vel.z *= DRAG;

    bool wall = false;
    if (checkSphereFull(m_kclRadius, 0x6CEBDFFF)) {
        m_pos += m_colInfo.tangentOff;

        if (!survives && (m_kclMask & 0xF0F8FFFF)) {
            startVanish(false);
            return;
        }

        auto *colDir = Field::CollisionDirector::Instance();
        if ((m_kclMask & KCL_ITEM_SOLID) &&
                colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_SOLID)) {
            if (m_colInfo.floorNrm.y >= FLOOR_NORMAL_Y) {
                checkLanding(m_colInfo.floorNrm);
            } else {
                wall = true;
                mergeWallInfo(m_colInfo.tangentOff, 1.0f);
            }
        }

        if ((m_kclMask & 0xD010F000) || wall) {
            if (m_colInfo.wallNrm.y >= WALL_VANISH_Y) {
                startVanish(true);
                return;
            }

            m_frameFlags.setBit(eFrameFlags::WallHit);
            slideOnWall(m_colInfo.wallNrm);
            if (checkBreakOnContact(m_colInfo.wallNrm)) {
                return;
            }
        }

        if (m_kclMask & 0x20000) {
            startVanish(true);
            return;
        }
    }

    m_flags.resetBit(eFlags::WallSet);
    if (!(m_kclMask & KCL_ITEM_SOLID)) {
        m_vel.y = std::max(m_vel.y - GRAVITY, MAX_FALL);
    }

    if (m_frameFlags.onBit(eFrameFlags::Landed) &&
            m_frameFlags.offBit(eFrameFlags::Breaking, eFrameFlags::Vanishing)) {
        landOnFloor();
    }
}

/// @brief The wall part of a sphere check that found a steep "floor": counts it as a wall.
void ItemObj::mergeWallInfo(const EGG::Vector3f &tangentOff, f32 maxDot) {
    EGG::Vector3f nrm = tangentOff;
    f32 dist = nrm.normalise();
    m_colInfo.bbox.min = m_colInfo.bbox.min.minimize(tangentOff);
    m_colInfo.bbox.max = m_colInfo.bbox.max.maximize(tangentOff);

    if (m_colInfo.wallDist > -std::numeric_limits<f32>::min()) {
        f32 perp = maxDot - m_colInfo.wallNrm.ps_dot(m_colInfo.floorNrm);
        if (m_colInfo.perpendicularity < perp) {
            m_colInfo.perpendicularity = std::min(perp, maxDot);
        }
    }

    if (m_colInfo.wallDist < dist) {
        m_colInfo.wallDist = dist;
        m_colInfo.wallNrm = m_colInfo.floorNrm;
    }

    m_colInfo.tangentOff = m_colInfo.bbox.min + m_colInfo.bbox.max;
}

/// @addr{0x807A234C}
/// @brief A short sphere check below the item: when it finds floor, the item lands this frame.
void ItemObj::checkLanding(const EGG::Vector3f &nrm) {
    constexpr f32 PROBE_RADIUS = 0.01f;

    EGG::Vector3f probe = m_pos - nrm * m_kclRadius;
    KCLTypeMask mask = KCL_NONE;
    if (Field::CollisionDirector::Instance()->checkSpherePartial(PROBE_RADIUS * m_kclRadius, probe,
                m_pos, KCL_ITEM_SOLID, nullptr, &mask, 0)) {
        if (m_flags.offBit(eFlags::WallSet)) {
            m_frameFlags.setBit(eFrameFlags::Landed);
            m_up = nrm;
        }
    }
}

/// @addr{0x807A2E68}
/// @brief Touching a wall from the other side of an item it already bounced off breaks it.
bool ItemObj::checkBreakOnContact(const EGG::Vector3f &nrm) {
    if (m_flags.onBit(eFlags::Contact) && nrm.ps_dot(m_contactNrm) < 0.0f &&
            m_frameFlags.offBit(eFrameFlags::Landed)) {
        startBreak(true);
        return true;
    }

    return false;
}

/// @addr{0x807A784C}
void ItemObj::onDroppedWall(const EGG::Vector3f &nrm) {
    slideOnWall(nrm);
}

/// @addr{0x807A2270}
/// @brief Removes the velocity into a wall and pushes the item off it (0x807DE934).
void ItemObj::slideOnWall(const EGG::Vector3f &wallNrm) {
    constexpr f32 MIN_PUSH = 6.75f;
    constexpr f32 PUSH_PER_Y = 23.25f;
    constexpr f32 KEEP = 0.7f;
    constexpr f32 MAX_PUSH = 30.0f;
    constexpr f32 BOUNCE = 0.45f;

    f32 minPush = MIN_PUSH;
    if (m_vel.x * wallNrm.x + m_vel.z * wallNrm.z > 0.0f) {
        minPush =
                std::min(std::max(MIN_PUSH + (PUSH_PER_Y * wallNrm.y) / KEEP, MIN_PUSH), MAX_PUSH);
    }

    EGG::Vector3f nrm = wallNrm;
    if (nrm.y > 0.0f) {
        nrm.y = 0.0f;
        if (!(nrm.normalise() > 0.0f)) {
            return;
        }
    }

    f32 dot = m_vel.ps_dot(nrm);
    EGG::Vector3f proj = nrm * dot;
    EGG::Vector3f out = (m_vel - proj) * KEEP;
    EGG::Vector3f push = proj * -BOUNCE;
    if (push.x * push.x + push.z * push.z < minPush * minPush) {
        push = nrm * minPush;
    }

    m_vel = out + push;
}

/// @addr{0x807B6910}
/// @brief The item comes to rest on the floor.
void ItemObj::landOnFloor() {
    constexpr f32 MAX_BOUNCE = -0.36f;

    m_landMask = m_kclMask;
    m_landPos = m_pos - m_up * m_kclRadius;
    m_flags.resetBit(eFlags::Dropped);
    m_flags.setBit(eFlags::Landed);

    if (!properties().noLandState && m_flags.offBit(eFlags::Breaking, eFlags::Vanishing)) {
        m_state = &ItemObj::calcLanded;
    }

    m_onWall = nullptr;
    m_scaleBounce = std::max(-EGG::Mathf::abs(m_vel.y) * properties().landBounce, MAX_BOUNCE);
    m_vel.setZero();
    ItemDirector::Instance()->holder(m_kind).markLanded(this);

    if (m_boxColUnit) {
        // 0x807B6B98: a resting item stops being recalculated every frame.
        m_boxColUnit->resize(m_hitRadius, 0.0f);
    }
}

/// @addr{0x807B6BFC}
/// @brief An item lying on the floor. Moving roads are not ported: it stays where it landed.
void ItemObj::calcLanded() {
    if (m_frameFlags.onBit(eFrameFlags::ScaleChanged)) {
        m_pos = m_landPos + m_up * m_kclRadius;
    }
}

/// @brief The game's `CourseModel::checkSphereFullPush` with the object's collision info.
bool ItemObj::checkSphereFull(f32 radius, KCLTypeMask mask) {
    m_colInfo.bbox.setZero();
    m_colInfo.floorDist = -std::numeric_limits<f32>::min();
    m_colInfo.wallDist = -std::numeric_limits<f32>::min();
    m_colInfo.movingFloorDist = -std::numeric_limits<f32>::min();
    m_colInfo.perpendicularity = 0.0f;
    return Field::CollisionDirector::Instance()->checkSphereFullPush(radius, m_pos, m_prevPos, mask,
            &m_colInfo, &m_kclMask, 0);
}

bool ItemObj::checkBreakOnWall(const EGG::Vector3f & /*nrm*/) {
    return false;
}

const ItemObjProperties &ItemObj::properties() const {
    return Properties(m_kind);
}

const ItemObjProperties &ItemObj::Properties(ItemObjKind kind) {
    ASSERT(kind < ItemObjKind::Count);
    return PROPERTIES[static_cast<size_t>(kind)];
}

void ItemObj::activate() {}

/// @addr{0x807A18FC}
/// @brief Applies an item-item response: dropped items that cannot survive hits vanish; 1 keeps
/// the two apart (a wall-like bounce), 2 knocks this item away and breaks it.
void ItemObj::calcItemPairResponse(s32 response, const EGG::Vector3f &otherPos,
        const EGG::Vector3f &otherVel) {
    constexpr f32 BREAK_KICK = 15.0f;

    if (m_flags.onBit(eFlags::Breaking)) {
        return;
    }

    if (m_flags2.offBit(eFlags2::Unk6) && m_flags.onBit(eFlags::Dropped) &&
            !properties().survivesHits) {
        startVanish(false);
        return;
    }

    if (response == 1) {
        if (m_flags.onBit(eFlags::Trailing)) {
            return;
        }

        EGG::Vector3f delta = m_prevPos - otherPos;
        if (m_flags.offBit(eFlags::Landed) && m_flags.onBit(eFlags::WallSlide)) {
            calcItemContact(otherPos, delta);
            return;
        }

        m_flags.setBit(eFlags::WallSet);
        if (m_vel.ps_dot(delta) <= 0.0f) {
            // Moving into the other item: bounce like off a wall, then out to the hit radius.
            NormaliseInto(delta, delta);
            if (m_onWall) {
                (this->*m_onWall)(delta);
            }

            if (m_flags.onBit(eFlags::Landed)) {
                return;
            }

            if (m_pos.ps_sqDistance(otherPos) < m_hitRadius * m_hitRadius) {
                m_pos.x = otherPos.x + delta.x * m_hitRadius;
                m_pos.z = otherPos.z + delta.z * m_hitRadius;
            }
        } else {
            if (m_flags.onBit(eFlags::Landed)) {
                return;
            }

            if (m_pos.ps_sqDistance(otherPos) < m_hitRadius * m_hitRadius) {
                NormaliseInto(delta, delta);
                m_pos.x = otherPos.x + delta.x * m_hitRadius;
                m_pos.z = otherPos.z + delta.z * m_hitRadius;
            }
        }
    } else if (response == 2) {
        if (m_flags.onBit(eFlags::Trailing)) {
            ItemDirector::Instance()->kartItem(m_owner).trail().detach(this);
        }

        EGG::Vector3f dir = EGG::Vector3f::zero;
        f32 speed = EGG::Mathf::sqrt(otherVel.ps_squareMag());
        if (speed > 0.0f) {
            dir = otherVel;
        }

        if (speed == 0.0f) {
            // The game reads an unset direction here; a still item never hits, so it does not
            // matter.
            calcKnockedAway(otherPos, otherVel, dir, false);
        } else {
            EGG::Vector3f away = m_pos - otherPos;
            if (away.y < 0.0f) {
                away.y = 0.0f;
            }
            NormaliseInto(dir, away);
            EGG::Vector3f kick = dir * BREAK_KICK;
            calcKnockedAway(otherPos, kick, dir, false);
        }

        startBreak(true);
    }
}

} // namespace Kinoko::Item
