#include "KartMove.hh"

#include "game/kart/KartCollide.hh"
#include "game/kart/KartDynamics.hh"
#include "game/kart/KartJump.hh"
#include "game/kart/KartKiller.hh"
#include "game/kart/KartObjectManager.hh"
#include "game/kart/KartParam.hh"
#include "game/kart/KartPhysics.hh"
#include "game/kart/KartScale.hh"
#include "game/kart/KartSub.hh"
#include "game/kart/KartSuspension.hh"

#include "game/field/CollisionDirector.hh"
#include "game/field/KCollisionTypes.hh"
#include "game/field/ObjectDirector.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/KartItem.hh"

#include "game/system/CourseMap.hh"
#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"
#include "game/system/map/MapdataCannonPoint.hh"
#include "game/system/map/MapdataJugemPoint.hh"

#include <egg/math/Math.hh>
#include <egg/math/Quat.hh>

namespace Kinoko::Kart {

struct CannonParameter {
    f32 speed;
    f32 height;
    f32 decelFactor;
    f32 endDecel;
};

static constexpr std::array<CannonParameter, 3> CANNON_PARAMETERS = {{
        {500.0f, 0.0f, 6000.0f, -1.0f},
        {500.0f, 5000.0f, 6000.0f, -1.0f},
        {120.0f, 2000.0f, 1000.0f, 45.0f},
}};

/// @addr{0x80577FC4}
KartMove::KartMove() : m_smoothedUp(EGG::Vector3f::ey), m_scale(1.0f, 1.0f, 1.0f) {
    static constexpr std::array<f32, 4> ENGINE_SPEED_FACTORS = {{0.8f, 0.9f, 1.0f, 0.7f}};
    m_engineSpeedFactor = ENGINE_SPEED_FACTORS[static_cast<size_t>(
            System::RaceConfig::Instance()->raceScenario().engineClass)];

    m_receivedSoftSpeedLimit = 0.0f;
    m_powDodgeTimer = 0;
    m_powHit = false;
    m_bulletScale = 1.0f;
    m_bulletTopRate = 0.0f;
    m_bulletTop = EGG::Vector3f::ey;
    m_totalScale = 1.0f;
    m_hitboxScale = 1.0f;
    m_shockSpeedMultiplier = 1.0f;
    m_invScale = 1.0f;
    m_padType.makeAllZero();
    m_flags.makeAllZero();
    m_jump = nullptr;
}

/// @addr{0x80587B78}
KartMove::~KartMove() {
    EGG::egg_delete(m_jump);
    EGG::egg_delete(m_halfPipe);
    EGG::egg_delete(m_kartScale);
}

/// @addr{0x8057821C}
void KartMove::createSubsystems(const KartParam::Stats &stats) {
    m_jump = EGG::egg_new<KartJump>(this);
    m_halfPipe = EGG::egg_new<KartHalfPipe>();
    m_kartScale = EGG::egg_new<KartScale>(stats);
}

/// @stage All
/// @brief Each frame, looks at player input and kart stats. Saves turn-related info.
/// @addr{0x8057A8B4}
void KartMove::calcTurn() {
    m_realTurn = 0.0f;
    m_rawTurn = 0.0f;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::InAction, eStatus::CannonStart, eStatus::InCannon,
                eStatus::OverZipper)) {
        return;
    }

    if (status.onBit(eStatus::BeforeRespawn)) {
        return;
    }

    if (status.offBit(eStatus::Hop) || m_hopStickX == 0) {
        m_rawTurn = -state()->stickX();
        if (status.onBit(eStatus::JumpPadMushroomCollision)) {
            m_rawTurn *= 0.35f;
        } else if (status.onBit(eStatus::AirtimeOver20)) {
            m_rawTurn *= 0.01f;
        }
    } else {
        m_rawTurn = static_cast<f32>(m_hopStickX);
    }

    f32 reactivity;
    if (state()->isDrifting()) {
        reactivity = param()->stats().driftReactivity;
    } else {
        reactivity = param()->stats().handlingReactivity;
    }

    m_weightedTurn = m_rawTurn * reactivity + m_weightedTurn * (1.0f - reactivity);
    m_weightedTurn = std::max(-1.0f, std::min(1.0f, m_weightedTurn));

    m_realTurn = m_weightedTurn;

    if (!state()->isDrifting()) {
        return;
    }

    m_realTurn = (m_weightedTurn + static_cast<f32>(m_hopStickX)) * 0.5f;
    m_realTurn = m_realTurn * 0.8f + 0.2f * static_cast<f32>(m_hopStickX);
    m_realTurn = std::max(-1.0f, std::min(1.0f, m_realTurn));
}

/// @addr{0x8057829C}
void KartMove::setTurnParams() {
    static constexpr std::array<DriftingParameters, 3> DRIFTING_PARAMS_ARRAY = {{
            {10.0f, 0.5f, 0.5f, 1.0f},
            {10.0f, 0.5f, 0.5f, 0.2f},
            {10.0f, 0.22f, 0.5f, 0.2f},
    }};

    init(false, false);
    m_dir = bodyFront();
    m_lastDir = m_dir;
    m_vel1Dir = m_dir;
    m_landingDir = m_dir;
    m_smoothedForward = m_dir;
    m_outsideDriftLastDir = m_dir;
    m_driftingParams = &DRIFTING_PARAMS_ARRAY[static_cast<u32>(param()->stats().driftType)];
    m_kartScale->reset();
}

/// @addr{0x805784D4}
void KartMove::init(bool b1, bool b2) {
    m_lastSpeed = 0.0f;
    m_baseSpeed = m_engineSpeedFactor * param()->stats().speed;
    m_jumpPadSoftSpeedLimit = m_softSpeedLimit = m_baseSpeed;
    m_speed = 0.0f;
    setKartSpeedLimit();
    m_acceleration = 0.0f;
    m_speedDragMultiplier = 1.0f;
    m_up = EGG::Vector3f::ey;
    m_smoothedUp = EGG::Vector3f::ey;
    m_smoothedForward = EGG::Vector3f::ez;
    m_vel1Dir = EGG::Vector3f::ez;
    m_lastDir = EGG::Vector3f::ez;
    m_dir = EGG::Vector3f::ez;
    m_landingDir = EGG::Vector3f::ez;
    m_dirDiff = EGG::Vector3f::zero;
    m_hasLandingDir = false;
    m_outsideDriftAngle = 0.0f;
    m_landingAngle = 0.0f;
    m_outsideDriftLastDir = EGG::Vector3f::ez;
    m_speedRatio = 0.0f;
    m_speedRatioCapped = 0.0f;
    m_kclSpeedFactor = 1.0f;
    m_kclRotFactor = 1.0f;
    m_kclWheelSpeedFactor = 1.0f;
    m_kclWheelRotFactor = 1.0f;

    if (!b2) {
        m_floorCollisionCount = 0;
    }

    m_hopStickX = 0;
    m_hopFrame = 0;
    m_hopUp = EGG::Vector3f::ey;
    m_hopDir = EGG::Vector3f::ez;
    m_divingRot = 0.0f;
    m_standStillBoostRot = 0.0f;
    m_driftState = DriftState::NotDrifting;
    m_smtCharge = 0;
    m_mtCharge = 0;
    m_outsideDriftBonus = 0.0f;
    m_boost.reset();
    m_zipperBoostTimer = 0;
    m_zipperBoostMax = 0;
    m_reject.reset();
    m_offroadInvincibility = 0;
    m_ssmtCharge = 0;
    m_ssmtLeewayTimer = 0;
    m_ssmtDisableAccelTimer = 0;
    m_nonZipperAirtime = 0;
    m_realTurn = 0.0f;
    m_weightedTurn = 0.0f;

    if (!b1) {
        m_scale.set(1.0f);
        m_totalScale = 1.0f;
        m_hitboxScale = 1.0f;
        m_shockSpeedMultiplier = 1.0f;
        m_invScale = 1.0f;
        m_mushroomBoostTimer = 0;
        m_shockTimer = 0;
        m_crushTimer = 0;
        m_megaScale = 1.0f;
        m_starTimer = 0;
        m_inkTimer = 0;
        m_inkPending = false;
        m_powDodgeTimer = 0;
        m_powHit = false;
        m_megaTimer = 0;
        m_megaShrinking = false;
    }

    m_jumpPadMinSpeed = 0.0f;
    m_jumpPadMaxSpeed = 0.0f;
    m_jumpPadBoostMultiplier = 0.0f;
    m_jumpPadProperties = nullptr;
    m_rampBoost = 0;
    m_autoDriftAngle = 0.0f;
    m_autoDriftStartFrameCounter = 0;

    m_cannonEntryOfsLength = 0.0f;
    m_cannonEntryPos.setZero();
    m_cannonEntryOfs.setZero();
    m_cannonOrthog.setZero();
    m_cannonProgress.setZero();

    m_hopVelY = 0.0f;
    m_hopPosY = 0.0f;
    m_hopGravity = 0.0f;
    m_timeInRespawn = 0;
    m_respawnPreLandTimer = 0;
    m_respawnPostLandTimer = 0;
    m_respawnTimer = 0;
    m_slipstreamTimer = 0;
    m_slipstreamFactor = 1.0f;
    m_wheelieBumpWobbleTimer = 0;
    m_wheelieBumpWobblePhase = 0;
    m_wheelieBumpWobbleAngle = 0.0f;
    m_bumpTimer = 0;
    m_drivingDirection = DrivingDirection::Forwards;
    m_padType.makeAllZero();
    m_flags.makeAllZero();
    m_jump->reset();
    m_halfPipe->reset();
    m_rawTurn = 0.0f;
}

/// @addr{0x8058348C}
void KartMove::clear() {
    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::OverZipper)) {
        status.setBit(eStatus::ActionMidZipper);
    }

    clearBoost();
    clearJumpPad();
    clearRampBoost();
    clearZipperBoost();
    clearSsmt();
    clearSlipstream();
    clearOffroadInvincibility();
    m_halfPipe->end(false);
    m_jump->end();
    clearRejectRoad();
}

/// @brief Initializes the kart's position and rotation. Calls tire suspension initializers.
/// @addr{0x80584044}
void KartMove::setInitialPhysicsValues(const EGG::Vector3f &position, const EGG::Vector3f &angles) {
    EGG::Quatf quaternion = EGG::Quatf::FromRPY(angles * DEG2RAD);
    EGG::Vector3f newPos = position;
    Field::CollisionInfo info;
    Field::KCLTypeMask kcl_flags = KCL_NONE;

    bool bColliding = Field::CollisionDirector::Instance()->checkSphereFullPush(100.0f, newPos,
            EGG::Vector3f::inf, KCL_ANY, &info, &kcl_flags, 0);

    if (bColliding && (kcl_flags & KCL_TYPE_FLOOR)) {
        newPos = newPos + info.tangentOff + (info.floorNrm * -100.0f);
        newPos += info.floorNrm * bsp().initialYPos;
    }

    setPos(newPos);
    setRot(quaternion);

    sub()->initPhysicsValues();

    physics()->setPos(pos());
    physics()->setVelocity(dynamics()->velocity());

    m_landingDir = bodyFront();
    m_dir = bodyFront();
    m_smoothedForward = bodyFront();
    m_up = bodyUp();
    dynamics()->setTop(m_up);

    for (u16 tireIdx = 0; tireIdx < suspCount(); ++tireIdx) {
        suspension(tireIdx)->setInitialState();
    }
}

/// @stage All
/// @brief Each frame, calculates the kart's movement.
/// @addr{0x805788DC}
/// @details Calls various functions to handle drifts, hops, boosts.
/// Afterwards, calculates the kart's speed and rotation.
void KartMove::calc() {
    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::InRespawn)) {
        calcInRespawn();
        return;
    }

    dynamics()->resetInternalVelocity();

    if (status.onBit(eStatus::InBullet)) {
        // The bullet bill drives: a shorter list, and no rotation of our own.
        calcTop();
        tryEndJumpPad();
        calcSpecialFloor();
        calcDirs();
        killer()->calc();
        calcStickyRoad();
        calcOffroad();

        if (status.offBit(eStatus::AutoDrift)) {
            calcManualDrift();
        }

        calcScale();

        if (status.onBit(eStatus::InCannon)) {
            calcCannon();
        }

        calcInk();
        calcAcceleration();
        return;
    }

    m_burnout.calc();
    calcSsmtStart();
    m_halfPipe->calc();
    calcTop();
    tryEndJumpPad();
    calcRespawnBoost();
    calcSpecialFloor();
    m_jump->calc();
    calcWheelieBumpWobble();

    m_bumpTimer = std::max(m_bumpTimer - 1, 0);

    calcAutoDrift();
    calcDirs();
    calcStickyRoad();
    calcOffroad();
    calcTurn();

    if (status.offBit(eStatus::AutoDrift)) {
        calcManualDrift();
    }

    calcWheelie();
    calcSsmt();
    calcBoost();
    calcMushroomBoost();
    calcZipperBoost();
    calcPow();
    calcMega();
    calcStar();
    calcShock();
    calcCrushed();
    calcScale();

    if (status.onBit(eStatus::InCannon)) {
        calcCannon();
    } else {
        calcSlipstream();
    }

    calcOffroadInvincibility();
    calcVehicleSpeed();
    calcInk();
    calcAcceleration();
    calcRotation();
}

/// @addr{0x80584334}
void KartMove::calcRespawnStart() {
    constexpr float RESPAWN_HEIGHT = 700.0f;

    const auto *jugemPoint = System::RaceManager::Instance()->jugemPoint(param()->playerIdx());
    const EGG::Vector3f &jugemRot = jugemPoint->rot();

    // Each player gets its own spot beside the point outside Time Trials (0x8051896C).
    EGG::Vector3f respawnPos = jugemPoint->respawnPos(param()->playerIdx());
    respawnPos.y += RESPAWN_HEIGHT;
    EGG::Vector3f respawnRot = EGG::Vector3f(0.0f, jugemRot.y, 0.0f);

    setInitialPhysicsValues(respawnPos, respawnRot);

    auto &kartItem = Item::ItemDirector::Instance()->kartItem(param()->playerIdx());
    kartItem.clear();

    status().resetBit(eStatus::TriggerRespawn).setBit(eStatus::InRespawn);

    // The kart's item route point jumps to the one ahead of the respawn point (0x807B99D4).
    u8 itemPointIdx = jugemPoint->itemPointIdx();
    if (System::CourseMap::Instance()->getItemPoint(itemPointIdx)) {
        kartItem.setRespawnItemPoint(itemPointIdx);
    }
}

/// @addr{0x80579A50}
void KartMove::calcInRespawn() {
    constexpr f32 LAKITU_VELOCITY = 1.5f;
    constexpr u16 RESPAWN_DURATION = 110;

    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::InRespawn)) {
        return;
    }

    EGG::Vector3f newPos = pos();
    newPos.y -= LAKITU_VELOCITY;
    dynamics()->setPos(newPos);
    dynamics()->setNoGravity(true);

    if (++m_timeInRespawn > RESPAWN_DURATION) {
        status.resetBit(eStatus::InRespawn).setBit(eStatus::AfterRespawn, eStatus::RespawnKillY);
        m_timeInRespawn = 0;
        m_flags.setBit(eFlags::Respawned);
        dynamics()->setNoGravity(false);
    }
}

/// @addr{0x80581C90}
void KartMove::calcRespawnBoost() {
    constexpr s16 RESPAWN_BOOST_DURATION = 30;
    constexpr s16 RESPAWN_BOOST_INPUT_LENIENCY = 4;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::AfterRespawn)) {
        if (status.onBit(eStatus::TouchingGround)) {
            if (m_respawnPreLandTimer > 0) {
                if (status.offBit(eStatus::BeforeRespawn, eStatus::InAction)) {
                    activateBoost(KartBoost::Type::AllMt, RESPAWN_BOOST_DURATION);
                    m_respawnTimer = RESPAWN_BOOST_DURATION;
                }
            } else {
                m_respawnPostLandTimer = RESPAWN_BOOST_INPUT_LENIENCY;
            }

            status.resetBit(eStatus::AfterRespawn);
            m_flags.resetBit(eFlags::Respawned);
        }

        m_respawnPreLandTimer = std::max(0, m_respawnPreLandTimer - 1);

        if (m_flags.onBit(eFlags::Respawned) && status.onBit(eStatus::AccelerateStart)) {
            m_respawnPreLandTimer = RESPAWN_BOOST_INPUT_LENIENCY;
            m_flags.resetBit(eFlags::Respawned);
        }
    } else {
        if (m_respawnPostLandTimer > 0) {
            if (status.onBit(eStatus::AccelerateStart)) {
                if (status.offBit(eStatus::BeforeRespawn, eStatus::InAction)) {
                    activateBoost(KartBoost::Type::AllMt, RESPAWN_BOOST_DURATION);
                    m_respawnTimer = RESPAWN_BOOST_DURATION;
                }

                m_respawnPostLandTimer = 0;
            }

            m_respawnPostLandTimer = std::max(0, m_respawnPostLandTimer - 1);
        } else {
            status.resetBit(eStatus::RespawnKillY);
        }
    }

    m_respawnTimer = std::max(0, m_respawnTimer - 1);
}

/// @addr{0x8057D398}
void KartMove::calcTop() {
    f32 stabilizationFactor = 0.1f;
    m_hasLandingDir = false;
    EGG::Vector3f inputTop = state()->top();
    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::InBullet) && killer()->calcTop()) {
        // 0x8059D0C0: the bullet's own up axis.
        m_smoothedUp += (m_bulletTop - m_smoothedUp) * m_bulletTopRate;
        m_smoothedUp.normalise();
        stabilizationFactor = m_bulletTopRate;
    } else if (status.onBit(eStatus::GroundStart) && m_nonZipperAirtime >= 3) {
        m_smoothedUp = inputTop;
        m_up = inputTop;
        m_landingDir = m_dir.perpInPlane(m_smoothedUp, true);
        m_dirDiff = m_landingDir.proj(m_landingDir);
        m_hasLandingDir = true;
    } else {
        if (status.onBit(eStatus::Hop) && m_hopPosY > 0.0f) {
            stabilizationFactor = m_driftingParams->stabilizationFactor;
        } else if (status.onBit(eStatus::TouchingGround)) {
            if ((m_flags.onBit(eFlags::TrickableSurface) || state()->trickableTimer() > 0) &&
                    inputTop.dot(m_dir) > 0.0f && m_speed > 50.0f &&
                    collide()->surfaceFlags().onBit(KartCollide::eSurfaceFlags::NotTrickable)) {
                inputTop = m_up;
            } else {
                m_up = inputTop;
            }

            f32 scalar = 0.8f;

            if (status.onBit(eStatus::HalfPipeRamp) ||
                    (status.offBit(eStatus::Boost, eStatus::RampBoost, eStatus::Wheelie,
                             eStatus::OverZipper) &&
                            (status.offBit(eStatus::ZipperBoost) || m_zipperBoostTimer > 15))) {
                f32 topDotZ = 0.8f - 6.0f * (EGG::Mathf::abs(inputTop.dot(componentZAxis())));
                scalar = std::min(0.8f, std::max(0.3f, topDotZ));
            }

            m_smoothedUp += (inputTop - m_smoothedUp) * scalar;
            m_smoothedUp.normalise();

            f32 bodyDotFront = bodyFront().dot(m_smoothedUp);

            if (bodyDotFront < -0.1f) {
                stabilizationFactor += std::min(0.2f, EGG::Mathf::abs(bodyDotFront) * 0.5f);
            }

            if (collide()->surfaceFlags().onBit(KartCollide::eSurfaceFlags::BoostRamp)) {
                stabilizationFactor = 0.4f;
            }
        } else {
            calcAirtimeTop();
        }
    }

    dynamics()->setStabilizationFactor(stabilizationFactor);

    m_nonZipperAirtime = status.onBit(eStatus::OverZipper) ? 0 : state()->airtime();
    m_flags.changeBit(collide()->surfaceFlags().onBit(KartCollide::eSurfaceFlags::Trickable),
            eFlags::TrickableSurface);
}

/// @addr{0x8057D888}
/// @brief Calculates rotation of the bike due to excessive airtime.
void KartMove::calcAirtimeTop() {
    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::OverZipper) || status.offBit(eStatus::AirtimeOver20)) {
        return;
    }

    if (m_smoothedUp.y <= 0.99f) {
        m_smoothedUp += (EGG::Vector3f::ey - m_smoothedUp) * 0.03f;
        m_smoothedUp.normalise();
    } else {
        m_smoothedUp = EGG::Vector3f::ey;
    }

    if (m_up.y <= 0.99f) {
        m_up += (EGG::Vector3f::ey - m_up) * 0.03f;
        m_up.normalise();
    } else {
        m_up = EGG::Vector3f::ey;
    }
}

/// @stage 2
/// @brief Every frame, calculates any boost resulting from a boost panel.
/// @addr{0x80587590}
void KartMove::calcSpecialFloor() {
    const auto *raceMgr = System::RaceManager::Instance();
    if (!raceMgr->isStageReached(System::RaceManager::Stage::Race)) {
        return;
    }

    if (m_padType.onBit(ePadType::BoostPanel)) {
        tryStartBoostPanel();
    }

    if (m_padType.onBit(ePadType::BoostRamp)) {
        tryStartBoostRamp();
    }

    if (m_padType.onBit(ePadType::JumpPad)) {
        tryStartJumpPad();
    }

    m_padType.makeAllZero();
}

/// @addr{0x8057A140}
void KartMove::calcDirs() {
    EGG::Vector3f right = dynamics()->mainRot().rotateVector(EGG::Vector3f::ex);
    EGG::Vector3f local_88 = right.cross(m_smoothedUp);
    local_88.normalise();
    m_flags.setBit(eFlags::LaunchBoost);
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::InATrick, eStatus::OverZipper) &&
            (((status.onBit(eStatus::TouchingGround) || status.offBit(eStatus::RampBoost) ||
                      !m_jump->isBoostRampEnabled()) &&
                     status.offBit(eStatus::JumpPad) && state()->airtime() <= 5) ||
                    status.onBit(eStatus::JumpPadMushroomCollision,
                            eStatus::NoSparkInvisibleWall))) {
        EGG::Vector3f local_94 = local_88;
        if (status.onBit(eStatus::Hop)) {
            local_94 = m_hopDir;
        }

        EGG::Matrix34f mat;
        mat.setAxisRotation(DEG2RAD * (m_autoDriftAngle + m_outsideDriftAngle + m_landingAngle),
                m_smoothedUp);
        EGG::Vector3f local_b8 = mat.multVector(local_94);
        local_b8 = local_b8.perpInPlane(m_smoothedUp, true);

        EGG::Vector3f dirDiff = local_b8 - m_dir;

        if (dirDiff.squaredLength() <= std::numeric_limits<f32>::epsilon()) {
            m_dir = local_b8;
            m_dirDiff.setZero();
        } else {
            EGG::Vector3f origDirCross = m_dir.cross(local_b8);
            m_dirDiff += m_kclRotFactor * dirDiff;
            m_dir += m_dirDiff;
            m_dir.normalise();
            m_dirDiff *= 0.1f;
            EGG::Vector3f newDirCross = m_dir.cross(local_b8);

            if (origDirCross.dot(newDirCross) < 0.0f) {
                m_dir = local_b8;
                m_dirDiff.setZero();
            }
        }

        m_vel1Dir = m_dir.perpInPlane(m_smoothedUp, true);
        m_flags.resetBit(eFlags::LaunchBoost);
    } else {
        m_vel1Dir = m_dir;
    }

    if (status.offBit(eStatus::OverZipper)) {
        m_jump->tryStart(m_smoothedUp.cross(m_dir));
    }

    EGG::Vector3f nextDir = m_up.cross(local_88);
    m_smoothedForward = nextDir.cross(m_up);
    m_smoothedForward.normalise();

    if (m_hasLandingDir) {
        f32 dot = m_dir.dot(m_landingDir);
        EGG::Vector3f cross = m_dir.cross(m_landingDir);
        f32 crossDot = cross.length();
        f32 angle = EGG::Mathf::atan2(crossDot, dot);
        angle = EGG::Mathf::abs(angle);

        f32 fVar4 = 1.0f;
        if (cross.dot(m_smoothedUp) < 0.0f) {
            fVar4 = -1.0f;
        }

        m_landingAngle += (angle * RAD2DEG) * fVar4;
    }

    if (m_landingAngle <= 0.0f) {
        if (m_landingAngle < 0.0f) {
            m_landingAngle = std::min(0.0f, m_landingAngle + 2.0f);
        }
    } else {
        m_landingAngle = std::max(0.0f, m_landingAngle - 2.0f);
    }
}

/// @addr{0x80583B88}
void KartMove::calcStickyRoad() {
    constexpr f32 STICKY_RADIUS = 200.0f;
    constexpr Field::KCLTypeMask STICKY_MASK =
            KCL_TYPE_BIT(COL_TYPE_STICKY_ROAD) | KCL_TYPE_BIT(COL_TYPE_MOVING_WATER);

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::OverZipper)) {
        status.resetBit(eStatus::StickyRoad);
        return;
    }

    if ((status.offBit(eStatus::StickyRoad) &&
                collide()->surfaceFlags().offBit(KartCollide::eSurfaceFlags::Trickable)) ||
            EGG::Mathf::abs(m_speed) <= 20.0f) {
        return;
    }

    EGG::Vector3f pos = dynamics()->pos();
    EGG::Vector3f vel = dynamics()->movingObjVel() + m_speed * m_vel1Dir;
    Field::CollisionInfo colInfo;
    colInfo.bbox.setZero();
    Field::KCLTypeMask kcl_flags = KCL_NONE;
    bool stickyRoad = false;

    for (size_t i = 0; i < 3; ++i) {
        EGG::Vector3f newPos = pos + vel;
        if (Field::CollisionDirector::Instance()->checkSphereFull(STICKY_RADIUS, newPos,
                    EGG::Vector3f::inf, STICKY_MASK, &colInfo, &kcl_flags, 0)) {
            m_vel1Dir = m_vel1Dir.perpInPlane(colInfo.floorNrm, true);
            dynamics()->setMovingObjVel(dynamics()->movingObjVel().rej(colInfo.floorNrm));
            dynamics()->setMovingRoadVel(dynamics()->movingRoadVel().rej(colInfo.floorNrm));

            if (status.onBit(eStatus::MovingWaterStickyRoad)) {
                m_up = colInfo.floorNrm;
                m_smoothedUp = colInfo.floorNrm;
            }

            stickyRoad = true;

            break;
        }
        vel *= 0.5f;
        pos += -STICKY_RADIUS * componentYAxis();
    }

    if (!stickyRoad) {
        status.resetBit(eStatus::StickyRoad);
    }
}

/// @stage 2
/// @brief Each frame, computes rotation and speed scalars from the floor KCL.
/// @addr{0x8057C3D4}
void KartMove::calcOffroad() {
    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BoostOffroadInvincibility)) {
        m_kclSpeedFactor = 1.0f;
        m_kclRotFactor = param()->stats().kclRot[0];
    } else {
        bool anyWheel = status.onBit(eStatus::AnyWheelCollision);
        if (anyWheel) {
            m_kclSpeedFactor = m_kclWheelSpeedFactor;
            m_floorCollisionCount = m_floorCollisionCount != 0 ? m_floorCollisionCount : 1;
            m_kclRotFactor = m_kclWheelRotFactor / static_cast<f32>(m_floorCollisionCount);
        }

        if (status.onBit(eStatus::VehicleBodyFloorCollision)) {
            const CollisionData &colData = collisionData();
            if (anyWheel) {
                if (colData.speedFactor < m_kclWheelSpeedFactor) {
                    m_kclSpeedFactor = colData.speedFactor;
                }
                m_kclRotFactor = (m_kclWheelRotFactor + colData.rotFactor) /
                        static_cast<f32>(m_floorCollisionCount + 1);
            } else {
                m_kclSpeedFactor = colData.speedFactor;
                m_kclRotFactor = colData.rotFactor;
            }
        }
    }

    calcRisingWater();
}

/// @addr{0x8058677C}
void KartMove::calcRisingWater() {
    auto *objDir = Field::ObjectDirector::Instance();
    auto *psea = objDir->psea();
    if (!psea) {
        return;
    }

    f32 pos = wheelPos(0).y;
    u16 count = tireCount();
    for (u16 wheelIdx = 0; wheelIdx < count; ++wheelIdx) {
        f32 tmp = wheelEdgePos(wheelIdx).y;
        if (wheelIdx == 0 || tmp < pos) {
            pos = tmp;
        }
    }

    if (objDir->risingWaterKillPlaneHeight() > pos) {
        collide()->activateOob(true, nullptr, false, false);
    }

    f32 dist = -objDir->distAboveRisingWater(pos);
    if (dist > 0.0f && status().offBit(Kart::eStatus::BoostOffroadInvincibility)) {
        f32 speedScale = std::min(1.0f, dist / 100.0f);
        m_kclSpeedFactor = 1.0f - (1.0f - param()->stats().kclSpeed[3]) * speedScale;
    }
}

/// @addr{0x80582694}
void KartMove::calcBoost() {
    auto &status = KartObjectProxy::status();

    if (m_boost.calc()) {
        status.setBit(eStatus::Accelerate);
    } else {
        status.resetBit(eStatus::Boost);
    }

    calcRampBoost();
}

/// @addr{0x80582804}
void KartMove::calcRampBoost() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::RampBoost)) {
        return;
    }

    status.setBit(eStatus::Accelerate);
    if (--m_rampBoost < 1) {
        m_rampBoost = 0;
        status.resetBit(eStatus::RampBoost);
    }
}

/// @addr{Inlined in 0x805828CC}
/// @stage 2
/// @brief Computes the current cooldown duration between braking and reversing.
void KartMove::calcDisableBackwardsAccel() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::DisableBackwardsAccel)) {
        return;
    }

    if (--m_ssmtDisableAccelTimer < 0 ||
            (m_flags.offBit(eFlags::SsmtLeeway) && status.offBit(eStatus::Brake))) {
        status.resetBit(eStatus::DisableBackwardsAccel);
        m_ssmtDisableAccelTimer = 0;
    }
}

/// @addr{0x805828CC}
/// @stage 2
/// @brief Calculates standstill mini-turbo components, if applicable.
void KartMove::calcSsmt() {
    constexpr s16 MAX_SSMT_CHARGE = 75;
    constexpr s16 SSMT_BOOST_FRAMES = 30;
    constexpr s16 LEEWAY_FRAMES = 1;
    constexpr s16 DISABLE_ACCEL_FRAMES = 20;

    calcDisableBackwardsAccel();

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::ChargingSSMT)) {
        if (++m_ssmtCharge > MAX_SSMT_CHARGE) {
            m_ssmtCharge = MAX_SSMT_CHARGE;
            m_flags.setBit(eFlags::SsmtCharged);
            m_ssmtLeewayTimer = 0;
        }

        return;
    }

    m_ssmtCharge = 0;

    if (m_flags.offBit(eFlags::SsmtCharged)) {
        return;
    }

    if (m_flags.onBit(eFlags::SsmtLeeway)) {
        if (--m_ssmtLeewayTimer < 0) {
            m_ssmtLeewayTimer = 0;
            m_flags.resetBit(eFlags::SsmtCharged, eFlags::SsmtLeeway);
            m_ssmtDisableAccelTimer = DISABLE_ACCEL_FRAMES;
            status.setBit(eStatus::DisableBackwardsAccel);
        } else {
            if (status.offBit(eStatus::Accelerate, eStatus::Brake)) {
                activateBoost(KartBoost::Type::AllMt, SSMT_BOOST_FRAMES);
                m_ssmtLeewayTimer = 0;
                m_flags.resetBit(eFlags::SsmtCharged, eFlags::SsmtLeeway);
            }
        }
    } else {
        if (status.onBit(eStatus::Accelerate) && status.offBit(eStatus::Brake)) {
            activateBoost(KartBoost::Type::AllMt, SSMT_BOOST_FRAMES);
            m_ssmtLeewayTimer = 0;
            m_flags.resetBit(eFlags::SsmtCharged, eFlags::SsmtLeeway);
        } else {
            m_ssmtLeewayTimer = LEEWAY_FRAMES;
            m_flags.setBit(eFlags::SsmtLeeway);
            status.setBit(eStatus::DisableBackwardsAccel);
            m_ssmtDisableAccelTimer = LEEWAY_FRAMES;
        }
    }
}

/// @stage 2
/// @addr{0x8057E804}
/// @brief Each frame, checks for hop or slipdrift. Computes drift direction based on player input.
/// @return Whether or not we are hopping or slipdrifting.
bool KartMove::calcPreDrift() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::TouchingGround, eStatus::Hop, eStatus::DriftManual)) {
        if (status.onBit(eStatus::StickLeft, eStatus::StickRight)) {
            if (status.offBit(eStatus::DriftInput)) {
                status.resetBit(eStatus::SlipdriftCharge);
            } else if (status.offBit(eStatus::SlipdriftCharge)) {
                if (m_hopStickX == 0) {
                    if (status.onBit(eStatus::StickRight)) {
                        m_hopStickX = -1;
                    } else if (status.onBit(eStatus::StickLeft)) {
                        m_hopStickX = 1;
                    }
                    status.setBit(eStatus::SlipdriftCharge);
                    onHop();
                }
            }
        }
    }

    if (status.onBit(eStatus::Hop)) {
        if (m_hopStickX == 0) {
            if (status.onBit(eStatus::StickRight)) {
                m_hopStickX = -1;
            } else if (status.onBit(eStatus::StickLeft)) {
                m_hopStickX = 1;
            }
        }
        if (m_hopFrame < 3) {
            ++m_hopFrame;
        }
    } else if (status.onBit(eStatus::SlipdriftCharge)) {
        m_hopFrame = 0;
    }

    return status.onBit(eStatus::Hop, eStatus::SlipdriftCharge);
}

/// @stage All
/// @brief Clears drift state. Called when touching ground and drift is canceled.
/// @addr{0x8057EA50}
void KartMove::resetDriftManual() {
    m_hopStickX = 0;
    m_hopFrame = 0;
    status().resetBit(eStatus::Hop, eStatus::DriftManual);
    m_driftState = DriftState::NotDrifting;
    m_smtCharge = 0;
    m_mtCharge = 0;
}

/// @stage 2
/// @addr{0x8057E348}
void KartMove::clearDrift() {
    m_flags.resetBit(eFlags::DriftReset);
    m_outsideDriftAngle = 0.0f;
    m_hopStickX = 0;
    m_hopFrame = 0;
    m_driftState = DriftState::NotDrifting;
    m_smtCharge = 0;
    m_mtCharge = 0;
    m_outsideDriftBonus = 0.0f;
    status().resetBit(eStatus::Hop, eStatus::SlipdriftCharge, eStatus::DriftManual,
            eStatus::DriftAuto);
    m_autoDriftAngle = 0.0f;
    m_hopStickX = 0;
    m_autoDriftStartFrameCounter = 0;
}

/// @addr{0x80582DB4}
void KartMove::clearJumpPad() {
    m_jumpPadMinSpeed = 0.0f;
    status().resetBit(eStatus::JumpPad);
}

/// @addr{0x80582DD8}
void KartMove::clearRampBoost() {
    m_rampBoost = 0;
    status().resetBit(eStatus::RampBoost);
}

/// @addr{0x80582F38}
void KartMove::clearZipperBoost() {
    m_zipperBoostTimer = 0;
    status().resetBit(eStatus::ZipperBoost);
}

/// @addr{0x80582D94}
void KartMove::clearBoost() {
    m_boost.resetActive();
    status().resetBit(eStatus::Boost);
}

/// @addr{0x80582F58}
void KartMove::clearSsmt() {
    m_ssmtCharge = 0;
    m_ssmtLeewayTimer = 0;
    m_ssmtDisableAccelTimer = 0;
    m_flags.resetBit(eFlags::SsmtCharged, eFlags::SsmtLeeway);
}

/// @addr{0x80582F7C}
void KartMove::clearOffroadInvincibility() {
    m_offroadInvincibility = 0;
    status().resetBit(eStatus::BoostOffroadInvincibility);
}

void KartMove::clearRejectRoad() {
    status().resetBit(eStatus::RejectRoadTrigger, eStatus::NoSparkInvisibleWall);
}

/// @stage 2
/// @brief Each frame, handles automatic transmission drifting.
/// @addr{0x8057E0DC}
void KartMove::calcAutoDrift() {
    constexpr s16 AUTO_DRIFT_START_DELAY = 12;

    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::AutoDrift)) {
        return;
    }

    if (canStartDrift() &&
            status.offBit(eStatus::OverZipper, eStatus::RejectRoadTrigger, eStatus::Wheelie) &&
            EGG::Mathf::abs(state()->stickX()) > 0.85f) {
        m_autoDriftStartFrameCounter =
                std::min<s16>(AUTO_DRIFT_START_DELAY, m_autoDriftStartFrameCounter + 1);
    } else {
        m_autoDriftStartFrameCounter = 0;
    }

    if (m_autoDriftStartFrameCounter >= AUTO_DRIFT_START_DELAY) {
        status.setBit(eStatus::DriftAuto);

        if (status.onBit(eStatus::TouchingGround)) {
            if (state()->stickX() < 0.0f) {
                m_hopStickX = 1;
                m_autoDriftAngle -= 30.0f * param()->stats().driftAutomaticTightness;

            } else {
                m_hopStickX = -1;
                m_autoDriftAngle += 30.0f * param()->stats().driftAutomaticTightness;
            }
        }

        f32 halfTarget = 0.5f * param()->stats().driftOutsideTargetAngle;
        m_autoDriftAngle = std::min(halfTarget, std::max(-halfTarget, m_autoDriftAngle));
    } else {
        status.resetBit(eStatus::DriftAuto);
        m_hopStickX = 0;

        if (m_autoDriftAngle > 0.0f) {
            m_autoDriftAngle =
                    std::max(0.0f, m_autoDriftAngle - param()->stats().driftOutsideDecrement);
        } else {
            m_autoDriftAngle =
                    std::min(0.0f, m_autoDriftAngle + param()->stats().driftOutsideDecrement);
        }
    }

    EGG::Quatf angleAxis;
    angleAxis.setAxisRotation(-m_autoDriftAngle * DEG2RAD, m_up);
    physics()->composeExtraRot(angleAxis);
}

/// @stage 2
/// @brief Each frame, handles hopping, drifting, and mini-turbos.
/// @addr{0x8057DC44}
void KartMove::calcManualDrift() {
    bool isHopping = calcPreDrift();
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::OverZipper)) {
        const EGG::Vector3f rotZ = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);

        if (status.offBit(eStatus::TouchingGround) &&
                param()->stats().driftType != KartParam::Stats::DriftType::Inside_Drift_Bike &&
                status.offBit(eStatus::JumpPadMushroomCollision) &&
                status.onBit(eStatus::DriftManual, eStatus::SlipdriftCharge) &&
                m_flags.onBit(eFlags::LaunchBoost)) {
            const EGG::Vector3f up = dynamics()->mainRot().rotateVector(EGG::Vector3f::ey);
            EGG::Vector3f driftRej = m_outsideDriftLastDir.rej(up);

            if (driftRej.normalise() != 0.0f) {
                f32 rejCrossDirMag = driftRej.cross(rotZ).length();
                f32 angle = EGG::Mathf::atan2(rejCrossDirMag, driftRej.dot(rotZ));
                f32 sign = 1.0f;
                if ((rotZ.z * (rotZ.x - driftRej.x)) - (rotZ.x * (rotZ.z - driftRej.z)) > 0.0f) {
                    sign = -1.0f;
                }

                m_outsideDriftAngle += angle * RAD2DEG * sign;
            }
        }

        m_outsideDriftLastDir = rotZ;
    }

    // TODO: Is this backwards/inverted?
    if (((status.offBit(eStatus::Hop) || m_hopFrame < 3) &&
                status.offBit(eStatus::SlipdriftCharge)) ||
            (status.onBit(eStatus::InAction) || status.offBit(eStatus::TouchingGround))) {
        if (canHop()) {
            hop();
            isHopping = true;
        }
    } else {
        startManualDrift();
        isHopping = false;
    }

    m_flags.resetBit(eFlags::DriftReset);

    if (status.offBit(eStatus::DriftManual)) {
        if (!isHopping && status.onBit(eStatus::TouchingGround)) {
            resetDriftManual();

            if (action()->flags().offBit(KartAction::eFlags::Rotating) || m_speed <= 20.0f) {
                f32 driftAngleDecr = param()->stats().driftOutsideDecrement;
                if (m_outsideDriftAngle > 0.0f) {
                    m_outsideDriftAngle = std::max(0.0f, m_outsideDriftAngle - driftAngleDecr);
                } else if (m_outsideDriftAngle < 0.0f) {
                    m_outsideDriftAngle = std::min(0.0f, m_outsideDriftAngle + driftAngleDecr);
                }
            }
        }
    } else {
        // This is a different comparison than @ref KartMove::canStartDrift().
        bool canStartDrift = m_speed > MINIMUM_DRIFT_THRESOLD * m_baseSpeed;

        if (status.offBit(eStatus::OverZipper) &&
                (status.offAnyBit(eStatus::DriftInput, eStatus::Accelerate) ||
                        status.onBit(eStatus::InAction, eStatus::RejectRoadTrigger,
                                eStatus::Wall3Collision, eStatus::WallCollision) ||
                        !canStartDrift)) {
            if (canStartDrift) {
                releaseMt();
            }

            resetDriftManual();
            m_flags.setBit(eFlags::DriftReset);
        } else {
            controlOutsideDriftAngle();
        }
    }
}

/// @stage 2
/// @brief Called when the player lands from a drift hop, or to start a slipdrift.
/// @addr{0x8057E3F4}
void KartMove::startManualDrift() {
    constexpr f32 OUTSIDE_DRIFT_BONUS = 0.5f;

    const auto &stats = param()->stats();
    auto &status = KartObjectProxy::status();

    if (stats.driftType != KartParam::Stats::DriftType::Inside_Drift_Bike) {
        f32 driftAngle = 0.0f;

        if (status.onBit(eStatus::Hop)) {
            const EGG::Vector3f rotZ = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);
            EGG::Vector3f rotRej = rotZ.rej(m_hopUp);

            if (rotRej.normalise() != 0.0f) {
                const EGG::Vector3f hopCrossRot = m_hopDir.cross(rotRej);
                driftAngle =
                        EGG::Mathf::atan2(hopCrossRot.length(), m_hopDir.dot(rotRej)) * RAD2DEG;
            }
        }

        m_outsideDriftAngle += driftAngle * static_cast<f32>(-m_hopStickX);
        m_outsideDriftAngle = std::max(-60.0f, std::min(60.0f, m_outsideDriftAngle));
    }

    status.resetBit(eStatus::Hop, eStatus::SlipdriftCharge);

    if (status.offBit(eStatus::DriftInput)) {
        return;
    }

    if (getAppliedHopStickX() == 0) {
        return;
    }

    status.setBit(eStatus::DriftManual).resetBit(eStatus::Hop);
    m_driftState = DriftState::ChargingMt;
    m_outsideDriftBonus = OUTSIDE_DRIFT_BONUS * (m_speedRatioCapped * stats.driftManualTightness);
}

/// @stage 2
/// @brief Stops charging a mini-turbo, and applies boost if charged.
/// @addr{0x80582F9C}
void KartMove::releaseMt() {
    constexpr f32 SMT_LENGTH_FACTOR = 3.0f;

    auto &status = KartObjectProxy::status();

    if (m_driftState < DriftState::ChargedMt || status.onBit(eStatus::Brake)) {
        m_driftState = DriftState::NotDrifting;
        return;
    }

    u16 mtLength = param()->stats().miniTurbo;

    if (m_driftState == DriftState::ChargedSmt) {
        mtLength *= SMT_LENGTH_FACTOR;
    }

    if (status.offBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        activateBoost(KartBoost::Type::AllMt, mtLength);
    }

    m_driftState = DriftState::NotDrifting;
}

/// @stage 2
/// @brief Every frame, handles mini-turbo charging and outside drifting bike rotation.
/// @addr{0x8057EAB8}
void KartMove::controlOutsideDriftAngle() {
    if (state()->airtime() > 5) {
        return;
    }

    if (param()->stats().driftType != KartParam::Stats::DriftType::Inside_Drift_Bike) {
        if (m_hopStickX == -1) {
            f32 angle = m_outsideDriftAngle;
            f32 targetAngle = param()->stats().driftOutsideTargetAngle;
            if (angle > targetAngle) {
                m_outsideDriftAngle = std::max(m_outsideDriftAngle - 2.0f, targetAngle);
            } else if (angle < targetAngle) {
                m_outsideDriftAngle += 150.0f * param()->stats().driftManualTightness;
                m_outsideDriftAngle = std::min(m_outsideDriftAngle, targetAngle);
            }
        } else if (m_hopStickX == 1) {
            f32 angle = m_outsideDriftAngle;
            f32 targetAngle = -param()->stats().driftOutsideTargetAngle;
            if (targetAngle > angle) {
                m_outsideDriftAngle = std::min(m_outsideDriftAngle + 2.0f, targetAngle);
            } else if (targetAngle < angle) {
                m_outsideDriftAngle -= 150.0f * param()->stats().driftManualTightness;
                m_outsideDriftAngle = std::max(m_outsideDriftAngle, targetAngle);
            }
        }
    }

    calcMtCharge();
}

/// @stage 1+
/// @brief Every frame, calculates kart rotation based on player input.
/// @addr{0x8057C69C}
void KartMove::calcRotation() {
    f32 turn;
    auto &status = KartObjectProxy::status();
    bool drifting = state()->isDrifting() && status.offBit(eStatus::JumpPadMushroomCollision);
    bool autoDrift = status.onBit(eStatus::AutoDrift);
    const auto &stats = param()->stats();

    if (drifting) {
        turn = autoDrift ? stats.driftAutomaticTightness : stats.driftManualTightness;
    } else {
        turn = autoDrift ? stats.handlingAutomaticTightness : stats.handlingManualTightness;
    }

    if (drifting && stats.driftType != KartParam::Stats::DriftType::Inside_Drift_Bike) {
        m_outsideDriftBonus *= 0.99f;
        turn += m_outsideDriftBonus;
    }

    bool forwards = true;
    if (status.onBit(eStatus::Brake) && m_speed <= 0.0f) {
        forwards = false;
    }

    turn *= m_realTurn;
    if (status.onBit(eStatus::ChargingSSMT)) {
        turn = m_realTurn * 0.04f;
    } else {
        if (status.onBit(eStatus::Hop) && m_hopPosY > 0.0f) {
            turn *= 1.4f;
        }

        if (!drifting) {
            bool noTurn = false;
            if (status.offBit(eStatus::WallCollision, eStatus::Wall3Collision) &&
                    EGG::Mathf::abs(m_speed) < 1.0f) {
                if (!(status.onBit(eStatus::Hop) && m_hopPosY > 0.0f)) {
                    turn = 0.0f;
                    noTurn = true;
                }
            }
            if (forwards && !noTurn) {
                // A mega kart's speed range is 25% wider (0x808B59CC), except the "fast" test
                // against 70, which the game leaves unscaled (Dolphin test11).
                f32 rangeScale = status.onBit(eStatus::InMega) ? 1.25f : 1.0f;
                f32 slowSpeed = 20.0f * rangeScale;
                if (m_speed >= slowSpeed) {
                    turn *= 0.5f;
                    if (m_speed < 70.0f) {
                        turn += (1.0f - (m_speed - slowSpeed) / (70.0f * rangeScale - slowSpeed)) *
                                turn;
                    }
                } else {
                    turn = (turn * 0.4f) + (m_speed / slowSpeed) * (turn * 0.6f);
                }
            }
        }

        if (!forwards) {
            turn = -turn;
        }

        // A star, mega mushroom or thunder cloud turns 10% sharper outside a manual drift
        // (0x808B59C8; Dolphin test10 and test11).
        if (status.onBit(eStatus::InStar, eStatus::InMega, eStatus::ThunderCloud) &&
                status.offBit(eStatus::DriftManual)) {
            turn *= 1.1f;
        }

        if (status.onBit(eStatus::ZipperBoost) && status.offBit(eStatus::DriftManual)) {
            turn *= 2.0f;
        }

        f32 stickX = EGG::Mathf::abs(state()->stickX());
        if (autoDrift && stickX > 0.3f) {
            f32 stickScalar = (stickX - 0.3f) / 0.7f;
            stickX = drifting ? 0.2f : 0.5f;
            turn += stickScalar * (turn * stickX * m_speedRatioCapped);
        }
    }

    if (status.offBit(eStatus::InAction, eStatus::ZipperTrick)) {
        if (status.offBit(eStatus::TouchingGround)) {
            if (status.onBit(eStatus::RampBoost) && m_jump->isBoostRampEnabled()) {
                turn = 0.0f;
            } else if (status.offBit(eStatus::JumpPadMushroomCollision)) {
                u32 airtime = state()->airtime();
                if (airtime >= 70) {
                    turn = 0.0f;
                } else if (airtime >= 30) {
                    turn = std::max(0.0f, turn * (1.0f - (airtime - 30) * 0.025f));
                }
            }
        }

        const EGG::Vector3f forward = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);
        f32 angle = EGG::Mathf::atan2(forward.cross(m_dir).length(), forward.dot(m_dir));
        angle = EGG::Mathf::abs(angle) * RAD2DEG;

        if (angle > 60.0f) {
            turn *= std::max(0.0f, 1.0f - (angle - 60.0f) / 40.0f);
        }
    }

    calcVehicleRotation(turn);
}

/// @stage 2
/// @brief Every frame, computes speed based on acceleration and any active boosts.
/// @addr{0x8057AB68}
void KartMove::calcVehicleSpeed() {
    const auto *raceMgr = System::RaceManager::Instance();
    auto &status = KartObjectProxy::status();

    if (raceMgr->isStageReached(System::RaceManager::Stage::Race)) {
        f32 speedFix = dynamics()->speedFix();
        if (status.onBit(eStatus::InAction) ||
                ((status.onBit(eStatus::WallCollisionStart) || state()->wallBonkTimer() == 0 ||
                         EGG::Mathf::abs(speedFix) >= 3.0f) &&
                        status.offBit(eStatus::DriftManual))) {
            m_speed += speedFix;
        }
    }

    if (m_speed < -20.0f) {
        m_speed += 0.5f;
    }

    bool water = false;

    if (status.onBit(eStatus::MovingWaterVertical) ||
            (status.onBit(eStatus::MovingWaterDecaySpeed) &&
                    status.offBit(eStatus::MushroomBoost) && EGG::Mathf::abs(m_speed) > 5.0f)) {
        water = true;
        m_speed *= collide()->pullPath().roadSpeedDecay();
    }

    m_acceleration = 0.0f;
    m_speedDragMultiplier = 1.0f;

    if (status.onBit(eStatus::InAction)) {
        action()->calcVehicleSpeed();
        return;
    }

    if ((status.onAllBit(eStatus::SoftWallPush, eStatus::TouchingGround) &&
                status.offBit(eStatus::AnyWheelCollision)) ||
            status.offBit(eStatus::TouchingGround) ||
            status.onBit(eStatus::DisableAcceleration, eStatus::ChargingSSMT)) {
        if (status.onBit(eStatus::RampBoost) && state()->airtime() < 4) {
            m_acceleration = 7.0f;
        } else {
            if (status.onBit(eStatus::JumpPad) && status.offBit(eStatus::Accelerate)) {
                m_speedDragMultiplier = 0.99f;
            } else {
                if (status.onBit(eStatus::OverZipper)) {
                    m_speedDragMultiplier = 0.999f;
                } else {
                    if (state()->airtime() > 5) {
                        m_speedDragMultiplier = 0.999f;
                    }
                }
            }
            m_speed *= m_speedDragMultiplier;
        }
    } else if (status.offBit(eStatus::Boost)) {
        if (status.offBit(eStatus::JumpPad, eStatus::RampBoost)) {
            if (status.onBit(eStatus::Accelerate)) {
                if (!water &&
                        (status.onBit(eStatus::InStar) ||
                                status.onBit(eStatus::ThunderCloud, eStatus::InMega))) {
                    m_acceleration = 3.0f;
                } else if (status.onBit(eStatus::SlipstreamBoost) && !water) {
                    m_acceleration = 0.7f;
                } else {
                    m_acceleration =
                            status.onBit(eStatus::HalfPipeRamp) ? 5.0f : calcVehicleAcceleration();
                }
            } else {
                if (status.offBit(eStatus::Brake) ||
                        status.onBit(eStatus::DisableBackwardsAccel, eStatus::SoftWallPush)) {
                    m_speed *= m_speed > 0.0f ? 0.98f : 0.95f;
                } else if (m_drivingDirection == DrivingDirection::Braking) {
                    m_acceleration = -1.5f;
                } else if (m_drivingDirection == DrivingDirection::WaitingForBackwards) {
                    if (++m_backwardsAllowCounter > 15) {
                        m_drivingDirection = DrivingDirection::Backwards;
                    }
                } else if (m_drivingDirection == DrivingDirection::Backwards) {
                    m_acceleration = -2.0f;
                }
            }

            if (status.offBit(eStatus::Boost, eStatus::DriftManual, eStatus::AutoDrift)) {
                const auto &stats = param()->stats();

                f32 x = 1.0f - EGG::Mathf::abs(m_weightedTurn) * m_speedRatioCapped;
                m_speed *= stats.turningSpeed + (1.0f - stats.turningSpeed) * x;
            }
        } else {
            m_acceleration = water ? calcVehicleAcceleration() : 7.0f;
        }
    } else {
        m_acceleration = water ? calcVehicleAcceleration() : m_boost.acceleration();
    }
}

/// @stage 2
/// @addr{0x8057B028}
void KartMove::calcDeceleration() {
    f32 vel = 0.0f;
    f32 initialVel = 1.0f - m_smoothedUp.y;
    if (EGG::Mathf::abs(m_speed) < 30.0f && m_smoothedUp.y > 0.0f && initialVel > 0.0f) {
        initialVel = std::min(initialVel * 2.0f, 2.0f);
        vel += initialVel;
        vel *= std::min(0.5f, std::max(-0.5f, -bodyFront().y));
    }
    m_speed += vel;
}

/// @stage 2
/// @brief Every frame, computes acceleration based off the character/vehicle stats.
/// @addr{0x8057B868}
f32 KartMove::calcVehicleAcceleration() const {
    f32 ratio = m_speed / m_softSpeedLimit;
    if (ratio < 0.0f) {
        return 1.0f;
    }

    std::span<const f32> as;
    std::span<const f32> ts;
    if (state()->isDrifting()) {
        as = param()->stats().accelerationDriftA;
        ts = param()->stats().accelerationDriftT;
    } else {
        as = param()->stats().accelerationStandardA;
        ts = param()->stats().accelerationStandardT;
    }

    size_t i = 0;
    f32 acceleration = 0.0f;
    f32 t_curr = 0.0f;
    for (; i < ts.size(); ++i) {
        if (ratio < ts[i]) {
            acceleration = as[i] + ((as[i + 1] - as[i]) / (ts[i] - t_curr)) * (ratio - t_curr);
            break;
        }

        t_curr = ts[i];
    }

    return i < ts.size() ? acceleration : as.back();
}

/// @stage 2
/// @brief Every frame, applies acceleration to the kart's internal velocity.
/// @addr{0x8057B9BC}
void KartMove::calcAcceleration() {
    constexpr f32 ROTATION_SCALAR_NORMAL = 0.5f;
    constexpr f32 ROTATION_SCALAR_MIDAIR = 0.2f;
    constexpr f32 ROTATION_SCALAR_BOOST_RAMP = 4.0f;
    constexpr f32 OOB_SLOWDOWN_RATE = 0.95f;
    constexpr f32 TERMINAL_VELOCITY = 90.0f;
    constexpr f32 WHEELIE_BUMP_SPEED_FACTOR = 0.4f;

    m_lastSpeed = m_speed;
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::InAction)) {
        dynamics()->setKillExtVelY(status.onBit(eStatus::RespawnKillY));
    }

    f32 speedLimit;
    if (status.offBit(eStatus::InBullet)) {
        if (status.onBit(eStatus::Burnout)) {
            m_speed = 0.0f;
        } else {
            if (m_acceleration < 0.0f) {
                if (m_speed < -20.0f) {
                    m_acceleration = 0.0f;
                } else {
                    if (m_speed + m_acceleration <= -20.0f) {
                        m_acceleration = -20.0f - m_speed;
                    }
                }
            }

            m_speed += m_acceleration;
        }

        if (status.onBit(eStatus::BeforeRespawn)) {
            m_speed *= OOB_SLOWDOWN_RATE;
        } else {
            if (status.onBit(eStatus::ChargingSSMT)) {
                m_speed *= 0.8f;
            } else {
                if (m_drivingDirection == DrivingDirection::Braking && m_speed < 0.0f) {
                    m_speed = 0.0f;
                    m_drivingDirection = DrivingDirection::WaitingForBackwards;
                    m_backwardsAllowCounter = 0;
                }
            }
        }

        speedLimit = status.onBit(eStatus::JumpPad) ? m_jumpPadMaxSpeed : m_baseSpeed;
        const f32 boostMultiplier = m_boost.multiplier();
        f32 boostSpdLimit = m_boost.speedLimit();
        if (status.onBit(eStatus::InMega, eStatus::ThunderCloud)) {
            boostSpdLimit = std::max(boostSpdLimit, 95.0f);
        }
        if (status.onBit(eStatus::InStar)) {
            boostSpdLimit = std::max(boostSpdLimit, 105.0f);
        }
        if (status.onBit(eStatus::SlipstreamBoost)) {
            boostSpdLimit = std::max(boostSpdLimit, 115.0f);
        }
        m_jumpPadBoostMultiplier = boostMultiplier;

        f32 scaleMultiplier = m_shockSpeedMultiplier;
        if (status.onBit(eStatus::Crushed)) {
            scaleMultiplier *= 0.7f;
        }

        f32 wheelieBonus = boostMultiplier + getWheelieSoftSpeedLimitBonus();
        speedLimit *= status.onBit(eStatus::JumpPadFixedSpeed) ?
                1.0f :
                scaleMultiplier * (wheelieBonus * m_kclSpeedFactor);

        bool ignoreScale = status.onBit(eStatus::RampBoost, eStatus::ZipperInvisibleWall,
                eStatus::OverZipper, eStatus::HalfPipeRamp);
        f32 boostSpeed = ignoreScale ? 1.0f : scaleMultiplier;
        boostSpeed *= boostSpdLimit * m_engineSpeedFactor * m_kclSpeedFactor;

        if (status.offBit(eStatus::JumpPad) && boostSpeed > 0.0f && boostSpeed > speedLimit) {
            speedLimit = boostSpeed;
        }

        m_jumpPadSoftSpeedLimit = boostSpdLimit * m_engineSpeedFactor * m_kclSpeedFactor;

        if (status.onBit(eStatus::RampBoost)) {
            speedLimit = std::max(speedLimit, 100.0f);
        }
    } else {
        // The bullet bill flies at its fixed speed.
        speedLimit = BULLET_SPEED * m_engineSpeedFactor * killer()->speedScale();
        m_softSpeedLimit = speedLimit;
        m_speed = speedLimit;
    }

    m_lastDir = (m_speed > 0.0f) ? 1.0f * m_dir : -1.0f * m_dir;

    f32 local_c8 = 1.0f;
    speedLimit *= calcWallCollisionSpeedFactor(local_c8);

    // A bike knocked out of a wheelie by another kart is held to 40% of its limit for the 25
    // frames of the wobble (0x8057BF08, checked against a Dolphin recording).
    if (status.onBit(eStatus::WheelieBumpWobble)) {
        speedLimit *= WHEELIE_BUMP_SPEED_FACTOR;
    }

    // 0x8057BF1C: a racer driven by another console never goes below the soft speed limit that
    // console sent (KartNetReceiver +0x84).
    if (status.onBit(eStatus::OnlineRemote) && m_receivedSoftSpeedLimit > speedLimit) {
        speedLimit = m_receivedSoftSpeedLimit;
    }

    if (m_softSpeedLimit <= speedLimit) {
        m_softSpeedLimit = speedLimit;
    } else if (status.offBit(eStatus::WallCollision, eStatus::Wall3Collision)) {
        m_softSpeedLimit = std::max(m_softSpeedLimit - 3.0f, speedLimit);
    } else {
        m_softSpeedLimit = speedLimit;
    }

    m_softSpeedLimit = std::min(m_hardSpeedLimit, m_softSpeedLimit);

    m_speed = std::min(m_softSpeedLimit, std::max(-m_softSpeedLimit, m_speed));

    if (status.onBit(eStatus::JumpPad)) {
        m_speed = std::max(m_speed, m_jumpPadMinSpeed);
    }

    calcWallCollisionStart(local_c8);

    m_speedRatio = EGG::Mathf::abs(m_speed / m_baseSpeed);
    m_speedRatioCapped = std::min(1.0f, m_speedRatio);

    EGG::Vector3f crossVec = m_smoothedUp.cross(m_dir);
    if (m_speed < 0.0f) {
        crossVec = -crossVec;
    }

    f32 rotationScalar = ROTATION_SCALAR_NORMAL;
    if (collide()->surfaceFlags().onBit(KartCollide::eSurfaceFlags::BoostRamp)) {
        rotationScalar = ROTATION_SCALAR_BOOST_RAMP;
    } else if (status.offBit(eStatus::TouchingGround)) {
        rotationScalar = ROTATION_SCALAR_MIDAIR;
    }

    EGG::Matrix34f local_90;
    local_90.setAxisRotation(DEG2RAD * rotationScalar, crossVec);
    m_vel1Dir = local_90.multVector33(m_vel1Dir);

    const auto *raceMgr = System::RaceManager::Instance();
    if (status.offBit(eStatus::InAction, eStatus::DisableBackwardsAccel, eStatus::Accelerate) &&
            status.onBit(eStatus::TouchingGround) &&
            raceMgr->isStageReached(System::RaceManager::Stage::Race)) {
        calcDeceleration();
    }

    m_processedSpeed = m_speed;
    EGG::Vector3f nextSpeed = m_speed * m_vel1Dir;

    f32 maxSpeedY = status.onBit(eStatus::OverZipper) ? KartHalfPipe::TerminalVelocity() :
                                                        TERMINAL_VELOCITY;
    nextSpeed.y = std::min(nextSpeed.y, maxSpeedY);

    dynamics()->setIntVel(dynamics()->intVel() + nextSpeed);

    if (status.onBit(eStatus::TouchingGround) &&
            status.offBit(eStatus::DriftManual, eStatus::Hop)) {
        if (status.onBit(eStatus::Brake)) {
            if (m_drivingDirection == DrivingDirection::Forwards) {
                m_drivingDirection = m_processedSpeed > 5.0f ? DrivingDirection::Braking :
                                                               DrivingDirection::Backwards;
            }
        } else {
            if (m_processedSpeed >= 0.0f) {
                m_drivingDirection = DrivingDirection::Forwards;
            }
        }
    } else {
        m_drivingDirection = DrivingDirection::Forwards;
    }
}

/// @addr{0x8057B108}
/// @stage 2
/// @brief Every frame, computes a speed scalar if we are colliding with a wall.
f32 KartMove::calcWallCollisionSpeedFactor(f32 &f1) {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::WallCollision, eStatus::Wall3Collision)) {
        return 1.0f;
    }

    onWallCollision();

    if (status.onBit(eStatus::ZipperInvisibleWall, eStatus::OverZipper)) {
        return 1.0f;
    }

    EGG::Vector3f wallNrm = collisionData().wallNrm;
    if (wallNrm.y > 0.0f) {
        wallNrm.y = 0.0f;
        wallNrm.normalise();
    }

    f32 dot = m_lastDir.dot(wallNrm);

    if (dot < 0.0f) {
        f1 = std::max(0.0f, dot + 1.0f);

        return std::min(1.0f, f1 * (status.onBit(eStatus::WallCollision) ? 0.4f : 0.7f));
    }

    return 1.0f;
}

/// @addr{0x8057B2A0}
/// @stage 2
/// @brief If we started to collide with a wall this frame, applies rotation.
void KartMove::calcWallCollisionStart(f32 param_2) {
    m_flags.resetBit(eFlags::WallBounce);

    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::WallCollisionStart)) {
        return;
    }

    m_outsideDriftAngle = 0.0f;
    if (status.offBit(eStatus::InAction)) {
        m_dir = bodyFront();
        m_vel1Dir = m_dir;
        m_landingDir = m_dir;
        m_smoothedForward = m_dir;
    }

    if (status.offBit(eStatus::ZipperInvisibleWall, eStatus::OverZipper) && param_2 < 0.9f) {
        f32 speedDiff = m_lastSpeed - m_speed;
        const CollisionData &colData = collisionData();

        if (speedDiff > 30.0f) {
            m_flags.setBit(eFlags::WallBounce);
            EGG::Vector3f newPos = colData.relPos + pos();
            f32 dot = -bodyUp().dot(colData.relPos) * 0.5f;
            EGG::Vector3f scaledUp = dot * bodyUp();
            newPos -= scaledUp;

            speedDiff = std::min(60.0f, speedDiff);
            EGG::Vector3f scaledWallNrm = speedDiff * colData.wallNrm;

            auto [proj, rej] = scaledWallNrm.projAndRej(m_vel1Dir);
            proj *= 0.3f;
            rej *= 0.9f;

            if (status.onBit(eStatus::Boost)) {
                proj = EGG::Vector3f::zero;
                rej = EGG::Vector3f::zero;
            }

            if (bodyFront().dot(colData.wallNrm) > 0.0f) {
                proj = EGG::Vector3f::zero;
            }
            rej *= 0.9f;

            EGG::Vector3f projRejSum = proj + rej;
            f32 bumpDeviation = 0.0f;
            if (m_flags.offBit(eFlags::DriftReset) && status.onBit(eStatus::TouchingGround)) {
                bumpDeviation = param()->stats().bumpDeviationLevel;
            }

            dynamics()->applyWrenchScaled(newPos, projRejSum, bumpDeviation);
        } else if (wallKclType() == COL_TYPE_SPECIAL_WALL && wallKclVariant() == 2) {
            dynamics()->addForce(colData.wallNrm * 15.0f);
            collide()->startFloorMomentRate();
        }

        if (wallKclType() == COL_TYPE_SPECIAL_WALL && wallKclVariant() == 0) {
            dynamics()->addForce(colData.wallNrm * 15.0f);
            collide()->startFloorMomentRate();
        }
    }
}

/// @stage 1+
/// @brief STAGE Computes the x-component of angular velocity based on the kart's speed.
/// @addr{0x8057D1D4}
void KartMove::calcStandstillBoostRot() {
    f32 next = 0.0f;
    f32 scalar = 1.0f;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::TouchingGround)) {
        if (System::RaceManager::Instance()->stage() == System::RaceManager::Stage::Countdown) {
            next = 0.015f * -state()->startBoostCharge();
        } else if (status.offBit(eStatus::ChargingSSMT)) {
            if (status.offBit(eStatus::JumpPad, eStatus::RampBoost,
                        eStatus::SoftWallUnlockRotation)) {
                f32 speedDiff = m_lastSpeed - m_speed;
                scalar = std::min(3.0f, std::max(speedDiff, -3.0f));

                if (status.onBit(eStatus::MushroomBoost)) {
                    next = (scalar * 0.15f) * 0.25f;
                    if (status.onBit(eStatus::Wheelie)) {
                        next *= 0.5f;
                    }
                } else {
                    next = (scalar * 0.15f) * 0.08f;
                }
                scalar = m_driftingParams->boostRotFactor;
            }
        } else {
            constexpr s16 MAX_SSMT_CHARGE = 75;
            next = 0.015f * (-static_cast<f32>(m_ssmtCharge) / static_cast<f32>(MAX_SSMT_CHARGE));
        }
    }

    if (m_flags.onBit(eFlags::WallBounce)) {
        m_standStillBoostRot = isBike() ? next * 3.0f : next * 10.0f;
    } else {
        m_standStillBoostRot += scalar * (next * m_invScale - m_standStillBoostRot);
    }
}

/// @stage 2
/// @brief Responds to player input to handle up/down kart tilt mid-air.
/// @addr{0x805869DC}
void KartMove::calcDive() {
    constexpr f32 DIVE_LIMIT = 0.8f;

    m_divingRot *= 0.96f;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::TouchingGround, eStatus::CannonStart, eStatus::InCannon,
                eStatus::InAction, eStatus::OverZipper)) {
        return;
    }

    f32 stickY = state()->stickY();

    if (status.onBit(eStatus::InATrick) && m_jump->type() == TrickType::BikeSideStuntTrick) {
        stickY = std::min(1.0f, stickY + 0.4f);
    }

    u32 airtime = state()->airtime();

    if (airtime > 50) {
        if (EGG::Mathf::abs(stickY) < 0.1f) {
            m_divingRot += 0.05f * (-0.025f - m_divingRot);
        }
    } else {
        stickY *= (airtime / 50.0f);
    }

    m_divingRot = std::max(-DIVE_LIMIT, std::min(DIVE_LIMIT, m_divingRot + stickY * 0.005f));

    EGG::Vector3f angVel2 = dynamics()->angVel2();
    angVel2.x += m_divingRot;
    dynamics()->setAngVel2(angVel2);

    if (state()->airtime() < 50) {
        return;
    }

    EGG::Vector3f topRotated = dynamics()->mainRot().rotateVector(EGG::Vector3f::ey);
    EGG::Vector3f forwardRotated = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);
    f32 upDotTop = m_up.dot(topRotated);
    EGG::Vector3f upCrossTop = m_up.cross(topRotated);
    f32 crossNorm = upCrossTop.length();
    f32 angle = EGG::Mathf::abs(EGG::Mathf::atan2(crossNorm, upDotTop));

    f32 fVar1 = angle * RAD2DEG - 20.0f;
    if (fVar1 <= 0.0f) {
        return;
    }

    f32 mult = std::min(1.0f, fVar1 / 20.0f);
    if (forwardRotated.y > 0.0f) {
        dynamics()->setGravity((1.0f - 0.2f * mult) * dynamics()->gravity());
    } else {
        dynamics()->setGravity((0.2f * mult + 1.0f) * dynamics()->gravity());
    }
}

/// @addr{Inlined in 0x805788DC}
/// @stage 2
/// @brief Calculates whether we are starting a standstill mini-turbo.
void KartMove::calcSsmtStart() {
    auto &status = KartObjectProxy::status();

    if (EGG::Mathf::abs(m_speed) >= 10.0f || status.onBit(eStatus::Boost, eStatus::RampBoost) ||
            status.offAnyBit(eStatus::Accelerate, eStatus::Brake)) {
        status.resetBit(eStatus::ChargingSSMT);
        return;
    }

    status.setBit(eStatus::ChargingSSMT).resetBit(eStatus::HopStart, eStatus::DriftInput);
}

/// @addr{0x80579968}
void KartMove::calcHopPhysics() {
    m_hopVelY = m_hopVelY * 0.998f + m_hopGravity;
    m_hopPosY += m_hopVelY;

    if (m_hopPosY < 0.0f) {
        m_hopPosY = 0.0f;
        m_hopVelY = 0.0f;
    }
}

/// @addr{0x80579960}
void KartMove::calcRejectRoad() {
    m_reject.calcRejectRoad();
}

/// @addr{0x80583F2C}
bool KartMove::calcZipperCollision(f32 radius, f32 scale, EGG::Vector3f &pos,
        EGG::Vector3f &upLocal, const EGG::Vector3f &prevPos, Field::CollisionInfo *colInfo,
        Field::KCLTypeMask *maskOut, Field::KCLTypeMask flags) const {
    upLocal = mainRot().rotateVector(EGG::Vector3f::ey);
    pos = dynamics()->pos() + (-scale * m_scale.y) * upLocal;

    auto *colDir = Field::CollisionDirector::Instance();
    return colDir->checkSphereFullPush(radius, pos, prevPos, flags, colInfo, maskOut, 0);
}

/// @addr{0x805879A4}
f32 KartMove::calcSlerpRate(f32 scale, const EGG::Quatf &from, const EGG::Quatf &to) const {
    f32 dotNorm = std::max(-1.0f, std::min(1.0f, from.dot(to)));
    f32 acos = EGG::Mathf::acos(dotNorm);
    return acos > 0.0f ? std::min(0.1f, scale / acos) : 0.1f;
}

/// @addr{0x80586DB4}
void KartMove::applyForce(f32 force, const EGG::Vector3f &hitDir, bool stop) {
    constexpr s16 BUMP_COOLDOWN = 5;

    if (m_bumpTimer >= 1) {
        return;
    }

    dynamics()->addForce(force * hitDir.perpInPlane(m_up, true));
    collide()->startFloorMomentRate();

    m_bumpTimer = BUMP_COOLDOWN;

    if (stop) {
        m_speed = 0.0f;
    }
}

/// @addr{0x8057CF0C}
/// @brief Every frame, calculates rotation, EV, and angular velocity for the kart.
void KartMove::calcVehicleRotation(f32 turn) {
    f32 tiltMagnitude = 0.0f;
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::InAction, eStatus::SoftWallUnlockRotation) &&
            status.onBit(eStatus::AnyWheelCollision)) {
        EGG::Vector3f front = componentZAxis();
        front = front.perpInPlane(m_up, true);
        EGG::Vector3f frontSpeed = velocity().rej(front).perpInPlane(m_up, false);
        f32 magnitude = tiltMagnitude;

        if (frontSpeed.squaredLength() > std::numeric_limits<f32>::epsilon()) {
            magnitude = frontSpeed.length();

            if (front.z * frontSpeed.x - front.x * frontSpeed.z > 0.0f) {
                magnitude = -magnitude;
            }

            tiltMagnitude = -1.0f;
            if (-1.0f <= magnitude) {
                tiltMagnitude = std::min(1.0f, magnitude);
            }
        }
    } else if (status.offBit(eStatus::Hop) || m_hopPosY <= 0.0f) {
        EGG::Vector3f angVel0 = dynamics()->angVel0();
        angVel0.z *= 0.98f;
        dynamics()->setAngVel0(angVel0);
    }

    f32 lean =
            m_invScale * (tiltMagnitude * param()->stats().tilt * EGG::Mathf::abs(m_weightedTurn));

    calcStandstillBoostRot();

    EGG::Vector3f angVel0 = dynamics()->angVel0();
    angVel0.x += m_standStillBoostRot;
    angVel0.z += lean;
    dynamics()->setAngVel0(angVel0);

    EGG::Vector3f angVel2 = dynamics()->angVel2();
    angVel2.y += turn;
    dynamics()->setAngVel2(angVel2);

    calcDive();
}

/// @stage 2
/// @brief Every frame during a drift, calculates MT/SMT charge based on player input.
/// @addr{0x8057EE50}
void KartMove::calcMtCharge() {
    // TODO: Some of these are shared between the base and derived class implementations.
    constexpr u16 MAX_MT_CHARGE = 270;
    constexpr u16 MAX_SMT_CHARGE = 300;
    constexpr u16 BASE_MT_CHARGE = 2;
    constexpr u16 BASE_SMT_CHARGE = 2;
    constexpr f32 BONUS_CHARGE_STICK_THRESHOLD = 0.4f;
    constexpr u16 EXTRA_MT_CHARGE = 3;

    if (m_driftState == DriftState::ChargedSmt) {
        return;
    }

    f32 stickX = state()->stickX();

    if (m_driftState == DriftState::ChargingMt) {
        m_mtCharge += BASE_MT_CHARGE;

        if (-BONUS_CHARGE_STICK_THRESHOLD <= stickX) {
            if (BONUS_CHARGE_STICK_THRESHOLD < stickX && m_hopStickX == -1) {
                m_mtCharge += EXTRA_MT_CHARGE;
            }
        } else if (m_hopStickX != -1) {
            m_mtCharge += EXTRA_MT_CHARGE;
        }

        if (m_mtCharge > MAX_MT_CHARGE) {
            m_mtCharge = MAX_MT_CHARGE;
            m_driftState = DriftState::ChargingSmt;
        }
    }

    if (m_driftState != DriftState::ChargingSmt) {
        return;
    }

    m_smtCharge += BASE_SMT_CHARGE;

    if (-BONUS_CHARGE_STICK_THRESHOLD <= stickX) {
        if (BONUS_CHARGE_STICK_THRESHOLD < stickX && m_hopStickX == -1) {
            m_smtCharge += EXTRA_MT_CHARGE;
        }
    } else if (m_hopStickX != -1) {
        m_smtCharge += EXTRA_MT_CHARGE;
    }

    if (m_smtCharge > MAX_SMT_CHARGE) {
        m_smtCharge = MAX_SMT_CHARGE;
        m_driftState = DriftState::ChargedSmt;
    }

    // 0x80578440
    if (KartObjectProxy::status().onBit(eStatus::InBullet)) {
        killer()->cancel();
    }
}

/// @addr{0x80583658}
void KartMove::initOob() {
    clearBoost();
    clearJumpPad();
    clearRampBoost();
    clearZipperBoost();
    clearSsmt();
    clearSlipstream();
    clearOffroadInvincibility();

    // 0x8058379C
    if (KartObjectProxy::status().onBit(eStatus::InBullet)) {
        killer()->cancel();
    }
}

/// @stage 2
/// @brief Initializes hop information, resets upwards EV and clears upwards force.
/// @addr{0x8057DA5C}
void KartMove::hop() {
    status().setBit(eStatus::Hop).resetBit(eStatus::DriftManual);
    onHop();

    m_hopUp = dynamics()->mainRot().rotateVector(EGG::Vector3f::ey);
    m_hopDir = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);
    m_driftState = DriftState::NotDrifting;
    m_smtCharge = 0;
    m_mtCharge = 0;
    m_hopStickX = 0;
    m_hopFrame = 0;
    m_hopPosY = 0.0f;
    m_hopGravity = dynamics()->gravity();
    m_hopVelY = m_driftingParams->hopVelY;
    m_outsideDriftBonus = 0.0f;

    EGG::Vector3f extVel = dynamics()->extVel();
    extVel.y = 0.0f + m_hopVelY;
    dynamics()->setExtVel(extVel);

    EGG::Vector3f totalForce = dynamics()->totalForce();
    totalForce.y = 0.0f;
    dynamics()->setTotalForce(totalForce);
}

/// @addr{Inlined at 0x80587590}
void KartMove::tryStartBoostPanel() {
    constexpr s16 BOOST_PANEL_DURATION = 60;

    if (status().onBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        return;
    }

    activateBoost(KartBoost::Type::MushroomAndBoostPanel, BOOST_PANEL_DURATION);
    setOffroadInvincibility(BOOST_PANEL_DURATION);
}

/// @stage 2
/// @brief Sets offroad invincibility and enables the ramp boost bitfield flag.
/// @addr{Inlined at 0x80587590}
void KartMove::tryStartBoostRamp() {
    constexpr s16 BOOST_RAMP_DURATION = 60;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        return;
    }

    status.setBit(eStatus::RampBoost);
    m_rampBoost = BOOST_RAMP_DURATION;
    setOffroadInvincibility(BOOST_RAMP_DURATION);
}

/// @stage 2
/// @brief Applies calculations to start interacting with a @ref COL_TYPE_JUMP_PAD "jump pad".
/// @addr{0x8057FD18}
/// @details If applicable, updates @ref KartDynamics::m_extVel "external velocity"
void KartMove::tryStartJumpPad() {
    static constexpr std::array<JumpPadProperties, 8> JUMP_PAD_PROPERTIES = {{
            {50.0f, 50.0f, 35.0f},
            {50.0f, 50.0f, 47.0f},
            {59.0f, 59.0f, 30.0f},
            {73.0f, 73.0f, 45.0f},
            {73.0f, 73.0f, 53.0f},
            {56.0f, 56.0f, 50.0f},
            {55.0f, 55.0f, 35.0f},
            {56.0f, 56.0f, 50.0f},
    }};

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn, eStatus::InAction, eStatus::HalfPipeRamp)) {
        return;
    }

    status.setBit(eStatus::JumpPad);
    s32 jumpPadVariant = state()->jumpPadVariant();
    m_jumpPadProperties = &JUMP_PAD_PROPERTIES[jumpPadVariant];

    if (jumpPadVariant == 3 || jumpPadVariant == 4) {
        if (m_jumpPadBoostMultiplier > 1.3f ||
                m_jumpPadSoftSpeedLimit > 110.0f * m_engineSpeedFactor) {
            // Set speed to 100 if the player has boost from a boost panel or mushroom(item) before
            // hitting the jump pad
            static constexpr std::array<JumpPadProperties, 2> JUMP_PAD_PROPERTIES_SHROOM_BOOST = {{
                    {100.0f, 100.0f, 70.0f},
                    {100.0f, 100.0f, 65.0f},
            }};
            m_jumpPadProperties = &JUMP_PAD_PROPERTIES_SHROOM_BOOST[jumpPadVariant != 3];
        }

        status.setBit(eStatus::JumpPadFixedSpeed);
    }

    if (jumpPadVariant == 4) {
        status.setBit(eStatus::JumpPadMushroomTrigger, eStatus::JumpPadMushroomVelYInc,
                eStatus::JumpPadMushroomCollision);
    } else {
        EGG::Vector3f extVel = dynamics()->extVel();
        EGG::Vector3f totalForce = dynamics()->totalForce();

        extVel.y = m_jumpPadProperties->velY;
        totalForce.y = 0.0f;

        dynamics()->setExtVel(extVel);
        dynamics()->setTotalForce(totalForce);

        if (jumpPadVariant != 3) {
            EGG::Vector3f dir = m_dir;
            dir.y = 0.0f;
            dir.normalise();
            m_speed *= m_dir.dot(dir);
            m_dir = dir;
            m_vel1Dir = dir;
            status.setBit(eStatus::JumpPadDisableYsusForce);
        }
    }

    m_jumpPadMinSpeed = m_jumpPadProperties->minSpeed;
    m_jumpPadMaxSpeed = m_jumpPadProperties->maxSpeed;
    m_speed = std::max(m_speed, m_jumpPadMinSpeed);
}

/// @addr{0x80582530}
void KartMove::tryEndJumpPad() {
    auto &status = KartObjectProxy::status();
    if (status.onBit(eStatus::JumpPadMushroomTrigger)) {
        if (status.onBit(eStatus::GroundStart)) {
            status.resetBit(eStatus::JumpPadMushroomTrigger, eStatus::JumpPadFixedSpeed,
                    eStatus::JumpPadMushroomVelYInc);
        }

        if (status.onBit(eStatus::JumpPadMushroomVelYInc)) {
            EGG::Vector3f newExtVel = dynamics()->extVel();
            newExtVel.y += 20.0f;
            if (m_jumpPadProperties->velY < newExtVel.y) {
                newExtVel.y = m_jumpPadProperties->velY;
                status.resetBit(eStatus::JumpPadMushroomVelYInc);
            }
            dynamics()->setExtVel(newExtVel);
        }
    }

    if (status.onBit(eStatus::GroundStart) && status.offBit(eStatus::JumpPadMushroomTrigger)) {
        cancelJumpPad();
    }
}

/// @addr{0x80582DB4}
void KartMove::cancelJumpPad() {
    m_jumpPadMinSpeed = 0.0f;
    status().resetBit(eStatus::JumpPad);
}

/// @addr{0x8057F090}
void KartMove::activateBoost(KartBoost::Type type, s16 frames) {
    if (m_boost.activate(type, frames)) {
        status().setBit(eStatus::Boost);
    }
}

/// @addr{0x8058212C}
void KartMove::applyStartBoost(s16 frames) {
    activateBoost(KartBoost::Type::AllMt, frames);
}

/// @addr{0x8057F3D8}
void KartMove::activateMushroom() {
    constexpr s16 MUSHROOM_DURATION = 90;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        return;
    }

    activateBoost(KartBoost::Type::MushroomAndBoostPanel, MUSHROOM_DURATION);

    m_mushroomBoostTimer = MUSHROOM_DURATION;
    status.setBit(eStatus::MushroomBoost);
    setOffroadInvincibility(MUSHROOM_DURATION);
}

/// @addr{0x8057F96C}
void KartMove::activateZipperBoost() {
    constexpr s16 BASE_DURATION = 50;
    constexpr s16 TRICK_DURATION = 100;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        return;
    }

    s16 boostDuration = status.onBit(eStatus::ZipperTrick) ? TRICK_DURATION : BASE_DURATION;
    activateBoost(KartBoost::Type::TrickAndZipper, boostDuration);

    setOffroadInvincibility(boostDuration);
    m_zipperBoostTimer = 0;
    m_zipperBoostMax = boostDuration;
    status.setBit(eStatus::ZipperBoost);
}

/// @stage 2
/// @brief Ignores offroad KCL collision for a set amount of time.
/// @addr{0x805824C8}
/// @param timer Framecount to ignore offroad
void KartMove::setOffroadInvincibility(s16 timer) {
    if (timer > m_offroadInvincibility) {
        m_offroadInvincibility = timer;
    }

    status().setBit(eStatus::BoostOffroadInvincibility);
}

/// @stage 2
/// @brief Checks a timer to see if we are still ignoring offroad slowdown.
/// @addr{0x805824F0}
void KartMove::calcOffroadInvincibility() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::BoostOffroadInvincibility)) {
        return;
    }

    if (--m_offroadInvincibility > 0) {
        return;
    }

    status.resetBit(eStatus::BoostOffroadInvincibility);
}

/// @stage 2
/// @brief Checks a timer to see if we are still boosting from a mushroom.
void KartMove::calcMushroomBoost() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::MushroomBoost)) {
        return;
    }

    if (--m_mushroomBoostTimer > 0) {
        return;
    }

    status.resetBit(eStatus::MushroomBoost);
}

/// @addr{0x80582E34}
void KartMove::calcZipperBoost() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::ZipperBoost)) {
        return;
    }

    status.setBit(eStatus::Accelerate);

    if (status.offBit(eStatus::OverZipper) && ++m_zipperBoostTimer >= m_zipperBoostMax) {
        m_zipperBoostTimer = 0;
        status.resetBit(eStatus::ZipperBoost);
    }

    if (m_zipperBoostTimer < 10) {
        EGG::Vector3f angVel = dynamics()->angVel0();
        angVel.y = 0.0f;
        dynamics()->setAngVel0(angVel);
    }
}

/// @addr{0x8057F7A8}
void KartMove::landTrick() {
    static constexpr std::array<s16, 3> KART_TRICK_BOOST_DURATION = {{
            40,
            70,
            85,
    }};
    static constexpr std::array<s16, 3> BIKE_TRICK_BOOST_DURATION = {{
            45,
            80,
            95,
    }};

    if (status().onBit(eStatus::BeforeRespawn, eStatus::InAction)) {
        return;
    }

    s16 duration;
    if (isBike()) {
        duration = BIKE_TRICK_BOOST_DURATION[static_cast<u32>(m_jump->variant())];
    } else {
        duration = KART_TRICK_BOOST_DURATION[static_cast<u32>(m_jump->variant())];
    }

    activateBoost(KartBoost::Type::TrickAndZipper, duration);
}

/// @addr{0x80580F28}
void KartMove::activateCrush(u16 timer) {
    status().setBit(eStatus::Crushed);
    m_crushTimer = timer;
    m_kartScale->startCrush();
}

/// @addr{0x80580F9C}
void KartMove::calcCrushed() {
    if (status().offBit(eStatus::Crushed)) {
        return;
    }

    if (--m_crushTimer == 0) {
        status().resetBit(eStatus::Crushed, eStatus::InCrushAction);
        m_kartScale->endCrush();
    }
}

/// @addr{0x8058160C}
void KartMove::calcScale() {
    m_kartScale->calc();

    const EGG::Vector3f sizeScale = m_kartScale->sizeScale();
    setScale(m_kartScale->pressScale() * sizeScale);
    m_totalScale = m_bulletScale * (m_shockSpeedMultiplier * m_megaScale);
    m_hitboxScale = std::max(sizeScale.z, m_totalScale);

    if (sizeScale.z != 1.0f) {
        setInertiaScale(m_scale);
    }

    m_invScale = m_scale.z > 1.0f ? 1.0f / m_scale.z : 1.0f;
}

/// @addr{Inlined in 0x80580768}
/// @brief A shrinking object hit us.
void KartMove::applyShrink(u16 timer) {
    static_cast<void>(applyLightningEffect(static_cast<s16>(timer), true, false));
}

/// @addr{0x80580438}
/// @brief Hit by another racer's lightning: the shrink lasts longer the better our rank.
/// @return Whether it hit (a mega only ends the mega).
bool KartMove::applyLightning() {
    static constexpr std::array<s16, 12> SHRINK_DURATIONS = {
            {690, 645, 600, 555, 510, 450, 390, 330, 270, 210, 150, 90}};
    constexpr s16 SHRINK_DURATION_BASE = 72;

    // The game uses a fixed 450 frames in battle; Kinoko has no battle mode.
    u8 rank = System::RaceManager::Instance()->player(param()->playerIdx()).rank();
    if (rank == 0 || rank > 12) {
        return false;
    }

    size_t playerCount = KartObjectManager::Instance()->count();
    s16 duration =
            static_cast<s16>(SHRINK_DURATIONS[rank + 11 - playerCount] + SHRINK_DURATION_BASE);
    return applyLightningEffect(duration, false, false);
}

/// @addr{0x805805EC}
/// @brief The thunder cloud we carry strikes: the same shock as a POW block's action, for 612
/// frames. The HUD field the game clears first is left out.
void KartMove::applyThunderCloud() {
    constexpr s16 SHRINK_DURATION = 612;

    static_cast<void>(applyLightningEffect(SHRINK_DURATION, false, true));
}

/// @addr{0x80580778}
/// @brief Shrinks and shocks us (lightning, a shrinking object, a POW): an action, the held item
/// lost and a shrink for timer frames. A mega mushroom only ends the mega instead.
/// @return Whether we were hit.
bool KartMove::applyLightningEffect(s16 timer, bool fromObject, bool fromPow) {
    auto &status = KartObjectProxy::status();
    if (status.onBit(eStatus::TriggerRespawn, eStatus::NoKartCollision40, eStatus::InRespawn,
                eStatus::AfterRespawn, eStatus::NoKartCollision82, eStatus::Vanished,
                eStatus::NoKartCollision89, eStatus::InStar, eStatus::CannonStart,
                eStatus::InCannon, eStatus::InBullet)) {
        return false;
    }

    if (status.onBit(eStatus::InMega)) {
        deactivateMega(false);
        return true;
    }

    Action hitAction = Action::LightningHit;
    if (fromObject) {
        hitAction = Action::UNK_15;
    } else if (fromPow) {
        hitAction = Action::PowHit;
    }

    action()->start(hitAction);
    Item::ItemDirector::Instance()->kartItem(param()->playerIdx()).loseItem();
    status.setBit(eStatus::Shocked);

    if (static_cast<s16>(m_shockTimer) < timer) {
        m_shockTimer = static_cast<u16>(timer);
        m_kartScale->startShrink(0);
    }

    return true;
}

/// @addr{0x80580268}
/// @brief Star: 450 frames in which actions can't start, offroad doesn't slow us, acceleration is
/// 3.0 and the boost speed limit at least 105. Clears ink and an ongoing crush.
void KartMove::activateStar() {
    constexpr s16 STAR_DURATION = 450;

    auto &status = KartObjectProxy::status();
    if (status.onBit(eStatus::NoKartCollision72, eStatus::NoKartCollision82, eStatus::Vanished,
                eStatus::InBullet, eStatus::InAction)) {
        return;
    }

    status.setBit(eStatus::InStar);
    m_starTimer = STAR_DURATION;
    if (m_offroadInvincibility < STAR_DURATION) {
        m_offroadInvincibility = STAR_DURATION;
    }
    status.setBit(eStatus::BoostOffroadInvincibility);

    if (status.onBit(eStatus::Inked)) {
        status.resetBit(eStatus::Inked);
        m_inkTimer = 0;
    }

    if (status.onBit(eStatus::Crushed)) {
        status.resetBit(eStatus::Crushed, eStatus::InCrushAction);
        m_kartScale->endCrush();
    }
}

/// @addr{0x805803F0}
void KartMove::calcStar() {
    auto &status = KartObjectProxy::status();
    if (status.onBit(eStatus::InStar) && --m_starTimer < 1) {
        status.resetBit(eStatus::InStar);
    }
}

/// @addr{0x80580B14}
/// @brief Mega mushroom: 450 frames big (the size grows through KartScale, the speed and hitbox
/// scale through m_megaScale), squashing other racers and ignoring most hits.
void KartMove::activateMega() {
    constexpr s16 MEGA_DURATION = 450;

    auto &status = KartObjectProxy::status();
    status.setBit(eStatus::InMega);
    m_megaTimer = MEGA_DURATION;
    setObjectCollisionScale(param()->stats().megaScale);

    if (status.onBit(eStatus::Shocked)) {
        deactivateShock(false);
    }

    if (status.onBit(eStatus::Crushed)) {
        status.resetBit(eStatus::Crushed, eStatus::InCrushAction);
        m_kartScale->endCrush();
    }

    m_kartScale->startShrink(1);
    m_megaShrinking = false;
}

/// @addr{0x80580DC0}
void KartMove::calcMega() {
    constexpr s16 MEGA_SHRINK_FRAMES = 26;
    constexpr f32 MEGA_GROW_STEP = 0.02f;
    constexpr f32 MEGA_SHRINK_STEP = 0.1f;

    auto &status = KartObjectProxy::status();
    if (status.offBit(eStatus::InMega)) {
        m_megaScale = std::max(m_megaScale - MEGA_SHRINK_STEP, 1.0f);
        return;
    }

    if (--m_megaTimer < 1) {
        status.resetBit(eStatus::InMega);
        m_megaTimer = 0;
        setObjectCollisionScale(1.0f);
        if (!m_megaShrinking) {
            m_kartScale->endShrink(1);
            m_megaShrinking = true;
        }
    } else if (!m_megaShrinking && m_megaTimer < MEGA_SHRINK_FRAMES) {
        m_megaShrinking = true;
        m_kartScale->endShrink(1);
    }

    m_megaScale = std::min(m_megaScale + MEGA_GROW_STEP, param()->stats().megaScale);
}

/// @addr{0x80580CDC}
void KartMove::deactivateMega(bool resetScale) {
    status().resetBit(eStatus::InMega);
    m_megaTimer = 0;
    setObjectCollisionScale(1.0f);
    if (!m_megaShrinking) {
        m_kartScale->endShrink(1);
        m_megaShrinking = true;
    }

    if (resetScale) {
        m_megaScale = 1.0f;
    }
}

/// @addr{0x80598338}
/// @brief Resizes the kart's box collision unit (the search sphere for objects and other karts)
/// for a size scale. The game also scales a second kart sphere Kinoko does not have.
void KartMove::setObjectCollisionScale(f32 scale) {
    constexpr f32 BOX_COL_PADDING = 25.0f;

    f32 radius = scale * collisionGroup()->boundingRadius();
    boxColUnit()->resize(radius + BOX_COL_PADDING, m_hardSpeedLimit);
}

/// @addr{0x805810A8}
/// @brief A trick input opens a 10-frame window that turns a POW spin into the light one (or
/// lightens one already started); a POW block's last shake spins us if we are on the ground.
/// The HUD, the battle statistics and the hit counter are left out.
void KartMove::calcPow() {
    constexpr s16 DODGE_FRAMES = 10;
    constexpr u32 DODGE_LAST_FRAME = 8;
    constexpr u32 HIT_FRAME = 8;

    auto *kartAction = action();

    if (--m_powDodgeTimer < 0) {
        m_powDodgeTimer = 0;
        if (inputs()->currentState().trick == System::Trick::Up) {
            if (kartAction->currentAction() == Action::PowSpin) {
                if (kartAction->frame() < DODGE_LAST_FRAME) {
                    static_cast<void>(kartAction->dodgePow());
                }
            } else {
                m_powDodgeTimer = DODGE_FRAMES;
            }
        }
    }

    if (kartAction->isPowStriking() && status().onBit(eStatus::TouchingGround)) {
        static_cast<void>(kartAction->start(Action::PowSpin));
    }

    m_powHit = kartAction->currentAction() == Action::PowSpin && kartAction->frame() == HIT_FRAME;
}

/// @addr{0x805858AC}
/// @brief The bullet bill starts: everything else ends, the kart faces its rotation, and the
/// KartKiller takes over.
void KartMove::activateBullet() {
    constexpr f32 TOP_RATE = 0.3f;
    constexpr f32 BULLET_SCALE = 1.5f;

    setBulletHardSpeedLimit();
    m_hitboxScale = param()->stats().megaScale;
    clearForBullet();
    m_dir = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);
    dynamics()->setExtVel(EGG::Vector3f::zero);
    dynamics()->setMovingRoadVel(EGG::Vector3f::zero);
    // Dynamics +0xC8 (a vector Kinoko does not model) is cleared too.
    physics()->clearDecayingRot();
    m_outsideDriftBonus = 0.0f;
    m_weightedTurn = 0.0f;
    m_realTurn = 0.0f;
    m_rawTurn = 0.0f;
    m_divingRot = 0.0f;
    m_bulletTopRate = TOP_RATE;
    m_bulletTop = m_smoothedUp;
    killer()->start(System::MapdataItemPoint::NONE);
    m_bulletScale = BULLET_SCALE;
}

/// @addr{0x805837CC}
/// @brief Ends every effect on the kart for a bullet bill: boosts, star, mega and shock. The
/// game's list also resets a few timers and flags that Kinoko's clear functions cover; the
/// sounds and the HUD are left out.
void KartMove::clearForBullet() {
    auto &status = KartObjectProxy::status();

    clear();

    if (status.onBit(eStatus::InStar)) {
        status.resetBit(eStatus::InStar);
        m_starTimer = 0;
    }

    if (status.onBit(eStatus::InMega)) {
        deactivateMega(true);
    }

    if (status.onBit(eStatus::Shocked)) {
        deactivateShock(false);
    }
}

/// @addr{0x80581A58}
/// @brief Hit by a blooper: 450 frames inked. Only the screen splat and CPU driving read it.
void KartMove::applyInk(bool fromSelf) {
    constexpr s16 INK_DURATION = 450;

    static_cast<void>(fromSelf);

    auto &status = KartObjectProxy::status();
    if (status.onBit(eStatus::TriggerRespawn, eStatus::NoKartCollision40, eStatus::InRespawn,
                eStatus::AfterRespawn, eStatus::NoKartCollision82, eStatus::Vanished,
                eStatus::NoKartCollision89)) {
        return;
    }

    status.setBit(eStatus::Inked);
    m_inkPending = true;
    m_inkTimer = INK_DURATION;
}

/// @addr{0x80581B1C}
/// @brief The ink countdown. The screen splat amount the game also computes here is visual.
void KartMove::calcInk() {
    auto &status = KartObjectProxy::status();
    if (status.offBit(eStatus::Inked)) {
        return;
    }

    if (--m_inkTimer < 0) {
        status.resetBit(eStatus::Inked);
        m_inkTimer = 0;
    }
}

/// @addr{0x80581C70}
void KartMove::clearInk() {
    status().resetBit(eStatus::Inked);
    m_inkTimer = 0;
}

/// @addr{0x80581A28}
/// @brief Called by the thunder cloud item object while we carry it.
void KartMove::activateThunderCloud() {
    status().setBit(eStatus::ThunderCloud);
}

/// @addr{0x80581A40}
void KartMove::deactivateThunderCloud() {
    status().resetBit(eStatus::ThunderCloud);
}

/// @addr{0x80580998}
void KartMove::calcShock() {
    auto &status = state()->status();

    if (status.onBit(eStatus::Shocked)) {
        if (--m_shockTimer == 0) {
            deactivateShock(false);
        }

        m_shockSpeedMultiplier = std::max(0.7f, m_shockSpeedMultiplier - 0.03f);
    } else {
        m_shockSpeedMultiplier = std::min(1.0f, m_shockSpeedMultiplier + 0.05f);
    }
}

/// @addr{0x80580A84}
void KartMove::deactivateShock(bool resetSpeed) {
    status().resetBit(eStatus::Shocked);
    m_shockTimer = 0;
    m_kartScale->endShrink(0);

    if (resetSpeed) {
        m_shockSpeedMultiplier = 1.0f;
    }
}

/// @addr{0x8058498C}
void KartMove::enterCannon() {
    init(true, true);
    physics()->clearDecayingRot();
    m_boost.resetActive();

    auto &status = KartObjectProxy::status();

    status.resetBit(eStatus::Boost);

    cancelJumpPad();
    clearRampBoost();
    clearZipperBoost();
    clearSsmt();
    clearSlipstream();
    clearOffroadInvincibility();

    dynamics()->reset();

    clearDrift();

    status.resetBit(eStatus::Hop, eStatus::CannonStart)
            .setBit(eStatus::InCannon, eStatus::SkipWheelCalc);

    const auto [cannonPos, cannonDir] = getCannonPosRot();
    m_cannonEntryPos = pos();
    m_cannonEntryOfs = cannonPos - pos();
    m_cannonEntryOfsLength = m_cannonEntryOfs.normalise();
    m_cannonEntryOfs.normalise();
    m_dir = m_cannonEntryOfs;
    m_vel1Dir = m_cannonEntryOfs;
    m_cannonOrthog = EGG::Vector3f::ey.perpInPlane(m_cannonEntryOfs, true);
    m_cannonProgress.setZero();
}

/// @addr{0x80584D58}
void KartMove::calcCannon() {
    auto [cannonPos, cannonDir] = getCannonPosRot();
    EGG::Vector3f forwardXZ = cannonPos - m_cannonEntryPos - m_cannonProgress;
    EGG::Vector3f forward = forwardXZ;
    f32 forwardLength = forward.normalise();
    forwardXZ.y = 0;
    forwardXZ.normalise();
    EGG::Vector3f local94 = m_cannonEntryOfs;
    local94.y = 0;
    local94.normalise();
    m_speedRatioCapped = 1.0f;
    m_speedRatio = 1.5f;
    EGG::Matrix34f cannonOrientation;
    cannonOrientation.makeOrthonormalBasis(forward, EGG::Vector3f::ey);
    EGG::Vector3f up = cannonOrientation.multVector33(EGG::Vector3f::ey);
    m_smoothedUp = up;
    m_up = up;

    if (forwardLength < 30.0f || local94.dot(forwardXZ) <= 0.0f) {
        exitCannon();
        return;
    }

    m_smoothedForward = cannonOrientation.multVector33(EGG::Vector3f::ez);
    m_speed = m_baseSpeed;
    const auto *cannonPoint =
            System::CourseMap::Instance()->getCannonPoint(state()->cannonPointId());
    size_t cannonParameterIdx = std::max<s16>(0, cannonPoint->parameterIdx());
    ASSERT(cannonParameterIdx < CANNON_PARAMETERS.size());
    const auto &cannonParams = CANNON_PARAMETERS[cannonParameterIdx];
    f32 newSpeed = cannonParams.speed;
    if (forwardLength < cannonParams.decelFactor) {
        f32 factor = std::max(0.0f, forwardLength / cannonParams.decelFactor);

        newSpeed = cannonParams.endDecel;
        if (newSpeed <= 0.0f) {
            newSpeed = m_baseSpeed;
        }

        newSpeed += factor * (cannonParams.speed - newSpeed);
        if (cannonParams.endDecel > 0.0f) {
            m_speed = std::min(newSpeed, m_speed);
        }
    }

    m_cannonProgress += m_cannonEntryOfs * newSpeed;

    EGG::Vector3f newPos = EGG::Vector3f::zero;
    if (cannonParams.height > 0.0f) {
        f32 fVar9 = EGG::Mathf::SinFIdx(
                (1.0f - (forwardLength / m_cannonEntryOfsLength)) * 180.0f * DEG2FIDX);
        newPos = fVar9 * cannonParams.height * m_cannonOrthog;
    }

    dynamics()->setPos(m_cannonEntryPos + m_cannonProgress + newPos);
    m_dir = m_cannonEntryOfs;
    m_vel1Dir = m_cannonEntryOfs;

    calcRotCannon(forward);

    dynamics()->setExtVel(EGG::Vector3f::zero);
}

/// @addr{0x805855BC}
void KartMove::calcRotCannon(const EGG::Vector3f &forward) {
    EGG::Vector3f local48 = forward;
    local48.normalise();
    EGG::Vector3f local54 = bodyFront();
    EGG::Vector3f local60 = local54 + ((local48 - local54) * 0.3f);
    local54.normalise();
    local60.normalise();
    // also local70, localA8
    EGG::Quatf local80;
    local80.makeVectorRotation(local54, local60);
    local80 *= dynamics()->fullRot();
    local80.normalise();
    EGG::Quatf localB8;
    localB8.makeVectorRotation(local80.rotateVector(EGG::Vector3f::ey), smoothedUp());
    EGG::Quatf newRot = local80.slerpTo(localB8.multSwap(local80), 0.3f);
    dynamics()->setFullRot(newRot);
    dynamics()->setMainRot(newRot);
}

/// @addr{0x805852C8}
void KartMove::exitCannon() {
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::InCannon)) {
        return;
    }

    status.resetBit(eStatus::InCannon, eStatus::SkipWheelCalc).setBit(eStatus::AfterCannon);
    dynamics()->setIntVel(m_cannonEntryOfs * m_speed);
}

/// @addr{0x805799AC}
void KartMove::triggerRespawn() {
    m_timeInRespawn = 0;
    status().setBit(eStatus::TriggerRespawn);
}

/// @addr{0x80586FA8}
/// @brief Slipstream. On this player's turn (one of 12 slots, see RaceManager::isSlipstreamTurn)
/// it searches for a racer ahead: one within the 1000-unit sphere 1000 units behind that racer,
/// inside its cone of travel, both going at least half their base speed. Off-turn frames charge
/// for 90 frames while still found on the last turn, then boost for 135 frames.
/// Effects, sound, rumble and mission counters are left out.
void KartMove::calcSlipstream() {
    constexpr f32 MIN_SPEED_RATIO = 0.5f;
    constexpr f32 DRAFT_DISTANCE = 1000.0f;
    constexpr f32 KART_DRAFT_CONE = 0.982f;
    constexpr f32 BIKE_DRAFT_CONE = 0.99f;
    constexpr u32 CHARGE_FRAMES = 90;
    constexpr u32 BOOST_FRAMES = 135;
    constexpr f32 CHARGE_FACTOR_STEP = 0.005f;
    constexpr f32 MAX_CHARGE_FACTOR = 1.1f;
    constexpr f32 BOOST_FACTOR = 1.4f;

    auto &status = KartObjectProxy::status();
    u8 selfIdx = param()->playerIdx();

    if (System::RaceManager::Instance()->isSlipstreamTurn(selfIdx) &&
            status.offBit(eStatus::Ghost, eStatus::InBullet, eStatus::SlipstreamBoost)) {
        bool found = false;

        if (status.offBit(eStatus::InAction) && m_speedRatioCapped >= MIN_SPEED_RATIO) {
            const auto *kartMgr = KartObjectManager::Instance();
            for (size_t i = 0; i < kartMgr->count(); ++i) {
                const KartObject *other = kartMgr->object(i);
                if (other->param()->playerIdx() == selfIdx ||
                        other->status().onBit(eStatus::Ghost) ||
                        other->move()->speedRatioCapped() < MIN_SPEED_RATIO) {
                    continue;
                }

                const EGG::Vector3f &otherPos = other->pos();
                const EGG::Vector3f &otherDir = other->move()->dir();
                EGG::Vector3f behind = otherPos + otherDir * -DRAFT_DISTANCE;

                // 0x80571234: sphere test, its push direction and depth outputs are unused here.
                f32 radiusSum = collide()->boundingRadius() + DRAFT_DISTANCE;
                if (radiusSum * radiusSum - (pos() - behind).squaredLength() <= 0.0f) {
                    continue;
                }

                EGG::Vector3f toOther = otherPos - pos();
                f32 dist = EGG::Mathf::sqrt(toOther.squaredLength());
                f32 cone = other->isBike() ? BIKE_DRAFT_CONE : KART_DRAFT_CONE;
                if (otherDir.dot(toOther) > dist * cone) {
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            status.setBit(eStatus::SlipstreamCharge);
        } else {
            m_slipstreamTimer = 0;
            status.resetBit(eStatus::SlipstreamCharge);
            m_slipstreamFactor = 1.0f;
        }

        return;
    }

    if (status.onBit(eStatus::SlipstreamCharge)) {
        m_slipstreamFactor = std::min(m_slipstreamFactor + CHARGE_FACTOR_STEP, MAX_CHARGE_FACTOR);

        if (++m_slipstreamTimer >= CHARGE_FRAMES &&
                status.offBit(eStatus::NoKartCollision72, eStatus::NoKartCollision82,
                        eStatus::Vanished, eStatus::InBullet, eStatus::BeforeRespawn,
                        eStatus::NoSlipstreamBoost135, eStatus::InAction)) {
            status.resetBit(eStatus::SlipstreamCharge).setBit(eStatus::SlipstreamBoost);
            m_slipstreamTimer = 0;
            m_slipstreamFactor = BOOST_FACTOR;
        }

        return;
    }

    if ((status.onBit(eStatus::SlipstreamBoost) && ++m_slipstreamTimer > BOOST_FRAMES) ||
            m_speedRatioCapped < MIN_SPEED_RATIO || status.onBit(eStatus::InAction)) {
        m_slipstreamTimer = 0;
        m_slipstreamFactor = 1.0f;
        status.resetBit(eStatus::SlipstreamBoost);
    }
}

/// @addr{0x80582488}
void KartMove::clearSlipstream() {
    m_slipstreamTimer = 0;
    m_slipstreamFactor = 1.0f;
    status().resetBit(eStatus::SlipstreamBoost, eStatus::SlipstreamCharge);
}

/// @addr{0x80579F6C}
/// @brief Called by kart-kart collision on a bike that was in a wheelie: starts a short wobble.
/// The game also tells this player's item holder which kart hit it; items are not in Kinoko yet.
void KartMove::startWheelieBumpWobble() {
    status().setBit(eStatus::WheelieBumpWobble);
    m_wheelieBumpWobbleTimer = 0;
    m_wheelieBumpWobbleAngle = 0.0f;
    Item::ItemDirector::Instance()->kartItem(param()->playerIdx()).dropTrail(true);
}

/// @addr{0x80579FE4}
/// @brief For 25 frames after a wheelie bump, while on the ground and not in an action, rocks the
/// bike's yaw rate with a sine scaled by speed.
void KartMove::calcWheelieBumpWobble() {
    constexpr s16 WOBBLE_DURATION = 25;
    constexpr f32 FULL_WOBBLE_SPEED_RATIO = 0.4f;
    constexpr u16 PHASE_STEP = 5000;
    constexpr f32 PHASE_TO_FIDX = 1.0f / 256.0f;
    constexpr f32 WOBBLE_AMPLITUDE = 0.1f;

    auto &status = KartObjectProxy::status();
    if (status.offBit(eStatus::WheelieBumpWobble)) {
        return;
    }

    if (++m_wheelieBumpWobbleTimer > WOBBLE_DURATION) {
        status.resetBit(eStatus::WheelieBumpWobble);
        return;
    }

    if (status.offBit(eStatus::TouchingGround) || status.onBit(eStatus::InAction)) {
        return;
    }

    f32 speedFactor = std::min(m_speedRatioCapped / FULL_WOBBLE_SPEED_RATIO, 1.0f);
    m_wheelieBumpWobblePhase = static_cast<u16>(m_wheelieBumpWobblePhase + PHASE_STEP);
    f32 sin = EGG::Mathf::SinFIdx(PHASE_TO_FIDX * static_cast<f32>(m_wheelieBumpWobblePhase));
    m_wheelieBumpWobbleAngle = speedFactor * (WOBBLE_AMPLITUDE * sin);

    EGG::Vector3f angVel2 = dynamics()->angVel2();
    angVel2 += EGG::Vector3f(0.0f, m_wheelieBumpWobbleAngle, 0.0f);
    dynamics()->setAngVel2(angVel2);
}

/// @addr{0x80587B30}
KartMoveBike::KartMoveBike() : m_leanRot(0.0f) {}

/// @addr{0x80589704}
KartMoveBike::~KartMoveBike() = default;

/// @brief STAGE 1+ - Sets the wheelie bit flag and some wheelie-related variables.
/// @addr{0x80588350}
void KartMoveBike::startWheelie() {
    constexpr f32 MAX_WHEELIE_ROTATION = 0.07f;
    constexpr u16 WHEELIE_COOLDOWN = 20;

    status().setBit(eStatus::Wheelie);
    m_wheelieFrames = 0;
    m_maxWheelieRot = MAX_WHEELIE_ROTATION;
    m_wheelieCooldown = WHEELIE_COOLDOWN;
    m_wheelieRotDec = 0.0f;
    m_autoHardStickXFrames = 0;
}

/// @addr{0x805883C4}
/// @stage 1+
/// @brief Clears the wheelie bit flag and resets the rotation decrement.
void KartMoveBike::cancelWheelie() {
    status().resetBit(eStatus::Wheelie);
    m_wheelieRotDec = 0.0f;
    m_autoHardStickXFrames = 0;
}

/// @addr{0x80587BB8}
void KartMoveBike::createSubsystems(const KartParam::Stats &stats) {
    m_jump = EGG::egg_new<KartJumpBike>(this);
    m_halfPipe = EGG::egg_new<KartHalfPipe>();
    m_kartScale = EGG::egg_new<KartScale>(stats);
}

/// @stage All
/// @brief Every frame, calculates rotation, EV, and angular velocity for the bike.
/// @addr{0x80587D68}
void KartMoveBike::calcVehicleRotation(f32 turn) {
    f32 leanRotInc = m_turningParams->leanRotIncRace;
    f32 leanRotCap = m_turningParams->leanRotCapRace;
    const auto *raceManager = System::RaceManager::Instance();

    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::ChargingSSMT)) {
        if (!raceManager->isStageReached(System::RaceManager::Stage::Race) ||
                EGG::Mathf::abs(m_speed) < 5.0f) {
            leanRotInc = m_turningParams->leanRotIncCountdown;
            leanRotCap = m_turningParams->leanRotCapCountdown;
        }
    } else {
        leanRotInc = m_turningParams->leanRotIncSSMT;
        leanRotCap = m_turningParams->leanRotCapSSMT;
    }

    m_leanRotCap += 0.3f * (leanRotCap - m_leanRotCap);
    m_leanRotInc += 0.3f * (leanRotInc - m_leanRotInc);

    f32 stickX = state()->stickX();
    f32 extVelXFactor = 0.0f;
    f32 leanRotMin = -m_leanRotCap;
    f32 leanRotMax = m_leanRotCap;

    if (status.onBit(eStatus::BeforeRespawn, eStatus::InAction, eStatus::Wheelie,
                eStatus::OverZipper, eStatus::RejectRoadTrigger, eStatus::AirtimeOver20,
                eStatus::SoftWallUnlockRotation, eStatus::SoftWallPush, eStatus::HWG,
                eStatus::CannonStart, eStatus::InCannon)) {
        m_leanRot *= m_turningParams->leanRotDecayFactor;
    } else if (!state()->isDrifting()) {
        if (stickX <= 0.2f) {
            if (stickX >= -0.2f) {
                m_leanRot *= m_turningParams->leanRotDecayFactor;
            } else {
                m_leanRot -= m_leanRotInc;
                extVelXFactor = m_turningParams->leanRotShallowFactor;
            }
        } else {
            m_leanRot += m_leanRotInc;
            extVelXFactor = -m_turningParams->leanRotShallowFactor;
        }
    } else {
        leanRotMax = m_turningParams->leanRotMaxDrift;
        leanRotMin = m_turningParams->leanRotMinDrift;

        if (m_hopStickX == 1) {
            leanRotMin = -leanRotMax;
            leanRotMax = -m_turningParams->leanRotMinDrift;
        }
        if (m_hopStickX == -1) {
            if (stickX == 0.0f) {
                m_leanRot += (0.5f - m_leanRot) * 0.05f;
            } else {
                m_leanRot += m_turningParams->driftStickXFactor * stickX;
                extVelXFactor = -m_turningParams->leanRotShallowFactor * stickX;
            }
        } else if (stickX == 0.0f) {
            m_leanRot += (-0.5f - m_leanRot) * 0.05f;
        } else {
            m_leanRot += m_turningParams->driftStickXFactor * stickX;
            extVelXFactor = -m_turningParams->leanRotShallowFactor * stickX;
        }
    }

    bool capped = false;
    if (leanRotMin <= m_leanRot) {
        if (leanRotMax < m_leanRot) {
            m_leanRot = leanRotMax;
            capped = true;
        }
    } else {
        m_leanRot = leanRotMin;
        capped = true;
    }

    if (!capped) {
        dynamics()->setExtVel(dynamics()->extVel() + componentXAxis() * extVelXFactor);
    }

    f32 leanRotScalar = state()->isDrifting() ? 0.065f : 0.05f;

    calcStandstillBoostRot();

    dynamics()->setAngVel2(dynamics()->angVel2() +
            EGG::Vector3f(m_standStillBoostRot, turn * wheelieRotFactor(),
                    m_leanRot * leanRotScalar));

    calcDive();

    EGG::Vector3f top = m_up;

    if (status.offBit(eStatus::RejectRoad, eStatus::HalfPipeRamp, eStatus::OverZipper)) {
        f32 scalar = (m_speed >= 0.0f) ? m_speedRatioCapped * 2.0f : 0.0f;
        scalar = std::min(1.0f, scalar);
        top = scalar * m_up + (1.0f - scalar) * EGG::Vector3f::ey;

        if (std::numeric_limits<f32>::epsilon() < top.squaredLength()) {
            top.normalise();
        }
    }

    dynamics()->setTop_(top);
}

/// @brief On init, sets the bike's lean rotation cap and increment.
/// @addr{0x80587C54}
/// In addition to setting the lean rotation cap and increment on init,
/// this function also gets called when falling out-of-bounds.
void KartMoveBike::setTurnParams() {
    static constexpr std::array<TurningParameters, 2> TURNING_PARAMS_ARRAY = {{
            {0.8f, 0.08f, 1.0f, 0.1f, 1.2f, 0.8f, 0.08f, 0.6f, 0.15f, 1.6f, 0.9f, 180},
            {1.0f, 0.1f, 1.0f, 0.05f, 1.5f, 0.7f, 0.08f, 0.6f, 0.15f, 1.3f, 0.9f, 180},
    }};

    KartMove::setTurnParams();

    if (param()->stats().driftType == KartParam::Stats::DriftType::Outside_Drift_Bike) {
        m_turningParams = &TURNING_PARAMS_ARRAY[0];
    } else if (param()->stats().driftType == KartParam::Stats::DriftType::Inside_Drift_Bike) {
        m_turningParams = &TURNING_PARAMS_ARRAY[1];
    }

    if (System::RaceManager::Instance()->isStageReached(System::RaceManager::Stage::Race)) {
        m_leanRotInc = m_turningParams->leanRotIncRace;
        m_leanRotCap = m_turningParams->leanRotCapRace;
    } else {
        m_leanRotInc = m_turningParams->leanRotIncCountdown;
        m_leanRotCap = m_turningParams->leanRotCapCountdown;
    }
}

/// @addr{0x80587D00}
void KartMoveBike::init(bool b1, bool b2) {
    KartMove::init(b1, b2);

    m_leanRot = 0.0f;
    m_leanRotCap = 0.0f;
    m_leanRotInc = 0.0f;
    m_wheelieRot = 0.0f;
    m_maxWheelieRot = 0.0f;
    m_wheelieFrames = 0;
    m_wheelieCooldown = 0;
    m_autoHardStickXFrames = 0;
}

/// @addr{0x80588950}
void KartMoveBike::clear() {
    KartMove::clear();
    cancelWheelie();
}

/// @brief STAGE 1+ - Every frame, checks player input for wheelies and computes wheelie rotation.
/// @addr{0x805883F4}
void KartMoveBike::calcWheelie() {
    constexpr u32 FAILED_WHEELIE_FRAMES = 15;
    constexpr f32 AUTO_WHEELIE_CANCEL_STICK_THRESHOLD = 0.85f;

    tryStartWheelie();
    m_wheelieCooldown = std::max(0, m_wheelieCooldown - 1);

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::Wheelie)) {
        bool cancelAutoWheelie = false;

        if (status.offBit(eStatus::AutoDrift) ||
                EGG::Mathf::abs(state()->stickX()) <= AUTO_WHEELIE_CANCEL_STICK_THRESHOLD) {
            m_autoHardStickXFrames = 0;
        } else {
            if (++m_autoHardStickXFrames > 15) {
                cancelAutoWheelie = true;
            }
        }

        ++m_wheelieFrames;
        if (m_turningParams->maxWheelieFrames < m_wheelieFrames || cancelAutoWheelie ||
                (!canWheelie() && FAILED_WHEELIE_FRAMES <= m_wheelieFrames)) {
            cancelWheelie();
        } else {
            m_wheelieRot += 0.01f;
            EGG::Vector3f angVel0 = dynamics()->angVel0();
            angVel0.x *= 0.9f;
            dynamics()->setAngVel0(angVel0);
        }
    } else if (status.onBit(eStatus::WheelieBumpWobble)) {
        // Knocked out of a wheelie by another kart: the front comes down at a fixed rate
        // (0x805885A8, checked against a Dolphin recording).
        m_wheelieRot -= 0.002f;
    } else if (0.0f < m_wheelieRot) {
        m_wheelieRotDec -= 0.001f;
        m_wheelieRotDec = std::max(-0.03f, m_wheelieRotDec);
        m_wheelieRot += m_wheelieRotDec;
    }

    m_wheelieRot = std::max(0.0f, std::min(m_wheelieRot, m_maxWheelieRot));

    f32 vel1DirUp = m_vel1Dir.dot(EGG::Vector3f::ey);

    if (m_wheelieRot > 0.0f) {
        if (vel1DirUp <= 0.5f || m_wheelieFrames < FAILED_WHEELIE_FRAMES) {
            EGG::Vector3f angVel2 = dynamics()->angVel2();
            angVel2.x -= m_wheelieRot * (1.0f - EGG::Mathf::abs(vel1DirUp));
            dynamics()->setAngVel2(angVel2);
        } else {
            cancelWheelie();
        }

        status.setBit(eStatus::WheelieRot);
    } else {
        status.resetBit(eStatus::WheelieRot);
    }
}

/// @stage 2
/// @brief Virtual function that just cancels wheelies when you hop.
/// @addr{0x80588B30}
/// @todo This function may be called without actually hopping (slipdrift), in which case we should
/// rename this function.
void KartMoveBike::onHop() {
    if (status().onBit(eStatus::AutoDrift)) {
        return;
    }

    cancelWheelie();
}

/// @stage 2
/// @brief Called when you collide with a wall. All it does for bikes is cancel wheelies.
void KartMoveBike::onWallCollision() {
    cancelWheelie();
}

/// @addr{0x80588FC0}
/// @brief Called after a kart-kart bump: bikes lose their wheelie.
void KartMoveBike::onPlayerBump() {
    cancelWheelie();
}

/// @stage 2
/// @brief Every frame during a drift, calculates MT charge based on player input.
/// @addr{0x80588888}
void KartMoveBike::calcMtCharge() {
    constexpr u16 MAX_MT_CHARGE = 270;
    constexpr u16 BASE_MT_CHARGE = 2;
    constexpr f32 BONUS_CHARGE_STICK_THRESHOLD = 0.4f;
    constexpr u16 EXTRA_MT_CHARGE = 3;

    if (m_driftState != DriftState::ChargingMt) {
        return;
    }

    m_mtCharge += BASE_MT_CHARGE;

    f32 stickX = state()->stickX();
    if (-BONUS_CHARGE_STICK_THRESHOLD <= stickX) {
        if (BONUS_CHARGE_STICK_THRESHOLD < stickX && m_hopStickX == -1) {
            m_mtCharge += EXTRA_MT_CHARGE;
        }
    } else if (m_hopStickX != -1) {
        m_mtCharge += EXTRA_MT_CHARGE;
    }

    if (m_mtCharge > MAX_MT_CHARGE) {
        m_mtCharge = MAX_MT_CHARGE;
        m_driftState = DriftState::ChargedMt;
    }
}

/// @addr{0x80588B58}
void KartMoveBike::initOob() {
    KartMove::initOob();
    cancelWheelie();
}

/// @brief STAGE 1+ - Every frame, checks player input to see if we should start or stop a wheelie.
/// @addr{0x80588798}
void KartMoveBike::tryStartWheelie() {
    constexpr s16 COOLDOWN_FRAMES = 20;
    bool dpadUp = inputs()->currentState().trickUp();
    auto &status = KartObjectProxy::status();

    if (status.offBit(eStatus::Wheelie)) {
        if (dpadUp && status.onBit(eStatus::TouchingGround)) {
            if (status.onBit(eStatus::DriftManual, eStatus::WallCollision, eStatus::Wall3Collision,
                        eStatus::Hop, eStatus::DriftAuto, eStatus::InAction)) {
                return;
            }

            if (m_wheelieCooldown > 0) {
                return;
            }

            startWheelie();
        }
    } else if (inputs()->currentState().trickDown() && m_wheelieCooldown <= 0) {
        cancelWheelie();
        m_wheelieCooldown = COOLDOWN_FRAMES;
    }
}

} // namespace Kinoko::Kart
