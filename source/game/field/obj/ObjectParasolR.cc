#include "ObjectParasolR.hh"

#include "game/field/CollisionDirector.hh"

#include "game/kart/KartCollide.hh"

namespace Kinoko::Field {

/// @addr{0x8077902C}
ObjectParasolR::ObjectParasolR(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), m_hopping(false), m_vel(EGG::Vector3f::zero) {}

/// @addr{0x80779EFC}
ObjectParasolR::~ObjectParasolR() = default;

/// @addr{0x8077906C}
/// @details The rest picks the open or closed animation from setting 0 (visual).
void ObjectParasolR::init() {
    m_hopping = false;
}

/// @addr{0x80779174}
/// @details The nearest-racer search (0x80782C14) only re-arms the visual open/close toggle.
void ObjectParasolR::calc() {
    constexpr f32 GRAVITY = 2.0f;

    if (!m_hopping) {
        return;
    }

    m_vel.y -= GRAVITY;
    setPos(pos() + m_vel);
    calcBounce();
}

/// @addr{0x8077971C}
/// @details Object reactions 3 and 5 only toggle the parasol open or closed (visual).
Kart::Reaction ObjectParasolR::onCollision(Kart::KartObject * /*kartObj*/,
        Kart::Reaction reactionOnKart, Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::UNK_4 || reactionOnObj == Kart::Reaction::UNK_6) {
        startHop();
    }

    return reactionOnKart;
}

/// @addr{0x80779A68}
s32 ObjectParasolR::onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == ItemHit::ObjHitHard || reactionOnObj == ItemHit::ObjHitHardRaceOnly) {
        startHop();
    }

    return reactionOnItem;
}

/// @brief The hop starts at 100 straight up (the bounce sound count +0xD4 is left out).
void ObjectParasolR::startHop() {
    constexpr f32 HOP_SPEED = 100.0f;

    m_hopping = true;
    m_vel = EGG::Vector3f::ey * HOP_SPEED;
}

/// @addr{0x80779400}
/// @brief On the floor (a 100 sphere 100 above the parasol) it bounces back at a fifth of its
/// speed, until it is slower than 0.2. The sounds are left out.
void ObjectParasolR::calcBounce() {
    constexpr f32 RADIUS = 100.0f;
    constexpr f32 BOUNCE = 0.2f;

    CollisionInfo info;
    if (!CollisionDirector::Instance()->checkSphereFull(RADIUS,
                pos() + EGG::Vector3f(0.0f, RADIUS, 0.0f), EGG::Vector3f::inf, KCL_TYPE_FLOOR,
                &info, nullptr, 0)) {
        return;
    }

    m_vel.y *= -BOUNCE;
    setPos(pos() + info.tangentOff);

    if (EGG::Mathf::sqrt(m_vel.z * m_vel.z + m_vel.x * m_vel.x + m_vel.y * m_vel.y) < BOUNCE) {
        m_hopping = false;
    }
}

} // namespace Kinoko::Field
