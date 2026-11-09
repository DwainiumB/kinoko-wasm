#pragma once

#include "game/field/obj/ObjectItembox.hh"
#include "game/field/obj/ObjectTwistedWay.hh"

namespace Kinoko::Field {

/// @brief An item box that rests on the floor below its point (`s_itembox`, object 0x76) and rides
/// it when it moves: on Bowser's Castle the twelve boxes on the wavy road (TwistedWay).
/// @details Vtable 0x808D79E0; everything but init and calc is the item box's. The game keeps the
/// floor in an attachment record (+0x14C, filled by the floor's collision check through the
/// collision info +0x58) and asks the floor object to carry the point (vtable +0x110 / +0x118).
/// Only the wavy road carries points in Kinoko; a box on static ground stays where it landed.
class ObjectItemboxFloor final : public ObjectItembox {
public:
    /// @addr{0x80829208}
    ObjectItemboxFloor(const System::MapdataGeoObj &params)
        : ObjectItembox(params), m_road(nullptr), m_point{0.0f, 0.0f, 0.0f} {}

    /// @addr{0x80829584}
    ~ObjectItemboxFloor() override = default;

    void init() override;
    void calc() override;

private:
    ObjectTwistedWay *m_road;               ///< The floor that carries the box, if it moves.
    ObjectTwistedWay::CarriedPoint m_point; ///< +0x14C record: +0x08..+0x10
};

} // namespace Kinoko::Field
