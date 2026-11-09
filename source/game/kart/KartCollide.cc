#include "KartCollide.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemObjBlueShell.hh"
#include "game/item/ItemObjBobOmb.hh"

#include "game/kart/KartBody.hh"
#include "game/kart/KartMove.hh"
#include "game/kart/KartPhysics.hh"
#include "game/kart/KartState.hh"

#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectCollisionKart.hh"
#include "game/field/ObjectDirector.hh"

#include "game/kart/KartObjectManager.hh"
#include "game/kart/KartParam.hh"
#include "game/system/RaceConfig.hh"

#include <egg/math/BoundBox.hh>
#include <egg/math/Math.hh>

namespace Kinoko::Kart {

/// @addr{0x8056E56C}
KartCollide::KartCollide() {
    m_boundingRadius = 100.0f;
    m_surfaceFlags.makeAllZero();
}

/// @addr{0x80573FF0}
KartCollide::~KartCollide() = default;

/// @addr{0x8056E624}
void KartCollide::init() {
    m_pullPath.init();
    calcBoundingRadius();
    m_floorMomentRate = 0.8f;
    m_surfaceFlags.makeAllZero();
    m_respawnTimer = 0;
    m_solidOobTimer = 0;
    m_shrinkTimer = 0;
    m_playerBumpTimer = 0;
    m_smoothedBack = 0.0f;
    m_sumHitboxBottomHeightFloorOnly = 0.0f;
    m_sumHitboxBottomHeightSoftWall = 0.0f;
    m_numFloorOnlyCollisions = 0;
    m_numSoftWallCollisions = 0;
    m_poleAngVelTimer = 0;
    m_poleYaw = 0.0f;
    m_colPerpendicularity = 0.0f;
}

/// @addr{0x805730D4}
void KartCollide::resetHitboxes() {
    CollisionGroup *hitboxGroup = physics()->hitboxGroup();
    for (u16 idx = 0; idx < hitboxGroup->hitboxCount(); ++idx) {
        hitboxGroup->hitbox(idx).setLastPos(scale(), pose());
    }
}

/// @stage All
/// @brief On each frame, calculates the positions for each hitbox
/// @addr{0x8056EE24}
void KartCollide::calcHitboxes() {
    CollisionGroup *hitboxGroup = physics()->hitboxGroup();
    for (u16 idx = 0; idx < hitboxGroup->hitboxCount(); ++idx) {
        hitboxGroup->hitbox(idx).calc(move()->totalScale(), body()->sinkDepth(), scale(), fullRot(),
                pos());
    }
}

/// @stage All
/// @addr{0x80572C20}
void KartCollide::findCollision() {
    bool wasHalfPipe = status().onBit(eStatus::EndHalfPipe, eStatus::ActionMidZipper);
    const EGG::Quatf &rot = wasHalfPipe ? mainRot() : fullRot();
    calcBodyCollision(move()->totalScale(), body()->sinkDepth(), rot, scale());

    auto &colData = collisionData();
    bool existingWallCollision = colData.bWall || colData.bWall3;
    bool newWallCollision =
            m_surfaceFlags.onBit(eSurfaceFlags::ObjectWall, eSurfaceFlags::ObjectWall3);
    if (existingWallCollision || newWallCollision) {
        if (!existingWallCollision) {
            colData.wallNrm = m_totalReactionWallNrm;
            if (m_surfaceFlags.onBit(eSurfaceFlags::ObjectWall)) {
                colData.bWall = true;
            } else if (m_surfaceFlags.onBit(eSurfaceFlags::ObjectWall3)) {
                colData.bWall3 = true;
            }
        } else if (newWallCollision) {
            colData.wallNrm += m_totalReactionWallNrm;
            if (m_surfaceFlags.onBit(eSurfaceFlags::ObjectWall)) {
                colData.bWall = true;
            } else if (m_surfaceFlags.onBit(eSurfaceFlags::ObjectWall3)) {
                colData.bWall3 = true;
            }
        }

        colData.wallNrm.normalise();
    }

    FUN_80572F4C();
}

/// @stage 2
/// @addr{0x80572F4C}
/// @rename
void KartCollide::FUN_80572F4C() {
    f32 fVar1;

    auto &status = KartObjectProxy::status();

    if (isInRespawn() ||
            status.onBit(eStatus::Boost, eStatus::OverZipper, eStatus::ZipperInvisibleWall,
                    eStatus::NoSparkInvisibleWall, eStatus::HalfPipeRamp)) {
        fVar1 = 0.0f;
    } else {
        fVar1 = 0.05f;
    }

    bool resetXZ = fVar1 > 0.0f && status.onBit(eStatus::AirtimeOver20) &&
            dynamics()->velocity().y < -50.0f;

    FUN_805B72B8(status.onBit(eStatus::InAction) ? 0.3f : 0.01f, fVar1, resetXZ,
            status.offBit(eStatus::JumpPadDisableYsusForce));
}

/// @stage All
/// @brief Affects velocity when landing from airtime.
/// @addr{0x805B72B8}
/// @details Every frame, this function checks the player's velocity relative to the floor collision
/// and applies external velocity and angular velocity.
/// This is mostly relevant when you were midair and just landed.
/// @rename
void KartCollide::FUN_805B72B8(f32 param_1, f32 param_2, bool lockXZ, bool addExtVelY) {
    const auto &colData = collisionData();

    if (!colData.bFloor && !colData.bWall && !colData.bWall3) {
        return;
    }

    EGG::Vector3f collisionDir = colData.floorNrm + colData.wallNrm;
    collisionDir.normalise();

    f32 directionalVelocity = colData.vel.dot(collisionDir);
    if (directionalVelocity >= 0.0f) {
        return;
    }

    EGG::Matrix34f rotMat;
    EGG::Matrix34f rotMatTrans;

    rotMat.makeQ(dynamics()->mainRot());
    rotMatTrans = rotMat.transpose();
    rotMat = rotMat.multiplyTo(dynamics()->invInertiaTensor()).multiplyTo(rotMatTrans);

    EGG::Vector3f relPos = colData.relPos;
    if (lockXZ) {
        relPos.x = 0.0f;
        relPos.z = 0.0f;
    }

    EGG::Vector3f step1 = relPos.cross(collisionDir);
    EGG::Vector3f step2 = rotMat.multVector33(step1);
    EGG::Vector3f step3 = step2.cross(relPos);
    f32 val = (-directionalVelocity * (1.0f + param_2)) / (1.0f + collisionDir.dot(step3));
    EGG::Vector3f step4 = collisionDir.cross(-colData.vel);
    EGG::Vector3f step5 = step4.cross(collisionDir);
    step5.normalise();

    f32 fVar1 = param_1 * EGG::Mathf::abs(val);
    f32 otherVal = (val * colData.vel.dot(step5)) / directionalVelocity;

    f32 fVar3 = otherVal;
    if (fVar1 < EGG::Mathf::abs(otherVal)) {
        fVar3 = fVar1;
        if (otherVal < 0.0f) {
            fVar3 = -param_1 * EGG::Mathf::abs(val);
        }
    }

    EGG::Vector3f step6 = val * collisionDir + fVar3 * step5;

    f32 local_1d0 = step6.y;
    if (!addExtVelY) {
        local_1d0 = 0.0f;
    } else if (colData.bFloor) {
        f32 velY = intVel().y;
        if (velY > 0.0f) {
            velY += extVel().y;
            if (velY < 0.0f) {
                EGG::Vector3f newExtVel = extVel();
                newExtVel.y = velY;
                dynamics()->setExtVel(newExtVel);
            }
        }
    }

    f32 prevExtVelY = extVel().y;
    EGG::Vector3f extVelAdd = step6;
    extVelAdd.y = local_1d0;
    dynamics()->setExtVel(extVel() + extVelAdd);

    if (prevExtVelY < 0.0f && extVel().y > 0.0f && extVel().y < 10.0f) {
        EGG::Vector3f extVelNoY = extVel();
        extVelNoY.y = 0.0f;
        dynamics()->setExtVel(extVelNoY);
    }

    EGG::Vector3f step7 = relPos.cross(step6);
    EGG::Vector3f step8 = rotMat.multVector33(step7);
    EGG::Vector3f step9 = mainRot().rotateVectorInv(step8);
    step9.y = 0.0f;
    dynamics()->setAngVel0(dynamics()->angVel0() + step9);
}

/// @stage All
/// @addr{0x805B6724}
/// @brief Checks and acts on collision for each kart hitbox.
/// @param totalScale
/// @param rot
/// @param scale
void KartCollide::calcBodyCollision(f32 totalScale, f32 sinkDepth, const EGG::Quatf &rot,
        const EGG::Vector3f &scale) {
    CollisionGroup *hitboxGroup = physics()->hitboxGroup();
    CollisionData &collisionData = hitboxGroup->collisionData();
    collisionData.reset();

    EGG::Vector3f posRel = EGG::Vector3f::zero;
    s32 count = 0;
    Field::CollisionInfo colInfo;
    colInfo.bbox.setDirect(EGG::Vector3f::zero, EGG::Vector3f::zero);
    Field::KCLTypeMask maskOut;
    Field::CourseColMgr::NoBounceWallColInfo noBounceWallInfo;
    EGG::BoundBox3f minMax;
    minMax.setZero();
    bool bVar1 = false;

    for (u16 hitboxIdx = 0; hitboxIdx < hitboxGroup->hitboxCount(); ++hitboxIdx) {
        Field::KCLTypeMask flags = KCL_TYPE_DRIVER_SOLID_SURFACE;
        Hitbox &hitbox = hitboxGroup->hitbox(hitboxIdx);

        if (hitbox.bspHitbox()->wallsOnly != 0) {
            flags = 0x4A109000;
            Field::CourseColMgr::Instance()->setNoBounceWallInfo(&noBounceWallInfo);
        }

        hitbox.calc(totalScale, sinkDepth, scale, rot, pos());

        if (Field::CollisionDirector::Instance()->checkSphereCachedFullPush(hitbox.radius(),
                    hitbox.worldPos(), hitbox.lastPos(), flags, &colInfo, &maskOut, 0)) {
            if (!!(maskOut & KCL_TYPE_VEHICLE_COLLIDEABLE)) {
                Field::CollisionDirector::Instance()->findClosestCollisionEntry(&maskOut,
                        KCL_TYPE_VEHICLE_COLLIDEABLE);
            }

            if (!FUN_805B6A9C(collisionData, hitbox, minMax, posRel, count, maskOut, colInfo)) {
                bVar1 = true;

                if (colInfo.movingFloorDist > -std::numeric_limits<f32>::min()) {
                    collisionData.bHasRoadVel = true;
                    collisionData.roadVelocity = colInfo.roadVelocity;
                }

                processBody(collisionData, hitbox, &colInfo, &maskOut);
            }
        }
    }

    if (bVar1) {
        EGG::Vector3f movement = minMax.min + minMax.max;
        applyBodyCollision(collisionData, movement, posRel, count);
    } else {
        collisionData.speedFactor = 1.0f;
        collisionData.rotFactor = 1.0f;
    }
}

/// @addr{0x80571634}
void KartCollide::calcFloorEffect() {
    if (status().onBit(eStatus::TouchingGround)) {
        m_surfaceFlags.setBit(eSurfaceFlags::Offroad, eSurfaceFlags::GroundBoostPanelOrRamp);
    }

    m_sumHitboxBottomHeightFloorOnly = 0.0f;
    m_surfaceFlags.resetBit(eSurfaceFlags::Wall, eSurfaceFlags::SolidOOB, eSurfaceFlags::BoostRamp,
            eSurfaceFlags::Offroad, eSurfaceFlags::Trickable, eSurfaceFlags::NotTrickable,
            eSurfaceFlags::MovingRoad, eSurfaceFlags::StopHalfPipeState);
    m_sumHitboxBottomHeightSoftWall = 0.0f;
    m_numFloorOnlyCollisions = 0;
    m_numSoftWallCollisions = 0;

    Field::KCLTypeMask mask = KCL_NONE;
    calcTriggers(&mask, pos(), false);

    auto *colDir = Field::CollisionDirector::Instance();

    if (m_solidOobTimer >= 3 && m_surfaceFlags.onBit(eSurfaceFlags::SolidOOB) &&
            m_surfaceFlags.offBit(eSurfaceFlags::Wall)) {
        if (mask & KCL_TYPE_BIT(COL_TYPE_SOLID_OOB)) {
            colDir->findClosestCollisionEntry(&mask, KCL_TYPE_BIT(COL_TYPE_SOLID_OOB));
        }

        activateOob(true, &mask, false, false);
    }

    mask = KCL_NONE;
    calcTriggers(&mask, pos(), true);

    m_solidOobTimer =
            m_surfaceFlags.onBit(eSurfaceFlags::SolidOOB) ? std::min(3, m_solidOobTimer + 1) : 0;

    if (status().onBit(eStatus::Wall3Collision, eStatus::WallCollision)) {
        Field::KCLTypeMask maskOut = KCL_NONE;

        if (colDir->checkSphereCachedPartialPush(m_boundingRadius, pos(), EGG::Vector3f::inf,
                    KCL_TYPE_BIT(COL_TYPE_FALL_BOUNDARY), nullptr, &maskOut, 0)) {
            calcFallBoundary(&maskOut, true);
        }
    }
}

/// @addr{0x805718D4}
void KartCollide::calcTriggers(Field::KCLTypeMask *mask, const EGG::Vector3f &pos, bool twoPoint) {
    EGG::Vector3f v1 = twoPoint ? physics()->pos() : EGG::Vector3f::inf;
    Field::KCLTypeMask typeMask = twoPoint ? KCL_TYPE_DIRECTIONAL : KCL_TYPE_NON_DIRECTIONAL;
    f32 radius = twoPoint ? 80.0f : 100.0f * move()->totalScale();
    f32 scalar = -bsp().initialYPos * move()->totalScale() * 0.3f;
    EGG::Vector3f scaledPos = pos + scalar * componentYAxis();
    EGG::Vector3f back = dynamics()->mainRot().rotateVector(EGG::Vector3f::ez);

    m_smoothedBack += (back.dot(move()->smoothedUp()) - m_smoothedBack) * 0.3f;

    scalar = m_smoothedBack * -physics()->fc() * 1.8f * move()->totalScale();
    scaledPos += scalar * back;

    bool collide = Field::CollisionDirector::Instance()->checkSphereCachedPartialPush(radius,
            scaledPos, v1, typeMask, nullptr, mask, 0);

    if (!collide) {
        return;
    }

    if (twoPoint) {
        handleTriggers(mask);
    } else {
        if (*mask & KCL_TYPE_FLOOR) {
            Field::CollisionDirector::Instance()->findClosestCollisionEntry(mask, KCL_TYPE_FLOOR);
        }

        if (*mask & KCL_TYPE_WALL) {
            m_surfaceFlags.setBit(eSurfaceFlags::Wall);
        }

        if (*mask & KCL_TYPE_BIT(COL_TYPE_SOLID_OOB)) {
            m_surfaceFlags.setBit(eSurfaceFlags::SolidOOB);
        }
    }
}

/// @addr{0x8056F510}
void KartCollide::handleTriggers(Field::KCLTypeMask *mask) {
    calcFallBoundary(mask, false);
    processCannon(mask);

    if (*mask & KCL_TYPE_BIT(COL_TYPE_EFFECT_TRIGGER)) {
        auto *colDir = Field::CollisionDirector::Instance();
        if (colDir->findClosestCollisionEntry(mask, KCL_TYPE_BIT(COL_TYPE_EFFECT_TRIGGER))) {
            if (colDir->closestCollisionEntry()->variant() == 4) {
                halfPipe()->end(true);
                status().setBit(eStatus::EndHalfPipe);
                m_surfaceFlags.setBit(eSurfaceFlags::StopHalfPipeState);
            }
        }
    }
}

/// @addr{0x80571D98}
void KartCollide::calcFallBoundary(Field::KCLTypeMask *mask, bool shortBoundary) {
    if (!(*mask & KCL_TYPE_BIT(COL_TYPE_FALL_BOUNDARY))) {
        return;
    }

    auto *colDir = Field::CollisionDirector::Instance();
    if (!colDir->findClosestCollisionEntry(mask, KCL_TYPE_BIT(COL_TYPE_FALL_BOUNDARY))) {
        return;
    }

    bool safe = false;
    const auto *entry = colDir->closestCollisionEntry();

    if (shortBoundary) {
        if (entry->variant() != 7) {
            safe = true;
        }
    }

    if (!safe) {
        activateOob(false, mask, false, false);
    }
}

/// @addr{0x80573ED4}
void KartCollide::calcBeforeRespawn() {
    if (pos().y < 0.0f) {
        activateOob(true, nullptr, false, false);
    }

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn)) {
        if (--m_respawnTimer > 0) {
            return;
        }

        status.resetBit(eStatus::BeforeRespawn);
        m_respawnTimer = 0;
        move()->triggerRespawn();
    }

    m_shrinkTimer = std::max(0, m_shrinkTimer - 1);
}

/// @addr{0x80573B00}
void KartCollide::activateOob(bool /*detachCamera*/, Field::KCLTypeMask * /*mask*/,
        bool /*somethingCPU*/, bool /*somethingBullet*/) {
    constexpr s16 RESPAWN_TIME = 130;

    auto &status = KartObjectProxy::status();

    if (status.onBit(eStatus::BeforeRespawn)) {
        return;
    }

    move()->initOob();

    m_respawnTimer = RESPAWN_TIME;
    status.setBit(eStatus::BeforeRespawn);
}

/// @stage All
/// @brief Checks wheel hitbox collision and stores position/velocity info.
/// @addr{0x805B6F4C}
/// @param hitboxGroup The wheel's collision information
/// @param colVel The wheel's velocity. In the base game, it is always \f$\vec{v} = \begin{bmatrix}
/// 0 \\ -13 \\ 0 \end{bmatrix}\f$
/// @param center The wheel's position
/// @param radius The wheel's size
void KartCollide::calcWheelCollision(u16 /*wheelIdx*/, CollisionGroup *hitboxGroup,
        const EGG::Vector3f &colVel, const EGG::Vector3f &center, f32 radius) {
    Hitbox &firstHitbox = hitboxGroup->hitbox(0);
    BSP::Hitbox *bspHitbox = const_cast<BSP::Hitbox *>(firstHitbox.bspHitbox());
    bspHitbox->radius = radius;
    hitboxGroup->resetCollision();
    firstHitbox.setWorldPos(center);

    Field::CollisionInfo colInfo;
    colInfo.bbox.setZero();
    Field::KCLTypeMask kclOut;
    Field::CourseColMgr::NoBounceWallColInfo noBounceWallInfo;
    Field::CourseColMgr::Instance()->setNoBounceWallInfo(&noBounceWallInfo);

    bool collided = Field::CollisionDirector::Instance()->checkSphereCachedFullPush(
            firstHitbox.radius(), firstHitbox.worldPos(), firstHitbox.lastPos(),
            KCL_TYPE_VEHICLE_COLLIDEABLE, &colInfo, &kclOut, 0);

    CollisionData &collisionData = hitboxGroup->collisionData();

    if (!collided) {
        collisionData.speedFactor = 1.0f;
        collisionData.rotFactor = 1.0f;
        return;
    }

    collisionData.tangentOff = colInfo.tangentOff;

    if (noBounceWallInfo.dist > std::numeric_limits<f32>::min()) {
        collisionData.tangentOff += noBounceWallInfo.tangentOff;
        collisionData.noBounceWallNrm = noBounceWallInfo.fnrm;
        collisionData.bSoftWall = true;
    }

    if (kclOut & KCL_TYPE_FLOOR) {
        collisionData.bFloor = true;
        collisionData.floorNrm = colInfo.floorNrm;
    }

    collisionData.relPos = firstHitbox.worldPos() - pos();
    collisionData.vel = colVel;

    if (colInfo.movingFloorDist > -std::numeric_limits<f32>::min()) {
        collisionData.bHasRoadVel = true;
        collisionData.roadVelocity = colInfo.roadVelocity;
    }

    processWheel(collisionData, firstHitbox, &colInfo, &kclOut);

    if (!(kclOut & KCL_TYPE_VEHICLE_COLLIDEABLE)) {
        return;
    }

    Field::CollisionDirector::Instance()->findClosestCollisionEntry(&kclOut,
            KCL_TYPE_VEHICLE_COLLIDEABLE);
}

/// @stage 2
/// @addr{0x8056F26C}
void KartCollide::calcSideCollision(CollisionData &collisionData, Hitbox &hitbox,
        Field::CollisionInfo *colInfo) {
    if (colInfo->perpendicularity <= 0.0f) {
        return;
    }

    m_colPerpendicularity = std::max(m_colPerpendicularity, colInfo->perpendicularity);

    if (collisionData.bWallAtLeftCloser || collisionData.bWallAtRightCloser) {
        return;
    }

    f32 bspPosX = hitbox.bspHitbox()->position.x;
    if (EGG::Mathf::abs(bspPosX) > 10.0f) {
        if (bspPosX > 0.0f) {
            collisionData.bWallAtLeftCloser = true;
        } else {
            collisionData.bWallAtRightCloser = true;
        }

        collisionData.colPerpendicularity = colInfo->perpendicularity;

        return;
    }

    EGG::Vector3f right = dynamics()->mainRot().rotateVector(EGG::Vector3f::ex);
    std::array<f32, 2> tangents = {0.0f, 0.0f};

    // The loop is just to do left/right wall
    for (size_t i = 0; i < tangents.size(); ++i) {
        f32 sign = i == 1 ? -1.0f : 1.0f;
        f32 effectiveRadius = sign * hitbox.radius();
        EGG::Vector3f effectivePos = hitbox.worldPos() + effectiveRadius * right;
        Field::CollisionInfoPartial tempColInfo;

        if (Field::CollisionDirector::Instance()->checkSphereCachedPartial(hitbox.radius(),
                    effectivePos, hitbox.lastPos(), KCL_TYPE_DRIVER_WALL, &tempColInfo, nullptr,
                    0)) {
            tangents[i] = colInfo->tangentOff.squaredLength();
        }
    }

    if (tangents[0] > tangents[1]) {
        collisionData.bWallAtLeftCloser = true;
        collisionData.colPerpendicularity = colInfo->perpendicularity;
    } else if (tangents[1] > tangents[0]) {
        collisionData.bWallAtRightCloser = true;
        collisionData.colPerpendicularity = colInfo->perpendicularity;
    }
}

/// @addr{0x8056E70C}
void KartCollide::calcBoundingRadius() {
    m_boundingRadius = collisionGroup()->boundingRadius() * move()->hitboxScale();
}

/// @addr{0x8057257C}
/// @brief Items touching the kart start their hit action (shell, fake item box: action 2, banana:
/// action 0; none in a mega mushroom; a mushroom or star is used). Sounds, hit statistics, battle
/// scoring, team checks and the online remote case (status bit 131) are left out.
void KartCollide::calcItemCollision() {
    std::array<Item::ItemObj *, Item::ItemDirector::MAX_KART_HITS> hits;
    const auto *kart = Kart::KartObjectManager::Instance()->object(param()->playerIdx());
    size_t count = Item::ItemDirector::Instance()->checkKartCollision(kart, hits);

    auto &status = KartObjectProxy::status();
    for (size_t i = 0; i < count; ++i) {
        Action newAction = Action::None;

        // The handler table 0x808B5468, by object kind.
        switch (hits[i]->kind()) {
        case Item::ItemObjKind::GreenShell:
        case Item::ItemObjKind::RedShell:
            // 0x80573190
            newAction = status.onBit(eStatus::InMega) ? Action::None : Action::UNK_2;
            break;
        case Item::ItemObjKind::Banana:
            // 0x80573178
            newAction = status.onBit(eStatus::InMega) ? Action::None : Action::UNK_0;
            break;
        case Item::ItemObjKind::Mushroom:
            // 0x805731E0
            Item::ItemDirector::Instance()->kartItem(param()->playerIdx()).activateMushroom();
            break;
        case Item::ItemObjKind::Star:
            // 0x80573224
            move()->activateStar();
            break;
        case Item::ItemObjKind::BlueShell:
            // 0x805731B0
            newAction = static_cast<Item::ItemObjBlueShell *>(hits[i])->explosionWeakened() ?
                    Action::UNK_0 :
                    Action::UNK_7;
            break;
        case Item::ItemObjKind::BobOmb:
            // 0x805731C8
            newAction = static_cast<Item::ItemObjBobOmb *>(hits[i])->explosionWeakened() ?
                    Action::UNK_0 :
                    Action::UNK_7;
            break;
        case Item::ItemObjKind::FakeItemBox:
            // 0x8057325C: the battle team check is left out.
            newAction = status.onBit(eStatus::InMega, eStatus::InStar, eStatus::InBullet) ?
                    Action::None :
                    Action::UNK_2;
            break;
        default:
            break;
        }

        if (newAction != Action::None) {
            static_cast<void>(action()->start(newAction));
        }
    }
}

/// @addr{0x80571F10}
void KartCollide::calcObjectCollision() {
    constexpr f32 COS_PI_OVER_4 = 0.707f;
    constexpr s32 DUMMY_POLE_ANG_VEL_TIME = 3;
    constexpr f32 DUMMY_POLE_ANG_VEL = 0.005f;
    constexpr s32 SHRINK_TIME = 60;

    m_totalReactionWallNrm = EGG::Vector3f::zero;
    m_surfaceFlags.resetBit(eSurfaceFlags::ObjectWall, eSurfaceFlags::ObjectWall3);

    auto *objColKart = objectCollisionKart();
    size_t collisionCount = objColKart->checkCollision(pose(), velocity());

    const auto *objectDirector = Field::ObjectDirector::Instance();

    for (size_t i = 0; i < collisionCount; ++i) {
        Reaction reaction = objectDirector->reaction(i);
        if (reaction != Reaction::None && reaction != Reaction::UNK_7) {
            size_t handlerIdx = static_cast<std::underlying_type_t<Reaction>>(reaction);
            Action newAction = (this->*s_objectCollisionHandlers[handlerIdx])(i);

            if (reaction == Reaction::SpinShrink && (m_shrinkTimer == 0)) {
                m_shrinkTimer = SHRINK_TIME;
                move()->activateShrink();
                move()->applyForce(30.0f, objColKart->GetHitDirection(i), false);
            } else if (reaction != Reaction::SmallBump && reaction != Reaction::BigBump) {
                const EGG::Vector3f &hitDepth = objectDirector->hitDepth(i);
                m_tangentOff += hitDepth;
                m_movement += hitDepth;

                if (newAction != Action::None) {
                    action()->setHitDepth(objectDirector->hitDepth(i));
                    action()->start(newAction);
                }
            }
        }

        if (objectDirector->collidingObject(i)->id() == Field::ObjectId::DummyPole) {
            EGG::Vector3f hitDirection = objectCollisionKart()->GetHitDirection(i);
            const EGG::Vector3f &lastDir = move()->lastDir();

            if (lastDir.dot(hitDirection) < -COS_PI_OVER_4) {
                EGG::Vector3f angVel = hitDirection.cross(lastDir);
                f32 sign = angVel.y > 0.0f ? -1.0f : 1.0f;

                m_poleAngVelTimer = DUMMY_POLE_ANG_VEL_TIME;
                m_poleYaw = DUMMY_POLE_ANG_VEL * sign;
            }
        }
    }

    calcPoleTimer();
}

/// @addr{0x8056F7F0}
/// @brief Kart-to-kart collision with one other racer found by the box search in
/// KartSub::calcPass1. Ported from the game; see docs/kart_collision_decomp.md.
/// @details Each pair is handled once per frame, by whichever of the two karts gets there first:
/// both karts are pushed apart by half the overlap through their tangent offset and movement,
/// then (unless a star, mega or bullet decides the outcome) each one gets applyPlayerBump.
/// Effects, sounds, rumble, CPU AI notifications, item hooks and battle scoring are left out:
/// they do not touch kart physics.
void KartCollide::calcPlayerCollision(KartObject *other) {
    constexpr f32 HALF_OVERLAP = -0.5f;
    constexpr Action ACTION_HIT_BY_STAR = Action::UNK_3;
    constexpr Action ACTION_HIT_BY_BULLET = Action::UNK_6;
    constexpr Action ACTION_HIT_BY_MEGA = Action::MegaSquash;

    KartObjectProxy *self = this;

    if (IsPlayerCollisionDisabled(self) || IsPlayerCollisionDisabled(other)) {
        return;
    }

    u8 selfIdx = param()->playerIdx();
    u8 otherIdx = other->param()->playerIdx();
    if (state()->kartPairMask() & (1 << otherIdx)) {
        return;
    }

    if (selfIdx == otherIdx) {
        return;
    }

    state()->setKartPairMask(state()->kartPairMask() | static_cast<u16>(1 << otherIdx));
    other->state()->setKartPairMask(
            other->state()->kartPairMask() | static_cast<u16>(1 << selfIdx));

    EGG::BoundBox3f pushBox;
    EGG::BoundBox3f contactBox;
    pushBox.setZero();
    contactBox.setZero();

    if (!checkPlayerHitboxOverlap(pushBox, contactBox, other)) {
        return;
    }

    EGG::Vector3f pushDir = pushBox.min + pushBox.max;
    f32 overlap = pushDir.normalise();

    EGG::Vector3f offset = pushDir * (HALF_OVERLAP * overlap);
    m_tangentOff += offset;
    m_movement += offset;

    KartCollide *otherCollide = other->collide();
    otherCollide->m_tangentOff += -offset;
    otherCollide->m_movement += -offset;

    // The game also checks RaceConfig's team flag here: karts of the same team always just bump.
    // Kinoko has no teams, so that is always false.
    const auto &selfStatus = status();
    const auto &otherStatus = other->status();
    bool selfStar = selfStatus.onBit(eStatus::InStar);
    bool otherStar = otherStatus.onBit(eStatus::InStar);
    bool selfMega = selfStatus.onBit(eStatus::InMega);
    bool otherMega = otherStatus.onBit(eStatus::InMega);
    bool selfCrushed = selfStatus.onBit(eStatus::Crushed);
    bool otherCrushed = otherStatus.onBit(eStatus::Crushed);
    bool selfBullet = selfStatus.onBit(eStatus::InBullet);
    bool otherBullet = otherStatus.onBit(eStatus::InBullet);

    bool bump = false;
    if (selfBullet != otherBullet) {
        if (otherBullet && !selfStar) {
            action()->setHitDepth(-pushDir);
            action()->start(ACTION_HIT_BY_BULLET, true);
        } else if (selfBullet && !otherStar) {
            other->action()->setHitDepth(pushDir);
            other->action()->start(ACTION_HIT_BY_BULLET, true);
        } else {
            bump = true;
        }
    } else if (selfStar != otherStar) {
        if (otherStar && !selfBullet) {
            action()->setHitDepth(-pushDir);
            action()->start(ACTION_HIT_BY_STAR, true);
        } else if (selfStar && !otherBullet) {
            other->action()->setHitDepth(pushDir);
            other->action()->start(ACTION_HIT_BY_STAR, true);
        } else {
            bump = true;
        }
    } else if ((selfMega && !otherMega && !otherCrushed) ||
            (otherMega && !selfMega && !selfCrushed)) {
        if (otherMega) {
            action()->start(ACTION_HIT_BY_MEGA);
        } else {
            other->action()->start(ACTION_HIT_BY_MEGA);
        }
    } else {
        bump = true;
    }

    if (!bump) {
        return;
    }

    // Each kart's sideways axis: its body's x axis, made perpendicular to its travel direction.
    EGG::Vector3f otherSide = other->bodyForward().perpInPlane(other->move()->dir(), true);
    EGG::Vector3f otherHitDir = pushDir.proj(otherSide);
    otherHitDir.normalise();

    EGG::Vector3f selfSide = bodyForward().perpInPlane(move()->dir(), true);
    EGG::Vector3f selfHitDir = otherHitDir.proj(selfSide);
    selfHitDir.normalise();

    if (otherStatus.offBit(eStatus::InBullet)) {
        applyPlayerBump(1.0f, self, other, otherHitDir, pushDir);
    }

    if (selfStatus.offBit(eStatus::InBullet)) {
        applyPlayerBump(-1.0f, other, self, selfHitDir, pushDir);
    }

    if (isBike() && selfStatus.onBit(eStatus::Wheelie)) {
        move()->startWheelieBumpWobble();
    }

    if (other->isBike() && otherStatus.onBit(eStatus::Wheelie)) {
        other->move()->startWheelieBumpWobble();
    }

    move()->onPlayerBump();
    other->move()->onPlayerBump();
}

/// @addr{Inlined in 0x8056F7F0}
/// @brief While any of these is set, a kart takes part in no kart-kart collision.
bool KartCollide::IsPlayerCollisionDisabled(const KartObjectProxy *kart) {
    const auto &status = kart->status();
    if (status.onBit(eStatus::TriggerRespawn, eStatus::NoKartCollision40, eStatus::InRespawn,
                eStatus::AfterRespawn, eStatus::NoKartCollision82, eStatus::Vanished,
                eStatus::NoKartCollision89, eStatus::Ghost, eStatus::CannonStart, eStatus::InCannon,
                eStatus::NoKartCollision72, eStatus::AfterCannon)) {
        return true;
    }

    return status.onBit(eStatus::Crushed) && status.onBit(eStatus::InCrushAction);
}

/// @addr{0x8056EEF4}
/// @brief Tests every pair of body hitboxes of the two karts. For each overlapping pair, grows
/// pushBox by (unit vector towards the other kart's sphere * overlap depth) and contactBox by the
/// point on our sphere facing it. Both boxes start at zero.
/// @return Whether any pair overlaps.
bool KartCollide::checkPlayerHitboxOverlap(EGG::BoundBox3f &pushBox, EGG::BoundBox3f &contactBox,
        KartObject *other) {
    bool found = false;
    CollisionGroup *mine = collisionGroup();
    CollisionGroup *theirs = other->collisionGroup();

    for (u16 i = 0; i < mine->hitboxCount(); ++i) {
        const Hitbox &hitbox = mine->hitbox(i);

        for (u16 j = 0; j < theirs->hitboxCount(); ++j) {
            const Hitbox &otherHitbox = theirs->hitbox(j);

            EGG::Vector3f toOther = otherHitbox.worldPos() - hitbox.worldPos();
            f32 radiusSum = hitbox.radius() + otherHitbox.radius();
            f32 radiusSumSq = radiusSum * radiusSum;
            f32 distSq = toOther.squaredLength();

            if (radiusSumSq - distSq <= 0.0f) {
                continue;
            }

            f32 depth = EGG::Mathf::sqrt(radiusSumSq) - EGG::Mathf::sqrt(distSq);
            toOther.normalise();

            EGG::Vector3f contact = hitbox.worldPos() + toOther * hitbox.radius();
            contactBox.min = contactBox.min.minimize(contact);
            contactBox.max = contactBox.max.maximize(contact);

            toOther *= depth;
            pushBox.min = pushBox.min.minimize(toOther);
            pushBox.max = pushBox.max.maximize(toOther);

            found = true;
        }
    }

    return found;
}

/// @addr{0x80570DA8}
/// @brief The bump itself, applied to target: a sideways force and wrench, a speed loss, a spin
/// kick and a 10-frame bump timer during which target takes no further bump.
/// @param sign +1 when target is the other kart, -1 when it is us.
/// @param hitDir target's sideways axis, signed along the push.
/// @param pushDir Unit vector from us towards the other kart.
/// @details Note the game uses OUR weight class and bump deviation level for both targets, not
/// target's.
void KartCollide::applyPlayerBump(f32 sign, const KartObjectProxy *source, KartObjectProxy *target,
        const EGG::Vector3f &hitDir, const EGG::Vector3f &pushDir) {
    constexpr f32 BOOST_WEIGHT_FACTOR = 1.5f;
    constexpr f32 TARGET_BOOST_WEIGHT_FACTOR = 0.5f;
    constexpr f32 BASE_STRENGTH = 3.0f;
    constexpr f32 STRENGTH_PER_SPEED = 0.11f;
    constexpr f32 MAX_STRENGTH = 100.0f;
    constexpr f32 WEIGHT_CLASS_OFFSET_FACTOR = 0.3f;
    constexpr f32 OVER_ZIPPER_FORCE_FACTOR = 0.2f;
    constexpr f32 LONG_AIRTIME_FORCE_FACTOR = 0.5f;
    constexpr f32 SPEED_LOSS_FACTOR = 0.5f;
    constexpr f32 SPIN_KICK_FACTOR = 0.05f;
    constexpr s16 PLAYER_BUMP_TIME = 10;

    KartCollide *targetCollide = target->collide();
    if (targetCollide->m_playerBumpTimer > 0) {
        return;
    }

    Item::ItemDirector::Instance()->passThunderCloud(static_cast<u8>(source->param()->playerIdx()),
            static_cast<u8>(target->param()->playerIdx()));

    const auto &sourceStatus = source->status();
    auto &targetStatus = target->status();

    f32 sourceWeight = source->param()->stats().weight;
    f32 targetWeight = target->param()->stats().weight;

    if (sourceStatus.onBit(eStatus::SlipstreamBoost) && sourceWeight < targetWeight) {
        sourceWeight = targetWeight;
    }

    if (targetStatus.onBit(eStatus::SlipstreamBoost) && targetWeight < sourceWeight) {
        targetWeight = sourceWeight;
    }

    // A mushroom or boost panel boost (KartMove +0x118 & 4), checked against a Dolphin recording.
    if (source->move()->boost().active(KartBoost::Type::MushroomAndBoostPanel)) {
        sourceWeight *= BOOST_WEIGHT_FACTOR;
    }

    // The game scales the source's weight here too, not the target's.
    if (target->move()->boost().active(KartBoost::Type::MushroomAndBoostPanel)) {
        sourceWeight *= TARGET_BOOST_WEIGHT_FACTOR;
    }

    f32 weightRatio = sourceWeight / targetWeight;

    const EGG::Vector3f &targetUp = target->move()->smoothedUp();
    EGG::Vector3f dir = hitDir.perpInPlane(targetUp, true);

    f32 strength = weightRatio *
            (BASE_STRENGTH + STRENGTH_PER_SPEED * EGG::Mathf::abs(source->move()->speed()));
    strength = std::min(strength, MAX_STRENGTH);

    const auto &ownerStats = param()->stats();
    f32 weightClass = static_cast<f32>(static_cast<u32>(ownerStats.weightClass));
    f32 offsetScale = (WEIGHT_CLASS_OFFSET_FACTOR * (1.0f + weightClass)) *
            (-sign * targetCollide->boundingRadius());
    EGG::Vector3f wrenchPoint = target->pos() + pushDir * offsetScale;

    f32 force = sign * strength;
    if (targetStatus.onBit(eStatus::OverZipper)) {
        force *= OVER_ZIPPER_FORCE_FACTOR;
    } else if (targetStatus.onBit(eStatus::AirtimeOver20)) {
        force *= LONG_AIRTIME_FORCE_FACTOR;
    }

    EGG::Vector3f push = (dir * force).rej(targetUp);
    target->dynamics()->addForce(push);
    target->dynamics()->applyWrenchScaled(wrenchPoint, push, ownerStats.bumpDeviationLevel);

    f32 speed = target->move()->speed();
    f32 absSpeed = EGG::Mathf::abs(speed);
    f32 slowedSpeed =
            std::max(absSpeed - SPEED_LOSS_FACTOR * strength, SPEED_LOSS_FACTOR * absSpeed);
    target->move()->setSpeed(slowedSpeed * (speed < 0.0f ? -1.0f : 1.0f));

    targetCollide->m_playerBumpTimer = PLAYER_BUMP_TIME;
    targetCollide->startFloorMomentRate();
    targetStatus.setBit(eStatus::FloorMomentRateReset);

    f32 spinAxisY = dir.cross(componentZAxis()).y;
    f32 spinSign = spinAxisY < 0.0f ? 1.0f : -1.0f;
    EGG::Vector3f angVel0 = target->dynamics()->angVel0();
    angVel0 += EGG::Vector3f(0.0f, 0.0f, spinSign * (SPIN_KICK_FACTOR * strength));
    target->dynamics()->setAngVel0(angVel0);
}

/// @addr{0x805713B8}
void KartCollide::calcPlayerBumpTimer() {
    m_playerBumpTimer = std::max<s16>(0, m_playerBumpTimer - 1);
}

/// @addr{Inlined in 0x80571F10}
void KartCollide::calcPoleTimer() {
    if (m_poleAngVelTimer > 0 && status().onBit(eStatus::Accelerate, eStatus::Brake)) {
        EGG::Vector3f angVel2 = dynamics()->angVel2();
        angVel2.y += m_poleYaw;
        dynamics()->setAngVel2(angVel2);
    }

    m_poleAngVelTimer = std::max(0, m_poleAngVelTimer - 1);
}

/// @stage All
/// @brief Processes moving water and floor collision effects
/// @addr{0x8056E8D4}
void KartCollide::processWheel(CollisionData &collisionData, Hitbox &hitbox,
        Field::CollisionInfo *colInfo, Field::KCLTypeMask *maskOut) {
    processMovingWater(collisionData, maskOut);
    processFloor(collisionData, hitbox, colInfo, maskOut, true);
}

/// @addr{0x8056E764}
void KartCollide::processBody(CollisionData &collisionData, Hitbox &hitbox,
        Field::CollisionInfo *colInfo, Field::KCLTypeMask *maskOut) {
    processMovingWater(collisionData, maskOut);

    bool hasWallCollision = processWall(collisionData, maskOut);

    processFloor(collisionData, hitbox, colInfo, maskOut, false);

    if (hasWallCollision) {
        calcSideCollision(collisionData, hitbox, colInfo);
    }

    processCannon(maskOut);
}

/// @addr{0x8056E930}
void KartCollide::processMovingWater(CollisionData &collisionData, Field::KCLTypeMask *maskOut) {
    if (!(*maskOut & KCL_TYPE_BIT(COL_TYPE_MOVING_WATER))) {
        return;
    }

    auto *colDir = Field::CollisionDirector::Instance();
    if (!colDir->findClosestCollisionEntry(maskOut, KCL_TYPE_BIT(COL_TYPE_MOVING_WATER))) {
        return;
    }

    state()->status().setBit(eStatus::StickyRoad);

    auto *entry = colDir->closestCollisionEntry();
    switch (entry->variant()) {
    case 1:
        collisionData.bMovingWaterMomentum = true;
        collisionData.bMovingWaterStickyRoad = true;
        collisionData.bMovingWaterDisableAccel = true;
        break;
    case 2:
        collisionData.bMovingWaterDecaySpeed = true;
        break;
    case 3:
        collisionData.bMovingWaterDecaySpeed = true;
        collisionData.bMovingWaterDisableAccel = true;
        collisionData.bMovingWaterVertical = true;
        break;
    default:
        collisionData.bMovingWaterMomentum = true;
        break;
    }
}

/// @addr{0x8056F184}
bool KartCollide::processWall(CollisionData &collisionData, Field::KCLTypeMask *maskOut) {
    if (!(*maskOut & KCL_TYPE_DRIVER_WALL_NO_INVISIBLE_WALL2)) {
        return false;
    }

    auto *colDirector = Field::CollisionDirector::Instance();
    if (!colDirector->findClosestCollisionEntry(maskOut, KCL_TYPE_DRIVER_WALL_NO_INVISIBLE_WALL2)) {
        return false;
    }

    if ((*maskOut & KCL_TYPE_DRIVER_WALL_NO_INVISIBLE_WALL) &&
            colDirector->findClosestCollisionEntry(maskOut,
                    KCL_TYPE_DRIVER_WALL_NO_INVISIBLE_WALL)) {
        auto *entry = colDirector->closestCollisionEntry();

        collisionData.closestWallFlags = entry->baseType();
        collisionData.closestWallSettings = entry->variant();

        if (entry->attribute.onBit(Field::CollisionDirector::eCollisionAttribute::Soft)) {
            collisionData.bSoftWall = true;
        }
    }

    return true;
}

/// @stage All
/// @addr{0x8056EA04}
/// @brief Processes the floor triangles' attributes.
/// @param collisionData Stores the resulting speed and handling info
/// @param maskOut Stores the flags from the floor KCL
/// @param wheel Differentiates between body and wheel floor collision (boost panels)
void KartCollide::processFloor(CollisionData &collisionData, Hitbox &hitbox,
        Field::CollisionInfo * /*colInfo*/, Field::KCLTypeMask *maskOut, bool wheel) {
    constexpr Field::KCLTypeMask BOOST_RAMP_MASK = KCL_TYPE_BIT(COL_TYPE_BOOST_RAMP);

    if (collisionData.bSoftWall) {
        ++m_numSoftWallCollisions;
        m_sumHitboxBottomHeightSoftWall += hitbox.worldPos().y - hitbox.radius();
    }

    if (!(*maskOut & KCL_TYPE_FLOOR)) {
        return;
    }

    auto *colDirector = Field::CollisionDirector::Instance();

    if (!colDirector->findClosestCollisionEntry(maskOut, KCL_TYPE_FLOOR)) {
        return;
    }

    const auto *closestColEntry = colDirector->closestCollisionEntry();

    if (closestColEntry->attribute.offBit(
                Field::CollisionDirector::eCollisionAttribute::Trickable)) {
        m_surfaceFlags.setBit(eSurfaceFlags::NotTrickable);
    } else {
        collisionData.bTrickable = true;
        m_surfaceFlags.setBit(eSurfaceFlags::Trickable);
    }

    collisionData.speedFactor = std::min(collisionData.speedFactor,
            param()->stats().kclSpeed[closestColEntry->baseType()]);

    collisionData.intensity = closestColEntry->intensity();
    collisionData.rotFactor += param()->stats().kclRot[closestColEntry->baseType()];

    auto &status = KartObjectProxy::status();

    if (closestColEntry->attribute.onBit(
                Field::CollisionDirector::eCollisionAttribute::RejectRoad)) {
        status.setBit(eStatus::RejectRoad);
    }

    collisionData.closestFloorFlags = closestColEntry->typeMask;
    collisionData.closestFloorSettings = closestColEntry->variant();

    if (wheel && !!(*maskOut & KCL_TYPE_BIT(COL_TYPE_BOOST_PAD))) {
        move()->padType().setBit(KartMove::ePadType::BoostPanel);
    }

    if (!!(*maskOut & BOOST_RAMP_MASK) &&
            colDirector->findClosestCollisionEntry(maskOut, BOOST_RAMP_MASK)) {
        closestColEntry = colDirector->closestCollisionEntry();
        move()->padType().setBit(KartMove::ePadType::BoostRamp);
        state()->setBoostRampType(closestColEntry->variant());
        m_surfaceFlags.setBit(eSurfaceFlags::BoostRamp, eSurfaceFlags::Trickable);
    } else {
        state()->setBoostRampType(-1);
        m_surfaceFlags.setBit(eSurfaceFlags::NotTrickable);
    }

    if (!collisionData.bSoftWall) {
        ++m_numFloorOnlyCollisions;
        m_sumHitboxBottomHeightFloorOnly += hitbox.worldPos().y - hitbox.radius();
    }

    if (*maskOut & KCL_TYPE_BIT(COL_TYPE_STICKY_ROAD)) {
        status.setBit(eStatus::StickyRoad);
    }

    Field::KCLTypeMask halfPipeRampMask = KCL_TYPE_BIT(COL_TYPE_HALFPIPE_RAMP);
    if ((*maskOut & halfPipeRampMask) &&
            colDirector->findClosestCollisionEntry(maskOut, halfPipeRampMask)) {
        status.setBit(eStatus::HalfPipeRamp);
        state()->setHalfPipeInvisibilityTimer(2);
        if (colDirector->closestCollisionEntry()->variant() == 1) {
            move()->padType().setBit(KartMove::ePadType::BoostPanel);
        }
    }

    Field::KCLTypeMask jumpPadMask = KCL_TYPE_BIT(COL_TYPE_JUMP_PAD);
    if (*maskOut & jumpPadMask && colDirector->findClosestCollisionEntry(maskOut, jumpPadMask)) {
        if (status.offAnyBit(eStatus::TouchingGround, eStatus::JumpPad) &&
                status.offBit(eStatus::JumpPadMushroomVelYInc)) {
            move()->padType().setBit(KartMove::ePadType::JumpPad);
            closestColEntry = colDirector->closestCollisionEntry();
            state()->setJumpPadVariant(closestColEntry->variant());
        }
        collisionData.bTrickable = true;
    }

    // 0x8056EDF4
    if (*maskOut & KCL_TYPE_BIT(COL_TYPE_MOVING_ROAD)) {
        m_surfaceFlags.setBit(eSurfaceFlags::MovingRoad);
    }
}

/// @addr{0x8056F490}
/// @brief Checks if we are colliding with a cannon trigger and sets the state flag if so.
void KartCollide::processCannon(Field::KCLTypeMask *maskOut) {
    auto *colDirector = Field::CollisionDirector::Instance();
    if (colDirector->findClosestCollisionEntry(maskOut, KCL_TYPE_BIT(COL_TYPE_CANNON_TRIGGER))) {
        state()->setCannonPointId(colDirector->closestCollisionEntry()->variant());
        status().setBit(eStatus::CannonStart);
    }
}

/// @stage All
/// @brief Applies external and angular velocity based on the collision with the floor
/// @addr{0x805B7928}
/// @param down Always 0.1f
/// @param rate Downward velocity? Related to suspension stiffness
/// @param hitboxGroup Used to retrieve CollisionData reference
/// @param forward Current world facing direction of the kart
/// @param nextDir Updated facing direction of the kart
/// @param speed Tire speed
void KartCollide::applySomeFloorMoment(f32 down, f32 rate, CollisionGroup *hitboxGroup,
        const EGG::Vector3f &forward, const EGG::Vector3f &nextDir, const EGG::Vector3f &speed,
        bool b1, bool b2, bool b3) {
    CollisionData &colData = hitboxGroup->collisionData();
    if (!colData.bFloor) {
        return;
    }

    f32 velDotFloorNrm = colData.vel.dot(colData.floorNrm);

    if (velDotFloorNrm >= 0.0f) {
        return;
    }

    EGG::Matrix34f rotMat;
    rotMat.makeQ(dynamics()->mainRot());
    EGG::Matrix34f tmp = rotMat.multiplyTo(dynamics()->invInertiaTensor());
    EGG::Matrix34f rotMatTrans = rotMat.transpose();
    tmp = tmp.multiplyTo(rotMatTrans);

    EGG::Vector3f crossVec = colData.relPos.cross(colData.floorNrm);
    crossVec = tmp.multVector(crossVec);
    crossVec = crossVec.cross(colData.relPos);

    f32 scalar = -velDotFloorNrm / (1.0f + colData.floorNrm.dot(crossVec));
    EGG::Vector3f negSpeed = -speed;
    crossVec = colData.floorNrm.cross(negSpeed);
    crossVec = crossVec.cross(colData.floorNrm);

    if (std::numeric_limits<f32>::epsilon() >= crossVec.squaredLength()) {
        return;
    }

    crossVec.normalise();
    f32 speedDot = std::min(0.0f, speed.dot(crossVec));
    crossVec *= ((scalar * speedDot) / velDotFloorNrm);

    auto [proj, rej] = crossVec.projAndRej(forward);

    f32 projNorm = proj.length();
    f32 rejNorm = rej.length();
    f32 projNorm_ = projNorm;
    f32 rejNorm_ = rejNorm;

    f32 dVar7 = down * EGG::Mathf::abs(scalar);
    if (dVar7 < EGG::Mathf::abs(projNorm)) {
        projNorm_ = dVar7;
        if (projNorm < 0.0f) {
            projNorm_ = -down * EGG::Mathf::abs(scalar);
        }
    }

    f32 dVar5 = rate * EGG::Mathf::abs(scalar);
    if (EGG::Mathf::abs(rejNorm) > dVar5) {
        rejNorm_ = dVar5;
        if (rejNorm < 0.0f) {
            rejNorm_ = -rate * EGG::Mathf::abs(scalar);
        }
    }

    proj.normalise();
    rej.normalise();

    proj *= projNorm_;
    rej *= rejNorm_;

    EGG::Vector3f projRejSum = proj + rej;
    EGG::Vector3f projRejSumOrig = projRejSum;

    if (!b1) {
        projRejSum.x = 0.0f;
        projRejSum.z = 0.0f;
    }
    if (!b2) {
        projRejSum.y = 0.0f;
    }

    projRejSum = projRejSum.rej(nextDir);

    dynamics()->setExtVel(dynamics()->extVel() + projRejSum);

    if (b3) {
        EGG::Vector3f rotation = colData.relPos.cross(projRejSumOrig);
        EGG::Vector3f rotation2 = dynamics()->mainRot().rotateVectorInv(tmp.multVector(rotation));

        EGG::Vector3f angVel = rotation2;
        angVel.y = 0.0f;
        if (!b1) {
            angVel.x = 0.0f;
        }
        dynamics()->setAngVel0(dynamics()->angVel0() + angVel);
    }
}

/// @stage All
/// @brief Called on collision of a new KCL type??? This only happens after airtime so far.
/// @addr{0x805B6A9C}
/// @rename
bool KartCollide::FUN_805B6A9C(CollisionData &collisionData, const Hitbox &hitbox,
        EGG::BoundBox3f &minMax, EGG::Vector3f &relPos, s32 &count,
        const Field::KCLTypeMask &maskOut, const Field::CollisionInfo &colInfo) {
    if (maskOut & KCL_TYPE_WALL) {
        if (!(maskOut & KCL_TYPE_FLOOR) && status().onBit(eStatus::HWG) &&
                state()->softWallSpeed().dot(colInfo.wallNrm) < 0.3f) {
            return true;
        }

        bool skipWalls = false;

        collisionData.wallNrm += colInfo.wallNrm;

        if (maskOut & KCL_TYPE_ANY_INVISIBLE_WALL) {
            collisionData.bInvisibleWall = true;

            if (!(maskOut & KCL_TYPE_4010D000)) {
                collisionData.bInvisibleWallOnly = true;

                if (maskOut & KCL_TYPE_BIT(COL_TYPE_HALFPIPE_INVISIBLE_WALL)) {
                    skipWalls = true;
                }
            }
        }

        if (!skipWalls) {
            if (maskOut & KCL_TYPE_BIT(COL_TYPE_WALL_2)) {
                collisionData.bWall3 = true;
            } else {
                collisionData.bWall = true;
            }
        }
    }

    if (maskOut & KCL_TYPE_FLOOR) {
        collisionData.floorNrm += colInfo.floorNrm;
        collisionData.bFloor = true;
    }

    EGG::Vector3f tangentOff = colInfo.tangentOff;
    minMax.min = minMax.min.minimize(tangentOff);
    minMax.max = minMax.max.maximize(tangentOff);
    tangentOff.normalise();

    relPos += hitbox.relPos();
    relPos += -hitbox.radius() * tangentOff;
    ++count;

    return false;
}

/// @stage 2
/// @brief Saves collision info when vehicle body collision occurs.
/// @details Additionally may apply a change in position for certain KCL types.
/// @addr{0x805B6D48}
void KartCollide::applyBodyCollision(CollisionData &collisionData, const EGG::Vector3f &movement,
        const EGG::Vector3f &posRel, s32 count) {
    setPos(pos() + movement);

    if (!collisionData.bFloor && (collisionData.bWall || collisionData.bWall3)) {
        collisionData.movement = movement;
    }

    f32 rotFactor = 1.0f / static_cast<f32>(count);
    EGG::Vector3f scaledRelPos = rotFactor * posRel;
    collisionData.rotFactor *= rotFactor;

    EGG::Vector3f scaledAngVel0 = dynamics()->angVel0Factor() * dynamics()->angVel0();
    EGG::Vector3f local_48 = mainRot().rotateVectorInv(scaledRelPos);
    EGG::Vector3f local_30 = scaledAngVel0.cross(local_48);
    local_30 = mainRot().rotateVector(local_30);
    local_30 += extVel();

    collisionData.vel = local_30;
    collisionData.relPos = scaledRelPos;

    if (collisionData.bFloor) {
        f32 intVelY = dynamics()->intVel().y;
        if (intVelY > 0.0f) {
            collisionData.vel.y += intVelY;
        }
        collisionData.floorNrm.normalise();
    }
}

/// @addr{0x805713D8}
void KartCollide::startFloorMomentRate() {
    m_floorMomentRate = 0.01f;
}

/// @addr{0x805713FC}
void KartCollide::calcFloorMomentRate() {
    m_floorMomentRate = status().onBit(eStatus::InAction) &&
                    action()->flags().onBit(KartAction::eFlags::Rotating) ?
            0.01f :
            std::min(m_floorMomentRate + 0.01f, 0.8f);
}

/// @addr{0x8056E564}
Action KartCollide::handleReactNone(size_t /*idx*/) {
    return Action::None;
}

/// @addr{0x8057363C}
Action KartCollide::handleReactWallAllSpeed(size_t idx) {
    m_totalReactionWallNrm += Field::ObjectCollisionKart::GetHitDirection(idx);
    m_surfaceFlags.setBit(eSurfaceFlags::ObjectWall);

    return Action::None;
}

/// @addr{0x805733CC}
Action KartCollide::handleReactSpinAllSpeed(size_t /*idx*/) {
    return Action::UNK_0;
}

/// @addr{0x805733D4}
Action KartCollide::handleReactSpinSomeSpeed(size_t /*idx*/) {
    return Action::UNK_1;
}

/// @addr{0x805735AC}
Action KartCollide::handleReactFireSpin(size_t /*idx*/) {
    return Action::UNK_9;
}

/// @addr{0x805733C4}
Action KartCollide::handleReactSmallLaunch(size_t /*idx*/) {
    return Action::UNK_2;
}

/// @addr{0x805733DC}
Action KartCollide::handleReactKnockbackSomeSpeedLoseItem(size_t /*idx*/) {
    return Action::UNK_3;
}

/// @addr{0x8057353C}
Action KartCollide::handleReactLaunchSpinLoseItem(size_t /*idx*/) {
    return Action::UNK_6;
}

/// @addr{0x805733EC}
Action KartCollide::handleReactKnockbackBumpLoseItem(size_t /*idx*/) {
    return Action::UNK_4;
}

/// @addr{0x805735B4}
Action KartCollide::handleReactLongCrushLoseItem(size_t /*idx*/) {
    return Action::UNK_12;
}

/// @addr{0x805737B8}
Action KartCollide::handleReactSmallBump(size_t idx) {
    move()->applyForce(30.0f, objectCollisionKart()->GetHitDirection(idx), false);
    return Action::None;
}

/// @addr{0x805735BC}
Action KartCollide::handleReactSpinShrink(size_t /*idx*/) {
    return m_shrinkTimer <= 0 ? Action::UNK_15 : Action::None;
}

/// @addr{0x805733E4}
Action KartCollide::handleReactHighLaunchLoseItem(size_t /*idx*/) {
    return Action::UNK_8;
}

/// @addr{0x80573754}
Action KartCollide::handleReactWeakWall(size_t /*idx*/) {
    move()->setSpeed(move()->speed() * 0.82f);
    return Action::None;
}

/// @addr{0x80573790}
Action KartCollide::handleReactOffroad(size_t /*idx*/) {
    status().setBit(eStatus::CollidingOffroad);
    m_surfaceFlags.setBit(eSurfaceFlags::Offroad);
    return Action::None;
}

/// @addr{0x805733F4}
Action KartCollide::handleReactLaunchSpin(size_t idx) {
    action()->setTranslation(objectCollisionKart()->translation(idx));
    return Action::UNK_5;
}

/// @addr{0x805736C8}
Action KartCollide::handleReactWallSpark(size_t idx) {
    m_totalReactionWallNrm += Field::ObjectCollisionKart::GetHitDirection(idx);
    m_surfaceFlags.setBit(eSurfaceFlags::ObjectWall3);

    return Action::None;
}

/// @addr{0x80573A2C}
Action KartCollide::handleReactRubberWall(size_t idx) {
    constexpr f32 BASE_DIR_FORCE_SCALAR = 0.95f;
    constexpr f32 DIR_FORCE_SCALAR = 0.050000012f;
    constexpr f32 MAX_FORCE = 70.0f;

    const EGG::Vector3f &hitDir = Field::ObjectCollisionKart::GetHitDirection(idx);
    const EGG::Vector3f &zAxis = componentZAxis();

    f32 force = BASE_DIR_FORCE_SCALAR + DIR_FORCE_SCALAR * EGG::Mathf::abs(hitDir.dot(zAxis));
    move()->applyForce(MAX_FORCE * force, hitDir, true);

    return Action::None;
}

/// @addr{0x805735EC}
Action KartCollide::handleReactUntrickableJumpPad(size_t /*idx*/) {
    move()->setPadType(KartMove::PadType(KartMove::ePadType::JumpPad));
    state()->setJumpPadVariant(0);

    return Action::None;
}

/// @addr{0x805735D4}
Action KartCollide::handleReactShortCrushLoseItem(size_t /*idx*/) {
    return Action::UNK_14;
}

/// @addr{0x805735DC}
Action KartCollide::handleReactCrushRespawn(size_t /*idx*/) {
    return Action::UNK_16;
}

/// @addr{0x805735E4}
Action KartCollide::handleReactExplosionLoseItem(size_t /*idx*/) {
    return Action::UNK_7;
}

std::array<KartCollide::ObjectCollisionHandler, 33> KartCollide::s_objectCollisionHandlers = {{
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactWallAllSpeed,
        &KartCollide::handleReactSpinAllSpeed,
        &KartCollide::handleReactSpinSomeSpeed,
        &KartCollide::handleReactFireSpin,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactSmallLaunch,
        &KartCollide::handleReactKnockbackSomeSpeedLoseItem,
        &KartCollide::handleReactLaunchSpinLoseItem,
        &KartCollide::handleReactKnockbackBumpLoseItem,
        &KartCollide::handleReactLongCrushLoseItem,
        &KartCollide::handleReactSmallBump,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactSpinShrink,
        &KartCollide::handleReactHighLaunchLoseItem,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactWeakWall,
        &KartCollide::handleReactOffroad,
        &KartCollide::handleReactLaunchSpin,
        &KartCollide::handleReactWallSpark,
        &KartCollide::handleReactRubberWall,
        &KartCollide::handleReactNone,
        &KartCollide::handleReactUntrickableJumpPad,
        &KartCollide::handleReactShortCrushLoseItem,
        &KartCollide::handleReactCrushRespawn,
        &KartCollide::handleReactExplosionLoseItem,
}};

} // namespace Kinoko::Kart
