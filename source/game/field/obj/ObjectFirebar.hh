#pragma once

#include "game/field/obj/ObjectCollidable.hh"
#include "game/field/obj/ObjectFireball.hh"

namespace Kinoko::Field {

class ObjectFirebar : public ObjectCollidable {
public:
    ObjectFirebar(const System::MapdataGeoObj &params);
    ~ObjectFirebar() override;

    void init() override;
    void calc() override;

    /// @addr{0x807687D8}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x808CE358}
    [[nodiscard]] const char *getKclName() const override {
        return id() == ObjectId::WLFirebarGC ? "WLfirebarGC" : "koopaFirebar";
    }

    /// Added for Kinoko web's renderer: this spawner doesn't override loadGraphics() so it loads
    /// the same fireball model as its children, but it never moves itself (only the orbiting
    /// fireballs it owns do) -- it sat frozen at its own placement, a permanent extra fireball on
    /// top of the ring.
    [[nodiscard]] bool isVisible() const override {
        return false;
    }

private:
    owning_span<ObjectFireball *> m_fireballs;
    u32 m_spokes; // The number of fireball "segments"
    f32 m_angSpeed;
    f32 m_degAngle;
    EGG::Vector3f m_axis;
    EGG::Vector3f m_initDir;
};

} // namespace Kinoko::Field
