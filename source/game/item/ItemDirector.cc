#include "ItemDirector.hh"

#include "game/item/ItemObjThunderCloud.hh"
#include "game/item/ItemRaceFlags.hh"
#include "game/item/ItemSlotData.hh"

#include "game/field/BoxColManager.hh"
#include "game/field/ObjectDirector.hh"

#include "game/kart/KartObject.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::Item {

/// @addr{0x80799794}
void ItemDirector::init() {
    m_blooperManager.init();
    m_powManager.init();

    // The game gives the starting triple mushrooms only in Time Trials.
    if (!ItemRaceFlags::s_timeTrial) {
        return;
    }

    for (auto &kart : m_karts) {
        kart.inventory().setItem(ItemId::TRIPLE_MUSHROOM);
    }
}

/// @addr{0x80799850}
/// @brief The strong items' cooldowns count down, the blooper and the POW block act, the item
/// objects move, then the total item count is taken, then every racer's item button and roulette.
/// The online parts are not ported.
void ItemDirector::calc() {
    constexpr s32 TOTAL_LIMIT = 30;

    ItemSlotData::Instance()->calcTimers();
    // 0x807BD340 is not ported.
    m_blooperManager.calc();
    m_powManager.calc();

    for (auto &holder : m_holders) {
        if (holder.activeCount() != 0) {
            holder.calc();
        }
    }

    s32 total = 0;
    for (auto &holder : m_holders) {
        s32 count = holder.spawnedCount() + heldCount(holder.kind());
        total += (ItemObj::Properties(holder.kind()).weight * count) & 0xFFFF;
    }
    m_overLimit = total - TOTAL_LIMIT;

    for (auto &kart : m_karts) {
        kart.calc();
    }
}

/// @addr{0x8079A65C}
/// @brief The objects of a kind still inside racers' inventories.
s32 ItemDirector::heldCount(ItemObjKind kind) const {
    s32 count = 0;
    for (const auto &kart : m_karts) {
        const auto &inventory = kart.inventory();
        if (inventory.id() != ItemId::NONE &&
                ItemInventory::Behaviour(inventory.id()).objKind == kind) {
            count += inventory.currentCount();
        }
    }

    return count;
}

/// @addr{0x80799BE8}
/// @brief Whether the objects of a whole pickup of this item fit in its kind's pool.
bool ItemDirector::hasRoomFor(ItemId id) const {
    if (id == ItemId::NONE) {
        return false;
    }

    const auto &behaviour = ItemInventory::Behaviour(id);
    const auto &holder = m_holders[static_cast<size_t>(behaviour.objKind)];
    s32 inUse = holder.spawnedCount() + heldCount(behaviour.objKind);
    return behaviour.count <= holder.room() - inUse;
}

/// @addr{0x807B7C20}
/// @brief Shrinks every other racer. Team mates would be spared; Kinoko has no teams.
void ItemDirector::activateLightning(size_t userIdx) {
    for (size_t i = 0; i < m_karts.size(); ++i) {
        if (i != userIdx) {
            static_cast<void>(m_karts[i].hitByLightning());
        }
    }
}

/// @addr{0x8079A298}
/// @brief The roulette gave a thunder cloud: it spawns over the racer at once.
void ItemDirector::spawnThunderCloud(u8 playerIdx) {
    auto &kartItem = m_karts[playerIdx];
    auto &cloudHolder = holder(ItemObjKind::ThunderCloud);

    ItemObj *obj = nullptr;
    cloudHolder.spawn(1, &obj, playerIdx, kartItem.kartObject()->pos());
    if (!obj) {
        return;
    }

    cloudHolder.activate(obj);
    obj->startRelease(kartItem.kartObject()->pos());
    obj->setState(obj->initialState());

    kartItem.roulette().setPending(ItemId::THUNDER_CLOUD);
    // The online event is left out.
}

/// @addr{0x80799FBC}
/// @brief A course object (a broken wooden box) drops an item of this kind, owned by playerIdx.
/// @details The game skips remote racers (player type 4), which Kinoko does not have, and builds
/// the model's basis from the velocity (visual). Kinds whose objects are not ported drop nothing.
void ItemDirector::spawnFromObject(ItemObjKind kind, const EGG::Vector3f &pos,
        const EGG::Vector3f &vel, u8 playerIdx) {
    if (playerIdx >= m_karts.size()) {
        return;
    }

    // 0x8079A12C
    auto &objHolder = holder(kind);
    if (!objHolder.canSpawn()) {
        return;
    }

    s32 inUse = objHolder.spawnedCount() + heldCount(kind);
    if (objHolder.room() - inUse <= 0) {
        return;
    }

    ItemObj *obj = nullptr;
    objHolder.spawn(1, &obj, playerIdx, pos);
    if (!obj) {
        return;
    }

    obj->m_flags.setBit(ItemObj::eFlags::Direct);
    objHolder.activate(obj);
    obj->placeFromObject(pos, vel);
}

/// @addr{0x807A3618}
ItemObjBlastProbe::ItemObjBlastProbe() {
    m_flags2.setBit(eFlags2::Unk6);
}

ItemObjBlastProbe::~ItemObjBlastProbe() = default;

/// @brief The first stores of 0x807A3648: the blast's kind, centre and radius.
void ItemObjBlastProbe::set(f32 radius, const EGG::Vector3f &pos, ItemObjKind kind) {
    m_kind = kind;
    m_pos = pos;
    m_hitRadius = radius;
}

/// @addr{0x8079A654}
/// @brief A course object explodes (Bowser's fireballs, the Shy Guy ship's cannonballs and, in the
/// game, Moonview's bomb cars): every item whose hit sphere meets the blast is knocked away and
/// broken, then the course objects inside it react as to an item of `kind` (always a bob-omb in
/// the game's callers), through the stand-in item at +0x2A4 (0x807A3648).
/// @details The item search uses the item and object bits (0xE), as the items' own search does.
/// The object hits' reactions on the stand-in are not used. A wooden box broken this way credits
/// its drop to the stand-in's owner, player 0 (its flags never say "no owner").
void ItemDirector::explodeAt(f32 radius, const EGG::Vector3f &pos, ItemObjKind kind) {
    m_blastProbe.set(radius, pos, kind);

    Field::BoxColFlag flags;
    flags.setBit(Field::eBoxColFlag::Item, Field::eBoxColFlag::Object,
            Field::eBoxColFlag::Drivable);
    auto *boxColMgr = Field::BoxColManager::Instance();
    boxColMgr->search(radius, pos, flags);

    while (ItemObj *item = boxColMgr->getNextItem()) {
        f32 reach = m_blastProbe.m_hitRadius + item->m_hitRadius;
        if (!(m_blastProbe.m_pos.ps_sqDistance(item->m_pos) < reach * reach)) {
            continue;
        }

        s32 response = (kind == ItemObjKind::BobOmb || kind == ItemObjKind::BlueShell) ?
                2 :
                item->itemPairResponse(&m_blastProbe);
        item->calcItemPairResponse(response, m_blastProbe.m_pos, m_blastProbe.m_vel);
    }

    [[maybe_unused]] size_t hits = Field::ObjectDirector::Instance()->checkItemObjectCollision(
            m_blastProbe.m_hitRadius, &m_blastProbe, m_blastProbe.m_pos);
}

/// @addr{0x8079A578}
/// @brief Two racers bumped: a thunder cloud over the first one may move over to the second.
void ItemDirector::passThunderCloud(u8 fromIdx, u8 toIdx) {
    auto &cloudHolder = holder(ItemObjKind::ThunderCloud);
    for (s32 i = 0; i < cloudHolder.activeCount(); ++i) {
        auto *cloud = static_cast<ItemObjThunderCloud *>(cloudHolder.obj(i));
        if (cloud->carrier() == fromIdx) {
            cloud->passTo(toIdx);
        }
    }
}

/// @addr{0x80799CAC}
/// @brief Finds the items touching a kart, among those its box collision search found. Held
/// items that hit are detached from their owner, and every hit item reacts (breaks).
/// @return The number of items that hit.
size_t ItemDirector::checkKartCollision(const Kart::KartObject *kart,
        std::array<ItemObj *, MAX_KART_HITS> &hits) {
    size_t count = 0;
    auto *boxColMgr = Field::BoxColManager::Instance();

    while (ItemObj *obj = boxColMgr->getNextItem()) {
        if (!obj->checkKartCollision(kart, false)) {
            continue;
        }

        // 0x8079FF14: a dropped item that does not survive just vanishes.
        bool vanishes =
                obj->flags().onBit(ItemObj::eFlags::Dropped) && !obj->properties().survivesHits;
        if (vanishes) {
            obj->startVanish(false);
            continue;
        }

        hits[count] = obj;
        count = std::min(count + 1, MAX_KART_HITS - 1);

        if (obj->isTrailing()) {
            m_karts[obj->owner()].trail().detach(obj);
        }

        obj->onKartHit(kart, false);
    }

    return count;
}

/// @addr{0x80799138}
ItemDirector *ItemDirector::CreateInstance() {
    ASSERT(!s_instance);
    // The game fills these from its player model manager's constructor (0x8078CA5C), which runs
    // earlier in the scene; Kinoko has no models.
    ItemRaceFlags::Init();
    s_instance = EGG::egg_new<ItemDirector>();
    return s_instance;
}

/// @addr{0x80799188}
void ItemDirector::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
    ItemSlotData::DestroyInstance();
}

/// @addr{0x807992D8}
ItemDirector::ItemDirector() : m_overLimit(0) {
    ItemSlotData::CreateInstance();

    u16 globalIdx = 0;
    for (size_t i = 0; i < m_holders.size(); ++i) {
        m_holders[i].init(static_cast<ItemObjKind>(i), globalIdx);
    }

    size_t playerCount = System::RaceConfig::Instance()->raceScenario().playerCount;
    m_karts = owning_span<KartItem>(playerCount);

    for (size_t i = 0; i < playerCount; ++i) {
        m_karts[i].init(i);
    }
}

/// @addr{0x80798F9C}
ItemDirector::~ItemDirector() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("ItemDirector instance not explicitly handled!");
    }
}

ItemDirector *ItemDirector::s_instance = nullptr; ///< @addr{0x809C3618}

} // namespace Kinoko::Item
