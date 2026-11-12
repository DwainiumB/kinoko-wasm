#include "ItemObjBlueShell.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemMath.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/BoxColManager.hh"
#include "game/field/CollisionDirector.hh"

#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"

#include "game/system/CourseMap.hh"
#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"

#include <cmath>

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

constexpr u8 NO_PLAYER = 12;
constexpr f32 CRUISE_SPEED = 130.0f;
constexpr f32 SPEED_STEP = 8.0f;
constexpr KCLTypeMask KCL_SOLID = 0x6CEBDBFF;

/// The explosion grows to this scale (0x809C3650, from 0x808A5BA8), ...
constexpr f32 EXPLOSION_SCALE = 36.0f;
/// ... this much per frame (0x809C3654).
constexpr f32 EXPLOSION_GROWTH = 3.12f;

const Kart::KartObject *Kart(u8 idx) {
    return Kart::KartObjectManager::Instance()->object(idx);
}

/// @addr{0x80591344}
/// @brief Respawning or otherwise out of play (the same bits as the thunder cloud's strike check).
bool IsHidden(const Kart::KartObject *kart) {
    return kart->status().onBit(Kart::eStatus::TriggerRespawn, Kart::eStatus::NoKartCollision40,
            Kart::eStatus::InRespawn, Kart::eStatus::AfterRespawn, Kart::eStatus::NoKartCollision82,
            Kart::eStatus::Vanished, Kart::eStatus::NoKartCollision89);
}

/// @addr{0x80591378}
/// @brief KartItem +0x240 (an item immunity timer) is not ported and taken as 0.
bool IsUntargetable(const Kart::KartObject *kart) {
    return kart->status().onBit(Kart::eStatus::TriggerRespawn, Kart::eStatus::InRespawn,
            Kart::eStatus::AfterRespawn, Kart::eStatus::Vanished);
}

/// @addr{0x807DF7B4}
/// @brief 1 - e^(-x / scale), exactly 1 past 16 scales.
f32 Ease(f32 scale, f32 x) {
    constexpr f32 CUTOFF = 16.0f;

    if (x > CUTOFF * scale) {
        return 1.0f;
    }

    return 1.0f - std::exp(-x / scale);
}

Course CurrentCourse() {
    return System::RaceConfig::Instance()->raceScenario().course;
}

/// @addr{0x807ADC08}
/// @brief The slowest dive rate, by engine class (0.15 online, not ported).
f32 LowDiveRate() {
    using EngineClass = System::RaceConfig::Scenario::EngineClass;

    switch (System::RaceConfig::Instance()->raceScenario().engineClass) {
    case EngineClass::CC100:
        return 0.22f;
    case EngineClass::CC150:
        return 0.25f;
    default:
        return 0.2f;
    }
}

f32 TowardsCruise(f32 speed) {
    if (CRUISE_SPEED <= speed) {
        return std::max(speed - SPEED_STEP, CRUISE_SPEED);
    }

    return std::min(speed + SPEED_STEP, CRUISE_SPEED);
}

} // namespace

ItemObjBlueShell::ItemObjBlueShell()
    : m_target(NO_PLAYER), m_tracking(false), m_onRoute(false), m_lockCandidate(NO_PLAYER),
      m_circleAngle(0), m_state(State::Slide), m_nextState(-1), m_stateFrame(0), m_height(0.0f),
      m_heightRate(0.0f), m_chosen(NO_PLAYER), m_chaseNext(false), m_flySpeed(0.0f),
      m_chosenMask(0), m_lowDive(false), m_circleRadius(0.0f), m_circleStep(0), m_weakened(false) {}

ItemObjBlueShell::~ItemObjBlueShell() = default;

/// @addr{0x807ABF70}
void ItemObjBlueShell::onSpawn() {
    // 0x807B3EFC
    m_target = NO_PLAYER;
    m_onRoute = false;
    m_lockCandidate = NO_PLAYER;

    m_state = State::Slide;
    m_nextState = -1;
    m_stateFrame = 0;
    m_chosen = chooseTarget();
    m_chaseNext = false;
    m_chosenMask = 0;
    m_weakened = false;
    m_lowDive = false;
}

/// @addr{0x807AC634}
/// @brief With an item point it takes the item route (60, no wall-bounce limit, branch search);
/// without one it slides off at 85 and goes straight for its target.
bool ItemObjBlueShell::onRelease(ItemTrail &trail) {
    constexpr f32 ROUTE_SPEED = 60.0f;
    constexpr s32 ROUTE_FRAMES = 20;
    constexpr f32 SLIDE_SPEED = 85.0f;

    m_onRoute = trail.kartItem()->itemPointTracker().idx != System::MapdataItemPoint::NONE;
    if (m_onRoute) {
        initRoute(ROUTE_SPEED, trail, ROUTE_FRAMES, -1, true, m_chosen, true);
        m_state = State::Route;
        m_nextState = -1;
        m_stateFrame = 0;
    }

    setUpThrow();
    m_basis[0] = trail.right();
    m_basis[1] = trail.up();
    m_basis[2] = trail.forward();

    // Online objects (flags2 bit 5) wait for their state instead: not ported.
    if (!m_onRoute) {
        initThrow(SLIDE_SPEED, trail, 0, true, true);
        m_flags.resetBit(eFlags::FromKart);
        m_chosen = chooseTarget();
        setTarget(m_chosen);
        m_nextState = static_cast<s32>(State::Homing);
    }

    // The model and its animation are visual.
    return true;
}

/// @addr{0x807AC49C}
void ItemObjBlueShell::setUpThrow() {
    constexpr f32 START_HEIGHT = 200.0f;
    constexpr f32 START_RATE = 50.0f;
    constexpr f32 START_SPEED = 260.0f;

    m_reflects = false;
    // +0x80 (the model's position) is visual.
    if (m_boxColUnit) {
        removeBoxCol();
    }

    m_flags.setBit(eFlags::RedForward);
    // 0x807B3EEC
    m_target = NO_PLAYER;
    m_circleAngle = 0;

    m_flags.resetBit(eFlags::Released);
    m_flags.setBit(eFlags::Unk14, eFlags::DroppedBack);
    m_chosen = chooseTarget();
    m_tilt.setZero();
    m_lastMove = m_vel - m_tilt;
    m_height = START_HEIGHT;
    m_heightRate = START_RATE;
    m_flySpeed = START_SPEED;
    m_moveDelta = m_vel;
}

/// @addr{0x807AE414}
/// @brief Hit by something, or it reached the floor: it explodes where it is (reaction 1: it
/// keeps its own state).
s32 ItemObjBlueShell::onBreak() {
    if (m_state == State::Explode) {
        return 1;
    }

    m_state = State::Slide;
    m_stateFrame = 0;
    m_nextState = static_cast<s32>(State::Explode);
    setState(static_cast<StateFunc>(&ItemObjBlueShell::calcBlue));
    // 0x8079D920 is visual.

    m_pos -= m_basis[1] * m_kclRadius;
    m_flags.resetBit(eFlags::Landed);
    m_flags.setBit(eFlags::Breaking);
    // The online break event is left out.
    m_flags.resetBit(eFlags::OwnerImmune);
    // The explosion effect and the HUD are visual.
    return 1;
}

/// @addr{0x807AD118}
/// @brief Runs the state; remembers this frame's motion for the model's orientation.
void ItemObjBlueShell::calcBlue() {
    constexpr f32 MIN_MOVE_SQ = 0.01f;

    EGG::Vector3f before = m_pos;
    if (m_nextState < 0) {
        ++m_stateFrame;
    } else {
        m_state = static_cast<State>(m_nextState);
        m_nextState = -1;
        m_stateFrame = 0;
        enterState(m_state);
    }

    calcState(m_state);

    m_moveDelta = m_pos - before;
    if (m_moveDelta.x * m_moveDelta.x + m_moveDelta.z * m_moveDelta.z < MIN_MOVE_SQ) {
        const EGG::Vector3f &forward = m_basis[2];
        m_moveDelta +=
                forward.x * forward.x + forward.z * forward.z >= MIN_MOVE_SQ ? forward : m_basis[1];
    }

    // The target warning on the HUD and the camera are visual.
}

void ItemObjBlueShell::enterState(State state) {
    switch (state) {
    case State::Homing:
        enterStateHoming();
        break;
    case State::Circle:
        enterStateCircle();
        break;
    case State::Dive:
        enterStateDive();
        break;
    case State::Descent:
        enterStateDescent();
        break;
    case State::Explode:
        enterStateExplode();
        break;
    default:
        break;
    }
}

void ItemObjBlueShell::calcState(State state) {
    switch (state) {
    case State::Slide:
        calcStateSlide();
        break;
    case State::Route:
        calcStateRoute();
        break;
    case State::Homing:
        calcStateHoming();
        break;
    case State::Circle:
        calcStateCircle();
        break;
    case State::Dive:
        calcStateDive();
        break;
    case State::Descent:
        calcStateDescent();
        break;
    case State::Explode:
        calcStateExplode();
        break;
    }
}

/// @addr{0x807AD5A4}
void ItemObjBlueShell::calcStateSlide() {
    calcMove(CRUISE_SPEED);
}

/// @addr{0x807AD5EC}
/// @brief Along the item route after the leader (the next racer when the thrower leads), until it
/// can lock on to one of the racers it chose so far (or after 1200 frames).
void ItemObjBlueShell::calcStateRoute() {
    constexpr f32 ROUTE_RATE = 0.008f;
    constexpr u32 GIVE_UP_FRAME = 1200;

    u32 frame = m_stateFrame;
    calcRoute(m_flySpeed, ROUTE_RATE, m_chosen, true);
    calcHeight(true);
    calcOrientation(m_moveDelta, true);

    u8 owner = m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;
    if (!m_chaseNext) {
        m_chosen = chooseTarget();
        if (m_chosen == owner) {
            m_chaseNext = true;
        } else {
            m_chosenMask |= static_cast<u16>(1 << m_chosen);
        }
    }

    if (m_chaseNext) {
        m_chosen = chooseTarget();
        m_chosenMask |= static_cast<u16>(1 << m_chosen);
    }

    if (tryLock(m_chosen)) {
        m_nextState = static_cast<s32>(State::Homing);
    } else {
        // Racers chosen before who have finished the race.
        auto *raceMgr = System::RaceManager::Instance();
        size_t count = Kart::KartObjectManager::Instance()->count();
        for (u8 i = 0; i < count; ++i) {
            if (!(m_chosenMask & (1 << i)) || i == m_chosen) {
                continue;
            }

            if (raceMgr->player(i).finished() && tryLock(i)) {
                m_nextState = static_cast<s32>(State::Homing);
            }
        }

        if (frame > GIVE_UP_FRAME && m_target == NO_PLAYER) {
            setTarget(m_chosen);
            m_nextState = static_cast<s32>(State::Homing);
        }
    }
}

/// @addr{0x807AD7E4}
void ItemObjBlueShell::enterStateHoming() {
    constexpr f32 RADIUS = 300.0f;

    m_flySpeed = m_vel.length();
    m_circleRadius = RADIUS;
}

/// @addr{0x807AD824}
/// @brief Straight for a point 300 ahead of the target, until within 800 of it.
void ItemObjBlueShell::calcStateHoming() {
    constexpr f32 RATE = 0.5f;
    constexpr f32 BLEND = 0.125f;
    constexpr f32 NEAR_SQ = 640000.0f;

    m_flySpeed = TowardsCruise(m_flySpeed);
    homeIn(m_flySpeed, m_circleRadius, RATE, BLEND, 0, true);
    if (m_aimDiff.x * m_aimDiff.x + m_aimDiff.z * m_aimDiff.z < NEAR_SQ) {
        m_nextState = static_cast<s32>(State::Circle);
    }

    calcHeight(true);
    calcOrientation(m_moveDelta, true);
}

/// @addr{0x807AD914}
void ItemObjBlueShell::enterStateCircle() {
    constexpr f32 HEIGHT = 450.0f;
    constexpr f32 RATE = 50.0f;
    constexpr u16 STEP = 1000;

    m_lastMove = m_aimDiff - m_tilt;
    m_height = HEIGHT;
    m_heightRate = RATE;
    m_circleStep = STEP;
}

/// @addr{0x807AD958}
/// @brief One loop around the target on a shrinking circle, faster and faster.
void ItemObjBlueShell::calcStateCircle() {
    constexpr f32 HEIGHT_STEP = 7.5f;
    constexpr u16 STEP_GROWTH = 35;
    constexpr u32 HALF_TURN = 0x8000;
    constexpr f32 FIRST_HALF_SHRINK = 4.0f;
    constexpr f32 SECOND_HALF_SHRINK = 8.0f;
    constexpr f32 RATE = 0.3f;
    constexpr f32 CANNON_RATE = 0.5f;
    constexpr f32 CANNON_SPEED = 600.0f;
    constexpr f32 BLEND = 0.5f;
    constexpr u32 END_ANGLE = 0x11F40;
    constexpr u32 TURN = 0x10000;
    constexpr f32 TILT_SIDE = 2.0f;
    constexpr f32 TILT_UP = 128.0f;

    m_flySpeed = TowardsCruise(m_flySpeed);
    m_height -= HEIGHT_STEP;
    m_circleStep += STEP_GROWTH;
    m_circleRadius -= m_circleAngle < HALF_TURN ? FIRST_HALF_SHRINK : SECOND_HALF_SHRINK;

    // 0x807B4400
    m_circleAngle += m_circleStep;
    u16 angle = static_cast<u16>(m_circleAngle);
    if (!targetInCannon() || CurrentCourse() == Course::N64_DKs_Jungle_Parkway) {
        homeIn(m_flySpeed, m_circleRadius, RATE, BLEND, angle, false);
    } else {
        homeIn(CANNON_SPEED, m_circleRadius, CANNON_RATE, BLEND, angle, false);
    }

    calcHeight(false);

    if (m_circleAngle < END_ANGLE) {
        f32 turn =
                static_cast<f32>(static_cast<s32>(m_circleAngle - TURN)) / static_cast<f32>(TURN);
        EGG::Vector3f move(TILT_SIDE * m_aimOffset.z, TILT_UP * turn, -TILT_SIDE * m_aimOffset.x);
        if (m_stateFrame == 0) {
            m_lastMove = move - m_tilt;
        }

        calcOrientation(move, true);
    } else {
        m_nextState = static_cast<s32>(State::Dive);
    }
}

/// @addr{0x807ADB54}
void ItemObjBlueShell::enterStateDive() {
    constexpr f32 HEIGHT = 300.0f;
    constexpr f32 RATE = 0.2f;
    constexpr f32 SPEED = -1.5f;
    constexpr f32 RADIUS = 0.35f;

    // 0x807A2F88 is a sound.
    m_height = HEIGHT;
    m_heightRate = RATE;
    m_scaleBounce = 0.0f;
    m_flags.setBit(eFlags::ScaleFrozen);
    m_diveMove = m_lastMove;
    m_flySpeed = SPEED;
    m_circleRadius = RADIUS;
    // The model's animation is visual.
}

/// @addr{0x807ADC5C}
/// @brief Rises over the target and closes in on it for 25 frames.
void ItemObjBlueShell::calcStateDive() {
    constexpr f32 HEIGHT_STEP = 3.0f;
    constexpr f32 CANNON_RATE = 0.5f;
    constexpr f32 MAX_RATE = 0.7f;
    constexpr f32 RATE_UP = 0.005f;
    constexpr f32 RATE_DOWN = 0.05f;
    constexpr u32 LOW_DIVE_FRAME = 8;
    constexpr u32 END_FRAME = 24;
    constexpr f32 SPEED_STEP_DIVE = -0.05f;
    constexpr f32 RADIUS_STEP = 0.063f;
    constexpr f32 MIN_RADIUS = 0.3f;
    constexpr f32 MOVE_DECAY = 0.1f;
    constexpr f32 MOVE_RATE = 10.0f;

    m_height += HEIGHT_STEP;
    bool parkway = CurrentCourse() == Course::N64_DKs_Jungle_Parkway;
    if (!targetInCannon() || parkway) {
        descend(m_heightRate, m_height);
    } else {
        descend(CANNON_RATE, m_height);
    }

    if (!m_lowDive) {
        if (m_heightRate < MAX_RATE) {
            m_heightRate += RATE_UP;
        }
    } else {
        m_heightRate = std::max(m_heightRate - RATE_DOWN, LowDiveRate());
    }

    // KartItem +0x238 of the target (not ported) also makes the dive low.
    if (m_stateFrame > LOW_DIVE_FRAME && targetInCannon() && parkway) {
        m_lowDive = true;
    }

    if (m_stateFrame > END_FRAME) {
        m_nextState = static_cast<s32>(State::Descent);
    }

    f32 radius = std::max(m_circleRadius - RADIUS_STEP, MIN_RADIUS);
    m_flySpeed -= SPEED_STEP_DIVE;
    m_circleRadius = radius;
    f32 k = MOVE_RATE * radius;
    m_diveMove.x += (m_targetDir.x - m_diveMove.x * MOVE_DECAY) * k;
    m_diveMove.y += (m_flySpeed - m_diveMove.y * MOVE_DECAY) * k;
    m_diveMove.z += (m_targetDir.z - m_diveMove.z * MOVE_DECAY) * k;
    calcOrientation(m_diveMove, false);
}

/// @addr{0x807ADF14}
void ItemObjBlueShell::enterStateDescent() {
    constexpr f32 HEIGHT = 400.0f;
    constexpr f32 SQUASH = 0.5f;

    m_height = HEIGHT;
    m_scaleBounce = SQUASH + 0.0f;
    insertBoxCol(false);
    // flags2 bit 13 is not used by the ported code; the model's animation is visual.
}

/// @addr{0x807ADFA4}
/// @brief Drops onto the target; explodes when it reaches the course (or 100 under the target,
/// 900 when the target is in a cannon).
void ItemObjBlueShell::calcStateDescent() {
    constexpr f32 HEIGHT_STEP = 70.0f;
    constexpr f32 RATE = 0.7f;
    constexpr f32 CANNON_RATE = 0.5f;
    constexpr f32 LIMIT = -100.0f;
    constexpr f32 CANNON_LIMIT = -900.0f;

    m_height -= HEIGHT_STEP;
    if (!targetInCannon() || CurrentCourse() == Course::N64_DKs_Jungle_Parkway) {
        descend(m_lowDive ? LowDiveRate() : RATE, m_height);
    } else {
        descend(CANNON_RATE, m_height);
    }

    f32 limit = targetInCannon() ? CANNON_LIMIT : LIMIT;
    if (m_height >= limit) {
        if (checkSphereFull(m_kclRadius, KCL_SOLID)) {
            m_pos += m_colInfo.tangentOff;
            startBreak(false);
        }
    } else {
        startBreak(false);
    }

    const auto &props = properties();
    bool grew = growScale(props.scale, props.throwScaleStep);
    if (!calcSquash() && grew) {
        setScale(m_scale);
    }
}

/// @addr{0x807AE180}
/// @brief The explosion: the hit sphere is widened to its full size.
void ItemObjBlueShell::enterStateExplode() {
    m_scaleBounce = 0.0f;
    m_flags.setBit(eFlags::ScaleFrozen);
    m_scaleVec = EGG::Vector3f(1.0f, 1.0f, 1.0f);
    // The explosion model, its animations and the sound are visual.

    if (!m_boxColUnit) {
        insertBoxCol(false);
    }

    if (m_boxColUnit) {
        m_boxColUnit->resize(EXPLOSION_SCALE * properties().hitRadius, 0.0f);
    }
}

/// @addr{0x807AE34C}
/// @brief As the bob-omb's: it launches racers for 30 frames, then only spins them, and is gone
/// after 86 frames.
void ItemObjBlueShell::calcStateExplode() {
    constexpr u32 WEAKEN_FRAME = 30;
    constexpr u32 END_HIT_FRAME = 42;
    constexpr u32 DESPAWN_FRAME = 85;

    // Frame 10 (0x807A61F8, the explosion's effect on the course) is not ported.
    if (growScale(EXPLOSION_SCALE, EXPLOSION_GROWTH)) {
        setScale(m_scale);
    }

    if (m_stateFrame == WEAKEN_FRAME) {
        m_weakened = true;
    } else if (m_stateFrame == END_HIT_FRAME) {
        removeBoxCol();
    }

    if (m_stateFrame > DESPAWN_FRAME) {
        ItemDirector::Instance()->holder(m_kind).despawn(this);
    }
}

/// @addr{0x807AC1A8}
/// @brief The leader, or the best-placed racer still racing who is not the thrower; the thrower
/// when it leads. Battle teams and online are left out.
u8 ItemObjBlueShell::chooseTarget() const {
    auto *raceMgr = System::RaceManager::Instance();
    size_t count = Kart::KartObjectManager::Instance()->count();
    u8 owner = m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;

    for (u8 rank = 1; rank <= count; ++rank) {
        for (u8 i = 0; i < count; ++i) {
            if (raceMgr->player(i).rank() != rank) {
                continue;
            }

            if (raceMgr->player(i).finished() && !m_chaseNext) {
                continue;
            }

            if (i != owner || rank == 1) {
                return i;
            }
        }
    }

    return owner;
}

/// @addr{0x807B45B0}
/// @brief Locks on to a racer within 8000 (5000 on Mario Circuit) and between 1500 below and 1000
/// above, or one whose item point is the shell's next one. The cone test the game also computes
/// does not change the result.
bool ItemObjBlueShell::tryLock(u8 playerIdx) {
    constexpr f32 RANGE = 8000.0f;
    constexpr f32 MARIO_CIRCUIT_RANGE = 5000.0f;
    constexpr f32 ABOVE = 1000.0f;
    constexpr f32 BELOW = -1500.0f;

    const auto *kart = Kart(playerIdx);
    if (IsHidden(kart)) {
        return false;
    }

    const EGG::Vector3f &kartPos = kart->pos();
    EGG::Vector3f d = kartPos - m_pos;

    if (m_lockCandidate != playerIdx) {
        u8 point = ItemDirector::Instance()->kartItem(playerIdx).itemPointTracker().idx;
        m_lockCandidate = point == m_routeNext.idx ? playerIdx : NO_PLAYER;
    }

    f32 range = CurrentCourse() == Course::Mario_Circuit ? MARIO_CIRCUIT_RANGE : RANGE;
    bool candidate = m_lockCandidate == playerIdx;
    if (!(d.x * d.x + d.z * d.z < range * range || candidate)) {
        return false;
    }

    if (!((d.y <= ABOVE && BELOW <= d.y) || candidate)) {
        return false;
    }

    m_target = playerIdx;
    m_targetPos = kartPos;
    m_tracking = true;
    return true;
}

/// @addr{0x807B4810}
void ItemObjBlueShell::setTarget(u8 playerIdx) {
    m_target = playerIdx;
    m_targetPos = Kart(playerIdx)->pos();
    m_tracking = true;
}

/// @addr{0x807B3FDC}
/// @brief Steers towards a point on a circle of the given radius around a point 300 ahead of the
/// target (angle 0: beside it), the speed capped; the blend ramps up over the first 30 frames of
/// a slide throw. Pushed up out of the course.
void ItemObjBlueShell::homeIn(f32 speed, f32 radius, f32 rate, f32 blend, u16 angle,
        bool noReverse) {
    constexpr f32 RAMP_FRAMES = 30.0f;
    constexpr f32 ANGLE_SCALE = 1.0f / 256.0f;
    constexpr f32 AHEAD = 300.0f;

    const auto *kart = Kart(m_target);
    m_targetDir = kart->move()->vel1Dir();

    if (!m_onRoute && m_frame < static_cast<s32>(RAMP_FRAMES)) {
        f32 ramp = 0.0f + ((blend - 0.0f) * (static_cast<f32>(m_frame) - 0.0f)) / RAMP_FRAMES;
        if (ramp < 0.0f) {
            blend = 0.0f;
        } else if (ramp <= blend) {
            blend = ramp;
        }
    }

    EGG::Vector3f side = m_targetDir.cross(kart->move()->smoothedUp());
    auto [sin, cos] = EGG::Mathf::SinCosFIdx(static_cast<f32>(angle) * ANGLE_SCALE);
    m_aimOffset = side * (radius * cos);
    m_aimOffset += m_targetDir * (radius * sin);
    EGG::Vector3f v = m_aimOffset + m_targetDir * AHEAD;

    if (IsUntargetable(kart)) {
        m_tracking = false;
    }

    if (m_tracking) {
        m_targetPos = kart->pos();
    }

    v = m_targetPos + v;
    m_aimDiff = v - m_pos;
    v = m_aimDiff * rate;

    if (noReverse && m_vel.x * v.x + m_vel.z * v.z < 0.0f) {
        if (m_vel.z * v.x - m_vel.x * v.z > 0.0f) {
            v.x = m_vel.z;
            v.z = -m_vel.x;
        } else {
            v.x = -m_vel.z;
            v.z = m_vel.x;
        }
    }

    f32 sq = v.ps_squareMag();
    if (!(sq <= speed * speed)) {
        v *= speed * EGG::Mathf::frsqrt(sq);
    }

    m_vel += (v - m_vel) * blend;
    m_pos += m_vel;

    m_colInfo.bbox.setZero();
    if (Field::CollisionDirector::Instance()->checkSphereFull(m_kclRadius, m_pos, m_prevPos,
                KCL_SOLID, &m_colInfo, nullptr, 0)) {
        m_pos.y += m_colInfo.tangentOff.y;
    }
}

/// @addr{0x807B4430}
/// @brief Moves towards a point 210 ahead of the target at the given height over it.
void ItemObjBlueShell::descend(f32 rate, f32 height) {
    constexpr f32 AHEAD = 210.0f;

    const auto *kart = Kart(m_target);
    m_targetDir = kart->move()->vel1Dir();
    if (IsUntargetable(kart)) {
        m_tracking = false;
    }

    if (m_tracking) {
        m_targetPos = kart->pos();
    }

    EGG::Vector3f aim(m_targetPos.x + m_targetDir.x * AHEAD,
            (m_targetPos.y + m_targetDir.y * AHEAD) + height,
            m_targetPos.z + m_targetDir.z * AHEAD);
    m_vel = (aim - m_pos) * rate;
    m_pos += m_vel;
}

/// @addr{0x807ACB24}
/// @brief The dive height and rate (only the parts that are not the model's bobbing).
void ItemObjBlueShell::calcHeight(bool update) {
    constexpr f32 MAX_HEIGHT = 450.0f;
    constexpr f32 HEIGHT_STEP = 15.0f;
    constexpr f32 EASE = 500.0f;
    constexpr f32 RISE = 2000.0f;
    constexpr f32 TARGET_MARGIN = 800.0f;
    constexpr f32 RATE_STEP = 1.0f;
    constexpr f32 MAX_RATE = 150.0f;

    if (!update) {
        // The model's position (+0x80) is visual.
        return;
    }

    if (m_onRoute) {
        m_height = std::min(m_height + HEIGHT_STEP, MAX_HEIGHT);
    } else {
        const EGG::Vector3f &ownerPos = Kart(m_owner)->pos();
        EGG::Vector3f fromOwner(m_pos.x - ownerPos.x, 0.0f, m_pos.z - ownerPos.z);
        f32 ownerDist = fromOwner.length();

        const EGG::Vector3f &targetPos = Kart(m_target)->pos();
        EGG::Vector3f fromTarget(m_pos.x - targetPos.x, 0.0f, m_pos.z - targetPos.z);
        f32 targetDist = std::max(fromTarget.length() - TARGET_MARGIN, 0.0f);

        f32 height = MAX_HEIGHT + RISE * Ease(EASE, targetDist);
        f32 nearOwner = RISE * Ease(EASE, ownerDist);
        m_height = nearOwner < height ? nearOwner : height;
    }

    m_heightRate = std::min(m_heightRate + RATE_STEP, MAX_RATE);
}

/// @addr{0x807ACE1C}
/// @brief The model's basis faces the motion, tilted towards its change; the explosion starts
/// one collision radius below its up axis.
void ItemObjBlueShell::calcOrientation(const EGG::Vector3f &move, bool follow) {
    constexpr s32 FIRST_FRAMES = 9;
    constexpr f32 FOLLOW = 0.08f;
    constexpr f32 DECAY = 0.75f;
    constexpr f32 TILT = 0.3f;
    constexpr f32 FLIP = 2.0f;

    if (!(1.0f <= move.ps_squareMag() || m_frame > FIRST_FRAMES)) {
        return;
    }

    EGG::Vector3f dir;
    NormaliseInto(dir, move);
    f32 minXZ = EGG::Mathf::CosFIdx(53.333336f);
    if (!(minXZ * minXZ <= dir.x * dir.x + dir.z * dir.z)) {
        return;
    }

    EGG::Vector3f &right = m_basis[0];
    EGG::Vector3f &up = m_basis[1];
    EGG::Vector3f &forward = m_basis[2];
    forward = dir;

    if (!follow) {
        m_tilt.x *= DECAY;
        m_tilt.z *= DECAY;
    } else {
        m_tilt.x += FOLLOW * ((move.x - m_lastMove.x) - m_tilt.x);
        m_tilt.z += FOLLOW * ((move.z - m_lastMove.z) - m_tilt.z);
    }

    EGG::Vector3f tilt(TILT * m_tilt.x, 1.0f, TILT * m_tilt.z);
    right = tilt.cross(forward);
    if (NormaliseLength(right, right) <= 0.0f) {
        right = EGG::Vector3f(forward.z, 0.0f, -forward.x);
    }

    up = forward.cross(right);
    if (up.y < 0.0f) {
        // Upside down: turn the right and up axes about the forward one.
        EGG::Vector3f oldUp = up;
        EGG::Vector3f oldRight = right;
        f32 t = right.y / up.y;
        f32 s = (FLIP * t) / (1.0f + t * t);
        f32 c = t * s - 1.0f;
        up = oldUp * c - oldRight * s;
        right = oldRight * c + oldUp * s;
    }

    m_lastMove = move;
}

/// @addr{0x8079FD84}
/// @brief Eases the y scale towards the squash target (+0x140), widening x and z to keep the
/// volume; the radii follow.
bool ItemObjBlueShell::calcSquash() {
    constexpr f32 RATE = 0.2f;
    constexpr f32 WIDEN = 2.0f;

    if (m_scaleBounce == 0.0f) {
        return false;
    }

    const auto &props = properties();
    f32 y = m_scaleVec.y + RATE * (m_scaleBounce - m_scaleVec.y);
    m_frameFlags.setBit(eFrameFlags::ScaleChanged);
    f32 xz = WIDEN * m_scale - y;
    m_scaleVec = EGG::Vector3f(xz, y, xz);
    m_hitRadius = xz * props.hitRadius;
    m_kclRadius = y * props.kclRadius;
    return true;
}

bool ItemObjBlueShell::targetInCannon() const {
    return m_target < NO_PLAYER && Kart(m_target)->status().onBit(Kart::eStatus::InCannon);
}

ItemObj::StateFunc ItemObjBlueShell::initialState() const {
    return static_cast<StateFunc>(&ItemObjBlueShell::calcBlue);
}

} // namespace Kinoko::Item
