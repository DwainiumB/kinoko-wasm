#pragma once

#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

class ObjectBoble : public ObjectCollidable {
public:
    ObjectBoble(const System::MapdataGeoObj &params);
    ~ObjectBoble() override;

    void init() override;
    void calc() override;

    /// @addr{0x8075E744}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    Kart::Reaction onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
            Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) override;
    s32 onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
            EGG::Vector3f &hitDepth) override;

private:
    void calcTangent();
    void startKnockAway(const EGG::Vector3f &hitDepth, const EGG::Vector3f &hitterVel);
    void calcKnockAway();

    EGG::Vector3f m_curTangentDir;
    bool m_knockedAway = false;    ///< +0xB0 (the game's state 1)
    s32 m_knockTimer = 0;          ///< +0xC0
    bool m_knockTimeUp = false;    ///< +0xC4
    EGG::Vector3f m_knockVel;      ///< +0xC8
    EGG::Vector3f m_spinRate;      ///< +0xDC Radians per frame.
    EGG::Vector3f m_spin;          ///< +0xE8
    EGG::Matrix34f m_knockBaseMat; ///< +0xF4
};

} // namespace Kinoko::Field
