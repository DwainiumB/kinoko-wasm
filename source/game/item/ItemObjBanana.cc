#include "ItemObjBanana.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemRaceFlags.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/CollisionDirector.hh"

#include "game/kart/KartObject.hh"
#include "game/system/RaceManager.hh"

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

constexpr KCLTypeMask KCL_ITEM_SOLID = 0x20E80FFF;

} // namespace

ItemObjThrowable::ItemObjThrowable() : m_bouncesLeft(0.0f) {}

ItemObjThrowable::~ItemObjThrowable() = default;

/// @addr{0x807B7104}
/// @brief Throws the item forward in an arc, or drops it behind the kart.
void ItemObjThrowable::initThrow(f32 forwardSpeed, f32 upSpeed, f32 backUpSpeed, ItemTrail &trail,
        u32 bounces, bool forward) {
    constexpr f32 BACK_SPEED_PER_DELTA = 0.15f;
    constexpr f32 BACK_MIN_SPEED = 14.0f;
    constexpr f32 DELTA_SCALE = 1.0f;
    constexpr f32 FORWARD_DELTA_SCALE = 1.7f;
    constexpr f32 DIR_SCALE = 70.0f;
    constexpr f32 SLOW_BONUS = 3.0f;
    constexpr f32 HOP_BONUS = 1.3f;
    constexpr f32 SLOW_SPEED = 30.0f;
    constexpr f32 SPREAD = 0.06f;
    constexpr f32 SPREAD_HALF = 0.03f;
    constexpr f32 UP_PER_SPEED = 0.125f;
    constexpr f32 RISE_SCALE = 2.0f;
    constexpr f32 FALL_SCALE = 0.5f;
    constexpr f32 MAX_RISE = 20.0f;
    constexpr f32 BACK_RISE_SCALE = 0.4f;
    constexpr f32 BACK_FALL_SCALE = 1.2f;
    constexpr f32 DROP_OFFSET = 20.0f;
    constexpr f32 MIN_SCALE = 0.8f;

    const EGG::Vector3f &delta = trail.posDelta();
    EGG::Vector3f dir;
    f32 speed;
    if (!forward) {
        speed = std::max(BACK_SPEED_PER_DELTA * delta.ps_dot(trail.forward()), BACK_MIN_SPEED);
        speed = trail.calcThrowBack(speed, DELTA_SCALE, DIR_SCALE, true, dir);
    } else {
        speed = trail.calcThrowForward(forwardSpeed, FORWARD_DELTA_SCALE, DIR_SCALE, true, dir);
    }

    f32 speedRatio = ItemDirector::Instance()->kartItem(m_owner).speedRatio();
    f32 hop = trail.hopHeight();
    speed += 0.0f + (DELTA_SCALE - speedRatio) * SLOW_BONUS;
    if (forward && hop > 0.0f) {
        speed += HOP_BONUS * hop;
    }

    f32 kartSpeed = trail.kartSpeed();
    if (kartSpeed < SLOW_SPEED) {
        auto &random = ItemRaceFlags::Random();
        dir.z += random.getF32(SPREAD) - SPREAD_HALF;
        dir.x += random.getF32(SPREAD) - SPREAD_HALF;
    }

    EGG::Vector3f vel = dir * speed;
    f32 up;
    if (!forward) {
        up = backUpSpeed + delta.y * (delta.y > 0.0f ? BACK_RISE_SCALE : BACK_FALL_SCALE);
    } else {
        f32 rise =
                std::min(std::max(delta.y * (delta.y > 0.0f ? RISE_SCALE : FALL_SCALE), -MAX_RISE),
                        MAX_RISE);
        up = upSpeed + UP_PER_SPEED * kartSpeed + rise;
    }

    m_vel = vel + trail.up() * up;

    if (!forward) {
        if (hop > 0.0f && kartSpeed < MAX_RISE && m_scale < MIN_SCALE) {
            m_pos += (trail.up() - trail.forward()) * DROP_OFFSET;
        }
    } else {
        // The kart's own position (KartObjectProxy::getPos), not the trail's anchor base.
        m_pos = trail.kartObject()->pos();
    }

    if (m_scale < MIN_SCALE) {
        m_scale = MIN_SCALE;
        m_scaleVec = EGG::Vector3f(MIN_SCALE, MIN_SCALE, MIN_SCALE);
    }

    m_bouncesLeft = static_cast<f32>(bounces);
    m_onWall = static_cast<WallFunc>(&ItemObjThrowable::onArcWall);
    m_flags.resetBit(eFlags::WallSlide, eFlags::Contact, eFlags::WallSet);
}

/// @addr{0x807B7570}
/// @brief One frame of flight: drag, floor bounces, landing, wall slides, a growing fall speed
/// limit.
void ItemObjThrowable::calcArc(f32 gravity, f32 drag, f32 bounce) {
    constexpr f32 FLOOR_NORMAL_Y = 0.02f;
    constexpr f32 BOUNCE_KEEP = 0.55f;
    constexpr f32 WALL_VANISH_Y = 0.98f;
    constexpr f32 MIN_FALL = 100.0f;
    constexpr f32 MAX_FALL = 250.0f;
    constexpr f32 FALL_RAMP = 150.0f;
    constexpr f32 FALL_RAMP_START = 150.0f;
    constexpr f32 FALL_RAMP_END = 30.0f;

    m_pos += m_vel;
    m_vel *= drag;

    bool wall = false;
    if (checkSphereFull(m_kclRadius, 0x6CEBDFFF)) {
        m_pos += m_colInfo.tangentOff;

        auto *colDir = Field::CollisionDirector::Instance();
        if ((m_kclMask & KCL_ITEM_SOLID) &&
                colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_SOLID)) {
            const EGG::Vector3f &floorNrm = m_colInfo.floorNrm;
            if (floorNrm.y >= FLOOR_NORMAL_Y) {
                f32 left = m_bouncesLeft;
                m_bouncesLeft = left - 1.0f;
                if (left > 0.0f) {
                    m_frameFlags.setBit(eFrameFlags::Bounced);
                    EGG::Vector3f proj = floorNrm * m_vel.ps_dot(floorNrm);
                    m_vel -= proj;
                    m_vel *= BOUNCE_KEEP;
                    m_vel += proj * -bounce;
                    m_up = floorNrm;
                } else {
                    checkLanding(floorNrm);
                }
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

            m_flags.resetBit(eFlags::OwnerImmune);
        }

        if (m_kclMask & 0x20000) {
            startVanish(true);
            return;
        }
    }

    m_flags.resetBit(eFlags::WallSet);
    if (!(m_kclMask & KCL_ITEM_SOLID)) {
        m_vel.y -= gravity;
        f32 maxFall = MIN_FALL +
                (FALL_RAMP * (static_cast<f32>(m_frame) - FALL_RAMP_START)) /
                        (FALL_RAMP_END - FALL_RAMP_START);
        maxFall = std::min(std::max(maxFall, MIN_FALL), MAX_FALL);
        if (m_vel.y < -maxFall) {
            m_vel.y = -maxFall;
        }
    }
}

/// @addr{0x807B7A48}
void ItemObjThrowable::onArcWall(const EGG::Vector3f &nrm) {
    slideOnWall(nrm);
    if (m_flags.offBit(eFlags::WallSlide)) {
        m_flags.setBit(eFlags::WallSlide);
        m_contactNrm.setZero();
    }
}

ItemObjBanana::ItemObjBanana() = default;

ItemObjBanana::~ItemObjBanana() = default;

/// @addr{0x807A3BE0}
/// @brief Dropped behind the kart, or thrown forward with the stick held up.
bool ItemObjBanana::onRelease(ItemTrail &trail) {
    constexpr f32 FORWARD_SPEED = 85.0f;
    constexpr f32 UP_SPEED = 40.0f;
    constexpr f32 BACK_UP_SPEED = 10.0f;

    m_flags.setBit(eFlags::OwnerImmune15, eFlags::DroppedBack);
    bool forward = trail.kartItem()->stickUp();
    initThrow(FORWARD_SPEED, UP_SPEED, BACK_UP_SPEED, trail, 0, forward);
    return forward;
}

/// @addr{0x807A40F4}
/// @brief A banana hit by something pops up and spins away (its own break animation).
s32 ItemObjBanana::onBreak() {
    constexpr f32 POP_SPEED = 30.0f;

    if (m_up.y > 0.0f) {
        f32 upSpeed = m_vel.ps_dot(m_up);
        if (upSpeed < POP_SPEED) {
            m_vel += m_up * (POP_SPEED - upSpeed);
        }
    }

    if (m_flags.offBit(eFlags::Breaking, eFlags::Vanishing)) {
        m_state = static_cast<StateFunc>(&ItemObjBanana::calcBanana);
    }

    m_flags.setBit(eFlags::ScaleFrozen);
    return 2;
}

/// @addr{0x807A3E78}
void ItemObjBanana::calcBanana() {
    constexpr f32 DROP_GRAVITY = 1.8f;
    constexpr f32 DROP_DRAG = 0.97f;
    constexpr f32 THROW_GRAVITY = 2.2f;
    constexpr f32 THROW_DRAG = 0.999f;
    constexpr f32 THROW_LAND_RISE = 2.0f;

    if (m_flags.onBit(eFlags::Breaking)) {
        calcBananaBreak();
        return;
    }

    if (m_flags.offBit(eFlags::FromKart)) {
        calcArc(DROP_GRAVITY, DROP_DRAG, 0.0f);
    } else {
        calcArc(THROW_GRAVITY, THROW_DRAG, 0.0f);
    }

    if (m_frameFlags.onBit(eFrameFlags::Landed) &&
            m_frameFlags.offBit(eFrameFlags::Breaking, eFrameFlags::Vanishing)) {
        if (m_flags.onBit(eFlags::FromKart)) {
            m_vel.y *= THROW_LAND_RISE;
        }

        landOnFloor();
    }

    // The model's up vector smoothing and the shadow are visual.
}

/// @addr{0x807A3C80}
/// @brief The break animation: the banana falls and shrinks, then despawns with the break timer.
void ItemObjBanana::calcBananaBreak() {
    constexpr f32 GRAVITY = 2.5f;

    m_vel.y -= GRAVITY;
    m_pos += m_vel;

    f32 t = 1.0f / static_cast<f32>(m_breakTimer);
    f32 root = t > 0.0f ? t * EGG::Mathf::frsqrt(t) : 0.0f;
    m_scale *= 1.0f - root;
    setScale(m_scale);
    calcBreaking();
}

ItemObj::StateFunc ItemObjBanana::initialState() const {
    return static_cast<StateFunc>(&ItemObjBanana::calcBanana);
}

} // namespace Kinoko::Item
