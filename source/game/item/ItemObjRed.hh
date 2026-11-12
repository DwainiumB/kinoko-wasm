#pragma once

#include "game/item/ItemObjKoura.hh"

namespace Kinoko::Item {

/// @brief The red shell (vtable 0x808D2228): follows the item route, picks a racer ahead and homes
/// in on it. A state machine (table 0x808D2148): 0 / 1 free slide, 2 free slide looking for a
/// target, 3 on the item route looking for a target, 4 homing, 5 homing close (online), 6 lost.
/// @nosubgrouping
class ItemObjRedShell : public ItemObjKoura {
public:
    ItemObjRedShell();
    ~ItemObjRedShell() override;

protected:
    [[nodiscard]] bool onRelease(ItemTrail &trail) override;
    [[nodiscard]] StateFunc initialState() const override;

private:
    void calcRed();
    void calcStateFree();
    void calcStateSearch();
    void enterStateRoute();
    void calcStateRoute();
    void enterStateHoming();
    void onRouteWall(const EGG::Vector3f &nrm);
    void calcStateHoming();
    void calcStateLost();
    void startState(s32 state);

    [[nodiscard]] u8 playerAhead() const;
    [[nodiscard]] bool searchTarget(bool fromOwnerPlace, bool roundRobin);
    [[nodiscard]] bool tryTarget(u8 playerIdx, f32 &bestDistSq, bool fromOwnerPlace);
    [[nodiscard]] bool calcHoming(f32 speed, f32 rate, bool breakOnWall);
    void dropTarget();

    u8 m_target;                ///< +0x280 Target racer (the game keeps its KartItem). 12: none.
    f32 m_targetDist;           ///< +0x284
    EGG::Vector3f m_aimPoint;   ///< +0x288
    EGG::Vector3f m_targetBack; ///< +0x294 Smoothed back axis of the target.
    EGG::Vector3f m_homingVel;  ///< +0x2A0
    s32 m_homingFrames;         ///< +0x2AC
    u16 m_state;                ///< +0x2B4
    s32 m_nextState;            ///< +0x2B8 -1: none.
    u32 m_stateFrame;           ///< +0x2BC
};

} // namespace Kinoko::Item
