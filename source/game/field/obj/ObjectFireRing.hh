#pragma once

#include "game/field/obj/ObjectCollidable.hh"
#include "game/field/obj/ObjectFireball.hh"

namespace Kinoko::Field {

class ObjectFireRing : public ObjectCollidable {
public:
    ObjectFireRing(const System::MapdataGeoObj &params);
    ~ObjectFireRing() override;

    void init() override;
    void calc() override;

    /// @addr{0x80768740}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x80768734}
    void createCollision() override {}

    /// Added for Kinoko web's renderer: same issue as ObjectFirebar -- this spawner doesn't
    /// override loadGraphics() so it loads a real fireball model but never moves itself, leaving a
    /// permanent extra frozen fireball on top of the ring it manages.
    [[nodiscard]] bool isVisible() const override {
        return false;
    }

private:
    owning_span<ObjectFireball *> m_fireballs;
    f32 m_angSpeed;
    f32 m_degAngle;
    EGG::Vector3f m_axis;
    EGG::Vector3f m_initDir;
    f32 m_radiusScale;
    f32 m_phase;
};

} // namespace Kinoko::Field
