#include "ObjectPakkunF.hh"

#include "game/kart/KartCollide.hh"

namespace Kinoko::Field {

/// @addr{0x807743A4}
ObjectPakkunF::ObjectPakkunF(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), m_attackFrames(0), m_currAttackFrame(0),
      m_waitDuration(static_cast<s32>(m_mapObj->setting(0))) {}

/// @addr{0x807754FC}
ObjectPakkunF::~ObjectPakkunF() = default;

/// @addr{0x807743E4}
void ObjectPakkunF::init() {
    m_state = State::Wait;
    m_waitFrames = m_waitDuration;

    calcTransform();
}

/// @addr{0x80774754}
void ObjectPakkunF::calc() {
    switch (m_state) {
    case State::Wait:
        calcWait();
        break;
    case State::Attack:
        calcAttack();
        break;
    case State::Damage:
        calcDamage();
        break;
    default:
        break;
    }

    calcCollisionTransform();
}

/// @addr{0x80775464}
void ObjectPakkunF::loadAnims() {
    std::array<const char *, 3> names = {{
            "attack",
            "damage",
            "wait",
    }};

    std::array<Render::AnmType, 3> types = {{
            Render::AnmType::Chr,
            Render::AnmType::Chr,
            Render::AnmType::Chr,
    }};

    linkAnims(names, types);
}

/// @addr{0x80775108}
void ObjectPakkunF::calcCollisionTransform() {
    constexpr EGG::Vector3f INIT_POS = EGG::Vector3f(0.0f, 620.0f, 70.0f);
    constexpr EGG::Vector3f FINAL_POS = EGG::Vector3f(0.0f, 160.0f, 550.0f);

    if (!m_collision) {
        return;
    }

    EGG::Vector3f posOffset;

    if (m_state == State::Attack) {
        if (m_currAttackFrame <= 10) {
            posOffset = INIT_POS * scale().x;
        } else if (m_currAttackFrame <= 20) {
            f32 fVar1 = 0.1f * static_cast<f32>(m_currAttackFrame - 10);
            posOffset = FINAL_POS * scale().x * fVar1 + INIT_POS * scale().x * (1.0f - fVar1);
        } else if (m_currAttackFrame <= 30) {
            f32 fVar1 = 0.1f * static_cast<f32>(m_currAttackFrame - 20);
            posOffset = INIT_POS * scale().x * fVar1 + FINAL_POS * scale().x * (1.0f - fVar1);
        } else {
            posOffset = INIT_POS * scale().x;
        }
    } else {
        posOffset = INIT_POS * scale().x;
    }

    EGG::Matrix34f rotMat;
    rotMat.makeR(rot());

    // Probably always evaluates to the identity matrix in time trials
    EGG::Matrix34f damageRotMat = EGG::Matrix34f::ident;
    damageRotMat.setAxisRotation(0.0f, EGG::Vector3f::ey);

    EGG::Matrix34f transformMat;
    transformMat.makeT(pos() + rotMat.multiplyTo(damageRotMat).ps_multVector(posOffset));

    m_collision->transform(transformMat, scale());
}

/// @addr{0x80774A00}
void ObjectPakkunF::calcWait() {
    if (--m_waitFrames == 0) {
        enterAttack();
    }
}

/// @addr{0x80774A84}
void ObjectPakkunF::calcAttack() {
    ++m_currAttackFrame;

    if (--m_attackFrames == 0) {
        m_state = State::Wait;
        m_waitFrames = m_waitDuration;
    }
}

/// @addr{0x80774CB0}
void ObjectPakkunF::enterAttack() {
    constexpr s32 LINGERING_FRAMES = 60; ///< Additional frames before switching back to idle state

    m_state = State::Attack;
    auto *anmMgr = m_drawMdl->anmMgr();
    anmMgr->playAnim(0.0f, 1.0f, 0);
    m_currAttackFrame = 0;
    auto *attackAnm = anmMgr->activeAnim(Render::AnmType::Chr);
    m_attackFrames = static_cast<s32>(attackAnm->frameCount()) + LINGERING_FRAMES;
}

/// @addr{0x80774F20}
/// @brief A kart in a star, mega mushroom or bullet bill knocks the plant into its damage state
/// (the rumble is left out). Any other touch only notes the racer for the VS targeting (see
/// calcDamage), which Kinoko does not have.
Kart::Reaction ObjectPakkunF::onCollision(Kart::KartObject * /*kartObj*/,
        Kart::Reaction reactionOnKart, Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        startDamage();
    }

    return reactionOnKart;
}

/// @addr{0x8077502C}
/// @brief A null item (a blast) or an item with object reaction 3 or 5 knocks the plant over.
s32 ObjectPakkunF::onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (!item) {
        startDamage();
    }

    if (reactionOnObj == ItemHit::ObjHit || reactionOnObj == ItemHit::ObjHitRaceOnly) {
        startDamage();
    }

    return reactionOnItem;
}

/// @addr{0x80774EB8}
/// @brief 180 frames of the damage animation; an attack in progress stops (the head's collision
/// goes back to its rest place).
void ObjectPakkunF::startDamage() {
    constexpr s32 DAMAGE_FRAMES = 180;

    if (m_state == State::Damage) {
        return;
    }

    m_state = State::Damage;
    m_drawMdl->anmMgr()->playAnim(0.0f, 1.0f, 1);
    m_attackFrames = DAMAGE_FRAMES;
}

/// @addr{0x80774754}
/// @brief The damage part of the game's calc: back to waiting when the time is up.
/// @details Offline VS races do not use the timed wait Kinoko has (0x807743E4 sets +0xB8 only in
/// Time Trials and online): there the plant attacks the nearest racer within 2000 (0x80782C14)
/// other than the last one it touched, turning its head toward where that kart will be
/// (0x80774CB0, +0xDC). That targeting is not in Kinoko; only the timed wait is.
void ObjectPakkunF::calcDamage() {
    if (--m_attackFrames == 0) {
        m_state = State::Wait;
        m_waitFrames = m_waitDuration;
        m_drawMdl->anmMgr()->playAnim(0.0f, 1.0f, 2);
    }
}

} // namespace Kinoko::Field
