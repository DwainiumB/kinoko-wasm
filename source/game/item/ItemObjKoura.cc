#include "ItemObjKoura.hh"

#include "game/item/ItemMath.hh"
#include "game/item/ItemTrail.hh"
#include "game/item/KartItem.hh"

#include "game/field/CollisionDirector.hh"

#include "game/kart/KartObject.hh"

#include "game/system/CourseMap.hh"

namespace Kinoko::Item {

using Field::KCLTypeMask;

namespace {

constexpr KCLTypeMask KCL_ITEM_SOLID = 0x20E80FFF;
constexpr KCLTypeMask KCL_ITEM_WALL = 0xD010F000;

} // namespace

ItemObjKoura::ItemObjKoura()
    : m_speed(0.0f), m_bouncesLeft(0), m_floats(false), m_limitBounces(false), m_reflects(false),
      m_touchedBoundary(false), m_airTimer(0), m_routeTimer(0), m_routeTimerInv(0.0f),
      m_routePassed(false), m_splineBlend(0.0f) {
    m_routeCur.idx = System::MapdataItemPoint::NONE;
    m_routeCur.forward = true;
    m_routeNext = m_routeCur;
}

ItemObjKoura::~ItemObjKoura() = default;

/// @addr{0x807B5178}
/// @brief Starts a thrown shell: direction and speed from the kart, the bounce budget.
void ItemObjKoura::initThrow(f32 speed, ItemTrail &trail, s32 bounces, bool forward, bool floats) {
    constexpr f32 MIN_SCALE = 0.8f;
    constexpr f32 DELTA_SCALE = 1.0f;
    constexpr f32 FORWARD_DIR_SCALE = 35.0f;
    constexpr f32 BACK_SPEED = 30.0f;
    constexpr f32 BACK_DIR_SCALE = 40.0f;
    constexpr s32 AIR_FRAMES = 30;

    if (forward) {
        m_speed = trail.calcThrowForward(speed, DELTA_SCALE, FORWARD_DIR_SCALE, false, m_dir);
    } else {
        m_speed = trail.calcThrowBack(BACK_SPEED, DELTA_SCALE, BACK_DIR_SCALE, false, m_dir);
    }

    m_moveDir = m_dir;
    m_side = EGG::Vector3f(-m_dir.z, 0.0f, m_dir.x);
    m_vel.x = m_dir.x * m_speed;
    m_vel.z = m_dir.z * m_speed;

    if (m_scale < MIN_SCALE) {
        m_scale = MIN_SCALE;
        m_scaleVec = EGG::Vector3f(MIN_SCALE, MIN_SCALE, MIN_SCALE);
        if (forward) {
            // The kart's own position (KartObjectProxy::getPos), not the trail's anchor base.
            m_pos = trail.kartObject()->pos();
        }
    }

    m_limitBounces = bounces >= 0;
    m_bouncesLeft = m_limitBounces ? bounces : 0;
    m_onWall = static_cast<WallFunc>(&ItemObjKoura::onWallBounce);
    m_reflects = true;
    m_floats = floats;

    // The game reads status bits 41 / 42 (word 1, 0x600): released in the air.
    const auto &status = trail.kartObject()->status();
    m_airTimer = status.onBit(Kart::eStatus::RampBoost, Kart::eStatus::OverZipper) ? AIR_FRAMES : 0;
    m_touchedBoundary = false;
    m_flags.setBit(eFlags::Released);
}

/// @addr{0x807B54F4}
/// @brief Moves a shell one frame: course collision, floor following, wall bounces, gravity.
void ItemObjKoura::calcMove(f32 targetSpeed) {
    constexpr f32 STEEP_FLOOR_Y = 0.003f;
    constexpr f32 CEILING_Y = -0.98f;
    constexpr f32 WALL_VANISH_Y = 0.98f;
    constexpr s32 BOUNCE_LIMIT_FRAME = 300;
    constexpr s32 AIR_FRAMES = 30;
    constexpr f32 MIN_DIR_XZ = 0.0001f;
    constexpr f32 MAX_STEP_UP = 50.0f;
    constexpr f32 SPEED_RATE = 0.01f;
    constexpr f32 FLOOR_PULL = 2.5f;
    constexpr f32 FALL_GRAVITY = 2.0f;
    constexpr f32 RISE_GRAVITY = 4.2f;
    constexpr f32 MAX_FALL = -100.0f;
    constexpr f32 RISE_DAMPING = 0.9f;
    constexpr f32 FAST_RISE = 100.0f;
    constexpr f32 FAST_RISE_DAMPING = 0.125f;

    m_pos += m_vel;

    KCLTypeMask mask = m_floats ? 0x6CEBDBFF : 0x6CEBDFFF;
    bool hit = checkSphereFull(m_kclRadius, mask);
    bool airReset = false;
    bool wallHit = false;
    auto *colDir = Field::CollisionDirector::Instance();

    if (hit && (m_kclMask & KCL_ITEM_SOLID) &&
            colDir->findClosestCollisionEntry(&m_kclMask, KCL_ITEM_SOLID)) {
        if (m_colInfo.floorNrm.y < STEEP_FLOOR_Y || (m_kclMask & 0x80000)) {
            if (m_frame > BOUNCE_LIMIT_FRAME && m_limitBounces && m_airTimer < 1) {
                --m_bouncesLeft;
            }

            m_airTimer = AIR_FRAMES;
            airReset = true;
        }
    }

    if (hit && (m_kclMask & 0x10000)) {
        m_touchedBoundary = true;
    }

    if (m_airTimer > 0) {
        Field::CollisionInfo wallInfo;
        wallInfo.bbox.setZero();
        wallInfo.floorDist = -std::numeric_limits<f32>::min();
        wallInfo.wallDist = -std::numeric_limits<f32>::min();
        wallInfo.movingFloorDist = -std::numeric_limits<f32>::min();
        wallInfo.perpendicularity = 0.0f;
        KCLTypeMask wallMask = KCL_NONE;

        wallHit = colDir->checkSphereFullPush(m_kclRadius, m_pos, m_prevPos, KCL_ITEM_WALL,
                &wallInfo, &wallMask, 0);
        if (wallHit) {
            hit = true;
            EGG::Vector3f nrm;
            f32 dist = NormaliseLength(nrm, wallInfo.tangentOff);
            m_colInfo.bbox.min = m_colInfo.bbox.min.minimize(wallInfo.tangentOff);
            m_colInfo.bbox.max = m_colInfo.bbox.max.maximize(wallInfo.tangentOff);

            if (wallInfo.wallNrm.y >= CEILING_Y) {
                if (m_colInfo.floorDist < dist) {
                    m_colInfo.floorDist = dist;
                    m_colInfo.floorNrm = nrm;
                }
            } else {
                wallHit = false;
                if (m_dir.y > 0.0f) {
                    m_dir.y = -m_dir.y;
                }

                m_flags.resetBit(eFlags::OwnerImmune);
                if (m_limitBounces) {
                    m_frameFlags.setBit(eFrameFlags::WallHit);
                    --m_bouncesLeft;
                }

                if (m_colInfo.wallDist > -std::numeric_limits<f32>::min()) {
                    f32 perp = 1.0f - m_colInfo.wallNrm.ps_dot(nrm);
                    if (m_colInfo.perpendicularity < perp) {
                        m_colInfo.perpendicularity = std::min(perp, 1.0f);
                    }
                }

                if (m_colInfo.wallDist < dist) {
                    m_colInfo.wallDist = dist;
                    m_colInfo.wallNrm = nrm;
                }
            }

            m_colInfo.tangentOff = m_colInfo.bbox.min + m_colInfo.bbox.max;
        }
    }

    if (hit) {
        if (!m_floats) {
            m_pos += m_colInfo.tangentOff;
        } else if (m_colInfo.tangentOff.y > 0.0f) {
            m_pos.y += m_colInfo.tangentOff.y;
        }

        if ((m_kclMask & KCL_ITEM_SOLID) || wallHit) {
            const EGG::Vector3f &floorNrm = m_colInfo.floorNrm;
            if (m_airTimer < 1) {
                m_moveDir = m_dir - floorNrm * m_dir.ps_dot(floorNrm);
                NormaliseInto(m_moveDir, m_moveDir);
                m_dir = m_moveDir;
            } else if (!m_limitBounces || m_bouncesLeft >= 0) {
                m_moveDir = m_vel - floorNrm * m_vel.ps_dot(floorNrm);
                NormaliseInto(m_moveDir, m_moveDir);
                if (m_moveDir.x * m_moveDir.x + m_moveDir.z * m_moveDir.z >= MIN_DIR_XZ) {
                    m_dir = m_moveDir;
                }
            } else {
                reflectOffWall(EGG::Vector3f(floorNrm.x, 0.0f, floorNrm.z));
                m_frameFlags.setBit(eFrameFlags::WallHit);
            }

            m_side = m_moveDir.cross(floorNrm);
            if (floorNrm.y >= CEILING_Y) {
                m_up = floorNrm;
            }
        }

        if (m_airTimer < 1 && (m_kclMask & KCL_ITEM_WALL)) {
            EGG::Vector3f wallNrm = m_colInfo.wallNrm;
            if (wallNrm.y >= WALL_VANISH_Y) {
                startVanish(true);
                return;
            }

            wallNrm.y = 0.0f;
            if (m_floats) {
                m_pos.x += m_colInfo.tangentOff.x;
                m_pos.z += m_colInfo.tangentOff.z;
            }

            if (m_dir.x * wallNrm.x + m_dir.z * wallNrm.z < 0.0f) {
                reflectOffWall(wallNrm);
                m_flags.resetBit(eFlags::OwnerImmune);
                if (m_limitBounces) {
                    m_frameFlags.setBit(eFrameFlags::WallHit);
                    --m_bouncesLeft;
                }

                if (m_wallTimer != 0) {
                    m_bouncesLeft = -1;
                }
            }
        }

        if (m_kclMask & (KCL_ITEM_SOLID | KCL_ITEM_WALL)) {
            m_vel = m_moveDir * m_speed;
        }

        if (m_frame > 2 && (m_kclMask & KCL_ITEM_SOLID) && !(m_kclMask & KCL_ITEM_WALL)) {
            f32 rise = m_pos.y - m_prevPos.y;
            if (m_airTimer < 1 && rise > MAX_STEP_UP) {
                rise = MAX_STEP_UP;
            }

            if (m_vel.y < rise) {
                m_vel.y = rise;
            }
        }

        if (m_bouncesLeft < 0) {
            startBreak(true);
            return;
        }

        if ((m_kclMask & 0x20000) && m_kind != ItemObjKind::BlueShell) {
            startVanish(true);
            return;
        }
    }

    if (m_airTimer < 1) {
        m_speed += SPEED_RATE * (targetSpeed - m_speed);
    }

    if (hit && !airReset && !wallHit && m_airTimer > 0) {
        --m_airTimer;
    }

    if (!(m_kclMask & KCL_ITEM_SOLID) || m_airTimer > 0) {
        if (m_vel.y <= 0.0f) {
            m_vel.y -= FALL_GRAVITY;
            if (m_flags.onBit(eFlags::Airborne) && m_vel.y < MAX_FALL) {
                m_vel.y = MAX_FALL;
            }
        } else {
            m_vel.y -= RISE_GRAVITY;
            if (m_flags.onBit(eFlags::Airborne)) {
                m_vel.y *= m_vel.y > FAST_RISE ? FAST_RISE_DAMPING : RISE_DAMPING;
            }
        }
    } else {
        m_vel -= m_up * FLOOR_PULL;
    }
}

/// @addr{0x807B62B8}
/// @brief The shell's wall callback: it bounced off another item or a course object, whose push
/// direction is `nrm`. A push without an upward part only turns the shell sideways. The sound is
/// left out.
void ItemObjKoura::onWallBounce(const EGG::Vector3f &nrm) {
    if (nrm.y > 0.0f) {
        reflectOffWall(nrm);
    } else {
        reflectOffWall(EGG::Vector3f(nrm.x, 0.0f, nrm.z));
    }

    m_vel = m_moveDir * m_speed;
}

/// @brief The wall reflection of 0x807B54F4 and of the wall callback 0x807B62B8.
void ItemObjKoura::reflectOffWall(const EGG::Vector3f &nrm) {
    constexpr f32 UP_DAMPING = 0.7f;

    f32 prevY = m_dir.y;
    EGG::Vector3f proj = nrm * m_dir.ps_dot(nrm);
    m_dir -= proj;
    if (m_reflects) {
        m_dir -= proj;
    }

    if (m_dir.y > 0.0f && -prevY < m_dir.y) {
        m_dir.y += UP_DAMPING * (-prevY - m_dir.y);
    }

    NormaliseInto(m_dir, m_dir);
    m_side = m_dir.cross(m_up);
    m_moveDir = m_dir;
}

/// @addr{0x807B61CC}
/// @brief The gravity part of the shell movement, on its own (used by the homing red shell).
void ItemObjKoura::calcGravity() {
    constexpr f32 FLOOR_PULL = 2.5f;

    if (!(m_kclMask & KCL_ITEM_SOLID) || m_airTimer > 0) {
        calcGravityY();
    } else {
        m_vel -= m_up * FLOOR_PULL;
    }
}

/// @addr{0x807B6148}
void ItemObjKoura::calcGravityY() {
    constexpr f32 FALL_GRAVITY = 2.0f;
    constexpr f32 RISE_GRAVITY = 4.2f;
    constexpr f32 MAX_FALL = -100.0f;
    constexpr f32 RISE_DAMPING = 0.9f;
    constexpr f32 FAST_RISE = 100.0f;
    constexpr f32 FAST_RISE_DAMPING = 0.125f;

    if (m_vel.y <= 0.0f) {
        m_vel.y -= FALL_GRAVITY;
        if (m_flags.onBit(eFlags::Airborne) && m_vel.y < MAX_FALL) {
            m_vel.y = MAX_FALL;
        }
    } else {
        m_vel.y -= RISE_GRAVITY;
        if (m_flags.onBit(eFlags::Airborne)) {
            m_vel.y *= m_vel.y > FAST_RISE ? FAST_RISE_DAMPING : RISE_DAMPING;
        }
    }
}

/// @addr{0x807B532C}
/// @brief Back to plain sliding: direction and speed from the current velocity.
void ItemObjKoura::freeFromVelocity() {
    m_speed = NormaliseLength(m_dir, m_vel);
    m_moveDir = m_dir - m_up * m_dir.ps_dot(m_up);
    NormaliseInto(m_moveDir, m_moveDir);
    m_dir = m_moveDir;
    m_side = m_moveDir.cross(m_up);
}

ItemObjGreenShell::ItemObjGreenShell() = default;

ItemObjGreenShell::~ItemObjGreenShell() = default;

/// @addr{0x807AEB98}
/// @brief Thrown forward (9 bounces) unless the stick is held down: then backwards (4 bounces).
bool ItemObjGreenShell::onRelease(ItemTrail &trail) {
    constexpr f32 FORWARD_SPEED = 85.0f;
    constexpr f32 BACK_SPEED = 50.0f;
    constexpr s32 FORWARD_BOUNCES = 9;
    constexpr s32 BACK_BOUNCES = 4;

    if (!trail.kartItem()->stickDown()) {
        initThrow(FORWARD_SPEED, trail, FORWARD_BOUNCES, true, false);
        return true;
    }

    initThrow(BACK_SPEED, trail, BACK_BOUNCES, false, false);
    return false;
}

/// @addr{0x807AEC64}
/// @brief A thrown green shell: 1500 frames of sliding at speed 105; after 900 frames its next
/// wall bounce breaks it.
void ItemObjGreenShell::calcGreen() {
    constexpr s32 LAST_BOUNCE_FRAME = 900;
    constexpr s32 LIFETIME = 1501;
    constexpr f32 SPEED = 105.0f;

    if (m_frame > LAST_BOUNCE_FRAME) {
        m_bouncesLeft = 0;
    }

    if (m_frame < LIFETIME) {
        calcMove(SPEED);
        // 0x807B60A0 (spin and tilt of the model) and the rolling sound are visual.
    } else {
        startBreak(true);
    }
}

ItemObj::StateFunc ItemObjGreenShell::initialState() const {
    return static_cast<StateFunc>(&ItemObjGreenShell::calcGreen);
}

} // namespace Kinoko::Item
