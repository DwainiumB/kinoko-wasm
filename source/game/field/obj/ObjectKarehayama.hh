#pragma once

#include "game/field/obj/ObjectBreakable.hh"

namespace Kinoko::Field {

/// @brief A pile of dry leaves on Maple Treeway ("karehayama", object 0x176). A kart driving
/// through it fast, or an item, scatters it and it may drop an item like a wooden box; a slow kart
/// only scatters it. It is back after 138 frames.
/// @details Vtable 0x808CE020. The leaf and shadow effects (+0xEC, +0x118, +0x11C, the respawn's
/// model and effect calls) and the sounds are left out: nothing in the race reads them.
class ObjectKarehayama : public ObjectBreakable {
public:
    /// @addr{0x80766F4C}
    ObjectKarehayama(const System::MapdataGeoObj &params) : ObjectBreakable(params) {}

    /// @addr{0x80766F88}
    ~ObjectKarehayama() override = default;

protected:
    void breakObject(u8 playerIdx, const EGG::Vector3f &dir) override;
    void breakByKart(Kart::KartObject *kartObj) override;
    void breakByKartUntimed(Kart::KartObject *kartObj) override;
    void breakByItem(Item::ItemObj *item, const EGG::Vector3f &hitDepth) override;
    void breakByItemUntimed(Item::ItemObj *item, const EGG::Vector3f &hitDepth) override;

private:
    void scatter();
    void breakByFastKart(Kart::KartObject *kartObj);

    static constexpr s32 RESPAWN_FRAMES = 138;   ///< 0x808CE008
    static constexpr f32 MIN_SPEED_RATIO = 0.4f; ///< 0x808A3B58: slower karts only scatter it.
};

} // namespace Kinoko::Field
