#pragma once

#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

/// @brief An item box (`itembox`, the game's `ObjItemBox`, 200 bytes). Driving through it starts
/// the racer's item roulette; it vanishes and comes back after a while.
/// @details The floor boxes (`s_itembox`, ObjectItemboxFloor) derive from it. The flying
/// (`f_itembox`) boxes and the ones other objects carry derive from it in the game; they are not
/// ported. The respawn animation (the box grows
/// back from the floor), its spin and the effects and sounds only move the model.
class ObjectItembox : public ObjectCollidable {
public:
    ObjectItembox(const System::MapdataGeoObj &params);
    ~ObjectItembox() override;

    void calc() override;

    /// @addr{0x806C69FC}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    Kart::Reaction onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
            Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) override;

    /// @beginGetters
    [[nodiscard]] bool isActive() const {
        return m_active;
    }
    /// @endGetters

private:
    bool m_active;       ///< +0xB0 Not broken: can be driven through.
    s32 m_timer;         ///< +0xB4 Frames since it broke, then since it came back.
    s32 m_respawnFrames; ///< +0xB8
    u16 m_spinPhase;     ///< +0xBC Random start of the model's spin.
    u16 m_screenMode;    ///< +0xBE KMP setting 3 (forced to 1 in a split-screen battle).
};

} // namespace Kinoko::Field
