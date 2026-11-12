#include "ItemObjHolder.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemObjBanana.hh"
#include "game/item/ItemObjBlueShell.hh"
#include "game/item/ItemObjBobOmb.hh"
#include "game/item/ItemObjFakeItemBox.hh"
#include "game/item/ItemObjKoura.hh"
#include "game/item/ItemObjMushroom.hh"
#include "game/item/ItemObjRed.hh"
#include "game/item/ItemObjThunderCloud.hh"

namespace Kinoko::Item {

ItemObjHolder::ItemObjHolder()
    : m_kind(ItemObjKind::None), m_room(0), m_spawnedCount(0), m_activeCount(0), m_landedCount(0),
      m_limit(0) {}

ItemObjHolder::~ItemObjHolder() {
    for (auto *obj : m_objs) {
        EGG::egg_delete(obj);
    }
}

/// @addr{0x80795CCC}
/// @brief Creates the pool. Kinds whose objects are not ported get an empty pool.
void ItemObjHolder::init(ItemObjKind kind, u16 &globalIdx) {
    m_kind = kind;
    const auto &props = ItemObj::Properties(kind);
    m_limit = props.limit;

    // 0x80790FB8: the pool holds the kind's limit plus the largest pickup of it (offline; online
    // pools of one get at least 2). The static property +0x08 is not used for it.
    s32 pickup = 1;
    for (size_t i = 0; i <= static_cast<size_t>(ItemId::TRIPLE_BANANA); ++i) {
        const auto &behaviour = ItemInventory::Behaviour(static_cast<ItemId>(i));
        if (behaviour.usable && behaviour.objKind == kind) {
            pickup = std::max(pickup, behaviour.count);
        }
    }
    m_room = m_limit - 1 + pickup;
    m_spawnedCount = 0;
    m_activeCount = 0;
    m_landedCount = 0;

    if (!Create(kind)) {
        return;
    }

    m_objs = owning_span<ItemObj *>(m_room);
    for (s32 i = 0; i < m_room; ++i) {
        m_objs[i] = Create(kind);
        m_objs[i]->init(globalIdx++, static_cast<u16>(i), kind);
    }
}

/// @addr{0x80795E04}
/// @brief Takes objects out of the pool. When the pool is full the oldest object on the course
/// is despawned first.
void ItemObjHolder::spawn(s32 count, ItemObj **out, u8 owner, const EGG::Vector3f &pos) {
    s32 capacity = static_cast<s32>(m_objs.size());
    while (capacity < m_spawnedCount + count) {
        if (m_activeCount < 1) {
            break;
        }

        ItemObj *oldest = m_objs[0];
        despawn(oldest);

        if (m_activeCount > 1) {
            ItemObj *first = m_objs[0];
            s32 i = 0;
            for (; i < m_activeCount - 1; ++i) {
                m_objs[i] = m_objs[i + 1];
            }
            m_objs[i] = first;
        }
    }

    for (s32 i = 0; i < count && m_spawnedCount < capacity; ++i) {
        ItemObj *obj = m_objs[m_spawnedCount++];
        out[i] = obj;
        obj->spawn(m_kind, owner, pos);
    }
}

/// @addr{0x80795F68}
void ItemObjHolder::despawn(ItemObj *obj) {
    s32 idx = 0;
    while (idx < m_spawnedCount && m_objs[idx] != obj) {
        ++idx;
    }

    if (idx < m_landedCount) {
        --m_landedCount;
        for (; idx < m_landedCount; ++idx) {
            m_objs[idx] = m_objs[idx + 1];
        }
    }

    if (idx < m_activeCount) {
        --m_activeCount;
        m_objs[idx] = m_objs[m_activeCount];
        idx = m_activeCount;
    }

    if (idx < m_spawnedCount) {
        --m_spawnedCount;
        m_objs[idx] = m_objs[m_spawnedCount];
        idx = m_spawnedCount;
    }

    m_objs[idx] = obj;

    // 0x8079E884
    obj->removeBoxCol();
    obj->m_frameFlags.setBit(ItemObj::eFrameFlags::Despawned);
    obj->onDespawn();
    obj->m_flags.resetBit(ItemObj::eFlags::OnCourse);
}

/// @addr{0x807961F0}
/// @brief Puts an object taken from the pool on the course (when it is released or dropped).
void ItemObjHolder::activate(ItemObj *obj) {
    s32 idx = m_activeCount;
    while (idx < m_spawnedCount && m_objs[idx] != obj) {
        ++idx;
    }

    if (m_activeCount < idx && idx < m_spawnedCount) {
        m_objs[idx] = m_objs[m_activeCount];
        m_objs[m_activeCount] = obj;
    }

    ++m_activeCount;
    obj->m_flags.setBit(ItemObj::eFlags::OnCourse);

    if (obj->properties().hitRadius > 0.0f && !obj->m_boxColUnit) {
        obj->insertBoxCol(false);
        obj->m_flags2.setBit(ItemObj::eFlags2::InBoxCol);
    }

    obj->m_frame = 0;
}

/// @addr{0x80796BBC}
/// @brief Moves an object that came to rest into the leading "landed" part of the active list.
void ItemObjHolder::markLanded(ItemObj *obj) {
    s32 idx = 0;
    while (idx < m_activeCount && m_objs[idx] != obj) {
        ++idx;
    }

    if (m_landedCount <= idx && idx < m_activeCount) {
        m_objs[idx] = m_objs[m_landedCount];
        m_objs[m_landedCount] = obj;
        ++m_landedCount;
    }
}

/// @addr{0x80796470}
/// @brief Updates every object on the course (the offline path: no online sub-steps). When the
/// kind or the total item limit is reached, the oldest resting object vanishes.
void ItemObjHolder::calc() {
    auto *director = ItemDirector::Instance();
    s32 count = m_activeCount;

    if (m_limit > 1) {
        s32 n = m_spawnedCount + director->heldCount(m_kind);
        s32 over = director->overLimit();
        if ((m_limit - n < 2 || over > -5) && (m_limit - n < 1 || over > 0) && m_activeCount > 0) {
            // 0x8079FE30 (the sort pass 0x80796348 that picks it is not ported: the first one).
            ItemObj *oldest = m_objs[0];
            if (oldest->m_frame > 300 && oldest->m_flags.onBit(ItemObj::eFlags::Landed) &&
                    oldest->m_state == &ItemObj::calcLanded) {
                oldest->startVanish(true);
            }
        }
    }

    for (s32 i = 0; i < count; ++i) {
        if (m_objs[i]->calc()) {
            count = m_activeCount;
            --i;
        }
    }
}

/// @brief Removes everything (race restart).
void ItemObjHolder::despawnAll() {
    while (m_spawnedCount > 0) {
        despawn(m_objs[0]);
    }
}

// Item objects must live in the engine heap (egg_new), not in plain malloc memory: a snapshot (Host::Context) only
// copies the engine heap, so objects outside it keep their post-snapshot state after a restore and the replay diverges.
/// @addr{0x807AEAEC}
/// @brief The game's per-kind array constructors (property +0x00). Only the ported kinds.
ItemObj *ItemObjHolder::Create(ItemObjKind kind) {
    switch (kind) {
    case ItemObjKind::GreenShell:
        return EGG::egg_new<ItemObjGreenShell>();
    case ItemObjKind::RedShell:
        return EGG::egg_new<ItemObjRedShell>();
    case ItemObjKind::Banana:
        return EGG::egg_new<ItemObjBanana>();
    case ItemObjKind::Mushroom:
        return EGG::egg_new<ItemObjMushroom>();
    case ItemObjKind::Star:
        return EGG::egg_new<ItemObjStar>();
    case ItemObjKind::FakeItemBox:
        return EGG::egg_new<ItemObjFakeItemBox>();
    case ItemObjKind::BlueShell:
        return EGG::egg_new<ItemObjBlueShell>();
    case ItemObjKind::BobOmb:
        return EGG::egg_new<ItemObjBobOmb>();
    case ItemObjKind::ThunderCloud:
        return EGG::egg_new<ItemObjThunderCloud>();
    default:
        return nullptr;
    }
}

} // namespace Kinoko::Item
