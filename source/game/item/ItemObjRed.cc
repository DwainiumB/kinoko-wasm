#include "ItemObjRed.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemMath.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/CollisionDirector.hh"

#include "game/kart/KartCollide.hh"
#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"

#include "game/system/CourseMap.hh"
#include "game/system/RaceManager.hh"

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

constexpr KCLTypeMask KCL_ITEM_SOLID = 0x20E80FFF;
constexpr KCLTypeMask KCL_ITEM_WALL = 0xD010F000;
constexpr u8 NO_PLAYER = 12;

const Kart::KartObject *Kart(u8 idx) {
    return Kart::KartObjectManager::Instance()->object(idx);
}

size_t PlayerCount() {
    return Kart::KartObjectManager::Instance()->count();
}

/// @addr{0x80591314}
/// @brief Respawning or hidden: the shells ignore it.
bool IsUntargetable(const Kart::KartObject *kart) {
    return kart->status().onBit(Kart::eStatus::TriggerRespawn, Kart::eStatus::InRespawn,
            Kart::eStatus::AfterRespawn, Kart::eStatus::Vanished);
}

/// @addr{0x80591264}
/// @brief The status bits that make a kart immune to items for now.
bool IsItemProof(const Kart::KartObject *kart) {
    return kart->status().onBit(Kart::eStatus::TriggerRespawn, Kart::eStatus::NoKartCollision40,
            Kart::eStatus::InRespawn, Kart::eStatus::AfterRespawn, Kart::eStatus::NoKartCollision82,
            Kart::eStatus::Vanished, Kart::eStatus::NoKartCollision89, Kart::eStatus::BeforeRespawn,
            Kart::eStatus::NoSlipstreamBoost135, Kart::eStatus::InAction,
            Kart::eStatus::CannonStart, Kart::eStatus::InCannon, Kart::eStatus::InStar,
            Kart::eStatus::InMega, Kart::eStatus::InBullet);
}

u8 Rank(u8 idx) {
    return System::RaceManager::Instance()->player(idx).rank();
}

u8 PlayerAtRank(u8 rank) {
    for (size_t i = 0; i < PlayerCount(); ++i) {
        if (Rank(static_cast<u8>(i)) == rank) {
            return static_cast<u8>(i);
        }
    }

    return NO_PLAYER;
}

} // namespace

ItemObjRedShell::ItemObjRedShell()
    : m_target(NO_PLAYER), m_targetDist(0.0f), m_homingFrames(0), m_state(0), m_nextState(-1),
      m_stateFrame(0) {}

ItemObjRedShell::~ItemObjRedShell() = default;

/// @addr{0x807AA688}
/// @brief Backward (stick down): a plain slide with no bounce. Forward: onto the item route,
/// aimed at the racer one place ahead (the last one when in first), or a plain slide on courses
/// without a route.
bool ItemObjRedShell::onRelease(ItemTrail &trail) {
    constexpr f32 BACK_SPEED = 50.0f;
    constexpr f32 FORWARD_SPEED = 75.0f;
    constexpr s32 ROUTE_FRAMES = 45;

    m_state = 0;
    m_nextState = -1;
    m_stateFrame = 0;
    m_target = NO_PLAYER;

    if (trail.kartItem()->stickDown()) {
        initThrow(BACK_SPEED, trail, 0, false, false);
        m_nextState = 1;
        m_routeCur.idx = System::MapdataItemPoint::NONE;
        m_routeCur.forward = true;
        m_routeCur.pos.setZero();
        return false;
    }

    if (trail.kartItem()->itemPointTracker().idx != System::MapdataItemPoint::NONE) {
        initRoute(FORWARD_SPEED, trail, ROUTE_FRAMES, 0, false, playerAhead(), false);
        m_nextState = 3;
    } else {
        initThrow(FORWARD_SPEED, trail, 0, true, false);
        m_nextState = 2;
        m_routeCur.idx = System::MapdataItemPoint::NONE;
        m_routeCur.forward = true;
        m_routeCur.pos.setZero();
    }

    m_flags.setBit(eFlags::RedForward);
    return true;
}

/// @brief The racer one place ahead of the owner, the last one for the leader (inlined in the
/// game).
u8 ItemObjRedShell::playerAhead() const {
    if (m_flags.onBit(eFlags::NoOwner) || m_owner >= NO_PLAYER || PlayerCount() < 2) {
        return NO_PLAYER;
    }

    u8 rank = Rank(m_owner) - 1;
    if (rank == 0) {
        rank = static_cast<u8>(PlayerCount());
    }

    return PlayerAtRank(rank);
}

/// @addr{0x807AA87C}
/// @brief 2400 frames, then it breaks. Enters a pending state, then runs the current one. The
/// model, the target marker and the sounds are visual.
void ItemObjRedShell::calcRed() {
    constexpr s32 LIFETIME = 2401;

    if (m_frame >= LIFETIME) {
        startBreak(true);
        return;
    }

    if (m_nextState < 0) {
        ++m_stateFrame;
    } else {
        m_state = static_cast<u16>(m_nextState);
        m_nextState = -1;
        m_stateFrame = 0;
        if (m_state == 3) {
            enterStateRoute();
        } else if (m_state == 4) {
            enterStateHoming();
        }
    }

    switch (m_state) {
    case 0:
    case 1:
        calcStateFree();
        break;
    case 2:
        calcStateSearch();
        break;
    case 3:
        calcStateRoute();
        break;
    case 4:
        calcStateHoming();
        break;
    case 6:
        calcStateLost();
        break;
    default:
        // State 5 (0x807AB238) is entered only by an online event.
        break;
    }
}

/// @addr{0x807AAB88}
void ItemObjRedShell::calcStateFree() {
    constexpr f32 SPEED = 105.0f;

    calcMove(SPEED);
}

/// @addr{0x807AAD78}
void ItemObjRedShell::calcStateSearch() {
    constexpr f32 SPEED = 105.0f;

    calcMove(SPEED);
    if (m_frameFlags.onBit(eFrameFlags::Breaking)) {
        return;
    }

    if (m_stateFrame < 17) {
        if (m_stateFrame > 4 && searchTarget(false, false)) {
            m_nextState = 4;
        }
    } else if (searchTarget(false, true)) {
        m_nextState = 4;
    }
}

/// @addr{0x807AAE58}
void ItemObjRedShell::enterStateRoute() {
    m_onWall = static_cast<WallFunc>(&ItemObjRedShell::onRouteWall);
}

/// @addr{0x807AAE78}
/// @brief Along the item route; after 1800 frames without a target it slides freely.
void ItemObjRedShell::calcStateRoute() {
    constexpr f32 SPEED = 130.0f;
    constexpr f32 RATE = 0.01f;
    constexpr u32 ROUTE_LIMIT = 0x709;

    calcRoute(SPEED, RATE, playerAhead(), false);
    if (m_frameFlags.onBit(eFrameFlags::Breaking)) {
        return;
    }

    if (m_stateFrame >= ROUTE_LIMIT) {
        freeFromVelocity();
        m_nextState = 1;
    } else if (m_stateFrame < 17) {
        if (m_stateFrame > 4 && searchTarget(true, false)) {
            m_nextState = 4;
        }
    } else if (searchTarget(true, true)) {
        m_nextState = 4;
    }
}

/// @addr{0x807AB004}
void ItemObjRedShell::enterStateHoming() {
    m_onWall = static_cast<WallFunc>(&ItemObjRedShell::onRouteWall);
}

/// @addr{0x807AA5AC}
/// @brief The wall callback while following the route or homing: bounced off another item or a
/// course object, the shell gives up and slides freely (state 1).
void ItemObjRedShell::onRouteWall(const EGG::Vector3f &nrm) {
    freeFromVelocity();
    onWallBounce(nrm);
    m_nextState = 1;
}

/// @addr{0x807AB024}
/// @brief Homing in on the target; losing it (respawn, hidden, a fall boundary, too far above or
/// below) turns it into a fast free slide.
void ItemObjRedShell::calcStateHoming() {
    constexpr f32 SPEED = 50.0f;
    constexpr f32 RATE = 0.2f;

    const auto *target = Kart(m_target);
    bool lost = IsUntargetable(target);
    if (!lost) {
        lost = calcHoming(SPEED, RATE, true);
    }

    if (m_flags.onBit(eFlags::Breaking, eFlags::Vanishing)) {
        return;
    }

    if (m_touchedBoundary) {
        lost = true;
    }

    if (lost) {
        dropTarget();
        m_nextState = 6;
        m_routeCur.idx = System::MapdataItemPoint::NONE;
        m_routeCur.forward = true;
        m_routeCur.pos.setZero();
    }
}

/// @addr{0x807AB37C}
void ItemObjRedShell::calcStateLost() {
    constexpr f32 SPEED = 140.0f;

    calcMove(SPEED);
}

/// @addr{0x807B3BAC}
/// @brief Looks at every racer for a target in front of the shell.
bool ItemObjRedShell::searchTarget(bool fromOwnerPlace, bool /*roundRobin*/) {
    f32 best = std::numeric_limits<f32>::max();
    bool found = false;
    for (size_t i = 0; i < PlayerCount(); ++i) {
        if (tryTarget(static_cast<u8>(i), best, fromOwnerPlace)) {
            found = true;
        }
    }

    return found;
}

/// @addr{0x807B381C}
/// @brief A racer within 4000 units, closer than the best so far, within about 18 degrees of the
/// shell's direction and not far above or below it. From the owner's place, only the leader may
/// target racers behind it (2nd and 3rd).
bool ItemObjRedShell::tryTarget(u8 idx, f32 &bestDistSq, bool fromOwnerPlace) {
    constexpr f32 MAX_DIST_SQ = 16000000.0f;
    constexpr f32 CONE = 0.95f;
    constexpr f32 MAX_ABOVE = 1000.0f;
    constexpr f32 MAX_BELOW = -1500.0f;

    u8 owner = m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;
    u8 targetRank = Rank(idx);
    if (owner < NO_PLAYER && fromOwnerPlace) {
        u8 ownerRank = Rank(owner);
        if (ownerRank <= targetRank) {
            if (ownerRank != 1 || targetRank > 3 || (targetRank == 1 && PlayerCount() > 1)) {
                return false;
            }
        }
    }

    const auto *kart = Kart(idx);
    if (IsUntargetable(kart)) {
        return false;
    }

    if (targetRank != 1 && IsItemProof(kart)) {
        return false;
    }

    EGG::Vector3f diff = kart->pos() - m_pos;
    f32 distSq = diff.x * diff.x + diff.z * diff.z;
    if (distSq > MAX_DIST_SQ || bestDistSq < distSq) {
        return false;
    }

    f32 dot = m_vel.ps_dot(diff);
    if (!(dot > 0.0f &&
                (1.0f - CONE * CONE) * (m_vel.ps_squareMag() * diff.ps_squareMag()) < dot * dot)) {
        return false;
    }

    f32 height = m_up.ps_dot(diff);
    if (height > MAX_ABOVE || height < MAX_BELOW) {
        return false;
    }

    m_target = idx;
    m_homingFrames = 0;
    f32 sq = m_pos.ps_sqDistance(kart->pos());
    m_targetDist = sq > 0.0f ? sq * EGG::Mathf::frsqrt(sq) : 0.0f;
    m_aimPoint = kart->pos();
    m_targetBack = kart->componentZAxis() * -1.0f;
    m_homingVel.setZero();
    bestDistSq = distSq;
    return true;
}

/// @addr{0x807B2F54}
/// @brief One frame of homing: aims at a point behind the target, steers a separate homing
/// velocity towards it and matches the target's speed, then moves and collides.
/// @return Whether the target is lost (far above or below it while close).
bool ItemObjRedShell::calcHoming(f32 speed, f32 rate, bool breakOnWall) {
    constexpr f32 BUSY_RATE = 0.01f;
    constexpr f32 BACK_RATE = 0.05f;
    constexpr f32 PROOF_SPEED = 0.5f;
    constexpr f32 AIM_FAR = 4000.0f;
    constexpr f32 AIM_MAX = 2000.0f;
    constexpr f32 LOST_ABOVE = 1050.0f;
    constexpr f32 LOST_BELOW = -1550.0f;
    constexpr f32 LOST_NEAR_SQ = 22500.0f;
    constexpr f32 STEER_BASE = 75.0f;
    constexpr f32 STEER_PER_SPEED = -50.0f;
    constexpr f32 OWNER_NEAR = 500.0f;
    constexpr f32 OWNER_FAR = 2000.0f;
    constexpr f32 AIR_DIST_FROM = 500.0f;
    constexpr f32 AIR_DIST_TO = 3000.0f;
    constexpr f32 AIR_KEEP = 0.93f;
    constexpr f32 AIR_KEEP_RANGE = 0.07f;

    const auto *target = Kart(m_target);
    bool lost = false;

    EGG::Vector3f back = target->componentZAxis() * -1.0f;
    bool busy = target->status().onBit(Kart::eStatus::ZipperTrick, Kart::eStatus::TrickRot);
    m_targetBack += (back - m_targetBack) * (busy ? BUSY_RATE : BACK_RATE);

    if (m_flags.offBit(eFlags::Airborne)) {
        if (IsItemProof(target)) {
            speed *= PROOF_SPEED;
        }

        f32 sq = m_pos.ps_sqDistance(target->pos());
        f32 dist = sq > 0.0f ? sq * EGG::Mathf::frsqrt(sq) : 0.0f;
        f32 aim = std::min(std::max(0.0f + (AIM_MAX * (dist - 0.0f)) / (AIM_FAR - 0.0f), 0.0f),
                AIM_MAX);
        aim *= target->move()->speedRatioCapped();
        m_aimPoint = target->pos() + m_targetBack * aim;

        EGG::Vector3f toAim = m_aimPoint - m_pos;
        f32 height = m_up.ps_dot(toAim);
        if ((height > LOST_ABOVE || height < LOST_BELOW) &&
                toAim.x * toAim.x + toAim.z * toAim.z < LOST_NEAR_SQ) {
            lost = true;
        }

        EGG::Vector3f dir(0.0f, 0.0f, 0.0f);
        m_targetDist = NormaliseLength(dir, toAim);
        EGG::Vector3f want = dir * speed;

        m_homingVel += (want - m_homingVel) * rate;
        f32 maxSteer = STEER_BASE + target->move()->speedRatioCapped() * STEER_PER_SPEED;
        f32 steerSq = m_homingVel.ps_squareMag();
        if (maxSteer * maxSteer < steerSq) {
            m_homingVel *= maxSteer * EGG::Mathf::frsqrt(steerSq);
        }

        const EGG::Vector3f &kartVel = target->velocity();
        EGG::Vector3f match((kartVel.x - m_vel.x) * rate, 0.0f, (kartVel.z - m_vel.z) * rate);

        u8 owner = m_flags.onBit(eFlags::NoOwner) ? NO_PLAYER : m_owner;
        if (owner < NO_PLAYER) {
            EGG::Vector3f fromOwner = m_pos - Kart(owner)->pos();
            if (match.ps_dot(fromOwner) < 0.0f) {
                EGG::Vector3f n(0.0f, 0.0f, 0.0f);
                f32 len = NormaliseLength(n, fromOwner);
                f32 k = std::min(
                        std::max(0.0f + (1.0f * (len - OWNER_FAR)) / (OWNER_NEAR - OWNER_FAR),
                                0.0f),
                        1.0f);
                match -= n * match.ps_dot(n) * k;
            }
        }

        m_vel += match;
    } else {
        f32 sq = m_pos.ps_sqDistance(target->pos());
        f32 dist = sq > 0.0f ? sq * EGG::Mathf::frsqrt(sq) : 0.0f;
        f32 keep = AIR_KEEP +
                (AIR_KEEP_RANGE * (dist - AIR_DIST_FROM)) / (AIR_DIST_TO - AIR_DIST_FROM);
        keep = std::min(std::max(keep, AIR_KEEP), 1.0f);
        m_vel.x *= keep;
        m_vel.z *= keep;
    }

    m_pos += m_homingVel;
    m_pos += m_vel;

    if (checkSphereFull(m_kclRadius, 0x6CEBDFFF)) {
        m_pos += m_colInfo.tangentOff;
        auto *colDir = Field::CollisionDirector::Instance();
        if ((m_kclMask & KCL_ITEM_SOLID) &&
                colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_SOLID)) {
            m_up = m_colInfo.floorNrm;
            m_vel -= m_up * m_vel.ps_dot(m_up);
        }

        if ((m_kclMask & KCL_ITEM_WALL) && !m_floats && breakOnWall) {
            startBreak(true);
            return false;
        }

        if (m_kclMask & 0x20000) {
            startVanish(true);
            return false;
        }

        if (m_kclMask & 0x10000) {
            m_touchedBoundary = true;
        }
    }

    calcGravity();
    return lost;
}

/// @addr{0x807B3EC0}
void ItemObjRedShell::dropTarget() {
    m_target = NO_PLAYER;
    m_vel += m_homingVel;
    freeFromVelocity();
}

ItemObj::StateFunc ItemObjRedShell::initialState() const {
    return static_cast<StateFunc>(&ItemObjRedShell::calcRed);
}

} // namespace Kinoko::Item
