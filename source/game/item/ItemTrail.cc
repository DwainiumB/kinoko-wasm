#include "ItemTrail.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemMath.hh"
#include "game/item/KartItem.hh"

#include "game/field/BoxColManager.hh"
#include "game/field/CollisionDirector.hh"

#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"
#include "game/kart/KartParam.hh"

namespace Kinoko::Item {

namespace {

/// @addr{0x807912E0}
/// @brief Makes right / up / forward orthonormal again, keeping up (PSVECNormalize and
/// PSVECCrossProduct).
void Orthonormalise(std::array<EGG::Vector3f, 4> &basis) {
    NormaliseInto(basis[1], basis[1]);
    basis[0] = basis[1].ps_cross(basis[2]);
    NormaliseInto(basis[0], basis[0]);
    basis[2] = basis[0].ps_cross(basis[1]);
}

/// @brief Status bits 32 (in an action), 47 and 54 (zipper and trick rotation): the game's
/// 0x807BD78C.
bool IsBusy(const Kart::KartObjectProxy &proxy) {
    return proxy.status().onBit(Kart::eStatus::InAction, Kart::eStatus::ZipperTrick,
            Kart::eStatus::TrickRot);
}

/// @brief `v += (target - v) * rate` with the multiply and add fused per component, as the game's
/// paired-single code does it (`ps_sub` then `ps_madds0`): the trail's up blend (0x80791EBC) and
/// the wall offsets (0x80792984, 0x80793C7C, 0x807944A0).
void LerpFused(EGG::Vector3f &v, const EGG::Vector3f &target, f32 rate) {
    EGG::Vector3f diff = target - v;
    v.x = EGG::Mathf::fma(diff.x, rate, v.x);
    v.y = EGG::Mathf::fma(diff.y, rate, v.y);
    v.z = EGG::Mathf::fma(diff.z, rate, v.z);
}

} // namespace

ItemTrail::ItemTrail()
    : m_kartItem(nullptr), m_playerIdx(0), m_speed(0.0f), m_speedRatio(0.0f), m_hopHeight(0.0f),
      m_movingForward(false), m_bob(0.0f), m_trailDist(0.0f), m_trailHeight(0.0f),
      m_trailDistShort(0.0f), m_trailHeightOfs(0.0f), m_trailUpOfs(0.0f), m_chainSlackBase(0.0f),
      m_chainSlack{}, m_lift(0.0f), m_liftDelta(0.0f), m_followY(0.0f), m_followXZ(0.0f),
      m_chainLiftRing{}, m_chainRingIdx(0), m_forcedRelease(false), m_orbitAngle(0),
      m_orbitSpeed(0), m_orbitRadiusSide(0.0f), m_orbitRadiusFwd(0.0f), m_orbitLift(0.0f),
      m_orbitSlots{} {
    reset();
}

ItemTrail::~ItemTrail() = default;

/// @addr{0x80791840}
void ItemTrail::init(KartItem *kartItem, size_t playerIdx) {
    constexpr f32 KART_SHORT = 0.9f;
    constexpr f32 BIKE_SHORT = 0.8f;

    m_kartItem = kartItem;
    m_playerIdx = static_cast<u8>(playerIdx);
    apply(playerIdx);

    const auto &stats = param()->stats();
    m_trailHeight = stats.itemUnk174;
    m_trailDist = stats.itemUnk170;
    m_trailDistShort = m_trailDist * (isBike() ? BIKE_SHORT : KART_SHORT);
    m_trailHeightOfs = stats.itemUnk178;
    m_trailUpOfs = stats.itemUnk17c;
    // Neither the constructor (0x80791760) nor this sets the scale: it is zero until the first
    // calcKartPose, which takes the forward offset (0x80791FD0) with the old scale. So the first
    // item of the race is placed without that offset on its first frame.
    m_scale = EGG::Vector3f::zero;
    reset();
}

/// @brief The inlined reset after the last held item is gone.
void ItemTrail::reset() {
    m_objKind = ItemObjKind::None;
    m_itemId = ItemId::NONE;
    m_mode = Mode::None;
    m_objs.fill(nullptr);
    m_count = 0;
    m_dropTimer = 0;
}

const Kart::KartObject *ItemTrail::kartObject() const {
    return Kart::KartObjectManager::Instance()->object(m_playerIdx);
}

/// @addr{0x80791910}
/// @brief Uses the inventory's item: instant items call their use function, the others are
/// spawned as objects held behind the kart.
void ItemTrail::use() {
    constexpr f32 TRAIL_OFFSET = 15.0f;

    auto &inventory = m_kartItem->inventory();
    ItemId id = inventory.id();
    const auto &behaviour = ItemInventory::Behaviour(id);

    if (behaviour.mode == static_cast<s32>(Mode::Instant)) {
        m_kartItem->useItem();
        return;
    }

    if (!ItemDirector::Instance()->holder(behaviour.objKind).canSpawn()) {
        // The kinds without objects are not ported.
        return;
    }

    m_objKind = behaviour.objKind;
    m_count = inventory.currentCount();
    m_mode = static_cast<Mode>(behaviour.mode);
    m_itemId = id;
    m_forcedRelease = false;

    ItemDirector::Instance()->holder(m_objKind).spawn(m_count, m_objs.data(), m_playerIdx,
            m_pose[3]);

    m_bob = 0.0f;
    const EGG::Matrix34f &mat = pose();
    m_pose[0] = EGG::Vector3f(mat[0, 0], mat[1, 0], mat[2, 0]);
    m_pose[1] = EGG::Vector3f(mat[0, 1], mat[1, 1], mat[2, 1]);
    m_pose[2] = EGG::Vector3f(mat[0, 2], mat[1, 2], mat[2, 2]);
    m_pose[3] = EGG::Vector3f(mat[0, 3], mat[1, 3], mat[2, 3]);
    static_cast<void>(calcKartPose());
    m_posDelta.setZero();

    // The mode's init (table 0x808D18D0).
    if (m_mode == Mode::Single) {
        initSingle();
    } else if (m_mode == Mode::Chain) {
        initChain();
    } else if (m_mode == Mode::Orbit) {
        initOrbit();
    }

    // The game clears the inventory without dropping anything (0x807BC9C0).
    inventory.clear();

    for (s32 i = 0; i < m_count; ++i) {
        ItemObj *obj = m_objs[i];

        // 0x8079EA58
        obj->m_prevPos = obj->m_pos;
        obj->m_trailFrame = 0;
        obj->m_vel = m_velocity;
        obj->setScaleTrailVec(m_scale);
        obj->m_flags.setBit(ItemObj::eFlags::Trailing);
        m_trailOffsets[i].setZero();
    }

    m_trailOffsets[0] = m_pose[1] * (TRAIL_OFFSET * m_scale.z);
}

/// @addr{0x8079231C}
void ItemTrail::calc() {
    if (calcKartPose()) {
        for (s32 i = 0; i < m_count; ++i) {
            m_objs[i]->setScaleTrailVec(m_scale);
        }
    }

    // The mode's calc (table 0x808D1900).
    if (m_mode == Mode::Single) {
        calcSingle();
    } else if (m_mode == Mode::Chain) {
        calcChain();
    } else if (m_mode == Mode::Orbit) {
        calcOrbit();
    }

    calcCollision();

    if (m_count == 0) {
        reset();
    }

    if (m_dropTimer > 0) {
        --m_dropTimer;
        if (m_dropTimer == 8 || m_dropTimer == 4 || m_dropTimer == 0) {
            if (m_count > 0) {
                s32 countBefore = m_count;
                ItemObj *obj = m_objs[--m_count];
                ItemDirector::Instance()->holder(obj->m_kind).activate(obj);
                obj->startRelease(pos());
                obj->m_flags.resetBit(ItemObj::eFlags::Trailing);
                obj->placeDropped(static_cast<u32>(countBefore), pos(), velocity(),
                        move()->smoothedUp());
                obj->onDrop();
            }

            if (m_count == 0) {
                reset();
            }
        }
    }
}

/// @addr{0x80791E5C}
/// @brief Caches the kart's pose for the held items.
/// @return Whether the kart's scale changed.
bool ItemTrail::calcKartPose() {
    constexpr f32 UP_RATE = 0.2f;
    constexpr f32 BOB_DECAY = 0.85f;

    const auto *kartMove = move();
    EGG::Vector3f up = IsBusy(*this) ? bodyUp() : kartMove->smoothedUp();
    LerpFused(m_pose[1], up, UP_RATE);
    m_pose[2] = bodyFront();
    Orthonormalise(m_pose);

    EGG::Vector3f prev = m_pose[3];
    m_pose[3] = pos();

    // The game lifts the held item with the wheelie (status bit 29) from three floats this
    // object never sets in a race; only the decay is ported.
    m_bob *= BOB_DECAY;

    // The bob is added before the forward offset (0x80791FDC).
    f32 ofs = m_trailUpOfs * m_scale.z;
    m_pose[3].y += m_bob;
    m_pose[3] += m_pose[2] * ofs;
    m_posDelta = m_pose[3] - prev;

    m_velocity = velocity();
    m_speed = kartMove->speed();
    m_speedRatio = kartMove->speedRatioCapped();
    m_hopHeight = kartMove->hopPosY();
    m_movingForward = m_pose[2].ps_dot(m_velocity) > 0.0f || IsBusy(*this);

    const EGG::Vector3f &kartScale = scale();
    bool changed = kartScale.x != m_scale.x || kartScale.y != m_scale.y || kartScale.z != m_scale.z;
    if (changed) {
        m_scale = kartScale;
    }

    return changed;
}

/// @addr{0x80792B08}
void ItemTrail::initSingle() {
    constexpr f32 TRAIL_BASE = 40.0f;
    constexpr f32 FOLLOW = 0.1f;

    m_smoothedBack = m_pose[2];
    m_trailDir = m_smoothedBack * -1.0f;
    f32 dist = (TRAIL_BASE + m_trailDistShort) * m_scale.z;
    m_trailAnchor = m_pose[3] + m_trailDir * dist;

    ItemObj *obj = m_objs[0];
    f32 height = obj->m_kclRadius + m_trailHeightOfs * m_scale.y;
    obj->m_pos = m_trailAnchor + m_pose[1] * height;

    m_followY = FOLLOW;
    m_followXZ = FOLLOW;
    m_liftDelta = 0.0f;
    m_lift = 0.0f;
    // The object's model basis is visual.
}

/// @addr{0x80792618}
/// @brief Places the held item behind the kart, lagging a little behind its turns.
void ItemTrail::calcTrailPos() {
    constexpr f32 DELTA_SCALE = 0.002f;
    constexpr f32 MAX_Y = 0.3f;
    constexpr f32 FOLLOW_FAST = 0.27f;
    constexpr f32 FOLLOW_MIN = 0.1f;
    constexpr f32 FOLLOW_DECAY = 0.01f;
    constexpr f32 TRAIL_BASE = 40.0f;
    constexpr f32 SINGLE_LIFT = 0.82f;
    constexpr f32 SINGLE_LIFT_DECAY = 0.59f;
    constexpr f32 MULTI_LIFT = 0.2f;
    constexpr f32 MULTI_LIFT_DECAY = 0.85f;
    constexpr f32 OFFSET_DECAY = 0.9f;
    constexpr f32 OFFSET_RATE = 0.2f;

    ItemObj *obj = m_objs[0];
    const auto &status = KartObjectProxy::status();

    EGG::Vector3f delta;
    if (status.onBit(Kart::eStatus::InCannon)) {
        delta.setZero();
    } else if (status.onBit(Kart::eStatus::StickyRoad)) {
        delta = m_posDelta - m_pose[2] * m_posDelta.ps_dot(m_pose[2]);
    } else {
        delta = m_posDelta;
    }

    EGG::Vector3f d = delta * DELTA_SCALE;
    d.y = std::min(std::max(d.y, -MAX_Y), MAX_Y);
    EGG::Vector3f target = m_pose[2] + d;

    if (!IsBusy(*this)) {
        if (status.offBit(Kart::eStatus::GroundStart)) {
            m_followY = std::max(m_followY - FOLLOW_DECAY, FOLLOW_MIN);
        } else {
            m_followY = FOLLOW_FAST;
        }

        m_followXZ = std::max(m_followXZ - FOLLOW_DECAY, FOLLOW_MIN);
    } else {
        m_followXZ = FOLLOW_FAST;
        m_followY = FOLLOW_FAST;
    }

    m_smoothedBack.x += m_followXZ * (target.x - m_smoothedBack.x);
    m_smoothedBack.y += m_followY * (target.y - m_smoothedBack.y);
    m_smoothedBack.z += m_followXZ * (target.z - m_smoothedBack.z);

    EGG::Vector3f back = m_smoothedBack;
    NormaliseInto(back, m_smoothedBack);
    m_trailDir = back * -1.0f;

    f32 dist = (TRAIL_BASE + m_trailDistShort) * m_scale.z;
    m_trailAnchor = m_pose[3] + m_trailDir * dist;

    f32 prevLift = m_lift;
    if (m_mode < Mode::Chain) {
        m_lift = (m_lift + SINGLE_LIFT * m_hopHeight) * SINGLE_LIFT_DECAY;
    } else {
        m_lift = (m_lift + MULTI_LIFT * m_hopHeight) * MULTI_LIFT_DECAY;
    }
    m_liftDelta = m_lift - prevLift;

    // Grouped as the game adds it (0x807928E0..0x80792918); left to right rounds differently.
    f32 height = obj->m_kclRadius + ((m_lift - m_hopHeight) + m_trailHeightOfs * m_scale.y);
    obj->m_pos = m_trailAnchor + m_pose[1] * height;

    obj->m_colInfo.bbox.setZero();
    bool hit = Field::CollisionDirector::Instance()->checkSphereFull(obj->m_kclRadius, obj->m_pos,
            obj->m_prevPos, 0x20E80FFF, &obj->m_colInfo, nullptr, 0);
    if (!hit) {
        m_trailOffsets[0] *= OFFSET_DECAY;
    } else {
        LerpFused(m_trailOffsets[0], obj->m_colInfo.tangentOff, OFFSET_RATE);
    }

    obj->m_pos += m_trailOffsets[0];
    // The object's model basis (0x80792A30..) is visual.
}

/// @addr{0x80792C68}
/// @brief The single held item: follows the kart, grows, and is released on the button.
void ItemTrail::calcSingle() {
    constexpr s32 GROW_DELAY = 12;
    constexpr f32 GROW_STEP = 0.2f;

    ItemObj *obj = m_objs[0];
    calcTrailPos();

    if (obj->m_trailFrame > GROW_DELAY &&
            obj->growScale(ItemObj::Properties(m_objKind).trailScale, GROW_STEP)) {
        obj->setScaleTrailVec(m_scale);
    }

    if (m_kartItem->releaseRequested() && m_objKind != ItemObjKind::None && m_dropTimer == 0 &&
            !m_forcedRelease) {
        releaseLast();
    }

    m_kartItem->armRelease();
}

/// @brief The release part of 0x80792C68.
void ItemTrail::releaseLast() {
    ItemObj *obj = m_objs[--m_count];
    releaseObj(obj);

    // 0x80798928
    if (obj->m_flags2.offBit(ItemObj::eFlags2::Online)) {
        f32 scale = ItemObj::Properties(m_objKind).trailScale * 0.5f;
        obj->m_scale = scale;
        obj->setScale(scale);
    }

    obj->m_flags2.setBit(ItemObj::eFlags2::Releasing);
    obj->m_releasePos = obj->m_pos;
}

/// @brief Puts a held item on the course in its released state (shared by the throws and the
/// chain's detach).
void ItemTrail::releaseObj(ItemObj *obj) {
    ItemDirector::Instance()->holder(obj->m_kind).activate(obj);
    obj->startRelease(pos());
    obj->m_flags.resetBit(ItemObj::eFlags::Trailing);

    if (!obj->onRelease(*this)) {
        obj->m_flags.resetBit(ItemObj::eFlags::FromKart);
    }

    if (obj->m_flags.offBit(ItemObj::eFlags::Breaking, ItemObj::eFlags::Vanishing)) {
        obj->m_state = obj->initialState();
    }
}

/// @addr{0x80791CC8}
void ItemTrail::calcCollision() {
    constexpr f32 MIN_SCALE = 0.8f;
    constexpr f32 TILTED_UP = 0.997f;

    for (s32 i = 0; i < m_count; ++i) {
        // 0x8079EBBC
        ItemObj *obj = m_objs[i];
        obj->m_vel = obj->m_pos - obj->m_prevPos;
        ++obj->m_trailFrame;
        obj->m_prevPos = obj->m_pos;
        if (m_dropTimer == 0) {
            obj->m_up = m_pose[1];
            if (obj->m_up.y < TILTED_UP) {
                obj->m_frameFlags.setBit(ItemObj::eFrameFlags::UpSet);
            }
        }
    }

    for (s32 i = m_count - 1; i >= 0; --i) {
        ItemObj *obj = m_objs[i];
        if (!obj->m_boxColUnit) {
            if (obj->m_scale >= MIN_SCALE && obj->properties().trailHitRadius != 0.0f) {
                obj->insertBoxCol(true);
            }
        } else {
            Field::BoxColFlag flags;
            flags.setBit(Field::eBoxColFlag::Item, Field::eBoxColFlag::Object);
            obj->m_boxColUnit->search(flags);
            obj->calcItemCollision();
            obj->calcObjectCollision();
        }
    }
}

/// @addr{0x80795350}
/// @brief The kart was hit: its held items drop over the next frames.
void ItemTrail::startDrop() {
    constexpr s32 DROP_FRAMES = 10;

    m_dropTimer = DROP_FRAMES;
}

/// @brief A held item hit something and leaves the trail (the mode's detach, table 0x808D1930).
void ItemTrail::detach(ItemObj *obj) {
    for (s32 i = 0; i < m_count; ++i) {
        if (m_objs[i] != obj) {
            continue;
        }

        if (m_mode == Mode::Chain) {
            detachChain(i, obj);
        } else if (m_mode == Mode::Orbit) {
            detachOrbit(i, obj);
        } else {
            // 0x80793164
            --m_count;
            ItemDirector::Instance()->holder(obj->m_kind).activate(obj);
            obj->startRelease(pos());
            obj->m_flags.resetBit(ItemObj::eFlags::Trailing);
        }

        if (m_count == 0) {
            reset();
        }

        return;
    }
}

/// @addr{0x80793F74}
/// @brief The bananas behind the hit one come loose and are released, each flung up a little more.
/// Online objects (flags2 bit 5, not ported) swap with the last one instead.
void ItemTrail::detachChain(s32 idx, ItemObj *obj) {
    constexpr f32 FLING = 3.0f;

    s32 released = 0;
    while (idx + 1 < m_count) {
        ItemObj *last = m_objs[--m_count];
        releaseObj(last);

        if (released++ > 0) {
            last->m_vel += last->m_up * (FLING * static_cast<f32>(released));
        }
    }

    --m_count;
    ItemDirector::Instance()->holder(obj->m_kind).activate(obj);
    obj->startRelease(pos());
    obj->m_flags.resetBit(ItemObj::eFlags::Trailing);
}

/// @addr{0x80794AB0}
/// @brief The hit shell trades places (and its place on the circle) with the last one.
void ItemTrail::detachOrbit(s32 idx, ItemObj *obj) {
    s32 last = m_count - 1;
    if (idx != last) {
        std::swap(m_objs[idx], m_objs[last]);
        std::swap(m_orbitSlots[idx], m_orbitSlots[last]);
    }

    --m_count;
    ItemDirector::Instance()->holder(obj->m_kind).activate(obj);
    obj->startRelease(pos());
    obj->m_flags.resetBit(ItemObj::eFlags::Trailing);
}

/// @addr{0x8079ED1C}
/// @brief Grows a held item towards its trail scale.
void ItemTrail::growHeld(ItemObj *obj, f32 step) {
    if (obj->growScale(ItemObj::Properties(m_objKind).trailScale, step)) {
        obj->setScaleTrailVec(m_scale);
    }
}

/// @addr{0x807931DC}
/// @brief Triple bananas: the first hangs like a single one, the others start on its anchor.
void ItemTrail::initChain() {
    constexpr f32 SLACK = 0.93f;

    initSingle();

    m_chainSlackBase = SLACK;
    for (s32 i = 1; i < m_count; ++i) {
        m_objs[i]->m_pos = m_objs[0]->m_pos;
        m_chainLinks[i - 1] = m_trailAnchor;
        m_chainSlack[i - 1] = m_chainSlackBase;
    }

    m_chainPrevDir = m_pose[2];
    m_chainRingIdx = 0;
    for (auto &ring : m_chainLiftRing) {
        ring.fill(0.0f);
    }
    // The objects' model bases are visual.
}

/// @addr{0x8079356C}
/// @brief Triple bananas: each one is dragged behind the one before it, swings out in turns and
/// copies the first one's hop a few frames later.
void ItemTrail::calcChain() {
    constexpr f32 SWING = 110.0f;
    constexpr f32 SWING_LINK = 15.0f;
    constexpr f32 LIFT_SPEED = 0.125f;
    constexpr f32 TRAIL_BASE = 40.0f;
    constexpr f32 LINK_SPACING = 35.0f;
    constexpr f32 BACK_STEP = 20.0f;
    constexpr f32 DROP_RATE = 0.4f;
    constexpr s32 LIFT_DELAY = 6;
    constexpr f32 SLACK_STEP = 0.15f;
    constexpr f32 GROW_STEP = 0.1f;
    constexpr f32 GROW_STEP_LINK = 0.05f;
    constexpr f32 OFFSET_DECAY = 0.9f;
    constexpr f32 OFFSET_RATE = 0.2f;

    calcTrailPos();
    growHeld(m_objs[0], GROW_STEP);

    EGG::Vector3f swing;
    f32 liftScale = 0.0f;
    u32 ringIdx = 0;
    if (m_count > 1) {
        if (m_movingForward) {
            swing = (m_chainPrevDir - m_trailDir) * (SWING * (m_speedRatio * m_speedRatio));
        }

        m_chainPrevDir = m_trailDir;
        ringIdx = m_chainRingIdx++ & 0xF;
        m_chainLiftRing[0][ringIdx] = m_lift;

        if (m_movingForward) {
            liftScale = std::min(LIFT_SPEED * m_speed, 1.0f);
        }
    }

    f32 back = (TRAIL_BASE + m_trailDistShort) * m_scale.z;
    f32 spacing = LINK_SPACING * m_scale.z;

    for (s32 i = 1; i < m_count; ++i) {
        ItemObj *obj = m_objs[i];
        EGG::Vector3f &link = m_chainLinks[i - 1];
        const EGG::Vector3f &prevLink = i == 1 ? m_trailAnchor : m_chainLinks[i - 2];
        f32 &slack = m_chainSlack[i - 1];
        f32 fi = static_cast<f32>(i);

        if (m_dropTimer != 0) {
            obj->m_vel += m_objs[i - 1]->m_vel;
        }

        if (m_movingForward) {
            link += swing * (SWING_LINK * fi);
        }

        if (slack > 0.0f) {
            link += m_posDelta * slack;
        }

        if (i > 1) {
            back -= BACK_STEP * static_cast<f32>(i - 1);
        }

        // Not closer to the kart than the trail distance.
        f32 push = back - (link - m_pose[3]).ps_dot(m_trailDir);
        if (push > 0.0f) {
            link += m_trailDir * push;
        }

        // Falls behind the one before it, but not faster.
        f32 drop = DROP_RATE * (prevLink.y - link.y);
        if (drop < 0.0f) {
            link.y += drop;
        }

        f32 lift = m_chainLiftRing[i - 1][(ringIdx - LIFT_DELAY) & 0xF];
        if (i == 1) {
            m_chainLiftRing[1][ringIdx] = lift;
        }

        EGG::Vector3f d = link - prevLink;
        EGG::Vector3f n = EGG::Vector3f::zero;
        f32 dist = NormaliseLength(n, d);

        if (dist > spacing) {
            link = prevLink + n * spacing;
            if (slack > 0.0f) {
                slack -= SLACK_STEP;
            }
        }
        // The objects' model bases (lerped towards the one before) are visual.

        // Grouped as the game adds it (0x80793C04..0x80793C14).
        f32 height = (lift - m_hopHeight) * liftScale +
                (obj->m_kclRadius + m_trailHeightOfs * m_scale.y);
        obj->m_pos = link + m_pose[1] * height;

        obj->m_colInfo.bbox.setZero();
        bool hit = Field::CollisionDirector::Instance()->checkSphereFull(obj->m_kclRadius,
                obj->m_pos, obj->m_prevPos, 0x20E80FFF, &obj->m_colInfo, nullptr, 0);
        EGG::Vector3f &ofs = m_trailOffsets[i];
        if (!hit) {
            ofs *= OFFSET_DECAY;
        } else {
            LerpFused(ofs, obj->m_colInfo.tangentOff, OFFSET_RATE);
        }

        obj->m_pos += ofs;
        growHeld(obj, GROW_STEP + GROW_STEP_LINK * fi);
    }

    if (m_kartItem->pressRequested() && m_objKind != ItemObjKind::None && m_dropTimer == 0 &&
            !m_forcedRelease) {
        releaseLast();
    }
}

/// @addr{0x8079455C}
/// @brief Triple shells: they start on the kart and spread out onto the circle.
void ItemTrail::initOrbit() {
    constexpr u16 START_SPEED = 600;

    m_orbitAngle = 0;
    m_orbitSpeed = START_SPEED;
    m_orbitRadiusSide = 0.0f;
    m_orbitRadiusFwd = 0.0f;
    m_orbitLift = 0.0f;
    for (s32 i = 0; i < m_count; ++i) {
        m_orbitSlots[i] = static_cast<u16>(i);
    }

    m_orbitCenter = m_pose[3] + m_pose[1] * (m_trailHeightOfs * m_scale.y);
    for (s32 i = 0; i < m_count; ++i) {
        m_objs[i]->m_pos = m_pose[3];
    }

    calcOrbitPositions();
}

/// @addr{0x80794694}
/// @brief Triple shells: they circle the kart faster and faster, the circle growing to the kart's
/// size; the button throws the last one.
void ItemTrail::calcOrbit() {
    constexpr f32 KART_SPEED_SCALE = 3.0f;
    constexpr f32 BUSY_SLOWDOWN = 0.7f;
    constexpr u16 MAX_SPEED = 1200;
    constexpr u16 ACCEL = 70;
    constexpr u16 MAX_KART_SPEED = 300;
    constexpr f32 HOP_LIFT = 0.4f;
    constexpr f32 LIFT_DECAY = 0.8f;
    constexpr f32 RADIUS_BASE = 30.0f;
    constexpr f32 RADIUS_RATE = 0.2f;
    constexpr f32 GROW_STEP = 0.1f;

    u16 kartSpeed = static_cast<u16>(static_cast<s32>(KART_SPEED_SCALE * m_speed));
    const auto &status = KartObjectProxy::status();
    if (status.offBit(Kart::eStatus::InAction)) {
        // 0x807BD768
        if (status.onBit(Kart::eStatus::ZipperTrick, Kart::eStatus::TrickRot)) {
            m_orbitSpeed = static_cast<u16>(
                    static_cast<s32>(static_cast<f32>(m_orbitSpeed) * BUSY_SLOWDOWN));
        }

        if (m_orbitSpeed < MAX_SPEED) {
            m_orbitSpeed += ACCEL;
        }

        m_orbitAngle += m_orbitSpeed;
        m_orbitAngle += std::min(kartSpeed, MAX_KART_SPEED);
    } else {
        m_orbitSpeed = 0;
    }

    m_orbitLift = (m_orbitLift + HOP_LIFT * m_hopHeight) * LIFT_DECAY;
    // Grouped as the game adds it (0x807947C0..0x807947FC).
    f32 height =
            m_objs[0]->m_kclRadius + ((m_orbitLift - m_hopHeight) + m_trailHeightOfs * m_scale.y);
    m_orbitRadiusSide +=
            RADIUS_RATE * (m_scale.z * (m_trailHeight + RADIUS_BASE) - m_orbitRadiusSide);
    m_orbitRadiusFwd += RADIUS_RATE * (m_scale.x * (m_trailDist + RADIUS_BASE) - m_orbitRadiusFwd);
    m_orbitCenter = m_pose[3] + m_pose[1] * height;

    calcOrbitPositions();

    for (s32 i = 0; i < m_count; ++i) {
        growHeld(m_objs[i], GROW_STEP);
        // The spin of the model is visual.
    }

    if (m_kartItem->pressRequested() && m_objKind != ItemObjKind::None && m_dropTimer == 0 &&
            !m_forcedRelease) {
        releaseLast();
    }
}

/// @addr{0x807941F0}
/// @brief Puts each shell on its third of the circle (an ellipse in the kart's plane), pushed out
/// of the course.
void ItemTrail::calcOrbitPositions() {
    constexpr f32 COS120 = -0.5f;
    constexpr f32 SIN120 = 0.8660254f; // 0x3F5DB3D7, not 0.866f
    constexpr f32 OFFSET_DECAY = 0.9f;
    constexpr f32 OFFSET_RATE = 0.2f;

    auto [sin, cos] = EGG::Mathf::SinCosFIdx(static_cast<f32>(m_orbitAngle) / 256.0f);
    EGG::Vector3f fwd = m_pose[2] * m_orbitRadiusFwd;
    EGG::Vector3f side = m_pose[0] * m_orbitRadiusSide;

    for (s32 i = 0; i < m_count; ++i) {
        ItemObj *obj = m_objs[i];

        f32 a;
        f32 b;
        switch (m_orbitSlots[i]) {
        case 0:
            a = cos;
            b = sin;
            break;
        case 1:
            a = COS120 * cos - SIN120 * sin;
            b = COS120 * sin + SIN120 * cos;
            break;
        default:
            a = COS120 * cos + SIN120 * sin;
            b = COS120 * sin - SIN120 * cos;
            break;
        }

        obj->m_pos = m_orbitCenter + fwd * a + side * b;

        obj->m_colInfo.bbox.setZero();
        bool hit = Field::CollisionDirector::Instance()->checkSphereFull(obj->m_kclRadius,
                obj->m_pos, obj->m_prevPos, 0x20E80FFF, &obj->m_colInfo, nullptr, 0);
        EGG::Vector3f &ofs = m_trailOffsets[i];
        if (!hit) {
            ofs *= OFFSET_DECAY;
        } else {
            LerpFused(ofs, obj->m_colInfo.tangentOff, OFFSET_RATE);
        }

        obj->m_pos += ofs;
    }
}

/// @addr{0x80794C44}
/// @brief Despawns the held items (respawn, race end).
void ItemTrail::clear() {
    m_forcedRelease = false;
    while (m_count > 0) {
        ItemObj *obj = m_objs[--m_count];
        ItemDirector::Instance()->holder(obj->m_kind).despawn(obj);
    }

    reset();
}

/// @addr{0x80794E88}
/// @brief Direction and speed of an item thrown forward.
f32 ItemTrail::calcThrowForward(f32 speed, f32 deltaScale, f32 dirScale, bool arc,
        EGG::Vector3f &dir) {
    constexpr f32 AIM_SCALE = 20.0f;

    f32 deltaLen = m_posDelta.length();
    EGG::Vector3f v;
    if (!m_movingForward) {
        v = m_pose[2];
        speed -= deltaLen;
    } else {
        v = m_pose[2] * dirScale;
        speed += deltaLen * deltaScale;
        if (v.ps_dot(m_posDelta) > 0.0f) {
            v += m_posDelta;
        }

        f32 aim = AIM_SCALE * -m_speedRatio * m_kartItem->smoothedStickX();
        v += m_pose[0] * aim;
    }

    finishThrowDir(arc, v, dir);
    return speed;
}

/// @addr{0x80795030}
/// @brief Direction and speed of an item thrown or dropped backwards.
f32 ItemTrail::calcThrowBack(f32 speed, f32 deltaScale, f32 dirScale, bool arc,
        EGG::Vector3f &dir) {
    f32 deltaLen = m_posDelta.length();
    EGG::Vector3f v;
    if (!m_movingForward) {
        speed += deltaLen * deltaScale;
        v = m_pose[2] * -1.0f;
    } else {
        f32 back = std::min(deltaLen, move()->baseSpeed());
        speed -= back;
        v = m_pose[2] * dirScale;
        v.x += m_posDelta.x;
        v.z += m_posDelta.z;
        v *= -1.0f;
    }

    finishThrowDir(arc, v, dir);
    return speed;
}

/// @addr{0x80794D1C}
void ItemTrail::finishThrowDir(bool arc, EGG::Vector3f &v, EGG::Vector3f &out) {
    constexpr f32 MIN_XZ = 0.0001f;
    constexpr f32 UP_BLEND = 0.2f;

    if (v.x * v.x + v.z * v.z < MIN_XZ) {
        v = m_pose[1];
    }

    if (!arc && KartObjectProxy::status().onBit(Kart::eStatus::StickyRoad)) {
        EGG::Vector3f n = move()->smoothedUp() + m_pose[2] * UP_BLEND;
        NormaliseInto(n, n);
        v = v - n * v.ps_dot(n);
    } else {
        // 0x807DF690: keep the pitch the kart item computed, scale the rest.
        f32 y = m_kartItem->throwPitch();
        f32 xz = v.x * v.x + v.z * v.z;
        if (xz <= 0.0f) {
            v = EGG::Vector3f(0.0f, y > 0.0f ? 1.0f : -1.0f, 0.0f);
        } else {
            f32 k = (1.0f - y * y) / xz;
            f32 s = k > 0.0f ? k * EGG::Mathf::frsqrt(k) : 0.0f;
            v = EGG::Vector3f(v.x * s, y, v.z * s);
        }
    }

    NormaliseInto(out, v);
}

} // namespace Kinoko::Item
