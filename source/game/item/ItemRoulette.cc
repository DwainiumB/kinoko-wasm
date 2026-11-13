#include "ItemRoulette.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemRaceFlags.hh"
#include "game/item/ItemSlotData.hh"
#include "game/item/KartItem.hh"

namespace Kinoko::Item {

std::array<ItemId, ItemRoulette::FORCED_ITEM_PLAYERS> ItemRoulette::s_forcedItem = {{
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
        ItemId::NONE,
}};

void ItemRoulette::SetForcedItem(size_t playerIdx, ItemId id) {
    ASSERT(playerIdx < FORCED_ITEM_PLAYERS);
    s_forcedItem[playerIdx] = id;
}

/// @addr{0x807B9F48}
ItemRoulette::ItemRoulette()
    : m_kartItem(nullptr), m_state(State::Idle), m_frame(0), m_duration(0), m_speed(0.0f),
      m_progress(0.0f), m_prevShown(ItemId::NONE), m_shown(ItemId::NONE), m_decided(ItemId::NONE),
      m_pending(ItemId::NONE), m_boxSetting(0), m_rank(0), m_byRank(false), m_isShort(false),
      m_waitingForNet(false), m_replaced(false), m_rollCount(0) {}

/// @addr{0x807B9F80}
void ItemRoulette::init(KartItem *kartItem) {
    m_kartItem = kartItem;
    finish();
}

/// @addr{0x807B9FB0}
/// @brief Starts the roulette after an item box: the item is decided here, the roulette only
/// shows it later.
/// @param boxSetting The box's KMP setting; 0xFF picks the shown items by place.
/// @param rank The racer's place (1-based).
/// @param human Whether the racer is a human (else the CPU odds).
/// @param isShort A 45-frame roulette (boxes with setting 5 = 2, 3 or 4).
/// @details The online parts (roll count limits, the partner's triple items, the item packets)
/// are not ported.
void ItemRoulette::start(u16 boxSetting, u8 rank, bool human, bool isShort) {
    constexpr s32 DURATION = 184;
    constexpr f32 SPEED = 1.1f;
    constexpr s32 SHORT_DURATION = 45;
    constexpr f32 SHORT_SPEED = 0.17f;

    bool byRank = boxSetting == 0xFF;
    if (byRank) {
        boxSetting = 0;
    }

    if (ItemRaceFlags::s_timeTrial || m_state != State::Idle || m_pending != ItemId::NONE) {
        return;
    }

    if (isShort) {
        m_duration = SHORT_DURATION;
        m_speed = SHORT_SPEED;
    } else {
        m_duration = DURATION;
        m_speed = SPEED;
    }

    bool noTriples = m_kartItem->hasTripleItem(true);
    m_decided =
            ItemSlotData::Instance()->decideItem(boxSetting, rank, human, noTriples, m_kartItem);

    ItemId forced = s_forcedItem[m_kartItem->playerIdx()];
    if (forced != ItemId::NONE) {
        m_decided = forced;
    }

    if (!ItemDirector::Instance()->hasRoomFor(m_decided)) {
        if (m_decided != ItemId::TRIPLE_MUSHROOM) {
            m_decided = ItemId::MUSHROOM;
        }

        m_replaced = true;
    }

    m_state = State::Spinning;
    m_frame = 0;
    m_progress = 0.0f;
    m_boxSetting = boxSetting;
    m_rank = rank;
    m_byRank = byRank;
    m_isShort = isShort;
    showNext();
}

/// @addr{0x807BA37C}
/// @brief The roulette slows down; after its duration, or when the item button is pressed after
/// a second, it stops on the decided item.
/// @return Whether the item is handed over this frame.
bool ItemRoulette::calc() {
    constexpr f32 SPEED_DECAY = 0.9675f;
    constexpr f32 SPEED_MIN_STEP = 0.002f;
    constexpr s32 EARLY_STOP_FRAME = 59;

    ++m_frame;

    if (m_shown != ItemId::NONE) {
        m_speed = m_speed * SPEED_DECAY + SPEED_MIN_STEP;
        m_progress = m_progress + m_speed;
    }

    if (m_state == State::Spinning) {
        if (m_progress >= 1.0f) {
            m_progress = m_progress - 1.0f;
            showNext();
        }

        if (m_frame <= m_duration) {
            bool stop =
                    m_kartItem->rouletteStopRequested() && m_frame > EARLY_STOP_FRAME && !m_isShort;
            if (!stop) {
                return false;
            }
        }

        m_state = State::Stopping;
    } else if (m_state == State::Stopping && m_progress >= 1.0f) {
        if (m_shown == m_decided) {
            m_progress = 0.0f;
            m_prevShown = m_shown;
            m_shown = ItemId::NONE;
            return true;
        }

        m_progress = m_progress - 1.0f;
        m_prevShown = m_shown;
        m_shown = m_decided;
    }

    return false;
}

/// @addr{0x807BA2D8}
/// @brief Back to idle after the item was handed over.
void ItemRoulette::finish() {
    m_state = State::Idle;
    m_frame = 0;
    m_prevShown = ItemId::NONE;
    m_shown = ItemId::NONE;
    m_decided = ItemId::NONE;
    m_pending = ItemId::NONE;
    m_waitingForNet = false;
    m_replaced = false;
}

/// @addr{0x807BA5D8}
/// @brief Stops a spinning roulette without an item (hit, respawn). The same as finish offline.
void ItemRoulette::cancel() {
    finish();
}

/// @addr{0x807BA56C}
void ItemRoulette::showNext() {
    ItemId next = ItemSlotData::Instance()->pickShownItem(m_boxSetting, m_rank, m_shown, m_byRank);
    m_prevShown = m_shown;
    m_shown = next;
}

} // namespace Kinoko::Item
