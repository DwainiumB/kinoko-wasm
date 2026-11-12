#include "ItemObjKoura.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemMath.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/CollisionDirector.hh"

#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"

#include "game/system/CourseMap.hh"

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

constexpr KCLTypeMask KCL_ITEM_SOLID = 0x20E80FFF;
constexpr KCLTypeMask KCL_ITEM_WALL = 0xD010F000;
constexpr u8 NO_PLAYER = 12;

const Kart::KartObject *Kart(u8 idx) {
    return Kart::KartObjectManager::Instance()->object(idx);
}

/// @addr{0x807DE6E8}
/// @brief Turns a towards b by t (an approximate spherical blend).
EGG::Vector3f Blend(f32 t, const EGG::Vector3f &a, const EGG::Vector3f &b) {
    f32 dot = -a.ps_dot(b);
    f32 w = std::min((1.0f - dot) * t, 1.0f);
    f32 rest = 1.0f - w;
    f32 extra = (1.0f + dot) * 0.5f * (w * rest);
    return a * (rest + extra) + b * (w + extra);
}

/// @brief The links of an item point along a direction (0x805181F0 / 0x805182CC and counts).
u8 Link(u8 idx, bool forward, u8 i) {
    auto *map = System::CourseMap::Instance();
    return forward ? map->getNextItemPoint(idx, i) : map->getPrevItemPoint(idx, i);
}

u8 LinkCount(u8 idx, bool forward) {
    auto *map = System::CourseMap::Instance();
    return forward ? map->getNextItemPointCount(idx) : map->getPrevItemPointCount(idx);
}

/// @brief An item point the shells may take: the tens digit of its second setting is 0.
bool IsOpenPoint(u8 idx) {
    s32 setting = static_cast<s16>(System::CourseMap::Instance()->getItemPoint(idx)->setting2());
    return (setting % 100) / 10 == 0;
}

/// @brief The number of item points on the main loop (following the first next link from point 0),
/// as the game counts them into 0x809C366C.
s32 MainLoopLength() {
    constexpr size_t MAX_POINTS = 0x100;

    std::array<bool, MAX_POINTS> seen;
    seen.fill(false);
    s32 length = 0;
    u8 point = 0;
    while (true) {
        seen[point] = true;
        point = Link(point, true, 0);
        if (point == System::MapdataItemPoint::NONE || seen[point]) {
            break;
        }

        ++length;
    }

    return length;
}

} // namespace

/// @addr{0x807B99D4}
void ItemRoutePos::set(u8 newIdx, bool newForward) {
    idx = newIdx;
    forward = newForward;
    if (idx != System::MapdataItemPoint::NONE) {
        pos = System::CourseMap::Instance()->getItemPoint(idx)->pos();
    }
}

/// @addr{0x807B7D9C}
void ItemRouteSpline::init(f32 newSpeed, const std::array<EGG::Vector3f, 4> &pts) {
    points = pts;
    calc();
    speed = newSpeed;
    step = newSpeed * invLength;
    t = 0.0f;
}

/// @addr{0x807B80E0}
/// @brief Control points from the neighbouring points' tangents, and the curve's length from 8
/// segments.
void ItemRouteSpline::calc() {
    constexpr f32 THIRD = 3.0f;
    constexpr f32 STEP = 0.125f;

    EGG::Vector3f d;
    f32 len = NormaliseLength(d, points[2] - points[1]);
    controls[0] = points[1];
    controls[3] = points[2];

    EGG::Vector3f t1(0.0f, 0.0f, 0.0f);
    NormaliseLength(t1, points[2] - points[0]);
    controls[1] = controls[0] + t1 * (len / THIRD);

    EGG::Vector3f t2(0.0f, 0.0f, 0.0f);
    NormaliseLength(t2, points[3] - points[1]);
    controls[2] = controls[3] + t2 * (-len / THIRD);

    t = 0.0f;
    length = 0.0f;
    EGG::Vector3f prev = controls[0];
    for (s32 i = 0; i < 8; ++i) {
        t += STEP;
        f32 u = 1.0f - t;
        f32 k = THIRD * t * u;
        EGG::Vector3f p = controls[0] * (u * u * u) + controls[1] * (k * u) +
                controls[2] * (k * t) + controls[3] * (t * t * t);
        f32 sq = prev.ps_sqDistance(p);
        length += sq > 0.0f ? sq * EGG::Mathf::frsqrt(sq) : 0.0f;
        prev = p;
    }

    invLength = length > 0.0f ? 1.0f / length : 1.0f;
}

/// @addr{0x807B7E58}
void ItemRouteSpline::setSpeed(f32 newSpeed) {
    speed = newSpeed;
    step = newSpeed * invLength;
}

/// @addr{0x807B854C}
/// @brief Moves the curve on by one point.
/// @return Whether t is still past the end.
bool ItemRouteSpline::advance(const EGG::Vector3f &next) {
    points[0] = points[1];
    points[1] = points[2];
    points[2] = points[3];
    points[3] = next;
    f32 rest = t - 1.0f;
    f32 oldLength = length;
    calc();
    t = rest * oldLength * invLength;
    step = speed * invLength;
    return t >= 1.0f;
}

/// @addr{0x807B7F5C}
EGG::Vector3f ItemRouteSpline::evalXZ() const {
    f32 u = 1.0f - t;
    f32 k = 3.0f * t * u;
    f32 b0 = u * u * u;
    f32 b1 = k * u;
    f32 b2 = k * t;
    f32 b3 = t * t * t;
    return EGG::Vector3f(
            controls[0].x * b0 + controls[1].x * b1 + controls[2].x * b2 + controls[3].x * b3, 0.0f,
            controls[0].z * b0 + controls[1].z * b1 + controls[2].z * b2 + controls[3].z * b3);
}

/// @addr{0x807B8010}
EGG::Vector3f ItemRouteSpline::evalLerpY() const {
    EGG::Vector3f p = evalXZ();
    p.y = controls[0].y + t * (controls[3].y - controls[0].y);
    return p;
}

/// @addr{0x807B4878}
/// @brief A forward throw onto the item route: some frames of steering towards the next points,
/// then the spline. The red shell has no wall bounce (a wall breaks it); the blue shell has no
/// bounce limit (bounces -1) and floats.
void ItemObjKoura::initRoute(f32 speed, ItemTrail &trail, s32 routeFrames, s32 bounces, bool floats,
        u8 target, bool branchSearch) {
    initThrow(speed, trail, bounces, true, floats);
    m_routeTimer = routeFrames;
    m_routeTimerInv = 1.0f / static_cast<f32>(routeFrames);
    const auto &tracker = trail.kartItem()->itemPointTracker();
    m_routeCur.idx = tracker.idx;
    m_routeCur.forward = tracker.moved;
    m_routeCur.pos = tracker.pos;
    chooseNextPoint(target, branchSearch);
    m_routePassed = false;
}

/// @addr{0x807B5068}
void ItemObjKoura::calcRoute(f32 speed, f32 rate, u8 target, bool branchSearch) {
    if (m_routeTimer < 0) {
        calcRouteSpline(speed, rate, target, branchSearch);
        return;
    }

    calcRouteDirect(speed, target, branchSearch);
    if (m_routeTimer == 0) {
        if (startRouteSpline()) {
            m_routeTimer = -1;
        }
    } else if (m_flags.offBit(eFlags::Airborne) || m_floats) {
        --m_routeTimer;
    }
}

/// @addr{0x807B4AE4}
/// @brief The first frames: a normal slide, steered towards the next route point more and more.
void ItemObjKoura::calcRouteDirect(f32 speed, u8 target, bool branchSearch) {
    constexpr f32 FLOAT_HEIGHT = 300.0f;
    constexpr f32 TURN_PASSED = 0.15f;
    constexpr f32 TURN = 0.08f;

    calcMove(speed);

    if (m_floats && m_flags.onBit(eFlags::Airborne) && m_owner < NO_PLAYER) {
        f32 minY = FLOAT_HEIGHT + Kart(m_owner)->pos().y;
        if (m_pos.y < minY) {
            m_pos.y = minY;
        }
    }

    if (m_frameFlags.onBit(eFrameFlags::Breaking)) {
        return;
    }

    if (!m_floats && (m_flags.onBit(eFlags::Airborne) || m_airTimer >= 1)) {
        return;
    }

    // 0x807B90F4: the next point is nearer than the current one: move on.
    f32 dCur = (m_routeCur.pos.x - m_pos.x) * (m_routeCur.pos.x - m_pos.x) +
            (m_routeCur.pos.z - m_pos.z) * (m_routeCur.pos.z - m_pos.z);
    f32 dNext = (m_routeNext.pos.x - m_pos.x) * (m_routeNext.pos.x - m_pos.x) +
            (m_routeNext.pos.z - m_pos.z) * (m_routeNext.pos.z - m_pos.z);
    if (dNext < dCur) {
        m_routeCur.idx = m_routeNext.idx;
        m_routeCur.forward = m_routeNext.forward;
        if (m_routeCur.idx != System::MapdataItemPoint::NONE) {
            m_routeCur.pos = System::CourseMap::Instance()->getItemPoint(m_routeCur.idx)->pos();
        }
        chooseNextPoint(target, branchSearch);
    }

    EGG::Vector3f toNext = m_routeNext.pos - m_pos;
    if (m_moveDir.ps_dot(toNext) < 0.0f) {
        m_side = m_moveDir.cross(m_up);
        toNext = m_side.ps_dot(toNext) <= 0.0f ? m_side * -1.0f : m_side;
        m_routePassed = true;
    }

    EGG::Vector3f n = toNext;
    NormaliseInto(n, toNext);
    f32 turn = (m_routePassed ? TURN_PASSED : TURN) *
            (1.0f - static_cast<f32>(m_routeTimer) * m_routeTimerInv);
    steerTowards(Blend(turn, m_dir, n));
}

/// @addr{0x807B660C}
void ItemObjKoura::steerTowards(const EGG::Vector3f &dir) {
    constexpr f32 DOWN = 0.25f;

    m_dir.x = dir.x;
    m_dir.z = dir.z;
    EGG::Vector3f flat = dir - m_up * dir.ps_dot(m_up);
    m_moveDir = flat - m_up * DOWN;
    NormaliseInto(m_moveDir, m_moveDir);
    m_vel.x = m_moveDir.x * m_speed;
    m_vel.z = m_moveDir.z * m_speed;
    m_side = dir.cross(m_up);
}

/// @addr{0x807B4944}
/// @brief Switches to the spline once the shell faces the next point (within 30 degrees).
bool ItemObjKoura::startRouteSpline() {
    constexpr f32 FACING = 0.8660254f;
    constexpr f32 HANDLE = 0.25f;

    EGG::Vector3f toNext = m_routeNext.pos - m_pos;
    EGG::Vector3f n = toNext;
    NormaliseInto(n, toNext);
    if (!(m_dir.ps_dot(n) > FACING) && !m_floats) {
        return false;
    }

    EGG::Vector3f handle = toNext * HANDLE;
    EGG::Vector3f proj = m_dir * handle.ps_dot(m_dir);
    handle += proj;

    std::array<EGG::Vector3f, 4> pts = {{m_pos - proj, m_pos, m_pos + handle, m_routeNext.pos}};
    m_spline.init(m_speed, pts);
    m_splineBlend = 1.0f;
    return true;
}

/// @addr{0x807B4D2C}
/// @brief On the spline: the shell's x/z follow the curve (blended), y keeps its own physics.
void ItemObjKoura::calcRouteSpline(f32 speed, f32 rate, u8 target, bool branchSearch) {
    constexpr f32 BLEND_BASE = 1000.0f;
    constexpr f32 BLEND_RATE = 0.01f;

    m_speed += rate * (speed - m_speed);
    m_spline.setSpeed(m_speed);
    m_spline.t += m_spline.step;
    if (m_spline.t >= 1.0f) {
        do {
            m_routeCur.set(m_routeNext.idx, m_routeNext.forward);
            chooseNextPoint(target, branchSearch);
        } while (m_spline.advance(m_routeNext.pos));
    }

    EGG::Vector3f pt;
    if (!m_floats || m_flags.offBit(eFlags::Airborne)) {
        pt = m_spline.evalXZ();
    } else {
        f32 y = m_pos.y;
        pt = m_spline.evalLerpY();
        m_pos.y = std::max(pt.y, y);
    }

    m_pos.y += m_vel.y;
    f32 blend = m_splineBlend;
    f32 want = m_speed / (BLEND_BASE + m_speed);
    m_pos.x += blend * (pt.x - m_pos.x);
    m_pos.z += blend * (pt.z - m_pos.z);
    if (want <= blend) {
        blend = std::max(blend - BLEND_RATE, want);
    } else {
        blend = std::min(blend + BLEND_RATE, want);
    }
    m_splineBlend = blend;

    if (checkSphereFull(m_kclRadius, m_floats ? 0x6CEBDBFF : 0x6CEBDFFF)) {
        if (!m_floats) {
            m_pos += m_colInfo.tangentOff;
        } else if (m_colInfo.tangentOff.y > 0.0f) {
            m_pos.y += m_colInfo.tangentOff.y;
        }

        auto *colDir = Field::CollisionDirector::Instance();
        if ((m_kclMask & KCL_ITEM_SOLID) &&
                colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_SOLID)) {
            m_up = m_colInfo.floorNrm;
            m_vel -= m_up * m_vel.ps_dot(m_up);
        }

        if ((m_kclMask & KCL_ITEM_WALL) && m_limitBounces) {
            startBreak(true);
            return;
        }

        if ((m_kclMask & 0x20000) && m_kind != ItemObjKind::BlueShell) {
            startVanish(true);
            return;
        }

        if (m_kclMask & 0x10000) {
            m_touchedBoundary = true;
        }
    }

    calcGravityY();
    m_vel.x = m_pos.x - m_prevPos.x;
    m_vel.z = m_pos.z - m_prevPos.z;
}

/// @addr{0x807B91E4}
/// @brief Picks the route point after the current one; at a fork, the open branch nearest the
/// target when the target is within 15000 units.
void ItemObjKoura::chooseNextPoint(u8 target, bool branchSearch) {
    constexpr f32 TARGET_RANGE = 15000.0f;
    constexpr f32 SEARCH_SHARE = 0.333f;

    if (m_routeCur.idx == System::MapdataItemPoint::NONE) {
        m_routeNext = m_routeCur;
        return;
    }

    bool fwd = m_routeCur.forward;
    u8 next = Link(m_routeCur.idx, fwd, 0);
    u8 count = LinkCount(m_routeCur.idx, fwd);
    u8 backCount = next == System::MapdataItemPoint::NONE ? 0 : LinkCount(next, !fwd);

    // The blue shell: follow the target's own item point forward (a third of the course) until it
    // meets one of the branches here, and take that one.
    if (branchSearch && (count > 1 || backCount > 1) && target < NO_PLAYER) {
        std::array<u8, 32> branches;
        s32 branchCount = 0;
        for (u8 i = 0; i < count; ++i) {
            branches[branchCount++] = Link(m_routeCur.idx, fwd, i);
        }

        for (u8 i = 0; i < backCount; ++i) {
            branches[branchCount++] = Link(next, !fwd, i);
        }

        u8 point = ItemDirector::Instance()->kartItem(target).itemPointTracker().idx;
        s32 depth = static_cast<s32>(SEARCH_SHARE * static_cast<f32>(MainLoopLength()));
        for (s32 d = 0; d < depth; ++d) {
            for (s32 j = 0; j < branchCount; ++j) {
                if (point != branches[j]) {
                    continue;
                }

                s32 k = j;
                if (branches[j] == m_routeCur.idx) {
                    if (backCount <= 1) {
                        continue;
                    }

                    s32 r = (j + 1) - count;
                    point = branches[count + r % backCount];
                    k = 0;
                }

                m_routeNext.set(point, k < count ? fwd : !fwd);
                return;
            }

            point = Link(point, true, 0);
        }
    }

    if (count < 2 && backCount < 2) {
        m_routeNext.set(next, fwd);
        return;
    }

    if (target < NO_PLAYER) {
        const EGG::Vector3f &kartPos = Kart(target)->pos();
        f32 dx = kartPos.x - m_routeCur.pos.x;
        f32 dz = kartPos.z - m_routeCur.pos.z;
        if (dx * dx + dz * dz < TARGET_RANGE * TARGET_RANGE) {
            u8 best = System::MapdataItemPoint::NONE;
            f32 bestDist = std::numeric_limits<f32>::max();
            for (u8 i = 0; i < count; ++i) {
                u8 c = Link(m_routeCur.idx, fwd, i);
                if (!IsOpenPoint(c)) {
                    continue;
                }

                const EGG::Vector3f &cPos = System::CourseMap::Instance()->getItemPoint(c)->pos();
                if (!(0.0f < dx * (cPos.x - m_routeCur.pos.x) + dz * (cPos.z - m_routeCur.pos.z)) ||
                        c == best) {
                    continue;
                }

                f32 dist = cPos.ps_sqDistance(kartPos);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = c;
                }
            }

            if (best != System::MapdataItemPoint::NONE) {
                next = best;
            }
        }
    }

    if (backCount < 2 || fwd || target >= NO_PLAYER) {
        m_routeNext.set(next, fwd);
        return;
    }

    // Travelling backwards into a merge: turn onto the first other branch.
    u8 other = System::MapdataItemPoint::NONE;
    for (u8 i = 0; i < backCount; ++i) {
        u8 c = Link(next, !fwd, i);
        if (c != m_routeCur.idx) {
            other = c;
            break;
        }
    }

    if (other == System::MapdataItemPoint::NONE) {
        m_routeNext.set(next, fwd);
    } else {
        m_routeNext.set(other, !fwd);
    }
}

} // namespace Kinoko::Item
