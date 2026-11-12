#include "ItemSlotData.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemRaceFlags.hh"

#include "game/kart/KartObject.hh"
#include "game/kart/KartObjectManager.hh"
#include "game/kart/KartState.hh"

#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"
#include "game/system/ResourceManager.hh"

namespace Kinoko::Item {

namespace {

/// @brief Chance multiplier and offset per item category (behaviour +0x0C: 0 basic, 1 middle,
/// 2 strong) for the frantic (0x808A6300 / 0x808A630C) and strategic (0x808A6314 / 0x808A6320)
/// item modes, and for an online battle of 5 to 8 players (0x808A62EC / 0x808A62F8).
constexpr std::array<f32, 3> FRANTIC_FACTOR = {{0.25f, 1.0f, 4.0f}};
constexpr std::array<u16, 3> FRANTIC_OFFSET = {{3, 0, 0}};
constexpr std::array<f32, 3> STRATEGIC_FACTOR = {{8.0f, 0.25f, 0.0f}};
constexpr std::array<u16, 3> STRATEGIC_OFFSET = {{0, 3, 0}};
constexpr std::array<f32, 3> ONLINE_BATTLE_FACTOR = {{0.5f, 1.0f, 2.0f}};
constexpr std::array<u16, 3> ONLINE_BATTLE_OFFSET = {{1, 0, 0}};

/// @brief The item category (behaviour table +0x0C), from the game's static data.
constexpr std::array<s32, ItemSlotData::ITEM_COUNT> ITEM_CATEGORY = {{
        0,
        1,
        0,
        1,
        0,
        1,
        1,
        2,
        2,
        2,
        2,
        2,
        1,
        1,
        1,
        2,
        1,
        2,
        2,
}};

/// @brief 0x808A6320 + count * 0x18: which of the 12 rows a race of `count` players uses for each
/// of its places (1-based).
constexpr std::array<std::array<u16, ItemSlotData::MAX_RANKS>, ItemSlotData::MAX_RANKS + 1>
        PLACE_ROWS = {{
                {{0}},
                {{1}},
                {{1, 4}},
                {{1, 3, 6}},
                {{1, 3, 6, 9}},
                {{1, 3, 5, 7, 10}},
                {{1, 3, 5, 7, 9, 11}},
                {{1, 3, 5, 7, 9, 10, 12}},
                {{1, 2, 4, 6, 7, 9, 10, 12}},
                {{1, 2, 4, 5, 7, 8, 9, 11, 12}},
                {{1, 2, 3, 5, 6, 7, 8, 10, 11, 12}},
                {{1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12}},
                {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        }};

[[nodiscard]] const u8 *LoadFile() {
    auto *resMgr = System::ResourceManager::Instance();
    auto *file = reinterpret_cast<const u8 *>(
            resMgr->getFile("ItemSlotTable/ItemSlotTable.slt", nullptr, System::ArchiveId::Course));
    if (!file) {
        file = reinterpret_cast<const u8 *>(
                resMgr->getFile("ItemSlot.bin", nullptr, System::ArchiveId::Core));
    }

    if (!file) {
        PANIC("ItemSlot.bin not found in Common.szs");
    }

    return file;
}

} // namespace

/// @addr{0x807BAE80}
void ItemSlotData::ItemList::add(ItemId id) {
    for (u32 i = 0; i < count; ++i) {
        if (items[i] == id) {
            return;
        }
    }

    items[count++] = id;
}

/// @addr{0x807BB42C}
/// @brief Draws the item a racer gets from a box.
/// @param boxSetting The box's KMP setting (0 = a normal box, else a row of the special table).
/// @param rank The racer's place (1-based).
/// @param human Whether the racer is a human (the CPU table otherwise).
/// @param noTriples The racer already holds a triple item: triple shells and bananas are left
/// out (the game also leaves them out when the total item count is over the limit).
/// @details The online part (the table reload when racers leave, the network timer) is not
/// ported.
ItemId ItemSlotData::decideItem(u16 boxSetting, u8 rank, bool human, bool noTriples,
        const KartItem *kartItem) {
    constexpr size_t NO_TRIPLES_COUNT = 16;

    const Table *table;
    u32 row;
    if (boxSetting == 0) {
        row = rank - 1;
        table = human ? &m_humanTable : &m_cpuTable;
    } else {
        table = &m_specialTable;
        row = (boxSetting - 1) & 0xFF;
    }

    const u16 *chances = table->chances + (row & 0xFF) * ITEM_COUNT;

    auto *itemDir = ItemDirector::Instance();
    if (itemDir->overLimit() > 0) {
        noTriples = true;
    }

    const auto &scenario = System::RaceConfig::Instance()->raceScenario();
    bool limited =
            scenario.gameMode != System::RaceConfig::Scenario::GameMode::Mission && noTriples;
    size_t count = limited ? NO_TRIPLES_COUNT : ITEM_COUNT;

    const Kart::KartObject *kart = kartItem ? kartItem->kartObject() : nullptr;

    s16 total = 0;
    for (size_t i = 0; i < count; ++i) {
        ItemId id = static_cast<ItemId>(i);
        const auto &behaviour = ItemInventory::Behaviour(id);
        ItemObjKind kind = behaviour.objKind;

        bool allowed = cooldown(kind) <= 0;

        if (allowed && (id == ItemId::MEGA_MUSHROOM || id == ItemId::THUNDER_CLOUD) && kart &&
                kart->status().onBit(Kart::eStatus::InMega)) {
            allowed = false;
        }

        if (allowed && id == ItemId::THUNDER_CLOUD && kart &&
                kart->status().onBit(Kart::eStatus::InStar)) {
            allowed = false;
        }

        if (allowed && behaviour.usable) {
            auto &holder = itemDir->holder(kind);
            s32 inUse = holder.spawnedCount() + itemDir->heldCount(kind);
            allowed = holder.limit() - inUse > 0;
        } else {
            allowed = false;
        }

        if (allowed && kind == ItemObjKind::BulletBill) {
            auto *kartMgr = Kart::KartObjectManager::Instance();
            for (size_t j = 0; j < scenario.playerCount; ++j) {
                if (kartMgr->object(j)->status().onBit(Kart::eStatus::InBullet)) {
                    allowed = false;
                    break;
                }
            }
        }

        if (allowed) {
            total += chances[i];
        }

        m_cumulativeChance[i] = total;
    }

    u16 draw = static_cast<u16>(
            ItemRaceFlags::Random().getU32(static_cast<u32>(static_cast<s32>(total))));

    ItemId result = ItemId::NONE;
    for (size_t i = 0; i < count; ++i) {
        if (draw < m_cumulativeChance[i]) {
            result = static_cast<ItemId>(i);
            break;
        }
    }

    if (result == ItemId::NONE && total == 0) {
        result = ItemId::MUSHROOM;
    }

    return result;
}

/// @addr{0x807BB8D0}
/// @brief The next item the roulette shows: a random one of the box's possible items, not the
/// one shown now.
/// @param byRank Use the player table's items of this place (boxes with setting 0xFF).
ItemId ItemSlotData::pickShownItem(u16 boxSetting, u8 rank, ItemId current, bool byRank) {
    const ItemList *list;
    if (byRank) {
        list = &m_rankItems[rank - 1];
    } else if (boxSetting == 0) {
        list = &m_shownItems;
    } else {
        list = &m_specialItems[boxSetting - 1];
    }

    u32 idx = ItemRaceFlags::Random().getU32(list->count);
    if (current == list->items[idx] && ++idx >= list->count) {
        idx = 0;
    }

    return list->items[idx];
}

/// @addr{0x807BB9EC}
/// @brief The strong items' cooldowns start full when the race starts and count down.
void ItemSlotData::calcTimers() {
    if (System::RaceManager::Instance()->getCountdownTimer() == 0) {
        m_lightningTimer = LIGHTNING_COOLDOWN;
        m_blueShellTimer = BLUE_SHELL_COOLDOWN;
        m_blooperTimer = BLOOPER_COOLDOWN;
        m_powTimer = POW_COOLDOWN;
    }

    if (m_lightningTimer > 0) {
        --m_lightningTimer;
    }

    if (m_blueShellTimer > 0) {
        --m_blueShellTimer;
    }

    if (m_blooperTimer > 0) {
        --m_blooperTimer;
    }

    if (m_powTimer > 0) {
        --m_powTimer;
    }
}

/// @addr{0x807BA77C}
ItemSlotData *ItemSlotData::CreateInstance() {
    if (!s_instance) {
        s_instance = EGG::egg_new<ItemSlotData>();
    }

    return s_instance;
}

/// @addr{0x807BA814}
void ItemSlotData::DestroyInstance() {
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

/// @addr{0x807BA8C8}
ItemSlotData::ItemSlotData()
    : m_humanTable{0, nullptr}, m_cpuTable{0, nullptr}, m_specialTable{0, nullptr},
      m_specialItems(nullptr), m_lightningTimer(0), m_blueShellTimer(0), m_blooperTimer(0),
      m_powTimer(0), m_playerCount(0), m_cumulativeChance{} {
    load();
}

/// @addr{0x807BA934}
ItemSlotData::~ItemSlotData() {
    EGG::egg_delete_array(m_humanTable.chances, m_humanTable.rowCount * ITEM_COUNT);
    EGG::egg_delete_array(m_cpuTable.chances, m_cpuTable.rowCount * ITEM_COUNT);
    EGG::egg_delete_array(m_specialTable.chances, m_specialTable.rowCount * ITEM_COUNT);
    EGG::egg_delete_array(m_specialItems, m_specialTable.rowCount);
}

/// @addr{0x807BAF48}
/// @brief Reads the tables of this race's mode, then lists each special row's items.
/// @details The battle tables (0x807BBB00) and the online ones are not ported: Kinoko only runs
/// Time Trials and offline VS races. A Time Trial reads the Grand Prix tables, as the game does.
void ItemSlotData::load() {
    m_shownItems.count = 0;
    m_shownItems.items.fill(ItemId::NONE);
    for (auto &list : m_rankItems) {
        list.count = 0;
        list.items.fill(ItemId::NONE);
    }

    m_playerCount = ItemRaceFlags::s_playerCount;
    ASSERT(m_playerCount <= MAX_RANKS);

    if (ItemRaceFlags::s_battle || ItemRaceFlags::s_online) {
        PANIC("Battle and online item tables are not ported");
    }

    const u8 *data = LoadFile() + 1;
    if (ItemRaceFlags::s_versus) {
        data = SkipTable(SkipTable(data));
        data = parseTable(data, m_humanTable, true, false);
        data = parseTable(data, m_cpuTable, false, false);
        data = SkipTable(data);
        static_cast<void>(parseTable(data, m_specialTable, true, true));
        scaleTableForPlayerCount(m_humanTable);
        scaleTableForPlayerCount(m_cpuTable);
    } else {
        data = parseTable(data, m_humanTable, true, false);
        data = parseTable(data, m_cpuTable, false, false);
        data = SkipTable(SkipTable(SkipTable(data)));
        static_cast<void>(parseTable(data, m_specialTable, true, true));
    }

    m_specialItems = EGG::egg_new_array<ItemList>(m_specialTable.rowCount);
    for (u32 row = 0; row < m_specialTable.rowCount; ++row) {
        ItemList &list = m_specialItems[row];
        list.count = 0;
        list.items.fill(ItemId::NONE);
        for (size_t i = 0; i < ITEM_COUNT; ++i) {
            if (m_specialTable.chances[row * ITEM_COUNT + i] != 0) {
                list.add(static_cast<ItemId>(i));
            }
        }
    }
}

/// @addr{0x807BA9D8}
/// @brief Reads one section into a table (rows = ranks, columns = items).
/// @param fillLists Record the items with a chance in the shown-item lists.
/// @param raw Keep the bytes as they are (no item mode scaling, no lists).
/// @return The next section.
const u8 *ItemSlotData::parseTable(const u8 *data, Table &table, bool fillLists, bool raw) {
    using ItemMode = System::RaceConfig::Scenario::ItemMode;

    u32 rankCount = data[0];
    u32 itemCount = data[1];
    data += 2;

    if (!table.chances) {
        table.rowCount = rankCount;
        table.chances = EGG::egg_new_array<u16>(rankCount * ITEM_COUNT);
    }

    ItemMode itemMode = System::RaceConfig::Instance()->raceScenario().itemMode;

    for (u32 item = 0; item < itemCount; ++item) {
        s32 category = ITEM_CATEGORY[item];
        f32 factor = 1.0f;
        u32 offset = 0;
        ItemMode mode = itemMode;

        if (ItemRaceFlags::s_online && ItemRaceFlags::s_battle) {
            if (ItemRaceFlags::s_playerCount < 5) {
                mode = ItemMode::Frantic;
            } else if (ItemRaceFlags::s_playerCount < 9) {
                factor = ONLINE_BATTLE_FACTOR[category];
                offset = ONLINE_BATTLE_OFFSET[category];
            }
        }

        if (mode == ItemMode::Frantic) {
            factor = FRANTIC_FACTOR[category];
            offset = FRANTIC_OFFSET[category];
        } else if (mode == ItemMode::Strategic) {
            factor = STRATEGIC_FACTOR[category];
            offset = STRATEGIC_OFFSET[category];
        }

        for (u32 rank = 0; rank < rankCount; ++rank) {
            u32 chance = *data++;
            if (!raw) {
                chance = static_cast<u32>(static_cast<s32>(
                        factor * static_cast<f32>(static_cast<s32>(chance + offset))));
            }

            table.chances[rank * ITEM_COUNT + item] = static_cast<u16>(chance);

            if (fillLists && !raw && (chance & 0xFFFF) != 0) {
                m_shownItems.add(static_cast<ItemId>(item));
                m_rankItems[rank].add(static_cast<ItemId>(item));
            }
        }
    }

    return data;
}

/// @addr{0x807BAD20}
/// @brief With fewer than 12 racers, place i uses the row PLACE_ROWS gives it; the rows past the
/// racer count are cleared.
void ItemSlotData::scaleTableForPlayerCount(Table &table) {
    u16 *chances = table.chances;
    u32 i = 0;
    for (; i < m_playerCount; ++i) {
        u32 src = (PLACE_ROWS[m_playerCount][i] - 1) & 0xFFFF;
        std::copy_n(chances + src * ITEM_COUNT, ITEM_COUNT, chances + i * ITEM_COUNT);
    }

    for (; i < MAX_RANKS; ++i) {
        std::fill_n(chances + i * ITEM_COUNT, ITEM_COUNT, 0);
    }
}

/// @addr{Inlined in 0x807BB42C}
/// @brief The cooldown of an item kind (0 for the kinds without one).
s32 ItemSlotData::cooldown(ItemObjKind kind) const {
    switch (kind) {
    case ItemObjKind::Lightning:
        return m_lightningTimer;
    case ItemObjKind::BlueShell:
        return m_blueShellTimer;
    case ItemObjKind::Blooper:
        return m_blooperTimer;
    case ItemObjKind::PowBlock:
        return m_powTimer;
    default:
        return 0;
    }
}

/// @addr{0x807BAD08}
const u8 *ItemSlotData::SkipTable(const u8 *data) {
    return data + data[1] * data[0] + 2;
}

ItemSlotData *ItemSlotData::s_instance = nullptr; ///< @addr{0x809C3670}

} // namespace Kinoko::Item
