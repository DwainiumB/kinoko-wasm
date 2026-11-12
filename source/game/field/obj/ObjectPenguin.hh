#pragma once

#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

/// @brief The base class for penguins on N64 Sherbet Land.
/// @details In the base game, the cave penguins derive from this class, but they do not override
/// any physics-dependent virtual functions. For Kinoko, we simply construct these penguins as a
/// base class instance. The cave penguins walk along a rail, always facing perpendicular to the
/// tangent line.
class ObjectPenguin : public ObjectCollidable {
public:
    ObjectPenguin(const System::MapdataGeoObj &params);
    ~ObjectPenguin() override;

    void init() override;

    /// @addr{0x80775764}
    void calc() override {
        if (m_state == State::Walk) {
            calcWalk();
        } else if (m_state == State::Hit) {
            calcHit();
        }
    }

    /// @addr{0x80777324}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x807757A0}
    virtual void calcWalk() {
        m_railInterpolator->calc();
        calcPos();
        calcRot();
    }

    void calcRot();

    /// @addr{0x80775C2C}
    virtual void calcPos() {
        setPos(m_railInterpolator->curPos());
    }

    virtual void enterWalk();

    Kart::Reaction onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
            Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) override;
    s32 onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
            EGG::Vector3f &hitDepth) override;

    virtual void startHit();
    virtual void calcHit();

protected:
    void calcHitSpin();

    static constexpr s32 HIT_DURATION = 180;
    static constexpr f32 HIT_SPIN_SPEED = 60.0f;

    enum class State {
        Walk = 0,
        Dive = 1,
        Slider = 2,
        SliderSlow = 3,
        StandUp = 4,
        Hit = 5, ///< Knocked into a spin by an item or a kart in a star, mega mushroom or bullet.
    };

    State m_state;
    EGG::Vector3f m_basis;
    s32 m_hitTimer;  ///< +0xC0 Frames left in the hit state.
    f32 m_spinSpeed; ///< +0xC4 Degrees per frame, from 60 down to 12.
};

/// @brief Represents the penguins on N64 Sherbet Land that slide on their stomach.
class ObjectPenguinS final : public ObjectPenguin {
public:
    ObjectPenguinS(const System::MapdataGeoObj &params);
    ~ObjectPenguinS() override;

    void init() override;
    void calc() override;

    void loadAnims() override;

    /// @addr{0x8077637C}
    void calcWalk() override {
        calcRail();
        calcPos();
        calcRot();
    }

    /// @addr{0x80776AF0}
    void enterWalk() override {
        m_state = State::Walk;
    }

    void startHit() override;
    void calcHit() override;

    /// Added for Kinoko web's renderer: unlike the base ObjectPenguin (always Walk), this penguin
    /// has 5 real states (Walk/Dive/Slider/SliderSlow/StandUp) that move it very differently
    /// (walking vs. belly-sliding down a rail) -- without exposing which one is active, the
    /// renderer picked one clip at spawn (always "walk", since ANIM_PREFERENCE always finds it) and
    /// kept it forever, so the model played its walk-cycle leg animation throughout the
    /// slide/dive/stand-up sections too.
    [[nodiscard]] s32 animState() const override {
        return static_cast<s32>(m_state);
    }

private:
    /// @addr{0x807763D0}
    void calcDive() {
        calcWalk();
    }

    void calcSlider();
    void calcStandUp();
    void calcRail();

    s32 m_anmTimer;
    State m_stateBeforeHit; ///< +0xD8
};

} // namespace Kinoko::Field
