#pragma once

#include "game/item/ItemObjKoura.hh"

namespace Kinoko::Item {

/// @brief The blue shell (vtable 0x808D23D8, 0x338 bytes): it follows the item route towards the
/// leader, locks on, circles above them, dives and explodes like a bob-omb. Its item state is a
/// state machine of its own (+0x2BC, table 0x808D2308): 0 slide (no item route), 1 on the route
/// choosing its target, 2 homing, 3 circling, 4 the dive, 5 the descent, 6 exploding.
/// @details Its box collision is removed when it is thrown and comes back for the descent and the
/// explosion. Model, effect, sound, HUD and camera code is not ported, nor the online paths and
/// the battle team checks.
/// @nosubgrouping
class ItemObjBlueShell : public ItemObjKoura {
public:
    ItemObjBlueShell();
    ~ItemObjBlueShell() override;

    /// @beginGetters
    /// @brief The explosion is past its launching part (+0x334): it only spins racers now.
    [[nodiscard]] bool explosionWeakened() const {
        return m_weakened;
    }

    [[nodiscard]] bool isExploding() const {
        return m_state == State::Explode;
    }
    /// @endGetters

protected:
    void onSpawn() override;
    [[nodiscard]] bool onRelease(ItemTrail &trail) override;
    [[nodiscard]] s32 onBreak() override;
    [[nodiscard]] StateFunc initialState() const override;

private:
    enum class State {
        Slide = 0,
        Route = 1,
        Homing = 2,
        Circle = 3,
        Dive = 4,
        Descent = 5,
        Explode = 6,
    };

    void calcBlue();
    void enterState(State state);
    void calcState(State state);

    void calcStateSlide();
    void calcStateRoute();
    void enterStateHoming();
    void calcStateHoming();
    void enterStateCircle();
    void calcStateCircle();
    void enterStateDive();
    void calcStateDive();
    void enterStateDescent();
    void calcStateDescent();
    void enterStateExplode();
    void calcStateExplode();

    void setUpThrow();
    [[nodiscard]] u8 chooseTarget() const;
    [[nodiscard]] bool tryLock(u8 playerIdx);
    void setTarget(u8 playerIdx);
    void homeIn(f32 speed, f32 radius, f32 rate, f32 blend, u16 angle, bool noReverse);
    void descend(f32 rate, f32 height);
    void calcHeight(bool update);
    void calcOrientation(const EGG::Vector3f &move, bool follow);
    [[nodiscard]] bool calcSquash();
    [[nodiscard]] bool targetInCannon() const;

    u8 m_target;               ///< +0x280 12: none (the game keeps the target's KartItem).
    EGG::Vector3f m_targetDir; ///< +0x284 The target's direction of motion.
    EGG::Vector3f m_aimDiff;   ///< +0x290
    EGG::Vector3f m_targetPos; ///< +0x29C
    bool m_tracking;           ///< +0x2A8 Following the target's position.
    bool m_onRoute;            ///< +0x2A9 Thrown with an item point: on the item route.
    u8 m_lockCandidate;        ///< +0x2AA
    EGG::Vector3f m_aimOffset; ///< +0x2AC
    u32 m_circleAngle;         ///< +0x2B8
    State m_state;             ///< +0x2C0
    s32 m_nextState;           ///< +0x2C4 -1: none.
    u32 m_stateFrame;          ///< +0x2C8
    f32 m_height;              ///< +0x2DC Height of the dive over the target.
    f32 m_heightRate;          ///< +0x2E0
    u8 m_chosen;               ///< +0x2EC The racer it is after.
    bool m_chaseNext;          ///< +0x2ED The thrower leads: it targets them too.
    f32 m_flySpeed;            ///< +0x2F0
    u16 m_chosenMask;          ///< +0x2F4 Every racer chosen so far.
    bool m_lowDive;            ///< +0x2F6
    f32 m_circleRadius;        ///< +0x2F8
    u16 m_circleStep;          ///< +0x2FC
    EGG::Vector3f m_moveDelta; ///< +0x300 Last frame's motion.
    EGG::Vector3f m_lastMove;  ///< +0x30C
    EGG::Vector3f m_tilt;      ///< +0x318
    EGG::Vector3f m_diveMove;  ///< +0x324
    bool m_weakened;           ///< +0x334
};

} // namespace Kinoko::Item
