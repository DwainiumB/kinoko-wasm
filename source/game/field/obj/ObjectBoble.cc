#include "ObjectBoble.hh"

#include "game/field/ObjectDirector.hh"

#include "game/item/ItemObj.hh"

#include "game/kart/KartObject.hh"

namespace Kinoko::Field {

/// @addr{0x8075DB3C}
ObjectBoble::ObjectBoble(const System::MapdataGeoObj &params) : ObjectCollidable(params) {}

/// @addr{0x8075E74C}
ObjectBoble::~ObjectBoble() = default;

/// @addr{0x8075DBA0}
void ObjectBoble::init() {
    m_railInterpolator->init(0.0f, 0);
    m_curTangentDir = m_railInterpolator->curTangentDir();
    m_railInterpolator->setPerPointVelocities(true);
    setScale(EGG::Vector3f::unit);
    m_knockedAway = false;
}

/// @addr{0x8075DCA0}
/// @details The sounds and the model's effect are visual.
void ObjectBoble::calc() {
    if (!m_knockedAway) {
        m_railInterpolator->calc();
        setPos(m_railInterpolator->curPos());
        calcTangent();
    } else {
        calcKnockAway();
    }
}

/// @addr{0x8075E070}
void ObjectBoble::calcTangent() {
    m_curTangentDir = Interpolate(0.2f, m_curTangentDir, m_railInterpolator->curTangentDir());
    m_curTangentDir.normalise();

    EGG::Vector3f axis = m_curTangentDir.cross(EGG::Vector3f::ex);
    if (axis.normalise() == 0.0f) {
        axis = m_curTangentDir.cross(EGG::Vector3f::ez);
        axis.normalise();
    }

    setMatrixTangentTo(axis.cross(m_curTangentDir), m_curTangentDir);
}

/// @addr{0x8075E1F4}
/// @brief A kart in a star, mega mushroom or bullet bill knocks the fireball away (the rumble is
/// left out).
Kart::Reaction ObjectBoble::onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
        Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) {
    if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        startKnockAway(hitDepth, kartObj->velocity());
    }

    return reactionOnKart;
}

/// @addr{0x8075E28C}
s32 ObjectBoble::onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f &hitDepth) {
    if (item && reactionOnObj == ItemHit::ObjHit) {
        startKnockAway(hitDepth, item->vel());
    }

    return reactionOnItem;
}

/// @addr{0x8075E2E0}
/// @brief The fireball flies off for 180 frames, away from the hit (yaw from the horizontal push
/// plus a random 0 to 180 degrees, pitched up 30 to 60), at 30, 50 or 100 by how hard the hit was,
/// tumbling at random rates. Six draws from the object generator (ObjectDirector::random).
void ObjectBoble::startKnockAway(const EGG::Vector3f &hitDepth, const EGG::Vector3f &hitterVel) {
    constexpr f32 RAD_TO_DEG = 57.295776f;  ///< 0x808A3830
    constexpr f32 SPIN_BASE = -0.10471976f; ///< 0x808A384C, -6 degrees in radians
    constexpr s32 KNOCK_FRAMES = 180;

    m_knockedAway = true;
    m_knockTimer = KNOCK_FRAMES;

    const EGG::Vector3f forward = EGG::Vector3f::ez;
    EGG::Vector3f push = -hitDepth + hitterVel;
    push.y = 0.0f;
    push.normalise();

    f32 dot = push.z * forward.z + (push.x * forward.x + push.y * forward.y);
    EGG::Vector3f c = push.cross(forward);
    f32 sinLen = EGG::Mathf::sqrt(c.z * c.z + (c.x * c.x + c.y * c.y));
    f32 angle = EGG::Mathf::atan2(sinLen, dot);
    if (angle <= 0.0f) {
        angle = -angle;
    }

    f32 yaw = angle * RAD_TO_DEG;
    if (!(hitDepth.x >= 0.0f)) {
        yaw = -yaw;
    }

    auto &random = ObjectDirector::Instance()->random();
    f32 yawDeg = (-90.0f + yaw) + random.getF32(180.0f);
    EGG::Matrix34f yawMat;
    yawMat.makeR(EGG::Vector3f(0.0f, yawDeg * DEG2RAD, 0.0f));

    f32 pitchDeg = 30.0f + random.getF32(30.0f);
    EGG::Matrix34f pitchMat;
    pitchMat.makeR(EGG::Vector3f(-pitchDeg * DEG2RAD, 0.0f, 0.0f));
    EGG::Matrix34f dirMat = yawMat.multiplyTo(pitchMat);

    f32 hitterSpeed = EGG::Mathf::sqrt(
            hitterVel.z * hitterVel.z + (hitterVel.x * hitterVel.x + hitterVel.y * hitterVel.y));
    f32 depthLen = EGG::Mathf::sqrt(
            hitDepth.z * hitDepth.z + (hitDepth.x * hitDepth.x + hitDepth.y * hitDepth.y));
    f32 strength = depthLen + hitterSpeed;
    f32 speed = 30.0f;
    if (strength >= 30.0f) {
        speed = strength >= 100.0f ? 100.0f : 50.0f;
    }

    m_knockVel = dirMat.multVector(forward) * speed;

    f32 spinZ = static_cast<f32>(random.getU32(12)) * DEG2RAD;
    f32 spinY = static_cast<f32>(random.getU32(12)) * DEG2RAD;
    f32 spinX = static_cast<f32>(random.getU32(12)) * DEG2RAD;
    m_spinRate =
            EGG::Vector3f(SPIN_BASE, SPIN_BASE, SPIN_BASE) + EGG::Vector3f(spinX, spinY, spinZ);

    switch (random.getU32(3)) {
    case 0:
        m_spinRate.x = 0.0f;
        break;
    case 1:
        m_spinRate.y = 0.0f;
        break;
    case 2:
        m_spinRate.z = 0.0f;
        break;
    default:
        break;
    }

    m_spin = EGG::Vector3f::zero;
    calcTransform();
    m_knockBaseMat = transform();
}

/// @addr{0x8075DEAC}
/// @brief Falls (2 per frame per frame) and tumbles while its rail goes on; once the 180 frames are
/// up it goes back to the rail at the next point with setting 1.
void ObjectBoble::calcKnockAway() {
    constexpr f32 GRAVITY = 2.0f;

    m_railInterpolator->calc();
    m_knockVel.y -= GRAVITY;
    EGG::Vector3f newPos = pos() + m_knockVel;
    if (--m_knockTimer == 0) {
        m_knockTimeUp = true;
    }

    m_spin += m_spinRate;
    EGG::Matrix34f spinMat;
    spinMat.makeR(m_spin);
    EGG::Matrix34f mat = m_knockBaseMat.multiplyTo(spinMat);
    mat.setBase(3, newPos);
    setTransform(mat);

    // The game never clears +0xC4 again, so a fireball knocked away a second time goes back to
    // its rail at the first setting-1 point, however short the flight.
    if (m_knockTimeUp && m_railInterpolator->curPoint().setting[1] == 1) {
        m_knockedAway = false;
    }
}

} // namespace Kinoko::Field
