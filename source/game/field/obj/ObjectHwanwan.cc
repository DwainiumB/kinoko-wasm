#include "ObjectHwanwan.hh"

#include "game/field/CollisionDirector.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::Field {

/// @addr{0x806E95B0}
ObjectHwanwan::ObjectHwanwan(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), StateManager(this, STATE_ENTRIES), m_initPos(pos()) {}

/// @addr{0x806EC6E0}
ObjectHwanwan::~ObjectHwanwan() = default;

/// @addr{0x806E9724}
void ObjectHwanwan::init() {
    m_workPos = m_initPos + EGG::Vector3f::ey * DIAMETER;
    m_extVel.setZero();
    m_bounceVel.setZero();
    m_tangent = EGG::Vector3f::ez;
    m_up = EGG::Vector3f::ey;
    m_targetUp = EGG::Vector3f::ey;
    m_touchingGround = false;
    m_initJumpVel = 0.0f;
    m_targetY = m_workPos.y;
    m_hitLandFrame = -1;
    m_hitJumpSpeed = 0.0f;
    m_floorVariant = 0;

    calcTransform();

    m_nextStateId = 0;
}

/// @addr{0x806E9A78}
void ObjectHwanwan::calc() {
    StateManager::calc();

    m_extVel += m_bounceVel - GRAVITY;
    m_workPos += m_extVel;
    m_bounceVel.setZero();

    checkFloorCollision();

    EGG::Matrix34f mat;
    SetRotTangentHorizontal(mat, m_up, m_tangent);
    mat.setBase(3, m_workPos);
    setTransform(mat);
}

/// @addr{0x806EA784}
void ObjectHwanwan::checkFloorCollision() {
    constexpr f32 RADIUS = DIAMETER * 0.5f;

    m_touchingGround = false;

    CollisionInfo colInfo;
    KCLTypeMask mask;
    EGG::Vector3f pos = m_workPos + EGG::Vector3f(0.0f, -DIAMETER, 0.0f);

    bool hasCol = CollisionDirector::Instance()->checkSphereFullPush(RADIUS, pos,
            EGG::Vector3f::inf, KCL_TYPE_FLOOR, &colInfo, &mask, 0);

    if (!hasCol || pos.y - m_targetY >= 300.0f) {
        return;
    }

    m_touchingGround = true;

    f32 len = colInfo.tangentOff.length();
    m_workPos += EGG::Vector3f::ey * len;

    if (colInfo.floorDist > -std::numeric_limits<f32>::min()) {
        m_targetUp = colInfo.floorNrm;
    }
    m_extVel.y = 0.0f;

    if (mask & KCL_TYPE_FLOOR) {
        auto *colDir = CollisionDirector::Instance();
        if (mask) {
            colDir->findClosestCollisionEntry(&mask, KCL_ANY);
        }
        m_floorVariant = (colDir->closestCollisionEntry()->attribute >> 8) & 7;
    }

    // A smoke effect on floors of type 1 with variant 1 is left out.
}

/// @addr{0x806E9CAC}
/// @brief A slow kart bounces off; a kart in a star, mega mushroom or bullet bill knocks the
/// Chain Chomp into a jump (the rumble is left out).
Kart::Reaction ObjectHwanwan::onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
        Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::None) {
        if (kartObj->speedRatioCapped() < 0.5f) {
            return Kart::Reaction::WallAllSpeed;
        }
    } else if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        if (m_currentStateId != 2) {
            m_nextStateId = 2;
        }
    }

    return reactionOnKart;
}

/// @addr{0x806E9D6C}
s32 ObjectHwanwan::onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj >= ItemHit::ObjHit && reactionOnObj <= ItemHit::ObjHitHardRaceOnly &&
            m_currentStateId != 2) {
        m_nextStateId = 2;
    }

    return reactionOnItem;
}

/// @addr{0x806EA288}
/// @brief The hit jump: 150 up, or 50 on Mario Circuit when the last floor had variant 1. (The
/// model's spin speeds are visual.)
void ObjectHwanwan::enterStateHit() {
    m_hitLandFrame = -1;
    m_hitJumpSpeed = 150.0f;

    auto course = System::RaceConfig::Instance()->raceScenario().course;
    if (course == Course::Mario_Circuit && m_floorVariant == 1) {
        m_hitJumpSpeed = 50.0f;
    }
}

/// @addr{0x806EA2FC}
/// @brief Rises on its own decelerating jump (on top of the usual gravity in calc), lands, makes a
/// smaller hop, and goes back to state 0 ten frames after landing. The spin and the model are
/// visual.
void ObjectHwanwan::calcStateHit() {
    constexpr f32 DECELERATION = 2.5f;
    constexpr f32 HOP_FACTOR = 0.6f;
    constexpr u32 LANDED_FRAMES = 10;

    if (m_hitLandFrame != -1 && m_currentFrame > static_cast<u32>(m_hitLandFrame) + LANDED_FRAMES &&
            m_touchingGround) {
        m_nextStateId = 0;
        return;
    }

    if (m_currentFrame > 9 && m_touchingGround) {
        m_hitLandFrame = static_cast<s32>(m_currentFrame);
    }

    if (m_hitLandFrame == -1) {
        m_workPos.y += m_hitJumpSpeed - DECELERATION * static_cast<f32>(m_currentFrame);
    } else {
        u32 sinceLanding = m_currentFrame - static_cast<u32>(m_hitLandFrame);
        m_workPos.y += HOP_FACTOR * m_hitJumpSpeed - DECELERATION * static_cast<f32>(sinceLanding);
    }
}

/// @addr{0x806EAAE8}
void ObjectHwanwan::calcUp() {
    m_up = Interpolate(0.1f, m_up, m_targetUp);
    if (m_up.squaredLength() > std::numeric_limits<f32>::epsilon()) {
        m_up.normalise2();
    } else {
        m_up = EGG::Vector3f::ey;
    }
}

/// @addr{0x806C5354}
ObjectHwanwanManager::ObjectHwanwanManager(const System::MapdataGeoObj &params)
    : ObjectCollidable(params) {
    m_hwanwan = EGG::egg_new<ObjectHwanwan>(params);
    m_hwanwan->setScale(2.0f);
    m_hwanwan->load();
}

/// @addr{0x806C56DC}
ObjectHwanwanManager::~ObjectHwanwanManager() = default;

/// @addr{0x806C571C}
void ObjectHwanwanManager::init() {
    m_railInterpolator->init(0.0f, 0);
    m_hwanwan->m_tangent = m_railInterpolator->curTangentDir();
    const auto &curPos = m_railInterpolator->curPos();
    m_hwanwan->m_workPos.x = curPos.x;
    m_hwanwan->m_workPos.z = curPos.z;
    m_hwanwan->m_targetY = curPos.y;

    m_hwanwan->calc();
    m_hwanwan->calc();

    ASSERT(m_mapObj);
    m_railInterpolator->setCurrVel(static_cast<f32>(m_mapObj->setting(0)));
}

/// @addr{0x806C5AC4}
void ObjectHwanwanManager::calc() {
    calcState();

    const auto &curPos = m_railInterpolator->curPos();
    m_hwanwan->m_workPos.x = curPos.x;
    m_hwanwan->m_workPos.z = curPos.z;
    m_hwanwan->m_targetY = curPos.y;
    m_hwanwan->m_tangent = m_railInterpolator->curTangentDir();
}

/// @addr{0x806C5DE0}
void ObjectHwanwanManager::calcState() {
    if (m_railInterpolator->calc() == RailInterpolator::Status::SegmentEnd &&
            m_railInterpolator->curPoint().setting[1] == 1 && m_hwanwan->m_currentStateId != 2) {
        m_hwanwan->m_nextStateId = 1;
    }

    if (m_hwanwan->m_currentStateId == 1 && m_hwanwan->m_currentFrame >= 60) {
        m_hwanwan->m_nextStateId = 0;
    }
}

} // namespace Kinoko::Field
