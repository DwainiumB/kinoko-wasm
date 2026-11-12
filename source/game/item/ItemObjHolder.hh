#pragma once

#include "game/item/ItemObj.hh"

#include <span>

namespace Kinoko::Item {

/// @brief The pool of item objects of one kind (the game's `ItemHolderItem`, 0x24 bytes at
/// ItemDirector +0x48 + kind * 0x24).
/// @details The pool array is ordered: [0, activeCount) are on the course, [activeCount,
/// spawnedCount) are taken out of the pool (held behind a kart or about to be dropped), the rest
/// are free.
/// @nosubgrouping
class ItemObjHolder {
public:
    ItemObjHolder();
    ~ItemObjHolder();

    void init(ItemObjKind kind, u16 &globalIdx);
    void spawn(s32 count, ItemObj **out, u8 owner, const EGG::Vector3f &pos);
    void despawn(ItemObj *obj);
    void activate(ItemObj *obj);
    void markLanded(ItemObj *obj);
    void calc();
    void despawnAll();

    /// @beginGetters
    [[nodiscard]] ItemObjKind kind() const {
        return m_kind;
    }

    [[nodiscard]] s32 activeCount() const {
        return m_activeCount;
    }

    [[nodiscard]] s32 spawnedCount() const {
        return m_spawnedCount;
    }

    /// @brief Most objects of this kind on the course at once.
    [[nodiscard]] s32 limit() const {
        return m_limit;
    }

    /// @brief The pool size: the limit plus the largest pickup of this kind, less one (+0x0C).
    [[nodiscard]] s32 room() const {
        return m_room;
    }

    /// @brief Whether this kind's objects are ported (the pool exists).
    [[nodiscard]] bool canSpawn() const {
        return !m_objs.empty();
    }

    [[nodiscard]] ItemObj *obj(s32 idx) {
        return m_objs[idx];
    }
    /// @endGetters

private:
    [[nodiscard]] static ItemObj *Create(ItemObjKind kind);

    ItemObjKind m_kind;            ///< +0x00
    owning_span<ItemObj *> m_objs; ///< +0x04 (+0x08 capacity)
    s32 m_room;                    ///< +0x0C limit - 1 + the most one pickup gives.
    s32 m_spawnedCount;            ///< +0x10
    s32 m_activeCount;             ///< +0x14
    s32 m_landedCount;             ///< +0x18 Leading part of the active ones resting on floor.
    s32 m_limit;                   ///< +0x1C
};

} // namespace Kinoko::Item
