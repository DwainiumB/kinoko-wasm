#pragma once

#include "game/kart/KartObjectProxy.hh"
#include "game/render/DriverItemAnchor.hh"

#include <egg/math/Matrix.hh>

/// @brief Pertains to rendering the kart model.
namespace Kinoko::Render {

/// @brief Included in Kinoko because it mysteriously sets an angle member variable in KartBody.
class KartModel : Kart::KartObjectProxy {
public:
    KartModel();
    virtual ~KartModel();

    virtual void vf_1c();

    void init();
    void calc();

    void FUN_807CB198();
    void FUN_807CB530();
    void FUN_807C7828(u8 playerIdx, bool isBike);

    /// @brief Where a held item leaves the driver's hand (0x807D171C): the right-hand anchor of
    /// the pose, pushed back by the item's and kart's size when dropping, scaled with the kart
    /// and placed with the driver matrix.
    /// @param forward true for a forward throw ("throw_f" pose), false for a drop ("throw_b").
    /// @param itemRadius The item's hit radius at its full scale.
    /// @param pos Receives the world position.
    /// @param kartPos Receives the kart's position (the game writes it to the item as well).
    /// @return false when this racer has no anchor data (no table entry).
    [[nodiscard]] bool itemHandPosition(bool forward, f32 itemRadius, EGG::Vector3f &pos,
            EGG::Vector3f &kartPos) const;

    [[nodiscard]] const EGG::Matrix34f &driverMatrix() const {
        return m_driverMatrix;
    }

    [[nodiscard]] bool hasItemAnchors() const {
        return m_hasItemAnchors;
    }

private:
    void calcDriverMatrix();

    bool m_somethingLeft;
    bool m_somethingRight;
    f32 _54;
    f32 _58;
    f32 _5c;
    f32 _64;
    bool m_isInsideDrift;
    f32 _2e8;

    EGG::Matrix34f m_driverOffsetMatrix; ///< +0x78: translation to the driver's seat.
    EGG::Matrix34f m_driverMatrix;       ///< +0x114: the driver model's world matrix.
    DriverItemAnchors m_itemAnchors;     ///< +0x6C0 / +0x6CC.
    bool m_hasItemAnchors;
};

class KartModelKart : public KartModel {
public:
    KartModelKart();
    ~KartModelKart();
};

class KartModelBike : public KartModel {
public:
    KartModelBike();
    ~KartModelBike();
};

} // namespace Kinoko::Render
