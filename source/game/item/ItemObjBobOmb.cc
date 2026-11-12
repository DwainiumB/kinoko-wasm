#include "ItemObjBobOmb.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/BoxColManager.hh"

#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::Item {

namespace {

constexpr u8 NO_PLAYER = 12;

/// The explosion grows to this scale (0x809C3630, from 0x808A54F8), ...
constexpr f32 EXPLOSION_SCALE = 36.0f;
/// ... this much per frame (0x809C3634).
constexpr f32 EXPLOSION_GROWTH = 3.12f;

/// @brief The radius of the full explosion: its scale times the hit radius (0x809C3638 holds
/// the square, set at 0x807A4384).
f32 ExplosionRadius() {
    return EXPLOSION_SCALE * ItemObj::Properties(ItemObjKind::BobOmb).hitRadius;
}

/// @brief The fuse a racer close by cuts it down to (0x809C363C, by engine class).
s32 NearFuse() {
    using EngineClass = System::RaceConfig::Scenario::EngineClass;

    switch (System::RaceConfig::Instance()->raceScenario().engineClass) {
    case EngineClass::CC50:
        return 180;
    case EngineClass::CC100:
        return 150;
    default:
        return 120;
    }
}

/// @addr{0x807872C0}
void Normalise(EGG::Vector3f &v) {
    f32 sq = v.ps_squareMag();
    if (sq > 0.0f) {
        v *= EGG::Mathf::frsqrt(sq);
    }
}

/// @addr{0x807912E0}
/// @brief Makes right / up / forward orthonormal again, keeping up.
void Orthonormalise(std::array<EGG::Vector3f, 3> &basis) {
    Normalise(basis[1]);
    basis[0] = basis[1].cross(basis[2]);
    Normalise(basis[0]);
    basis[2] = basis[0].cross(basis[1]);
}

/// @addr{0x80591314}
bool IsUntargetable(const Kart::KartObject *kart) {
    return kart->status().onBit(Kart::eStatus::TriggerRespawn, Kart::eStatus::InRespawn,
            Kart::eStatus::AfterRespawn, Kart::eStatus::Vanished);
}

} // namespace

ItemObjBobOmb::ItemObjBobOmb()
    : m_subState(SubState::Flying), m_nextSubState(-1), m_subFrame(0), m_bounce(0.0f),
      m_weakened(false), m_fuse(0), m_nearestKart(NO_PLAYER), m_nearestDistSq(0.0f),
      m_noTrigger(0) {}

ItemObjBobOmb::~ItemObjBobOmb() = default;

/// @addr{0x807A5BC4}
void ItemObjBobOmb::onSpawn() {
    constexpr s32 FUSE = 300;

    m_subState = SubState::Flying;
    m_nextSubState = -1;
    m_subFrame = 0;
    m_fuse = FUSE;
    m_nearestKart = NO_PLAYER;
    m_nearestDistSq = std::numeric_limits<f32>::max();
    m_noTrigger = 0;
    m_weakened = false;
    // The battle team (+0x1C4) and the model's swell (+0x1C8) are left out.
}

/// @addr{0x807A4A18}
/// @brief Thrown forward in an arc (far with the stick held up), or dropped behind with the stick
/// held down, which starts a short fuse.
bool ItemObjBobOmb::onRelease(ItemTrail &trail) {
    constexpr f32 BOUNCE_BASE = 0.3f;
    constexpr f32 BOUNCE_PER_SPEED = 0.007f;
    constexpr f32 BOUNCE_MAX = 0.8f;
    constexpr f32 THROW_SPEED = 45.0f;
    constexpr f32 FAR_THROW_SPEED = 100.0f;
    constexpr f32 UP_SPEED = 50.0f;
    constexpr f32 BACK_UP_SPEED = 25.0f;
    constexpr f32 DROP_UP_BASE = 15.0f;
    constexpr f32 DROP_UP_PER_SPEED = 10.0f;
    constexpr u32 THROW_BOUNCES = 3;
    constexpr u32 DROP_BOUNCES = 2;
    constexpr s32 DROP_FUSE = 90;

    m_flags.setBit(eFlags::OwnerImmune15, eFlags::DroppedBack);
    m_bounce = std::min(BOUNCE_BASE + BOUNCE_PER_SPEED * trail.kartSpeed(), BOUNCE_MAX);

    const auto *kartItem = trail.kartItem();
    if (!kartItem->stickDown()) {
        f32 speed = kartItem->stickUp() ? FAR_THROW_SPEED : THROW_SPEED;
        initThrow(speed, UP_SPEED, BACK_UP_SPEED, trail, THROW_BOUNCES, true);
        // The throw sound (0x807A2F18) is left out.
        return true;
    }

    f32 upSpeed =
            std::min(std::max(DROP_UP_BASE + trail.kartSpeed() * DROP_UP_PER_SPEED, DROP_UP_BASE),
                    BACK_UP_SPEED);
    initThrow(FAR_THROW_SPEED, UP_SPEED, upSpeed, trail, DROP_BOUNCES, false);
    m_fuse = DROP_FUSE;
    return false;
}

/// @addr{0x807A5F6C}
/// @brief Hit by something, or the fuse ran out: the bob-omb explodes where it is (reaction 1:
/// it keeps its own state).
s32 ItemObjBobOmb::onBreak() {
    if (m_subState == SubState::Explode) {
        return 1;
    }

    m_subState = SubState::Flying;
    m_subFrame = 0;
    m_nextSubState = static_cast<s32>(SubState::Explode);
    setState(static_cast<StateFunc>(&ItemObjBobOmb::calcBobOmb));
    // 0x8079D920 is visual.

    m_pos -= m_basis[1] * m_kclRadius;
    m_flags.resetBit(eFlags::Landed);
    m_flags.setBit(eFlags::Breaking);
    // The online break event is left out.
    m_flags.resetBit(eFlags::OwnerImmune);
    // The explosion effect is visual.
    return 1;
}

/// @addr{0x807A60F8}
/// @brief Dropped by a hit kart: it falls and lies with its fuse burning, and racers cannot set
/// it off for a while.
void ItemObjBobOmb::onDrop() {
    constexpr s32 NO_TRIGGER_FRAMES = 270;

    m_subState = SubState::Flying;
    m_nextSubState = -1;
    m_subFrame = 0;
    setState(static_cast<StateFunc>(&ItemObjBobOmb::calcBobOmb));
    m_nextSubState = static_cast<s32>(SubState::Fuse);
    m_noTrigger = NO_TRIGGER_FRAMES;
    m_flags.resetBit(eFlags::FromKart);
    m_bouncesLeft = 0.0f;
}

/// @addr{0x807A4CDC}
/// @brief Runs the sub-state, the fuse, and shortens the fuse when a racer comes close.
void ItemObjBobOmb::calcBobOmb() {
    constexpr f32 NEAR_BOUNCES = 3.0f;
    constexpr f32 DROPPED_BOUNCES = 2.0f;
    constexpr s32 NO_TRIGGER_FUSE = 16;
    constexpr f32 FAR_SQ = 64.0f;
    constexpr f32 MID_SQ = 16.0f;
    constexpr f32 CLOSE_SQ = 4.0f;
    constexpr s32 LAST_FUSE = 4;

    if (m_nextSubState < 0) {
        ++m_subFrame;
    } else {
        m_subState = static_cast<SubState>(m_nextSubState);
        m_nextSubState = -1;
        m_subFrame = 0;
        if (m_subState == SubState::Explode) {
            enterExplode();
        }
    }

    switch (m_subState) {
    case SubState::Flying:
        calcFlying();
        break;
    case SubState::Fuse:
        calcFuse();
        break;
    case SubState::Explode:
        calcExplode();
        break;
    }

    if (m_flags.onBit(eFlags::Breaking) || m_fuse < 0) {
        // 0x807A64F0
        startBreak(false);
    }

    s32 fuse = --m_fuse;
    if (m_noTrigger > 0) {
        --m_noTrigger;
        if (fuse == 0) {
            m_fuse = NO_TRIGGER_FUSE;
        }
    }

    if (!(m_bouncesLeft < NEAR_BOUNCES)) {
        return;
    }

    if (m_flags.offBit(eFlags::FromKart) && m_bouncesLeft >= DROPPED_BOUNCES) {
        return;
    }

    if (m_flags.offBit(eFlags::NoOwner)) {
        size_t count = Kart::KartObjectManager::Instance()->count();
        for (size_t i = 0; i < count; ++i) {
            checkKartNear(static_cast<u8>(i));
        }

        if (m_noTrigger == 0 && m_nearestDistSq <= FAR_SQ * ExplosionRadius() * ExplosionRadius()) {
            f32 radiusSq = ExplosionRadius() * ExplosionRadius();
            s32 nearFuse = NearFuse();
            if (m_nearestDistSq > MID_SQ * radiusSq) {
                m_fuse = std::min(m_fuse, nearFuse);
            } else if (m_nearestDistSq > CLOSE_SQ * radiusSq) {
                m_fuse = std::min(m_fuse, nearFuse >> 1);
            } else if (m_nearestDistSq > radiusSq) {
                m_fuse = std::min(m_fuse, nearFuse >> 2);
            } else if (m_fuse > LAST_FUSE) {
                m_fuse = LAST_FUSE;
            }
        }
    }

    // The blinking (+0x1E0) is visual.
}

/// @addr{0x807A5D88}
/// @brief Tracks the racer closest to the bob-omb (not its owner, nor one respawning or immune).
void ItemObjBobOmb::checkKartNear(u8 playerIdx) {
    u8 owner = m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;
    if (playerIdx == owner) {
        return;
    }

    // The battle team check (0x807A33E0) is left out. KartItem +0x240 (item immunity) is not
    // ported and taken as 0.
    const auto *kartObjMgr = Kart::KartObjectManager::Instance();
    if (IsUntargetable(kartObjMgr->object(playerIdx))) {
        return;
    }

    // The distance to the nearest racer so far is measured again: it moved.
    if (m_nearestKart < NO_PLAYER) {
        m_nearestDistSq = m_pos.ps_sqDistance(kartObjMgr->object(m_nearestKart)->pos());
    }

    f32 distSq = m_pos.ps_sqDistance(kartObjMgr->object(playerIdx)->pos());
    if (distSq < m_nearestDistSq) {
        m_nearestKart = playerIdx;
        m_nearestDistSq = distSq;
    }
}

/// @addr{0x807A4B48}
/// @brief Sub-state 0: in the air until it lands, each floor bounce lower.
void ItemObjBobOmb::calcFlying() {
    constexpr f32 DROP_GRAVITY = 2.8f;
    constexpr f32 DROP_DRAG = 0.973f;
    constexpr f32 THROW_GRAVITY = 2.3f;
    constexpr f32 THROW_DRAG = 0.999f;
    constexpr f32 THROW_BOUNCE = 0.45f;
    constexpr f32 BOUNCE_DECAY = 0.8f;
    constexpr f32 UP_RATE = 0.02f;

    if (m_flags.offBit(eFlags::FromKart)) {
        calcArc(DROP_GRAVITY, DROP_DRAG, m_bounce);
    } else {
        calcArc(THROW_GRAVITY, THROW_DRAG, THROW_BOUNCE);
    }

    if (m_frameFlags.onBit(eFrameFlags::Landed)) {
        m_bounce *= BOUNCE_DECAY;
    }

    if (m_frameFlags.onBit(eFrameFlags::Landed) &&
            m_frameFlags.offBit(eFrameFlags::Breaking, eFrameFlags::Vanishing)) {
        landOnFloor();
        setState(static_cast<StateFunc>(&ItemObjBobOmb::calcBobOmb));
        m_nextSubState = static_cast<s32>(SubState::Fuse);
    }

    // The bounce sound is visual. The model's up vector is kept: the explosion starts below it.
    m_basis[1] += (m_up - m_basis[1]) * UP_RATE;
    Orthonormalise(m_basis);
}

/// @addr{0x807A43FC}
/// @brief Sub-state 1: on the floor. One dropped by a hit falls first, and its fuse only starts
/// when it lies still.
void ItemObjBobOmb::calcFuse() {
    constexpr s32 DROP_FUSE = 90;

    if (m_flags.onBit(eFlags::Landed)) {
        calcLanded();
    } else if (m_flags.onBit(eFlags::Dropped)) {
        calcDropped();
        m_fuse = DROP_FUSE;
        m_bouncesLeft = 0.0f;
    }

    // The fuse sound and the model's animations are visual.
}

/// @addr{0x807A45E0}
/// @brief Sub-state 2 starts: the hit sphere is widened to the full explosion.
void ItemObjBobOmb::enterExplode() {
    m_flags.setBit(eFlags::ScaleFrozen);
    m_scaleBounce = 0.0f;
    m_scaleVec = EGG::Vector3f(1.0f, 1.0f, 1.0f);
    // The explosion model, its animations and the sounds are visual.

    if (m_boxColUnit) {
        m_boxColUnit->resize(ExplosionRadius(), 0.0f);
    }
}

/// @addr{0x807A4734}
/// @brief Sub-state 2: the explosion grows; it launches racers for 30 frames, then only spins
/// them, and the bob-omb is gone after 86 frames.
void ItemObjBobOmb::calcExplode() {
    constexpr s32 WEAKEN_FRAME = 30;
    constexpr s32 END_HIT_FRAME = 42;
    constexpr s32 DESPAWN_FRAME = 85;

    if (growScale(EXPLOSION_SCALE, EXPLOSION_GROWTH)) {
        setScale(m_scale);
    }

    // Frame 10 (0x8078DB10, the explosion's effect on the course) is not ported.

    if (m_subFrame == WEAKEN_FRAME) {
        m_weakened = true;
    } else if (m_subFrame == END_HIT_FRAME) {
        removeBoxCol();
        // flags2 bit 19 is not used by the ported code.
    }

    if (m_subFrame > DESPAWN_FRAME) {
        ItemDirector::Instance()->holder(m_kind).despawn(this);
    }
}

ItemObj::StateFunc ItemObjBobOmb::initialState() const {
    return static_cast<StateFunc>(&ItemObjBobOmb::calcBobOmb);
}

} // namespace Kinoko::Item
