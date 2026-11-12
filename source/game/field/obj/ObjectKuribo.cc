#include "ObjectKuribo.hh"

#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectDirector.hh"

#include "game/item/ItemDirector.hh"

#include "game/kart/KartObject.hh"

#include "game/system/RaceConfig.hh"

#include <cmath>

namespace Kinoko::Field {

/// @addr{0x806DB184}
ObjectKuribo::ObjectKuribo(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), StateManager(this, STATE_ENTRIES),
      m_hiddenDuration(static_cast<s16>(params.setting(0))),
      m_dropsMushroom(!ObjectDirector::Instance()->isTimeTrial() &&
              System::RaceConfig::Instance()->raceScenario().itemMode !=
                      System::RaceConfig::Scenario::ItemMode::None) {
    ASSERT(m_mapObj);
    m_animStep = static_cast<f32>(m_mapObj->setting(2)) / 100.0f;
    m_speedStep = static_cast<f32>(m_mapObj->setting(1)) / 100.0f;
}

/// @addr{0x806DB3A0}
ObjectKuribo::~ObjectKuribo() = default;

/// @addr{0x806DB40C}
void ObjectKuribo::init() {
    calcTransform();
    m_origin = transform().base(2);

    m_railInterpolator->init(0.0f, 0);
    m_railInterpolator->setCurrVel(0.0f);

    m_currSpeed = 0.0f;
    m_animTimer = 0.0f;
    m_frameCount = 0;
    m_squashing = false;
    m_squashTimer = 0;
    m_hidden = false;
    m_hiddenTimer = 0;
    m_regrowing = false;
    m_regrowTimer = 0;
    m_knockDir = m_origin;
    m_railTangent = m_railInterpolator->curTangentDir();

    auto *anmMgr = m_drawMdl->anmMgr();
    anmMgr->playAnim(0.0f, m_animStep, 0);
    m_maxAnimTimer = anmMgr->activeAnim(Render::AnmType::Chr)->frameCount();
    m_nextStateId = 1;
}

/// @addr{0x806DB5B0}
void ObjectKuribo::calc() {
    m_animTimer = ::fmodf(static_cast<f32>(m_frameCount) * m_animStep, m_maxAnimTimer);

    StateManager::calc();

    ++m_frameCount;
}

/// @addr{0x806dd278}
void ObjectKuribo::loadAnims() {
    std::array<const char *, 2> names = {{
            "walk_l",
            "walk_r",
    }};

    std::array<Render::AnmType, 2> types = {{
            Render::AnmType::Chr,
            Render::AnmType::Chr,
    }};

    linkAnims(names, types);
}

void ObjectKuribo::enterStateStub() {}

void ObjectKuribo::calcStateStub() {}

/// @addr{0x806DC220}
/// @brief Called when the Goomba is changing direction.
void ObjectKuribo::calcStateReroute() {
    if (m_railInterpolator->curPoint().setting[0] < m_currentFrame) {
        m_nextStateId = 1;
    }

    checkSphereFull();
    calcRot();
    setMatrixTangentTo(m_rot, m_origin);
    calcHitStages();
}

/// @addr{0x806DC3F8}
/// @brief Called when Goomba is walking along the rail.
void ObjectKuribo::calcStateWalk() {
    calcAnim();
    calcHitStages();
}

/// @addr{0x806DB764}
/// @brief A kart in a star, mega mushroom or bullet bill squashes the Goomba (the rumble is left
/// out).
Kart::Reaction ObjectKuribo::onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
        Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        calcKnockDir(kartObj->velocity());
        if (!m_squashing) {
            squash(static_cast<u8>(kartObj->param()->playerIdx()));
        }
    }

    return reactionOnKart;
}

/// @addr{0x806DBA98}
/// @brief An item squashes the Goomba; the mushroom is credited to the item's thrower. (The game
/// also accepts a null item: then nothing is dropped.)
s32 ObjectKuribo::onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj < ItemHit::ObjHit || reactionOnObj > ItemHit::ObjHitHardRaceOnly) {
        return reactionOnItem;
    }

    bool alreadyHit = m_squashing || m_hidden;
    if (item) {
        calcKnockDir(item->vel());
    }

    if (m_squashing) {
        return reactionOnItem;
    }

    if (item) {
        squash(item->hitOwner());
    } else if (!alreadyHit) {
        disableCollision();
        m_squashScaleY = 1.0f;
        m_squashScaleXZ = 1.0f;
        m_squashFrames = 0;
        m_squashing = true;
        m_squashTimer = 0;
    }

    return reactionOnItem;
}

/// @addr{0x806DD128}
/// @brief The Goomba's facing at the start (m_origin), turned around when the hitter's
/// horizontal velocity, put at the middle of the rail, lies on the other side of the rail's line.
void ObjectKuribo::calcKnockDir(const EGG::Vector3f &hitterVel) {
    f32 along = m_railTangent.z * hitterVel.z + m_railTangent.x * hitterVel.x +
            m_railTangent.y * hitterVel.y;
    EGG::Vector3f flat = hitterVel - m_railTangent * along;

    const EGG::Vector3f &first = m_railInterpolator->point(0).pos;
    const EGG::Vector3f &last = m_railInterpolator->point(m_railInterpolator->pointCount() - 1).pos;
    EGG::Vector3f mid = (first + last) * 0.5f;

    m_knockDir = m_origin;
    EGG::Vector3f p = mid + flat;

    // 0x806B38A8: the 2D cross product of (last - first) and (p - first).
    f32 side = (last.x - first.x) * (p.z - first.z) - (p.x - first.x) * (last.z - first.z);
    if (side > 0.0f) {
        m_knockDir *= -1.0f;
    }
}

/// @addr{0x806DC460}
/// @brief Starts the squash: the collision goes off until the Goomba has grown back, and a
/// mushroom flies out along m_knockDir (no random draw: ItemDirector::spawnFromObject). The sound,
/// an effect stub (0x806B5A38, empty) and the mission counter are left out.
void ObjectKuribo::squash(u8 playerIdx) {
    constexpr f32 DROP_SPEED = 50.0f;

    disableCollision();
    m_squashScaleY = 1.0f;
    m_squashScaleXZ = 1.0f;
    m_squashFrames = 0;
    m_squashing = true;
    m_squashTimer = 0;

    if (m_dropsMushroom) {
        Item::ItemDirector::Instance()->spawnFromObject(Item::ItemObjKind::Mushroom, pos(),
                m_knockDir * DROP_SPEED, playerIdx);
    }
}

/// @addr{0x806DCFD8}
/// @brief The hit stages, each after the state's own update. Each stage calls calcAnim again, so
/// a hit Goomba in the walk state moves along its rail twice per frame (as in the game).
void ObjectKuribo::calcHitStages() {
    if (m_squashing) {
        calcSquash();
    }

    if (m_hidden) {
        calcHidden();
    }

    if (m_regrowing) {
        calcRegrow();
    }
}

/// @addr{0x806DC5E0}
/// @brief Bounces on a sine (period 20 frames, deeper once below full height) until flatter than
/// 0.1, widening up to 1.5.
void ObjectKuribo::calcSquash() {
    constexpr f32 MIN_SCALE = 0.1f;
    constexpr f32 MAX_WIDTH = 1.5f;
    constexpr f32 WIDEN_STEP = 0.1f;
    constexpr f32 TWO_PI = 6.2831855f;      ///< 0x808A03BC
    constexpr f32 RAD_TO_FIDX = 40.743664f; ///< 0x808A03C4

    calcAnim();

    f32 amplitude = 1.5f - 1.0f;
    if (m_squashScaleY < 1.0f) {
        amplitude = 1.0f;
    }

    f32 angle = RAD_TO_FIDX * ((TWO_PI * static_cast<f32>(m_squashTimer)) / (4.0f * 5.0f));
    f32 scaleY = 1.0f + amplitude * EGG::Mathf::SinFIdx(angle);
    m_squashScaleY = scaleY;

    if (scaleY < MIN_SCALE) {
        m_squashScaleY = MIN_SCALE;
        m_squashFrames = static_cast<u16>(m_squashTimer);
        m_squashing = false;
        m_squashTimer = 0;
        m_hidden = true;
        m_hiddenTimer = 0;
        setScale(EGG::Vector3f(MAX_WIDTH, MIN_SCALE, MAX_WIDTH));
        return;
    }

    if (scaleY < 1.0f) {
        m_squashScaleXZ += WIDEN_STEP;
        if (m_squashScaleXZ > MAX_WIDTH) {
            m_squashScaleXZ = MAX_WIDTH;
        }
    }

    setScale(EGG::Vector3f(m_squashScaleXZ, m_squashScaleY, m_squashScaleXZ));
    ++m_squashTimer;
}

/// @addr{0x806DC790}
/// @brief Waits (blinking, visual) until m_hiddenDuration frames have passed since the hit.
void ObjectKuribo::calcHidden() {
    calcAnim();

    if (static_cast<s32>(m_hiddenDuration) < m_hiddenTimer + static_cast<s32>(m_squashFrames)) {
        m_hidden = false;
        m_hiddenTimer = 0;
        m_regrowing = true;
        m_regrowTimer = 0;
    } else {
        ++m_hiddenTimer;
    }
}

/// @addr{0x806DC9D8}
/// @brief Grows back on a sine (overshooting to 1.5); the collision comes back at full size.
void ObjectKuribo::calcRegrow() {
    constexpr f32 MAX_HEIGHT = 1.5f;
    constexpr f32 WIDEN_STEP = 0.1f;
    constexpr f32 MIN_FRAMES = 10.0f;
    constexpr f32 TWO_PI = 6.2831855f;      ///< 0x808A03BC
    constexpr f32 RAD_TO_FIDX = 40.743664f; ///< 0x808A03C4

    calcAnim();

    f32 angle = RAD_TO_FIDX * ((TWO_PI * static_cast<f32>(m_regrowTimer)) / (4.0f * 10.0f));
    f32 scaleY = MAX_HEIGHT * EGG::Mathf::SinFIdx(angle);
    m_squashScaleY = scaleY;

    if (static_cast<f32>(m_regrowTimer) > MIN_FRAMES && scaleY <= 1.0f) {
        m_regrowing = false;
        m_regrowTimer = 0;
        enableCollision();
        setScale(EGG::Vector3f(1.0f, 1.0f, 1.0f));
        return;
    }

    m_squashScaleXZ += WIDEN_STEP;
    if (m_squashScaleXZ > 1.0f) {
        m_squashScaleXZ = 1.0f;
    }

    setScale(EGG::Vector3f(m_squashScaleXZ, m_squashScaleY, m_squashScaleXZ));
    ++m_regrowTimer;
}

/// @addr{0x806DCDDC}
void ObjectKuribo::calcAnim() {
    bool shouldMove;

    if (m_railInterpolator->isMovementDirectionForward()) {
        shouldMove = m_animTimer > 15.0f && m_animTimer < 25.0f;
    } else {
        shouldMove = m_animTimer > 45.0f && m_animTimer < 55.0f;
    }

    if (shouldMove) {
        m_currSpeed = std::min(10.0f, m_currSpeed + m_speedStep);
    } else {
        m_currSpeed = std::max(0.0f, m_currSpeed - m_speedStep);
    }

    m_railInterpolator->setCurrVel(m_currSpeed);
    const auto &curPos = m_railInterpolator->curPos();
    setPos(EGG::Vector3f(curPos.x, pos().y, curPos.z));

    if (m_railInterpolator->calc() == RailInterpolator::Status::ChangingDirection) {
        m_nextStateId = 0;
        m_currSpeed = 0.0f;
    }

    checkSphereFull();
    calcRot();
    setMatrixTangentTo(m_rot, m_origin);
}

/// @addr{0x806DCC9C}
void ObjectKuribo::calcRot() {
    m_rot = Interpolate(0.1f, m_rot, m_floorNrm);

    if (m_rot.squaredLength() > std::numeric_limits<f32>::epsilon()) {
        m_rot.normalise2();
    } else {
        m_rot = EGG::Vector3f::ey;
    }
}

/// @addr{0x806DCB58}
void ObjectKuribo::checkSphereFull() {
    constexpr f32 RADIUS = 50.0f;

    // Apply gravity if we're not changing direction
    if (m_currentStateId != 0) {
        subPos(EGG::Vector3f(0.0f, 2.0f, 0.0f));
    }

    CollisionInfo colInfo;
    EGG::Vector3f colPos = pos();
    colPos.y += RADIUS;

    bool hasCol = CollisionDirector::Instance()->checkSphereFull(RADIUS, colPos, EGG::Vector3f::inf,
            KCL_TYPE_FLOOR, &colInfo, nullptr, 0);

    if (hasCol) {
        addPos(colInfo.tangentOff);

        if (colInfo.floorDist > -std::numeric_limits<f32>::min()) {
            m_floorNrm = colInfo.floorNrm;
        }
    }
}

} // namespace Kinoko::Field
