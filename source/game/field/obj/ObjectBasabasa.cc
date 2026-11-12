#include "ObjectBasabasa.hh"

#include "game/field/ObjectDirector.hh"
#include "game/field/RailManager.hh"

#include "game/item/ItemObj.hh"

#include "game/kart/KartObject.hh"

#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"

namespace Kinoko::Field {

/// @addr{0x806B5C84}
ObjectBasabasaDummy::ObjectBasabasaDummy(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), StateManager(this, STATE_ENTRIES),
      m_bigBump(params.setting(7) == 1) {
    m_active = true;
}

/// @addr{0x806B7630}
ObjectBasabasaDummy::~ObjectBasabasaDummy() = default;

/// @addr{0x806B5E80}
void ObjectBasabasaDummy::init() {
    m_railInterpolator->init(0.0f, 0);
    setPos(m_railInterpolator->curPos());

    auto &rng = System::RaceManager::Instance()->random();
    rng.next();
    f32 y = rng.getF32(ObjectBasabasa::initialYRange());
    f32 x = rng.getF32(ObjectBasabasa::initialXRange());

    m_initialPos = EGG::Vector3f(x - ObjectBasabasa::initialXRange() * 0.5f, y, 0.0f);

    m_nextStateId = 0;
}

/// @addr{0x806B6874}
Kart::Reaction ObjectBasabasaDummy::onCollision(Kart::KartObject *kartObj,
        Kart::Reaction reactionOnKart, Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    auto course = System::RaceConfig::Instance()->raceScenario().course;
    if (course == Course::Dry_Dry_Ruins && m_bigBump) {
        const auto *director = ObjectDirector::Instance();
        s16 column = ObjectDirector::KartHitColumn(kartObj);
        const auto &hitTable = director->hitTableKart();
        const auto &objTable = director->hitTableKartObject();
        reactionOnKart = hitTable.kartReaction(hitTable.slot(ObjectId::BasabasaDummy), column);
        reactionOnObj = objTable.kartReaction(objTable.slot(ObjectId::BasabasaDummy), column);
    }

    // A kart in a star, mega mushroom or bullet bill knocks the bat away (the rumble is left out).
    if ((reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) &&
            m_currentStateId != 1) {
        startHit(kartObj->pos(), kartObj->velocity());
    }

    return reactionOnKart;
}

/// @addr{0x806B6A18}
/// @brief An item knocks the bat away along its velocity, or away from the item when it is slow
/// (the game also handles a null item: then backwards from the bat's facing).
s32 ObjectBasabasaDummy::onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    constexpr f32 SLOW_SPEED = 30.0f;
    constexpr f32 PUSH_SPEED = 80.0f;

    if (reactionOnObj < ItemHit::ObjHit || reactionOnObj > ItemHit::ObjHitHardRaceOnly ||
            m_currentStateId == 1) {
        return reactionOnItem;
    }

    EGG::Vector3f vel;
    if (!item) {
        calcTransform();
        vel = -transform().base(2) * PUSH_SPEED;
    } else {
        vel = item->vel();
        if (EGG::Mathf::sqrt(vel.z * vel.z + vel.x * vel.x + vel.y * vel.y) < SLOW_SPEED) {
            EGG::Vector3f away = pos() - item->pos();
            away.normalise2();
            vel = away * PUSH_SPEED;
        }
    }

    startHit(pos(), vel);
    return reactionOnItem;
}

/// @brief Common part of both hit handlers: the hit state starts on the next calc.
void ObjectBasabasaDummy::startHit(const EGG::Vector3f &hitPos, const EGG::Vector3f &hitVel) {
    m_nextStateId = 1;
    m_hitPos = hitPos;
    m_hitVel = hitVel;
}

/// @addr{0x806B6288}
/// @brief The bat was hit: it loses its collision and flies off along the hitter's velocity,
/// tilted 20 degrees up.
void ObjectBasabasaDummy::enterStateHit() {
    constexpr f32 FLY_SPEED = 150.0f;
    constexpr f32 LIFT_ANGLE = 20.0f * DEG2FIDX;

    unregisterCollision();

    EGG::Vector3f dir = m_hitVel;
    f32 sqLen = dir.x * dir.x + dir.z * dir.z;
    f32 len = sqLen > 0.0f ? EGG::Mathf::sqrt(sqLen) : 0.0f;
    dir.y = (len * EGG::Mathf::SinFIdx(LIFT_ANGLE)) / EGG::Mathf::CosFIdx(LIFT_ANGLE);
    dir.normalise2();

    m_flySpeed = FLY_SPEED;
    m_flyVel = dir * m_flySpeed;

    // The rest sets up the tumbling model's axes, the animation speed and a mission counter.
}

/// @addr{0x806B652C}
/// @brief The knocked-away bat falls under gravity; after 130 frames it is reset (init, which
/// draws from the race generator again) and hidden until the spawner sends it out again.
/// @details The tumbling rotation and the blinking are visual. A sink-down branch reads +0x14E,
/// which nothing in the game sets, so it is left out.
void ObjectBasabasaDummy::calcStateHit() {
    constexpr f32 GRAVITY = 2.0f;
    constexpr u32 HIT_DURATION = 130;

    m_flyVel.y -= GRAVITY;
    setPos(pos() + m_flyVel);

    if (m_currentFrame > HIT_DURATION) {
        init();
        m_active = false;
    }
}

/// @addr{0x806B6100}
void ObjectBasabasaDummy::calcState0() {
    if (!m_active) {
        return;
    }

    if (m_railInterpolator->calc() == RailInterpolator::Status::ChangingDirection) {
        m_active = false;
        unregisterCollision();
    } else {
        setMatrixFromOrthonormalBasisAndPos(m_railInterpolator->curTangentDir());
        calcTransform();

        EGG::Matrix34f mat = transform();
        mat.setBase(3, EGG::Vector3f::zero);
        setPos(m_railInterpolator->curPos() + mat.ps_multVector(m_initialPos));
    }
}

/// @addr{0x806B70D0}
ObjectBasabasa::ObjectBasabasa(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), m_initialTimer(params.setting(1)),
      m_batsPerGroup(params.setting(2)), m_startFrame(params.setting(6)),
      m_batSpacing(static_cast<u32>(static_cast<f32>(params.setting(5)) /
              static_cast<f32>(params.setting(0)) / static_cast<f32>(m_batsPerGroup))) {
    f32 railLen = RailManager::Instance()->rail(params.pathId())->getPathLength();
    u32 groupCount = static_cast<u32>(railLen / static_cast<f32>(params.setting(0)) /
                             static_cast<f32>(m_initialTimer)) +
            1;
    u32 batCount = groupCount * m_batsPerGroup;

    m_bats = owning_span<ObjectBasabasaDummy *>(batCount);

    for (auto *&bat : m_bats) {
        bat = EGG::egg_new<ObjectBasabasaDummy>(params);
        bat->load();
    }

    s_initialXRange = params.setting(3);
    s_initialYRange = params.setting(4);
}

/// @addr{0x806B72F4}
ObjectBasabasa::~ObjectBasabasa() = default;

/// @addr{0x806B7334}
void ObjectBasabasa::init() {
    for (auto *&bat : m_bats) {
        if (bat->active()) {
            bat->setActive(false);
            bat->unregisterCollision();
        }
    }

    m_cycleTimer = m_initialTimer;
    m_batsActive = 0;
}

/// @addr{0x806B74C4}
void ObjectBasabasa::calc() {
    if (System::RaceManager::Instance()->timer() <= m_startFrame) {
        return;
    }

    if (m_cycleTimer == m_initialTimer + m_batSpacing * (m_batsActive % m_batsPerGroup)) {
        auto *&bat = m_bats[m_batsActive++];
        bat->init();
        bat->setActive(true);
        bat->loadAABB(0.0f);

        if ((m_batsActive % m_batsPerGroup) == 0) {
            m_cycleTimer = m_batSpacing * m_batsPerGroup;
        }
    }

    m_batsActive %= m_bats.size();

    ++m_cycleTimer;
}

f32 ObjectBasabasa::s_initialXRange;
f32 ObjectBasabasa::s_initialYRange;

} // namespace Kinoko::Field
