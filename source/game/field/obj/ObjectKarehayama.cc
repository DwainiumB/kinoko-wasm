#include "ObjectKarehayama.hh"

#include "game/field/ObjectDirector.hh"

#include "game/item/ItemObj.hh"

#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"
#include "game/kart/KartParam.hh"

namespace Kinoko::Field {

/// @addr{0x807672A4}
/// @brief The drop's direction is turned about the vertical axis by a random angle of up to five
/// degrees either way (one draw from the course-object generator), then the pile breaks like
/// every breakable object.
void ObjectKarehayama::breakObject(u8 playerIdx, const EGG::Vector3f &dir) {
    constexpr f32 ANGLE_RANGE = 10.0f;          ///< 0x808CE018, degrees
    constexpr f32 HALF = 0.5f;                  ///< 0x808A3B50
    constexpr f32 DEG2RAD = 0.017453292f;       ///< 0x808A3B54

    f32 r = ObjectDirector::Instance()->random().getF32(ANGLE_RANGE);
    f32 angle = (r - ANGLE_RANGE * HALF) * DEG2RAD;
    EGG::Matrix34f mat;
    mat.setAxisRotation(angle, EGG::Vector3f::ey);
    ObjectBreakable::breakObject(playerIdx, mat.ps_multVector(dir));
}

/// @brief Scattered without a drop (the slow-kart and online-item paths).
void ObjectKarehayama::scatter() {
    m_state = 2;
    disableCollision();
}

/// @brief The shared part of 0x8076734C and 0x80767448.
void ObjectKarehayama::breakByFastKart(Kart::KartObject *kartObj) {
    breakObject(static_cast<u8>(kartObj->param()->playerIdx()), kartObj->velocity());
}

/// @addr{0x8076734C}
void ObjectKarehayama::breakByKart(Kart::KartObject *kartObj) {
    if (kartObj->move()->speedRatioCapped() <= MIN_SPEED_RATIO) {
        scatter();
    } else {
        breakByFastKart(kartObj);
    }

    m_respawnTimer = RESPAWN_FRAMES;
}

/// @addr{0x80767448}
/// @details A ghost kart outside a Time Trial replay does nothing (Kinoko's VS races have no
/// ghosts). In Time Trials (ObjectDirector +0x55) every kart only scatters it.
void ObjectKarehayama::breakByKartUntimed(Kart::KartObject *kartObj) {
    if (kartObj->move()->speedRatioCapped() <= MIN_SPEED_RATIO ||
            ObjectDirector::Instance()->isTimeTrial()) {
        scatter();
    } else {
        breakByFastKart(kartObj);
    }

    m_respawnTimer = RESPAWN_FRAMES;
}

/// @addr{0x8076757C}
/// @brief The drop flies away from the item, credited to the item's owner.
void ObjectKarehayama::breakByItem(Item::ItemObj *item, const EGG::Vector3f &hitDepth) {
    breakObject(item->hitOwner(), -hitDepth);
    m_respawnTimer = RESPAWN_FRAMES;
}

/// @addr{0x80767648}
void ObjectKarehayama::breakByItemUntimed(Item::ItemObj * /*item*/,
        const EGG::Vector3f & /*hitDepth*/) {
    scatter();
    m_respawnTimer = RESPAWN_FRAMES;
}

} // namespace Kinoko::Field
