#pragma once

#include "game/field/ObjectDirector.hh"
#include "game/field/StateManager.hh"
#include "game/field/obj/ObjectProjectile.hh"

namespace Kinoko::Field {

class ObjectFireSnakeKid : public ObjectCollidable {
public:
    ObjectFireSnakeKid(const System::MapdataGeoObj &params);
    ~ObjectFireSnakeKid() override;

    /// @addr{0x806C2B60}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// Added for Kinoko web's renderer: ObjectCollidable::load() automatically registers every
    /// object's collision (loadAABB()) the moment it's created, regardless of this snake's state --
    /// but calcChildren() is the thing that's actually meant to control when a kid first becomes
    /// active (staggered 10/20 frames into the parent's first Falling cycle). Before that first
    /// cycle ever runs (e.g. while a long per-placement launch delay hasn't elapsed yet), the kid sat
    /// auto-registered and visible at its raw, never-updated construction position -- a stale
    /// duplicate on top of every other not-yet-launched kid sharing the same placement. Skipping the
    /// auto-registration here (but still building the real collision shape via createCollision(), so
    /// calcCollisionTransform() stays safe once calcChildren() does register it) defers registration
    /// entirely to that existing, correctly-timed call.
    void load() override {
        loadGraphics();
        loadAnims();
        createCollision();
        loadRail();
        ObjectDirector::Instance()->addObject(this);
    }

    /// Added for Kinoko web's renderer: this kid shares its parent ObjectFireSnake's transform via
    /// calcChildren(), which only registers/unregisters its collision in lockstep with the parent's
    /// Despawned <-> Falling transition -- using that as a visibility proxy avoids exporting kids
    /// (and the despawned parent itself, see below) collapsed on top of each other at a stale
    /// position while idle.
    [[nodiscard]] bool isVisible() const override {
        return getUnit() != nullptr;
    }
};

class ObjectFireSnake : public ObjectProjectile, virtual public StateManager {
public:
    ObjectFireSnake(const System::MapdataGeoObj &params);
    ~ObjectFireSnake() override;

    void init() override;
    void calc() override;

    /// @addr{0x806C2A5C}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    void initProjectile(const EGG::Vector3f &pos) override;
    void onLaunch() override;

    /// Added for Kinoko web's renderer: see ObjectFireSnakeKid::isVisible(). While Despawned the
    /// snake sits frozen at m_sunPos (often shared by several placements) with no collision.
    [[nodiscard]] bool isVisible() const override {
        return getUnit() != nullptr;
    }

    void enterDespawned();
    void enterFalling();
    void enterHighBounce();
    void enterRest();
    void enterBounce() {}
    void enterDespawning() {}

    void calcDespawned() {}
    void calcFalling();
    void calcHighBounce();
    void calcRest();
    void calcBounce();
    void calcDespawning() {}

protected:
    void calcChildren();

    EGG::Vector3f m_sunPos;
    EGG::Vector3f m_initialPos;
    EGG::Vector3f m_initRot;
    EGG::Vector3f m_visualPos;
    EGG::Vector3f m_bounceDir;
    u16 m_age; ///< How long the firesnake has been spawned
    u16 m_delayFrame;

private:
    void calcBounce(f32 initialVel);

    bool isCollisionEnabled() const {
        return m_currentStateId == 2 || m_currentStateId == 3 || m_currentStateId == 4;
    }

    std::array<ObjectFireSnakeKid *, 2> m_kids;
    const s16 m_maxAge; ///< Number of frames until the snake will disappear
    EGG::Vector3f m_xzSunDist;
    EGG::Vector3f m_fallAxis;
    f32 m_xzSpeed;
    u16 m_fallDuration;                              ///< How long the firesnake falls from the sun
    std::array<EGG::Matrix34f, 21> m_prevTransforms; ///< The last 21 transformation matrices

    static constexpr std::array<StateManagerEntry, 6> STATE_ENTRIES = {{
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterDespawned,
                    &ObjectFireSnake::calcDespawned>(0)},
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterFalling,
                    &ObjectFireSnake::calcFalling>(1)},
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterHighBounce,
                    &ObjectFireSnake::calcHighBounce>(2)},
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterRest, &ObjectFireSnake::calcRest>(
                    3)},
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterBounce,
                    &ObjectFireSnake::calcBounce>(4)},
            {StateEntry<ObjectFireSnake, &ObjectFireSnake::enterDespawning,
                    &ObjectFireSnake::calcDespawning>(5)},
    }};

    static constexpr f32 GRAVITY = 3.0f;
};

} // namespace Kinoko::Field
