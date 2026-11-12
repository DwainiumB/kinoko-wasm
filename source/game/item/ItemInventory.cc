#include "ItemInventory.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemRaceFlags.hh"
#include "game/item/KartItem.hh"

#include "game/kart/KartMove.hh"
#include "game/kart/KartObject.hh"

#include "game/system/RaceManager.hh"

#include <egg/math/Math.hh>

#include <array>

namespace Kinoko::Item {

namespace {

/// @brief The game's runtime item behaviour table (0x809C36A0), written out by hand from the
/// static data `ItemProperties_init` (0x807BCAE0) copies in.
constexpr std::array<ItemInventory::ItemBehaviour, 19> ITEM_BEHAVIOURS = {{
        {true, ItemObjKind::GreenShell, 1, false, 1},    // green shell
        {true, ItemObjKind::RedShell, 1, false, 1},      // red shell
        {true, ItemObjKind::Banana, 1, false, 1},        // banana
        {true, ItemObjKind::FakeItemBox, 1, false, 1},   // fake item box
        {true, ItemObjKind::Mushroom, 1, false, 0},      // mushroom
        {true, ItemObjKind::Mushroom, 3, false, 0},      // triple mushroom
        {true, ItemObjKind::BobOmb, 1, false, 1},        // bob-omb
        {true, ItemObjKind::BlueShell, 1, false, 1},     // blue shell
        {true, ItemObjKind::Lightning, 1, false, 0},     // lightning
        {true, ItemObjKind::Star, 1, false, 0},          // star
        {true, ItemObjKind::GoldenMushroom, 1, true, 0}, // golden mushroom
        {true, ItemObjKind::MegaMushroom, 1, false, 0},  // mega mushroom
        {true, ItemObjKind::Blooper, 1, false, 0},       // blooper
        {true, ItemObjKind::PowBlock, 1, false, 0},      // POW
        {true, ItemObjKind::ThunderCloud, 1, false, 0},  // thunder cloud
        {true, ItemObjKind::BulletBill, 1, false, 0},    // bullet bill
        {true, ItemObjKind::GreenShell, 3, false, 3},    // triple green shells
        {true, ItemObjKind::RedShell, 3, false, 3},      // triple red shells
        {true, ItemObjKind::Banana, 3, false, 2},        // triple bananas
}};

/// @brief How the drops of one hit spread around the kart (0x808A6628): one row per count.
constexpr std::array<std::array<std::array<f32, 2>, 3>, 3> DROP_SPREAD = {{
        {{{1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}}},
        {{{-0.5f, 0.8660254f}, {-0.5f, -0.8660254f}, {0.0f, 0.0f}}},
        {{{1.0f, 0.0f}, {-0.5f, 0.8660254f}, {-0.5f, -0.8660254f}}},
}};

} // namespace

ItemInventory::ItemInventory()
    : m_kartItem(nullptr), m_currentId(ItemId::NONE), m_currentCount(0), m_dropTimer(0),
      m_hasTimer(false), m_timer(0), m_timerUse(0), m_replaced(false) {
    m_drops.fill(nullptr);
}

ItemInventory::~ItemInventory() = default;

/// @addr{0x807BC130}
void ItemInventory::init(KartItem *kartItem) {
    m_kartItem = kartItem;
}

/// @addr{0x807BC6E8}
/// @brief Drops the objects of a hit (one at 7, 5 and 3 frames into the countdown), then counts
/// down a timed item (the golden mushroom): when its timer runs out, one use is spent.
void ItemInventory::calc() {
    if (m_dropTimer > 0) {
        --m_dropTimer;
        if (m_dropTimer == 7 || m_dropTimer == 5 || m_dropTimer == 3) {
            dropOne();
            if (m_currentCount == 0) {
                clear();
            }
        }
    }

    if (!m_hasTimer || m_timer == 0 || m_dropTimer != 0) {
        return;
    }

    if (--m_timer == 0 && --m_currentCount == 0) {
        clear();
    }
}

/// @addr{0x807BC940}
/// @param replaced The roulette gave a mushroom instead of an item with no room (+0x29).
void ItemInventory::setItem(ItemId id, bool replaced) {
    const auto &behaviour = Behaviour(id);
    m_currentId = id;
    m_currentCount = behaviour.count;
    m_dropTimer = 0;
    m_replaced = replaced;
    m_hasTimer = behaviour.timed;
    m_timer = 0;
    m_timerUse = 0;
}

/// @addr{0x807BC97C}
void ItemInventory::useItem(int count) {
    m_currentCount -= count;
    if (m_currentCount != 0) {
        return;
    }

    clear();
}

/// @addr{0x807BC9C0}
void ItemInventory::clear() {
    m_currentId = ItemId::NONE;
    m_currentCount = 0;
    m_replaced = false;
    m_dropTimer = 0;
    m_drops.fill(nullptr);
    m_hasTimer = false;
    m_timer = 0;
    m_timerUse = 0;
}

/// @addr{0x807BC9F4}
/// @brief Clears, despawning objects still waiting to be dropped.
void ItemInventory::clearAndDespawn() {
    if (m_dropTimer != 0) {
        for (s32 i = 0; i < m_currentCount; ++i) {
            if (m_drops[i]) {
                ItemDirector::Instance()->holder(m_drops[i]->kind()).despawn(m_drops[i]);
            }
        }
    }

    clear();
}

/// @addr{0x807BC610}
/// @brief The kart was hit: the item falls off as objects over the next frames.
void ItemInventory::startDrop() {
    constexpr s32 DROP_FRAMES = 10;

    // An item the roulette gave in place of one with no room is not dropped.
    if (m_replaced) {
        clearAndDespawn();
        return;
    }

    if (!ItemDirector::Instance()->holder(Behaviour(m_currentId).objKind).canSpawn()) {
        // This item's objects are not ported: it is just lost.
        clear();
        return;
    }

    m_dropTimer = DROP_FRAMES;
    spawnDrop();
}

/// @addr{0x807BC350}
/// @brief Takes the dropped objects out of the pool with their outward velocities.
void ItemInventory::spawnDrop() {
    constexpr f32 ANGLE_SCALE = 1.0f / 256.0f;
    constexpr f32 SURVIVOR_DIST = 20.0f;
    constexpr f32 DIST = 15.0f;
    constexpr f32 JITTER = 0.2f;
    constexpr f32 DROP_SCALE = 0.05f;

    auto &random = ItemRaceFlags::Random();
    u16 angle = static_cast<u16>(random.getU32(0x10000));
    auto [sin, cos] = EGG::Mathf::SinCosFIdx(static_cast<f32>(angle) * ANGLE_SCALE);

    ItemObjKind kind = Behaviour(m_currentId).objKind;
    f32 dist = ItemObj::Properties(kind).survivesHits ? SURVIVOR_DIST : DIST;
    sin *= dist;
    cos *= dist;

    const auto *kart = m_kartItem->kartObject();
    s32 count = std::min(m_currentCount, 3);
    ItemDirector::Instance()->holder(kind).spawn(count, m_drops.data(),
            static_cast<u8>(m_kartItem->playerIdx()), kart->pos());

    const auto &row = DROP_SPREAD[count - 1];
    for (s32 i = 0; i < count; ++i) {
        f32 a = row[i][0] * (1.0f + random.getF32(JITTER));
        f32 b = row[i][1] * (1.0f + random.getF32(JITTER));
        f32 front = a * cos - b * sin;
        f32 side = b * cos + a * sin;

        ItemObj *obj = m_drops[i];
        obj->setDropVelocity(kart->bodyFront() * front + kart->bodyForward() * side, DROP_SCALE);
    }
}

/// @addr{0x807BC170}
void ItemInventory::dropOne() {
    constexpr f32 KART_VEL_SCALE = 0.7f;

    if (m_currentCount <= 0) {
        return;
    }

    ItemObj *obj = m_drops[--m_currentCount];
    if (!obj) {
        return;
    }

    const auto *kart = m_kartItem->kartObject();
    ItemDirector::Instance()->holder(obj->kind()).activate(obj);
    obj->dropFromKart(kart->pos(), kart->velocity(), kart->move()->smoothedUp(), KART_VEL_SCALE);
}

/// @addr{0x807BCAB0}
/// @brief Starts a timed item's timer on its first use.
/// @return Whether the timer was started now.
bool ItemInventory::startTimer(int frames) {
    if (m_timer != 0) {
        return false;
    }

    m_timer = frames;
    return true;
}

/// @addr{0x807BCAD0}
void ItemInventory::setTimerUse(int frames) {
    m_timerUse = m_timer - frames;
}

const ItemInventory::ItemBehaviour &ItemInventory::Behaviour(ItemId id) {
    ASSERT(static_cast<size_t>(id) < ITEM_BEHAVIOURS.size());
    return ITEM_BEHAVIOURS[static_cast<size_t>(id)];
}

} // namespace Kinoko::Item
