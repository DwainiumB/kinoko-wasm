#pragma once

#include "game/field/obj/ObjectWoodbox.hh"

namespace Kinoko::Field {

class ObjectWoodboxWSub final : public ObjectWoodbox {
public:
    ObjectWoodboxWSub(const System::MapdataGeoObj &params);
    ~ObjectWoodboxWSub() override;

    /// @addr{0x8077E3E4}
    void init() override {
        ObjectBreakable::init();
        m_state = 0;
    }

    void calc() override;

    /// @addr{0x8077EDA4}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x8077E444}
    void enableCollision() override {
        ObjectBreakable::enableCollision();
        m_railInterpolator->init(0.0f, 0);
        m_railInterpolator->setPerPointVelocities(true);
    }

    /// Added for Kinoko web's renderer: every box in the spawner's pool shares the same placement
    /// params, so while m_state is 0 (not yet enabled by the spawner, or reset back to 0 after a
    /// rail direction change -- see calcPosition()) this box has never been positioned by its own
    /// rail and sits frozen on top of every other still-inactive box in the pool.
    [[nodiscard]] bool isVisible() const override {
        return m_state != 0;
    }

private:
    void calcPosition();
};

} // namespace Kinoko::Field
