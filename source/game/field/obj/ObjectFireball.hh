#pragma once

#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

class ObjectFireball : public ObjectCollidable {
public:
    /// @addr{0x80768650}
    ObjectFireball(const System::MapdataGeoObj &params) : ObjectCollidable(params) {}

    /// @addr{0x807689AC}
    ~ObjectFireball() = default;

    /// @addr{0x80768728}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x8076871C}
    [[nodiscard]] const char *getKclName() const override {
        return "fireBPlane";
    }

    /// Added for Kinoko web's renderer: Bowser's Castle's koopaFirebar (id 421) lists two resources
    /// ("koopaFirebar;WLfirebarGC"), which loadGraphics() looked up as one file name and never
    /// found, so those fire bars had no model and were never reported. The fireballs draw the same
    /// model as the fire rings (WLfirebarGC: the fire ball and its glow).
    [[nodiscard]] const char *getResources() const override {
        return id() == ObjectId::KoopaFirebar ? "WLfirebarGC" : ObjectBase::getResources();
    }

    void setDistance(f32 dist) {
        m_distance = dist;
    }

    void setAngle(f32 angle) {
        m_angle = angle;
    }

    [[nodiscard]] f32 distance() const {
        return m_distance;
    }

    [[nodiscard]] f32 angle() const {
        return m_angle;
    }

private:
    f32 m_distance;
    f32 m_angle;
};

} // namespace Kinoko::Field
