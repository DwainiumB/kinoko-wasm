/// TODO: Also need 806d2908

#pragma once

#include "game/field/StateManager.hh"
#include "game/field/obj/ObjectCollidable.hh"

namespace Kinoko::Field {

class ObjectHighwayManager;

class ObjectCarTGE : public ObjectCollidable, public StateManager {
public:
    enum class CarType {
        Normal = 0,
        Truck = 1,
        BombCar = 2, ///< Unused in time trials
    };

    ObjectCarTGE(const System::MapdataGeoObj &params);
    ~ObjectCarTGE() override;

    void init() override;
    void calc() override;

    /// @addr{0x806DA7AC}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x806D68B0}
    [[nodiscard]] const char *getResources() const override {
        return m_carName;
    }

    /// @addr{0x806DA7A4}
    [[nodiscard]] const char *getKclName() const override {
        return m_mdlName;
    }

    void createCollision() override;
    void calcCollisionTransform() override;
    [[nodiscard]] f32 getCollisionRadius() const override;
    Kart::Reaction onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
            Kart::Reaction reactionOnObj, EGG::Vector3f &hitDepth) override;
    bool checkCollision(ObjectCollisionBase *lhs, EGG::Vector3f &dist) override;
    [[nodiscard]] const EGG::Vector3f &collisionCenter() const override;

    void setHighwayManager(const ObjectHighwayManager *highwayMgr) {
        m_highwayMgr = highwayMgr;
    }

    s32 onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
            EGG::Vector3f &hitDepth) override;

    /// @addr{0x806D9A04}
    void reset() {
        m_squashed.fill(false);
    }

    /// @brief This frame the car hit the player (+0x1B1, one per player).
    [[nodiscard]] bool squashed(size_t playerIdx) const {
        return m_squashed[playerIdx];
    }

private:
    static constexpr f32 TOLL_BOOTH_ACCEL = 200.0f;

    void enterStateStub();
    void calcStateStub();
    void calcState1();
    void calcState2();

    void calcPos();
    void startLaunch();
    void calcLaunch();

    const ObjectHighwayManager *m_highwayMgr;
    ObjectCollisionBase *m_auxCollision;
    f32 m_highwayVel; ///< Speed while on the highway
    f32 m_localVel;   ///< Speed while off the highway
    char m_carName[32];
    char m_mdlName[32];
    CarType m_carType; ///< Car, truck, or bomb car
    ObjectId m_dummyId;
    EGG::Vector3f m_scaledTangentDir;
    f32 m_currSpeed;
    EGG::Vector3f m_up;
    EGG::Vector3f m_tangent;
    std::array<bool, 12> m_squashed;
    bool m_launched;      ///< +0x1BD Knocked into the air by an item or a special kart.
    u32 m_launchTimer;    ///< +0x1C0
    f32 m_launchBaseY;    ///< +0x1C4
    f32 m_launchRollRate; ///< +0x204 Degrees per frame on the way up.
    f32 m_landFrame;      ///< +0x208 Launch frame of the first landing (200/3 before it).
    bool m_landed;        ///< +0x220 Bounced once.
    bool m_landedAgain;   ///< +0x221
    bool m_bouncedOnLeft; ///< +0x230 The first bounce was on the left corner: the roll reverses.
    bool m_hasAuxCollision;
    f32 m_hitAngle;

    static constexpr std::array<StateManagerEntry, 3> STATE_ENTRIES = {{
            {StateEntry<ObjectCarTGE, &ObjectCarTGE::enterStateStub, &ObjectCarTGE::calcStateStub>(
                    0)},
            {StateEntry<ObjectCarTGE, &ObjectCarTGE::enterStateStub, &ObjectCarTGE::calcState1>(1)},
            {StateEntry<ObjectCarTGE, &ObjectCarTGE::enterStateStub, &ObjectCarTGE::calcState2>(2)},
    }};
};

} // namespace Kinoko::Field
