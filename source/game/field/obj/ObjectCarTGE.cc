#include "ObjectCarTGE.hh"

#include "game/field/ObjectCollisionCylinder.hh"
#include "game/field/ObjectDirector.hh"
#include "game/field/RailManager.hh"
#include "game/field/obj/ObjectHighwayManager.hh"

#include "game/kart/KartObject.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::Field {

/// @addr{0x806D5EE4}
ObjectCarTGE::ObjectCarTGE(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), StateManager(this, STATE_ENTRIES), m_auxCollision(nullptr),
      m_carName{}, m_mdlName{}, m_carType(CarType::Normal), m_dummyId(ObjectId::None),
      m_scaledTangentDir(EGG::Vector3f::zero), m_currSpeed(0.0f), m_up(EGG::Vector3f::zero),
      m_tangent(EGG::Vector3f::zero), m_launched(false), m_launchTimer(0), m_launchBaseY(0.0f),
      m_launchRollRate(0.0f), m_landFrame(0.0f), m_landed(false), m_landedAgain(false),
      m_bouncedOnLeft(false) {
    m_squashed.fill(false);
    u32 carVariant = static_cast<u32>(params.setting(3));
    m_highwayVel = static_cast<f32>(params.setting(2));
    m_localVel = static_cast<f32>(params.setting(1));

    s16 pathId = params.pathId();

    // The base game returns before the StateManager sets its state entries.
    // Since we handle this in the template specialization's constructor in Kinoko,
    // we need to reset the StateManager parameters back to their default values before returning.
    if (pathId == -1) {
        m_nextStateId = -1;
        m_currentStateId = 0;
        m_currentFrame = 0;

        return;
    }

    // The base game preemptively calculates collision for all points along the rail.
    RailManager::Instance()->rail(pathId)->checkSphereFull();

    const auto *name = getName();

    if (strcmp(name, "car_body") == 0) {
        snprintf(m_carName, sizeof(m_carName), "%s", "K_car_body");
        m_carType = CarType::Normal;
    } else if (strcmp(name, "kart_truck") == 0) {
        snprintf(m_carName, sizeof(m_carName), "%s", "K_truck");
        m_carType = CarType::Truck;
    } else if (strcmp(name, "K_bomb_car") == 0) {
        PANIC("Bomb cars are not implemented!");
    }

    const auto *resourceName = getResources();

    switch (carVariant) {
    case 0: {
        if (strcmp(resourceName, "K_truck") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_b", "K_truck");
        } else if (strcmp(resourceName, "K_car_body") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_b", "K_car");
        }
    } break;
    case 1: {
        if (strcmp(resourceName, "K_truck") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_o", "K_truck");
        } else if (strcmp(resourceName, "K_car_body") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_r", "K_car");
        }
    } break;
    case 2: {
        if (strcmp(resourceName, "K_truck") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_g", "K_truck");
        } else if (strcmp(resourceName, "K_car_body") == 0) {
            snprintf(m_mdlName, sizeof(m_mdlName), "%s_y", "K_car");
        }
    } break;
    default: {
        PANIC("Bomb cars are not implemented!");
        break;
    }
    }

    switch (m_carType) {
    case CarType::Normal:
        m_dummyId = ObjectDirector::Instance()->flowTable().getIdFromName("car_body_dummy");
        break;
    case CarType::Truck:
        m_dummyId = ObjectDirector::Instance()->flowTable().getIdFromName("kart_truck_dummy");
        break;
    default:
        PANIC("Bomb cars are not implemented!");
        break;
    }

    if (System::RaceConfig::Instance()->raceScenario().course == Course::Moonview_Highway) {
        registerManagedObject();
    }
}

/// @addr{0x806D691C}
ObjectCarTGE::~ObjectCarTGE() {
    EGG::egg_delete(m_auxCollision);
}

/// @addr{0x806D6B14}
void ObjectCarTGE::init() {
    constexpr f32 HIT_ANGLE_TRUCK = 20.0f;
    constexpr f32 HIT_ANGLE_NORMAL = 40.0f;

    ASSERT(m_mapObj);
    if (m_mapObj->pathId() == -1) {
        return;
    }

    u16 idx = m_mapObj->setting(0);
    m_railInterpolator->init(0.0f, idx);

    auto *rail = RailManager::Instance()->rail(m_mapObj->pathId());
    u16 speedSetting = rail->points()[idx].setting[1];
    if (speedSetting == 1) {
        m_railInterpolator->setCurrVel(m_highwayVel);
    } else if (speedSetting == 0) {
        m_railInterpolator->setCurrVel(m_localVel);
    }

    u16 curPointSpeedSetting = m_railInterpolator->curPoint().setting[1];
    u16 nextPointSpeedSetting = m_railInterpolator->nextPoint().setting[1];

    if (curPointSpeedSetting == 0 && nextPointSpeedSetting == 1) {
        m_nextStateId = 1;
    } else if (curPointSpeedSetting == 1 && nextPointSpeedSetting == 0) {
        m_nextStateId = 2;
    }

    m_squashed.fill(false);
    m_launched = false;
    setPos(m_railInterpolator->curPos());
    m_currSpeed = m_railInterpolator->speed();
    m_scaledTangentDir = m_railInterpolator->curTangentDir() * m_currSpeed;
    m_hitAngle = (m_carType == CarType::Truck) ? HIT_ANGLE_TRUCK : HIT_ANGLE_NORMAL;
}

/// @addr{0x806D6ECC}
void ObjectCarTGE::calc() {
    StateManager::calc();

    if (m_railInterpolator->calc() == RailInterpolator::Status::SegmentEnd) {
        u16 curPointSpeedSetting = m_railInterpolator->curPoint().setting[1];
        u16 nextPointSpeedSetting = m_railInterpolator->nextPoint().setting[1];

        if (curPointSpeedSetting == 0 && nextPointSpeedSetting == 1) {
            m_nextStateId = 1;
        } else if (curPointSpeedSetting == 1 && nextPointSpeedSetting == 0) {
            m_nextStateId = 2;
        }
    }

    if (!m_launched) {
        calcPos();
    } else {
        calcLaunch();
    }

    m_hasAuxCollision = false;
}

/// @addr{0x806D7AF8}
void ObjectCarTGE::createCollision() {
    constexpr f32 TRUCK_RADIUS = 190.0f;
    constexpr f32 TRUCK_HEIGHT = 500.0f;
    constexpr f32 NORMAL_RADIUS = 150.0f;
    constexpr f32 NORMAL_HEIGHT = 200.0f;

    ASSERT(m_carType == CarType::Truck || m_carType == CarType::Normal);

    bool isTruck = (m_carType == CarType::Truck);

    ObjectCollidable::createCollision();

    f32 radius = isTruck ? TRUCK_RADIUS : NORMAL_RADIUS;
    f32 height = isTruck ? TRUCK_HEIGHT : NORMAL_HEIGHT;

    m_auxCollision = EGG::egg_new<ObjectCollisionCylinder>(radius, height, collisionCenter());
}

/// @addr{0x806D7BF8}
void ObjectCarTGE::calcCollisionTransform() {
    auto *col = collision();

    if (!col) {
        return;
    }

    calcTransform();
    col->transform(transform(), scale(), m_scaledTangentDir);
    calcTransform();
    EGG::Matrix34f mat;
    SetRotTangentHorizontal(mat, transform().base(2), EGG::Vector3f::ey);
    calcTransform();
    mat.setBase(3, transform().base(3));
    m_auxCollision->transform(mat, scale(), m_scaledTangentDir);
}

/// @addr{0x806D69C0}
f32 ObjectCarTGE::getCollisionRadius() const {
    constexpr f32 NORMAL_RADIUS = 600.0f;
    constexpr f32 TRUCK_RADIUS = 1100.0f;

    ASSERT(m_carType == CarType::Truck || m_carType == CarType::Normal);
    return (m_carType == CarType::Truck) ? TRUCK_RADIUS : NORMAL_RADIUS;
}

/// @addr{0x806D7328}
/// @details A car on the course of a bomb car sets the reaction from its own state; bomb cars are
/// not in Kinoko. The rumble and the driver's look-round (0x80590F58) are left out.
Kart::Reaction ObjectCarTGE::onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
        Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) {
    constexpr u32 SQUASH_INVULNERABILITY = 200;

    if (!m_hasAuxCollision) {
        EGG::Vector3f hitDepthNorm = hitDepth;
        hitDepthNorm.normalise2();

        if (hitDepthNorm.y > 0.9f) {
            return Kart::Reaction::UntrickableJumpPad;
        }
    }

    size_t playerIdx = kartObj->param()->playerIdx();
    if (m_highwayMgr && m_highwayMgr->squashTimer(playerIdx) < SQUASH_INVULNERABILITY) {
        const auto *director = ObjectDirector::Instance();
        s16 column = ObjectDirector::KartHitColumn(kartObj);
        const auto &hitTable = director->hitTableKart();
        const auto &objTable = director->hitTableKartObject();
        reactionOnKart =
                hitTable.kartReaction(hitTable.slot(static_cast<ObjectId>(m_dummyId)), column);
        reactionOnObj =
                objTable.kartReaction(objTable.slot(static_cast<ObjectId>(m_dummyId)), column);
    }

    if (reactionOnObj == Kart::Reaction::None) {
        if (reactionOnKart != Kart::Reaction::None &&
                reactionOnKart != Kart::Reaction::WallAllSpeed) {
            m_squashed[playerIdx] = true;
            calcTransform();
            EGG::Vector3f v2 = transform().base(2);
            v2.y = 0.0f;
            v2.normalise2();
            EGG::Vector3f posDelta = kartObj->pos() - pos();
            posDelta.y = 0.0f;
            posDelta.normalise2();

            if (v2.dot(posDelta) < EGG::Mathf::CosFIdx(0.7111111f * m_hitAngle) &&
                    !m_hasAuxCollision) {
                reactionOnKart = Kart::Reaction::LaunchSpin;
            }

            hitDepth.setZero();
        }
    } else if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        if (reactionOnKart != Kart::Reaction::None) {
            m_squashed[playerIdx] = true;
        }

        startLaunch();
    }

    return reactionOnKart;
}

/// @addr{0x806D7780}
/// @details The game also notes a hit by a bob-omb (+0x218), which only bomb cars read.
s32 ObjectCarTGE::onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj >= ItemHit::ObjHit && reactionOnObj <= ItemHit::ObjHitHardRaceOnly) {
        startLaunch();
    }

    return reactionOnItem;
}

/// @addr{0x806D7EA4}
/// @brief The car loses its collision and flies 100 per frame up (3 per frame per frame down),
/// rolling 390/(200/3) degrees per frame, while following its rail.
void ObjectCarTGE::startLaunch() {
    constexpr f32 LAUNCH_SPEED = 100.0f;
    constexpr f32 GRAVITY = 3.0f;
    constexpr f32 ROLL_TOTAL = 390.0f;

    if (m_launched) {
        return;
    }

    m_launched = true;
    m_landed = false;
    m_landedAgain = false;
    m_launchTimer = 0;
    disableCollision();
    f32 airTime = (2.0f * LAUNCH_SPEED) / GRAVITY;
    m_landFrame = airTime;
    m_launchBaseY = pos().y;
    m_launchRollRate = ROLL_TOTAL / airTime;
}

/// @addr{0x806D827C}
/// @brief One frame of the launch: the car keeps its place on the rail but flies on a parabola
/// above it, rolling about its forward axis; when its lower side corner (350 to the side) comes
/// below the rail it bounces once (30% of the speed, rolling back), and the second landing ends the
/// launch with the collision back. The sounds are left out. Where the game takes the drawn model's
/// matrix at a landing, Kinoko takes the car's own (the same rotation: nothing turns it in
/// between).
void ObjectCarTGE::calcLaunch() {
    constexpr f32 NORMAL_LENGTH = 1500.0f;
    constexpr f32 TRUCK_LENGTH = 1600.0f;
    constexpr f32 LAUNCH_SPEED = 100.0f;
    constexpr f32 GRAVITY = 3.0f;
    constexpr f32 BOUNCE_FACTOR = 0.3f;
    constexpr f32 CORNER_OFFSET = 350.0f;
    constexpr u32 MIN_AIR_FRAMES = 6;
    constexpr f32 MIN_BOUNCE_FRAMES = 5.0f;

    f32 length = (m_carType == CarType::Truck) ? TRUCK_LENGTH : NORMAL_LENGTH;
    f32 t = length * scale().z * 0.5f;

    EGG::Vector3f behindPos;
    EGG::Vector3f behindTangent;
    m_railInterpolator->evalCubicBezierOnPath(t, behindPos, behindTangent);

    const EGG::Vector3f railPos = m_railInterpolator->curPos();
    EGG::Vector3f dir = railPos - behindPos;
    dir.normalise2();
    m_tangent.x += 0.1f * (dir.x - m_tangent.x);
    m_tangent.z += 0.1f * (dir.z - m_tangent.z);
    m_tangent.y = dir.y;
    m_tangent.normalise2();

    EGG::Vector3f forward = m_tangent;
    EGG::Vector3f up = OrthonormalBasis(forward).base(1);

    // A global in the game (0x808C5880), set here before every use.
    f32 rollBack = 20.0f;
    if (forward.y <= 0.05f) {
        rollBack = 24.0f;
        if (forward.y < -0.05f) {
            rollBack = 33.0f;
        }
    }

    f32 rollSign = m_bouncedOnLeft ? 1.0f : -1.0f;
    f32 frame = static_cast<f32>(m_launchTimer);
    f32 roll;
    if (!m_landed) {
        roll = frame * m_launchRollRate * DEG2RAD;
    } else {
        roll = m_landFrame * m_launchRollRate * DEG2RAD -
                (frame - m_landFrame) * ((rollSign * rollBack) / (BOUNCE_FACTOR * m_landFrame)) *
                        DEG2RAD;
    }

    // 0x806B3AC4: turn `up` about `forward`.
    EGG::Matrix34f rollMat;
    rollMat.setAxisRotation(roll, forward);
    rollMat.setBase(3, EGG::Vector3f::zero);
    up = rollMat.multVector(up);
    up.normalise2();
    forward.normalise2();

    setPos(railPos - (forward * t) * 0.5f);

    f32 height;
    if (!m_landed) {
        height = CalcParabolicDisplacement(LAUNCH_SPEED, GRAVITY, m_launchTimer);
    } else {
        u32 sinceLanding = static_cast<u32>(frame - m_landFrame);
        height = CalcParabolicDisplacement(BOUNCE_FACTOR * LAUNCH_SPEED, GRAVITY, sinceLanding);
    }

    calcTransform();
    EGG::Matrix34f rot = transform();
    rot.setBase(3, EGG::Vector3f::zero);
    EGG::Vector3f rightCorner = rot.multVector(EGG::Vector3f(CORNER_OFFSET, 0.0f, 0.0f));
    EGG::Vector3f leftCorner = rot.multVector(EGG::Vector3f(-CORNER_OFFSET, 0.0f, 0.0f));

    EGG::Vector3f base(pos().x, m_launchBaseY + height, pos().z);
    EGG::Vector3f rightPos = base + rightCorner;
    EGG::Vector3f leftPos = base + leftCorner;

    bool leftLower = false;
    f32 lowestY = rightPos.y;
    if (leftPos.y < rightPos.y) {
        leftLower = true;
        lowestY = leftPos.y;
    }

    f32 cornerDrop = rightCorner.y;
    if (leftCorner.y < rightCorner.y) {
        cornerDrop = leftCorner.y;
    }

    f32 railY = pos().y;
    if (railY <= lowestY || m_launchTimer < MIN_AIR_FRAMES || m_landed || m_landedAgain) {
        if (railY <= lowestY || frame <= MIN_BOUNCE_FRAMES + m_landFrame || !m_landed ||
                m_landedAgain) {
            setPos(EGG::Vector3f(pos().x, m_launchBaseY + height, pos().z));
            m_tangent = forward;
            m_up = up;
            setMatrixTangentTo(m_up, m_tangent);
        } else {
            // The second landing.
            m_launched = false;
            enableCollision();
            m_landedAgain = true;
            m_landFrame = frame;

            EGG::Matrix34f mat = transform();
            mat.setBase(3, pos());
            setTransform(mat);
            f32 y = pos().y - cornerDrop;
            setPos(EGG::Vector3f(pos().x, y, pos().z));
            m_launchBaseY = y;
            calcTransform();
            m_up = transform().base(1);
        }
    } else {
        // The first landing: bounce.
        m_landed = true;
        if (leftLower) {
            m_bouncedOnLeft = true;
        }

        m_landFrame = frame;
        EGG::Matrix34f mat = transform();
        mat.setBase(3, pos());
        setTransform(mat);
        f32 y = pos().y - cornerDrop;
        setPos(EGG::Vector3f(pos().x, y, pos().z));
        m_launchBaseY = y;
    }

    ++m_launchTimer;
}

/// @addr{0x806DA660}
bool ObjectCarTGE::checkCollision(ObjectCollisionBase *lhs, EGG::Vector3f &dist) {
    dist = EGG::Vector3f::zero;
    bool hasCol = lhs->check(*m_collision, dist);

    if (!hasCol) {
        hasCol = lhs->check(*m_auxCollision, dist);
        m_hasAuxCollision = hasCol;
    }

    return hasCol;
}

/// @addr{0x806D7CF8}
const EGG::Vector3f &ObjectCarTGE::collisionCenter() const {
    static constexpr EGG::Vector3f CENTER_TRUCK = EGG::Vector3f(0.0f, 300.0f, 0.0f);
    static constexpr EGG::Vector3f CENTER_NORMAL = EGG::Vector3f(0.0f, 100.0f, 0.0f);
    static constexpr EGG::Vector3f CENTER_DEFAULT = EGG::Vector3f(0.0f, 0.0f, 0.0f);

    switch (m_carType) {
    case CarType::Truck:
        return CENTER_TRUCK;
    case CarType::Normal:
        return CENTER_NORMAL;
    default:
        return CENTER_DEFAULT;
    }
}

void ObjectCarTGE::enterStateStub() {}

void ObjectCarTGE::calcStateStub() {}

/// @addr{0x806D7D70}
/// @brief The state when cars are speeding up.
/// @details On Moonview Highway, the speed cap m_highwayVel is 70, which means this state is only
/// executed for 1 frame when cars get on the highway.
void ObjectCarTGE::calcState1() {
    m_currSpeed += TOLL_BOOTH_ACCEL;

    if (m_currSpeed > m_highwayVel) {
        m_currSpeed = m_highwayVel;
        m_nextStateId = 0;
    }

    m_railInterpolator->setCurrVel(m_currSpeed);
    m_scaledTangentDir = m_railInterpolator->curTangentDir() * m_currSpeed;
}

/// @addr{0x806D7E0C}
/// @brief The state when cars are slowing down.
/// @details On Moonview Highway, the speed floor m_localVel is 40, which means this state is only
/// executed for 1 frame when cars get off the highway.
void ObjectCarTGE::calcState2() {
    m_currSpeed -= TOLL_BOOTH_ACCEL;

    if (m_currSpeed < m_localVel) {
        m_currSpeed = m_localVel;
        m_nextStateId = 0;
    }

    m_railInterpolator->setCurrVel(m_currSpeed);
    m_scaledTangentDir = m_railInterpolator->curTangentDir() * m_currSpeed;
}

/// @addr{0x806D9000}
void ObjectCarTGE::calcPos() {
    constexpr f32 NORMAL_SPEED = 1500.0f;
    constexpr f32 TRUCK_SPEED = 1600.0f;

    f32 speed = (m_carType == CarType::Truck) ? TRUCK_SPEED : NORMAL_SPEED;
    f32 t = speed * scale().z * 0.5f;

    EGG::Vector3f curDir;
    EGG::Vector3f curTangentDir;
    m_railInterpolator->evalCubicBezierOnPath(t, curDir, curTangentDir);

    const EGG::Vector3f curPos = m_railInterpolator->curPos();
    EGG::Vector3f posDelta = curPos - curDir;
    posDelta.normalise2();
    m_tangent += 0.1f * (posDelta - m_tangent);
    m_tangent.y = posDelta.y;
    m_tangent.normalise2();

    if (m_tangent.y > -0.05f) {
        setPos(curPos - m_tangent * t * 0.5f);
    } else {
        setPos(curPos - m_tangent * t * 0.5f - EGG::Vector3f::ey * 5.0f);
    }

    m_up = OrthonormalBasis(m_tangent).base(1);
    setMatrixTangentTo(m_up, m_tangent);
}

} // namespace Kinoko::Field
