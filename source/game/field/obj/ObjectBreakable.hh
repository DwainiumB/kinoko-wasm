#pragma once

#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

/// @brief An object that a kart can break (the wooden boxes). It drops a random item when items
/// are on, then waits and falls back into place.
class ObjectBreakable : public ObjectCollidable {
public:
    /// @addr{0x8076EBE0}
    ObjectBreakable(const System::MapdataGeoObj &params)
        : ObjectCollidable(params), m_state(0), m_respawnTimer(0) {}

    /// @addr{0x8076EC28}
    ~ObjectBreakable() = default;

    /// @addr{0x8076EC68}
    /// @details The break effect set-up (+0xEC) and the sound picked by the KCL name are left out.
    void init() override {
        enableCollision();
    }

    void calc() override;

    /// @addr{0x807677E4}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    Kart::Reaction onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
            Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) override;
    s32 onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
            EGG::Vector3f &hitDepth) override;

    /// @addr{0x8076ED70}
    /// @details Showing the model (+0x68) and the rumble flag (+0xBC) are left out.
    virtual void enableCollision() {
        m_state = 1;
        ObjectCollidable::enableCollision();
    }

protected:
    /// @addr{0x8076EDD4}
    virtual void respawn() {
        enableCollision();
    }

    virtual void breakObject(u8 playerIdx, const EGG::Vector3f &dir);

    /// @addr{0x807677E0}
    virtual void calcFall() {}

    /// @brief Broken by a kart (object reaction 3).
    /// @addr{0x8076F1D4}
    virtual void breakByKart(Kart::KartObject * /*kartObj*/) {}

    /// @brief Broken by a kart outside Time Trials (object reaction 5, online races).
    /// @addr{0x8076F1D8}
    virtual void breakByKartUntimed(Kart::KartObject * /*kartObj*/) {}

    /// @brief Broken by an item (object reaction 3).
    /// @addr{0x8076F2D8}
    virtual void breakByItem(Item::ItemObj * /*item*/, const EGG::Vector3f & /*hitDepth*/) {}

    /// @brief Broken by an item outside Time Trials (object reaction 5, online races).
    /// @addr{0x8076F2DC}
    virtual void breakByItemUntimed(Item::ItemObj * /*item*/, const EGG::Vector3f & /*hitDepth*/) {}

    u32 m_state;        ///< +0xB0: 1 standing, 2 broken, 3 falling back.
    s32 m_respawnTimer; ///< +0xB4: frames until a broken object comes back (0: never).
};

} // namespace Kinoko::Field
