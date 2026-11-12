#pragma once

#include "game/item/ItemBlooperManager.hh"
#include "game/item/ItemObjHolder.hh"
#include "game/item/ItemPowManager.hh"
#include "game/item/KartItem.hh"

#include <span>

namespace Kinoko {

namespace Host {

class Context;

} // namespace Host

/// @brief Pertains to item handling.
namespace Item {

/// @brief The stand-in item ItemDirector keeps at +0x2A4 for blasts of course objects
/// (ItemDirector::explodeAt): never on the course, no box collision unit, no velocity, its
/// second flag word 0x40 so that hits never make it vanish (set up by 0x807A3618).
class ItemObjBlastProbe final : public ItemObj {
public:
    ItemObjBlastProbe();
    ~ItemObjBlastProbe() override;

    [[nodiscard]] StateFunc initialState() const override {
        return nullptr;
    }

    void set(f32 radius, const EGG::Vector3f &pos, ItemObjKind kind);
};

/// @addr{0x809C3618}
class ItemDirector : EGG::Disposer {
    friend class Host::Context;

public:
    /// @brief Most items one kart can touch in a frame (ItemDirector +0x264).
    static constexpr size_t MAX_KART_HITS = 16;

    void init();
    void calc();
    void activateLightning(size_t userIdx);
    void spawnThunderCloud(u8 playerIdx);
    void spawnFromObject(ItemObjKind kind, const EGG::Vector3f &pos, const EGG::Vector3f &vel,
            u8 playerIdx);
    void passThunderCloud(u8 fromIdx, u8 toIdx);
    void explodeAt(f32 radius, const EGG::Vector3f &pos, ItemObjKind kind);
    [[nodiscard]] size_t checkKartCollision(const Kart::KartObject *kart,
            std::array<ItemObj *, MAX_KART_HITS> &hits);

    [[nodiscard]] KartItem &kartItem(size_t idx) {
        ASSERT(idx < m_karts.size());
        return m_karts[idx];
    }

    [[nodiscard]] ItemPowManager &powManager() {
        return m_powManager;
    }

    [[nodiscard]] ItemBlooperManager &blooperManager() {
        return m_blooperManager;
    }

    [[nodiscard]] ItemObjHolder &holder(ItemObjKind kind) {
        ASSERT(kind < ItemObjKind::Count);
        return m_holders[static_cast<size_t>(kind)];
    }

    [[nodiscard]] s32 heldCount(ItemObjKind kind) const;
    [[nodiscard]] bool hasRoomFor(ItemId id) const;

    /// @brief How far the total item count is over 30 (+0x42C, last frame's).
    [[nodiscard]] s32 overLimit() const {
        return m_overLimit;
    }

    static ItemDirector *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] const ItemInventory &itemInventory(s16 idx) const {
        return m_karts[idx].inventory();
    }

    [[nodiscard]] static ItemDirector *Instance() {
        return s_instance;
    }

private:
    EGG_NEW_DELETE_FRIEND

    ItemDirector();
    ~ItemDirector() override;

    owning_span<KartItem> m_karts;                                                ///< +0x14
    std::array<ItemObjHolder, static_cast<size_t>(ItemObjKind::Count)> m_holders; ///< +0x48
    ItemObjBlastProbe m_blastProbe;                                               ///< +0x2A4
    s32 m_overLimit;                                                              ///< +0x42C
    ItemBlooperManager m_blooperManager; ///< @addr{0x809C3610} (a separate instance in the game)
    ItemPowManager m_powManager;         ///< @addr{0x809C3640} (a separate instance in the game)

    static ItemDirector *s_instance; ///< @addr{0x809C3618}
};

} // namespace Item

} // namespace Kinoko
