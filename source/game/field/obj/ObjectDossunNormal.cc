#include "ObjectDossunNormal.hh"

namespace Kinoko::Field {

/// @addr{0x80760188}
ObjectDossunNormal::ObjectDossunNormal(const System::MapdataGeoObj &params)
    : ObjectDossun(params) {}

/// @addr{0x80760188}
ObjectDossunNormal::~ObjectDossunNormal() = default;

/// @addr{0x8076023C}
void ObjectDossunNormal::init() {
    ObjectDossun::init();

    m_stompState = StompState::Inactive;
    // The resting angle, kept in (-pi, pi]: only an angle of at least pi (0x808A38F0) loses a turn
    // (0x808A38EC). The comparison was reversed, so almost every Thwomp rested at its angle minus
    // 2 pi: the same direction, but its sin / cos fall on other table steps (Dolphin test10).
    m_currRot = rot().y;

    if (m_currRot >= F_PI) {
        m_currRot -= F_TAU;
    }
}

/// @addr{0x807602E0}
void ObjectDossunNormal::calc() {
    m_touchingGround = false;

    switch (m_stompState) {
    case StompState::Inactive:
        calcInactive();
        break;
    case StompState::Active:
        calcStomp();
        break;
    default:
        break;
    }

    if (m_spinning) {
        calcSpin();
    }
}

/// @addr{0x80760AD4}
s32 ObjectDossunNormal::onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem,
        s32 reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == ItemHit::ObjHit || reactionOnObj == ItemHit::ObjHitRaceOnly) {
        startSpin();
    }

    return reactionOnItem;
}

/// @addr{0x80760A44}
/// @brief Spins for at least 60 frames (the animation is left out).
void ObjectDossunNormal::startSpin() {
    constexpr f32 SPIN_SPEED = 60.0f;
    constexpr s32 SPIN_FRAMES = 60;

    if (m_spinning) {
        return;
    }

    m_anmState = AnmState::Spinning;
    m_spinSpeed = SPIN_SPEED;
    m_spinning = true;
    m_spinFrames = SPIN_FRAMES;
}

/// @addr{0x80760820}
void ObjectDossunNormal::startStill() {
    m_anmState = AnmState::Still;
    m_shakePhase = 0;
    m_vel = 0.0f;
    setRot(EGG::Vector3f(rot().x, m_currRot, rot().z));
    m_stompState = StompState::Inactive;
    m_stillTimer = static_cast<s32>(m_mapObj->setting(3));
}

/// @addr{0x80760964}
void ObjectDossunNormal::startBeforeFall() {
    m_stompState = StompState::Active;
    m_anmState = AnmState::BeforeFall;
    m_beforeFallTimer = static_cast<s32>(BEFORE_FALL_DURATION);
    m_cycleTimer = static_cast<s32>(m_fullDuration);
}

/// @addr{0x80760490}
void ObjectDossunNormal::calcInactive() {
    constexpr s32 SHAKE_DURATION = 30;
    constexpr s32 SHAKE_PHASE_CHANGE = 30;
    constexpr f32 SHAKE_AMPLITUDE = 30.0f;

    if (--m_stillTimer == 0) {
        startBeforeFall();
    }

    if (m_stillTimer <= SHAKE_DURATION) {
        m_shakePhase += SHAKE_PHASE_CHANGE;
        f32 posY = m_initialPosY +
                SHAKE_AMPLITUDE * EGG::Mathf::sin(static_cast<f32>(m_shakePhase) * DEG2RAD);
        setPos(EGG::Vector3f(pos().x, posY, pos().z));
    }
}

} // namespace Kinoko::Field
