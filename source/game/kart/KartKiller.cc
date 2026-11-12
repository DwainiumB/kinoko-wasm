#include "KartKiller.hh"

#include "game/kart/CollisionGroup.hh"
#include "game/kart/KartCollide.hh"
#include "game/kart/KartDynamics.hh"
#include "game/kart/KartMove.hh"
#include "game/kart/KartObjectManager.hh"
#include "game/kart/KartParam.hh"
#include "game/kart/KartState.hh"

#include "game/item/ItemDirector.hh"

#include "game/field/CollisionDirector.hh"

#include "game/system/CourseMap.hh"
#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"

namespace Kinoko::Kart {

namespace {

constexpr u8 NO_POINT = System::MapdataItemPoint::NONE;
constexpr f32 RAISE = 100.0f;
constexpr f32 WIDTH_UNIT = 50.0f;

/// @brief The point flags (0x8059D2D4): setting 1 = 1, 2, 3 -> 1, 2, 4; digits of setting 2 that
/// are 1 add 8 (ones), 0x10 (tens), 0x20 (hundreds).
u32 PointFlags(u8 point) {
    const auto *itemPoint = System::CourseMap::Instance()->getItemPoint(point);
    u32 flags = 0;
    switch (itemPoint->setting1()) {
    case 1:
        flags = 1;
        break;
    case 2:
        flags = 2;
        break;
    case 3:
        flags = 4;
        break;
    default:
        break;
    }

    s32 setting = static_cast<s16>(itemPoint->setting2());
    if (setting > 0) {
        if (setting % 10 == 1) {
            flags |= 8;
        }

        if ((setting % 100) / 10 == 1) {
            flags |= 0x10;
        }

        if ((setting % 1000) / 100 == 1) {
            flags |= 0x20;
        }
    }

    return flags;
}

u8 NextPoint(u8 point) {
    return System::CourseMap::Instance()->getNextItemPoint(point, 0);
}

/// @addr{0x8059C798}
/// @brief The quaternion product, in the game's order of operations.
EGG::Quatf Multiply(const EGG::Quatf &a, const EGG::Quatf &b) {
    EGG::Vector3f v = a.v.cross(b.v);
    v = v + b.v * a.w;
    v = v + a.v * b.w;
    f32 w = a.w * b.w - (a.v.z * b.v.z + a.v.x * b.v.x + a.v.y * b.v.y);
    return EGG::Quatf(w, v.x, v.y, v.z);
}

/// @brief The angle in degrees between two directions in the horizontal plane.
f32 HorizontalAngle(const EGG::Vector3f &a, const EGG::Vector3f &b) {
    constexpr f32 RAD_TO_DEG = 57.29578f;

    EGG::Vector3f h0(a.x, 0.0f, a.z);
    EGG::Vector3f h1(b.x, 0.0f, b.z);
    if (h0.normalise() == 0.0f || h1.normalise() == 0.0f) {
        return 0.0f;
    }

    f32 dot = h0.x * h1.x;
    f32 dotY = h0.y * h1.y;
    f32 dotZ = h0.z * h1.z;
    EGG::Vector3f c = h0.cross(h1);
    f32 angle = EGG::Mathf::atan2(EGG::Mathf::sqrt(c.z * c.z + c.x * c.x + c.y * c.y),
            dotZ + dot + dotY);
    if (angle <= 0.0f) {
        angle = -angle;
    }

    return RAD_TO_DEG * angle;
}

} // namespace

KartKiller::KartKiller()
    : m_point(NO_POINT), m_prevPoint(NO_POINT), m_frame(0), m_endRank(1), m_wallFrames(0),
      m_wallGrace(0), m_turnRate(0.0f), m_speedScale(1.0f), m_ending(false), m_finished(false),
      m_raised(false), m_flying(false), m_voidCheck(false), m_noFlyTimer(0), m_pointFlags(0),
      m_wallNrm(EGG::Vector3f::ex), m_liftTimer(0), m_lift(0.0f), m_sideTimer(0), m_side(0.0f) {
    // 0x8059B658: the end of the bullet can fall through on Rainbow Road.
    m_voidCheck = System::RaceConfig::Instance()->raceScenario().course == Course::Rainbow_Road;
}

KartKiller::~KartKiller() = default;

/// @addr{0x8059B7B8}
/// @brief Starts at the item point the kart is at; ends early once it gains enough places.
void KartKiller::start(u8 point) {
    constexpr f32 START_TURN_RATE = 0.03f;
    static constexpr std::array<s16, 12> RANK_GAIN = {{1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5}};

    // The online start point (when given) is not ported.
    static_cast<void>(point);
    size_t playerIdx = param()->playerIdx();
    u8 startPoint = Item::ItemDirector::Instance()->kartItem(playerIdx).itemPointTracker().idx;
    m_point = startPoint;
    m_prevPoint = NO_POINT;
    m_pointFlags = PointFlags(startPoint);

    status().setBit(eStatus::InBullet);
    m_frame = 0;
    move()->setBulletHardSpeedLimit();
    move()->setObjectCollisionScale(param()->stats().megaScale);
    m_turnRate = START_TURN_RATE;
    // The models are hidden (0x80590A9C) and the sound is played: visual.

    size_t count = KartObjectManager::Instance()->count();
    u8 rank = System::RaceManager::Instance()->player(playerIdx).rank();
    m_endRank = static_cast<s16>(rank - RANK_GAIN[count - 1]);
    if (m_endRank < 1) {
        m_endRank = 1;
    }

    m_finished = false;
    m_ending = false;
    m_wallFrames = 0;
    m_wallGrace = 0;
    // The bullet model's blend in is visual.

    dynamics()->setBikeAsKart(true);
    m_speedScale = 1.0f;
    m_raised = false;
    m_flying = false;
    m_noFlyTimer = 0;
    m_liftTimer = 0;
    m_lift = 0.0f;
    m_side = 0.0f;
    m_sideTimer = 0;
    m_wallNrm = EGG::Vector3f::ex;
}

/// @addr{0x8059BC44}
void KartKiller::calc() {
    constexpr f32 STICK_YAW = -0.01f;
    constexpr s16 RESYNC_FRAME_1 = 10;
    constexpr s16 RESYNC_FRAME_2 = 60;

    calcAim();

    EGG::Vector3f angVel2 = dynamics()->angVel2();
    angVel2 += EGG::Vector3f(0.0f, STICK_YAW * state()->stickX(), 0.0f);
    dynamics()->setAngVel2(angVel2);

    calcPoint();

    if (m_wallFrames == RESYNC_FRAME_1 || m_wallFrames == RESYNC_FRAME_2) {
        u8 point = Item::ItemDirector::Instance()
                           ->kartItem(param()->playerIdx())
                           .itemPointTracker()
                           .idx;
        u8 next = NextPoint(point);
        if (m_point != point && m_point != next) {
            m_prevPoint = NO_POINT;
            m_point = point;
            m_pointFlags = PointFlags(point);
        }
    }

    if (status().offBit(eStatus::InCannon)) {
        calcSteer();
        if (!m_flying) {
            dynamics()->setNoGravity(false);
        } else {
            dynamics()->setNoGravity(true);
            dynamics()->setExtVel(EGG::Vector3f::zero);
        }
    }

    // The rumble is left out.
    ++m_frame;
    calcLift();
    calcEnd();

    if (!m_voidCheck || !calcVoid()) {
        if (calcWall() || m_finished) {
            cancel();
        }
    } else {
        cancel();
        collide()->activateOob(false, nullptr, false, true);
    }
}

/// @addr{0x8059C118}
void KartKiller::cancel() {
    size_t playerIdx = param()->playerIdx();
    auto &kartItem = Item::ItemDirector::Instance()->kartItem(playerIdx);
    kartItem.setBulletItemPoint(m_point);

    status().resetBit(eStatus::InBullet);
    move()->setRegularHardSpeedLimit();
    move()->setObjectCollisionScale(1.0f);
    // The models are shown again (0x80590A9C): visual.
    move()->setScale(EGG::Vector3f(1.0f, 1.0f, 1.0f));
    // The sound is left out.
    kartItem.clearBulletFromInventory();
    // The rumble is left out.
    status().setBit(eStatus::AfterCannon);
    dynamics()->setNoGravity(false);
    dynamics()->setBikeAsKart(false);
    move()->setBulletScale(1.0f);
    move()->setHitboxScale(1.0f);
}

/// @addr{0x8059D0C0}
/// @brief While flying, the kart's up axis is turned to face the next point.
/// @return Whether it overrides KartMove::calcTop.
bool KartKiller::calcTop() {
    constexpr f32 LIFT_SCALE = 5.0f;
    constexpr f32 TOP_RATE = 0.2f;

    if (!m_flying) {
        return false;
    }

    EGG::Vector3f target = pointPos(m_flying, nullptr);
    if (m_liftTimer > 0) {
        target.y += m_lift * LIFT_SCALE;
    }

    EGG::Vector3f toTarget = target - pos();
    EGG::Vector3f right = dynamics()->mainRot().rotateVector(EGG::Vector3f::ex);
    right.y = 0.0f;
    EGG::Vector3f up = toTarget.cross(right);
    if (up.y < 0.0f) {
        up.y *= -1.0f;
    }

    if (up.squaredLength() > std::numeric_limits<f32>::epsilon()) {
        up.normalise2();
    } else {
        up = EGG::Vector3f::ey;
    }

    move()->setBulletTop(up, TOP_RATE);
    return true;
}

/// @addr{0x8059D44C}
/// @brief Whether it flies: by the point's setting, or once inside the route's corridor; never
/// after a long time against a wall facing down, nor while the kart is out of play.
void KartKiller::calcAim() {
    constexpr s16 WALL_FRAMES = 10;
    constexpr f32 DOWN_WALL = -0.75f;
    constexpr s32 NO_FLY_FRAMES = 60;

    u32 flags = m_pointFlags;
    if (flags & 1) {
        m_flying = false;
        m_raised = false;
    } else if (flags & 2) {
        m_flying = true;
        m_raised = false;
    } else if (!m_raised) {
        if (inCorridor(pos(), (flags & 4) == 0)) {
            m_flying = true;
            m_raised = true;
        }
    }

    if (m_wallFrames > WALL_FRAMES && m_wallNrm.y < DOWN_WALL) {
        m_noFlyTimer = NO_FLY_FRAMES;
    }

    if (status().onBit(eStatus::BeforeRespawn) || m_noFlyTimer > 0) {
        m_flying = false;
        m_raised = false;
    }

    if (--m_noFlyTimer < 0) {
        m_noFlyTimer = 0;
    }
}

/// @addr{0x8059C29C}
/// @brief Moves on to the next point once past the current one (its plane, or within 700 to
/// 1000 of it).
void KartKiller::calcPoint() {
    constexpr f32 MIN_RANGE = 700.0f;
    constexpr f32 MAX_RANGE = 1000.0f;

    EGG::Vector3f prev = prevPointPos(m_flying, nullptr);
    EGG::Vector3f cur = pointPos(m_flying, nullptr);
    EGG::Vector3f dir = cur - prev;
    dir.normalise2();

    // 0x8059CB54
    bool passed = false;
    if (dir.squaredLength() > std::numeric_limits<f32>::epsilon()) {
        const EGG::Vector3f &kartPos = pos();
        passed = 0.0f < dir.z * (kartPos.z - cur.z) + dir.x * (kartPos.x - cur.x) +
                        0.0f * (kartPos.y - cur.y);
    }

    if (!passed) {
        f32 range = System::CourseMap::Instance()->getItemPoint(m_point)->deviation() * WIDTH_UNIT;
        range = std::min(std::max(range, MIN_RANGE), MAX_RANGE);
        const EGG::Vector3f &kartPos = pos();
        f32 dz = cur.z - kartPos.z;
        f32 dx = cur.x - kartPos.x;
        passed = dz * dz + dx * dx + 0.0f * 0.0f < range * range;
    }

    if (passed) {
        m_prevPoint = m_point;
        m_point = NextPoint(m_point);
        m_pointFlags = PointFlags(m_point);
    }
}

/// @addr{0x8059C44C}
/// @brief Turns the kart's main rotation towards the next point, faster the sharper the turn.
void KartKiller::calcSteer() {
    constexpr f32 RATE_BLEND = 0.05f;
    constexpr f32 BASE_RATE = 0.03f;
    constexpr f32 RATE_PER_DEGREE = 0.003f;

    EGG::Vector3f target = pointPos(m_flying, nullptr);
    EGG::Vector3f toTarget = target - pos();
    if (status().onBit(eStatus::WallCollision, eStatus::Wall3Collision)) {
        toTarget.y = 0.0f;
    }

    if (toTarget.normalise() == 0.0f) {
        return;
    }

    EGG::Quatf rot = dynamics()->mainRot();
    EGG::Vector3f forward = rot.rotateVector(EGG::Vector3f::ez);
    EGG::Quatf turn;
    turn.makeVectorRotation(forward, toTarget);
    turn = Multiply(turn, rot);

    f32 angle = HorizontalAngle(forward, target - pos());
    m_turnRate += RATE_BLEND * ((BASE_RATE + angle * RATE_PER_DEGREE) - m_turnRate);
    rot = rot.slerpTo(turn, m_turnRate);

    if (m_sideTimer > 0) {
        EGG::Vector3f up = dynamics()->mainRot().rotateVector(EGG::Vector3f::ey);
        EGG::Quatf side;
        side.setAxisRotation(m_side, up);
        rot = Multiply(side, rot);
    }

    rot.normalise();
    dynamics()->setMainRot(rot);
}

/// @addr{0x8059D658}
/// @brief Stuck against a wall while slow: after 30 frames it aims higher for 60 frames, after 80
/// it turns aside for 40.
void KartKiller::calcLift() {
    constexpr f32 SLOW_SPEED = 50.0f;
    constexpr s16 LIFT_FRAME = 30;
    constexpr s32 LIFT_FRAMES = 60;
    constexpr f32 MIN_LIFT = 200.0f;
    constexpr s16 SIDE_FRAME = 80;
    constexpr s32 SIDE_FRAMES = 40;
    constexpr f32 SIDE_TURN = 0.08f;

    if (m_noFlyTimer == 0 && move()->speed() < SLOW_SPEED) {
        if (m_wallFrames == LIFT_FRAME) {
            m_liftTimer = LIFT_FRAMES;
            EGG::Vector3f target = pointPos(m_flying, nullptr);
            m_lift = std::max(target.y - pos().y, MIN_LIFT);
        }

        if (m_wallFrames == SIDE_FRAME) {
            m_sideTimer = SIDE_FRAMES;
            EGG::Vector3f cur = pointPos(false, nullptr);
            EGG::Vector3f seg = cur - prevPointPos(false, nullptr);
            EGG::Vector3f toCur = pointPos(false, nullptr) - pos();
            EGG::Vector3f perp = EGG::Vector3f::zero;
            if (seg.squaredLength() > std::numeric_limits<f32>::epsilon()) {
                perp = toCur - toCur.proj(seg);
            }

            EGG::Vector3f right = dynamics()->mainRot().rotateVector(EGG::Vector3f::ex);
            m_side = right.z * perp.z + right.x * perp.x + right.y * perp.y < 0.0f ? -SIDE_TURN :
                                                                                     SIDE_TURN;
        }
    }

    if (--m_liftTimer < 0 || m_wallFrames == 0) {
        m_liftTimer = 0;
    }

    if (--m_sideTimer < 0 || m_wallFrames == 0) {
        m_sideTimer = 0;
    }
}

/// @addr{0x8059CC60}
/// @brief Ends after 480 frames, or after 180 once in the lead or at the end rank (not in a
/// cannon nor on a point that forbids it); then 70 frames of slowing down (sound only).
void KartKiller::calcEnd() {
    constexpr s16 EARLY_FRAME = 180;
    constexpr s16 LAST_FRAME = 480;
    constexpr s16 ENDING_FRAMES = 70;

    bool mayEnd = (m_pointFlags & 8) == 0;
    if (!m_ending) {
        u8 rank = System::RaceManager::Instance()->player(param()->playerIdx()).rank();
        bool rankReached = rank == 1 || static_cast<s16>(rank) <= m_endRank;
        bool early = m_frame > EARLY_FRAME && rankReached;
        if ((m_frame > LAST_FRAME || early) && status().offBit(eStatus::InCannon) && mayEnd) {
            m_ending = true;
            m_frame = 0;
            // The HUD is left out.
        }

        if (m_frame > LAST_FRAME + 1) {
            m_frame = LAST_FRAME + 1;
        }
    } else {
        // The engine sound fades over the 70 frames.
        if (m_frame > ENDING_FRAMES && mayEnd) {
            m_finished = true;
        }
    }
}

/// @addr{0x8059C888}
/// @brief Counts the frames against walls; 120 of them end the bullet.
bool KartKiller::calcWall() {
    constexpr s16 GRACE = 4;
    constexpr s16 MAX_WALL_FRAMES = 119;

    bool wall = status().onBit(eStatus::WallCollision, eStatus::Wall3Collision);
    if (wall) {
        m_wallNrm = collisionData().wallNrm;
        m_wallGrace = GRACE;
    }

    bool end = false;
    if (!wall && m_wallGrace < 1) {
        m_wallFrames = 0;
    }

    if (++m_wallFrames > MAX_WALL_FRAMES) {
        m_wallFrames = 0;
        end = true;
    }

    if (--m_wallGrace < 0) {
        m_wallGrace = 0;
    }

    return end;
}

/// @addr{0x8059DCDC}
/// @brief Rainbow Road: in the air, with no wall time, touching the course's fall boundary.
bool KartKiller::calcVoid() {
    constexpr f32 RADIUS = 10.0f;

    // The game offsets the probe by the kart's forward and up axes times two constants that are
    // never set (0.0): it is the kart's position.
    EGG::Vector3f probe = pos();
    Field::CollisionInfo info;
    info.bbox.setZero();
    bool hit = Field::CollisionDirector::Instance()->checkSphereFull(RADIUS, probe,
            EGG::Vector3f::inf, 0x20E80DFF, &info, nullptr, 0);
    return status().offBit(eStatus::TouchingGround) && m_wallFrames == 0 && hit;
}

/// @addr{0x8059DAE4}
/// @brief Whether the kart is within the route's width around the segment to the next point
/// (and, with checkSide, on the near side of the point's plane).
bool KartKiller::inCorridor(const EGG::Vector3f &kartPos, bool checkSide) const {
    constexpr f32 PROBE_RADIUS = 100.0f;

    f32 prevWidth = 0.0f;
    f32 width = 0.0f;
    EGG::Vector3f prev = prevPointPos(false, &prevWidth);
    EGG::Vector3f cur = pointPos(false, &width);
    EGG::Vector3f toCur = cur - kartPos;

    // 0x8059D97C: the plane normal is the floor's under the point, else horizontal.
    EGG::Vector3f seg = cur - prev;
    EGG::Vector3f nrm;
    Field::CollisionInfo info;
    info.bbox.setZero();
    info.floorDist = -std::numeric_limits<f32>::min();
    bool floor = Field::CollisionDirector::Instance()->checkSphereFull(PROBE_RADIUS, cur,
                         EGG::Vector3f::inf, 0x20E80FFF, &info, nullptr, 0) &&
            info.floorDist > -std::numeric_limits<f32>::min();
    if (floor) {
        nrm = info.floorNrm;
    } else {
        nrm = seg.cross(EGG::Vector3f::ey).cross(seg);
    }

    nrm.normalise2();

    if (checkSide && !(toCur.y * nrm.y + toCur.x * nrm.x + toCur.z * nrm.z <= 0.0f)) {
        return false;
    }

    EGG::Vector3f dir = cur - prev;
    f32 len = dir.normalise();
    f32 along = toCur.y * dir.y + toCur.x * dir.x + toCur.z * dir.z;
    f32 dist = 0.0f;
    f32 limit = width;
    if (len > 0.0f) {
        EGG::Vector3f off = dir * along - toCur;
        dist = EGG::Mathf::sqrt(off.z * off.z + off.x * off.x + off.y * off.y);
        f32 t = std::min(std::max(along / len, 0.0f), 1.0f);
        limit = width + t * (prevWidth - width);
    }

    return dist < limit;
}

/// @addr{0x8059CF04}
EGG::Vector3f KartKiller::pointPos(bool raised, f32 *width) const {
    const auto *point = System::CourseMap::Instance()->getItemPoint(m_point);
    EGG::Vector3f p = point->pos();
    if (raised) {
        p.y += RAISE;
    }

    if (width) {
        *width = point->deviation() * WIDTH_UNIT;
    }

    return p;
}

/// @addr{0x8059CE24}
EGG::Vector3f KartKiller::prevPointPos(bool raised, f32 *width) const {
    u8 idx = m_prevPoint;
    if (idx == NO_POINT) {
        idx = System::CourseMap::Instance()->getPrevItemPoint(m_point, 0);
    }

    const auto *point = System::CourseMap::Instance()->getItemPoint(idx);
    EGG::Vector3f p = point->pos();
    if (raised) {
        p.y += RAISE;
    }

    if (width) {
        *width = point->deviation() * WIDTH_UNIT;
    }

    return p;
}

} // namespace Kinoko::Kart
