#include "KartModel.hh"

#include "game/kart/KartBody.hh"
#include "game/kart/KartCollide.hh"
#include "game/kart/KartMove.hh"
#include "game/kart/KartParam.hh"
#include "game/kart/KartParamFileManager.hh"
#include "game/kart/KartPhysics.hh"
#include "game/kart/KartState.hh"

#include "game/system/RaceConfig.hh"

#include <egg/math/Math.hh>

namespace Kinoko::Render {

KartModel::KartModel() {
    m_somethingLeft = false;
    m_somethingRight = false;
    _58 = 0.0f;
    _54 = 1.0f;
    _5c = 0.0f;
    _64 = 0.0f;
    _2e8 = 0.0f;
    m_driverOffsetMatrix = EGG::Matrix34f::ident;
    m_driverMatrix = EGG::Matrix34f::ident;
    m_itemAnchors = {EGG::Vector3f::zero, EGG::Vector3f::zero};
    m_hasItemAnchors = false;
}

KartModel::~KartModel() = default;

/// @addr{0x807CD32C}
void KartModel::vf_1c() {
    const auto &status = KartObjectProxy::status();

    if (status.onBit(Kart::eStatus::Burnout)) {
        _54 = 1.0f;

        f32 pitch = move()->burnout().pitch();
        f32 fVar2 = pitch + 75.0f * (pitch - _2e8);
        f32 fVar4 = std::min(0.2f, 0.04f * EGG::Mathf::abs(fVar2));
        fVar4 = fVar2 > 0.0f ? fVar4 : -fVar4;

        _2e8 = pitch;
        _58 += fVar4;
    } else {
        _2e8 = 0.0f;
        _58 *= 0.9f;
    }

    bool frozenInIce =
            System::RaceConfig::Instance()->raceScenario().course == Course::N64_Sherbet_Land &&
            status.onBit(Kart::eStatus::InRespawn, Kart::eStatus::AfterRespawn);
    f32 xStick = frozenInIce ? 0.0f : inputs()->currentState().stick.x;
    bool isInCannon = status.onBit(Kart::eStatus::InCannon);
    f32 fVar2 = isInCannon ? 0.02f : 0.1f;

    f32 local_f31 = _58;
    if (xStick <= 0.2f) {
        if (xStick < -0.2f) {
            _58 -= fVar2;
        }
    } else {
        _58 += fVar2;
    }

    xStick = EGG::Mathf::abs(xStick);

    if (isInCannon) {
        xStick *= 0.8f;
        fVar2 = 0.05f;
    }

    _54 += fVar2 * (xStick - _54);

    if (local_f31 < -_54 || _54 < local_f31) {
        if (-_54 <= _58) {
            if (_54 < _58) {
                _58 -= 0.1f;
            }
        } else {
            _58 += 0.1f;
        }
    } else if (-_54 <= _58) {
        _58 = std::min(_54, _58);
    } else {
        _58 = -_54;
    }

    f32 dVar13 = _58;

    if (isBike()) {
        if (state()->isDrifting()) {
            dVar13 = m_isInsideDrift ? 5.0f : 20.0f;
        } else {
            dVar13 = 15.0f;
        }
    } else {
        dVar13 = 15.0f;
    }

    f32 dVar12 = 0.0f;
    _64 = dVar13 * 0.1f;

    if (isBike()) {
        dVar12 = -_58 * dVar13;

        if (m_somethingLeft) {
            dVar12 += m_isInsideDrift ? 5.0f : 10.0f;
        } else if (m_somethingRight) {
            dVar12 -= m_isInsideDrift ? 5.0f : 10.0f;
        }
    } else {
        if (!m_somethingLeft && m_somethingRight) {
            dVar12 -= 5.0f;
        } else {
            dVar12 += 5.0f;
        }
    }

    if (dVar12 <= _5c) {
        _5c -= _64;
        _5c = std::max(_5c, dVar12);
    } else {
        _5c += _64;
        _5c = std::min(_5c, dVar12);
    }

    body()->setAngle(_5c);
}

/// @addr{0x807C8758}
void KartModel::init() {
    FUN_807C7828(param()->playerIdx(), isBike());

    // 0x807C7828: the driver sits at the vehicle/character offset from kartDriverDispParam.bin,
    // and the driver matrix starts from the pose at setup.
    const auto &player =
            System::RaceConfig::Instance()->raceScenario().players[param()->playerIdx()];
    EGG::Vector3f offset =
            Kart::KartParamFileManager::Instance()->driverOffset(player.vehicle, player.character);
    m_driverOffsetMatrix.makeT(offset);
    calcDriverMatrix();

    // 0x807C8758: the right hand's position in the two item poses, from the driver model.
    m_hasItemAnchors = DriverItemAnchor::Lookup(player.vehicle, player.character, m_itemAnchors);
    if (!m_hasItemAnchors) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            WARN("No driver item anchors for vehicle %d / character %d: items will leave from the "
                 "kart's centre (generate %s with kinoko_host item-anchors)",
                    static_cast<int>(player.vehicle), static_cast<int>(player.character),
                    DriverItemAnchor::TABLE_PATH);
        }
    }

    _2e8 = 0.0f;
}

/// @addr{0x807CB360}
void KartModel::calc() {
    FUN_807CB530();
    calcDriverMatrix();
}

/// @addr{0x807CB360}
/// @brief The driver model's matrix: the body matrix (the pose with the engine rumble) with the
/// seat offset, the offset scaled with the kart. King Boo floats and uses the plain pose.
void KartModel::calcDriverMatrix() {
    const auto &player =
            System::RaceConfig::Instance()->raceScenario().players[param()->playerIdx()];
    const EGG::Matrix34f &bodyRot =
            player.character == Character::King_Boo ? physics()->pose() : body()->matrix();
    const EGG::Vector3f &s = scale();

    if (s.x == 1.0f && s.y == 1.0f && s.z == 1.0f) {
        m_driverMatrix = bodyRot.multiplyTo(m_driverOffsetMatrix);
        return;
    }

    EGG::Matrix34f scaled = m_driverOffsetMatrix;
    scaled[0, 3] = scaled[0, 3] * s.x;
    scaled[1, 3] = scaled[1, 3] * s.y;
    scaled[2, 3] = scaled[2, 3] * s.z;
    m_driverMatrix = bodyRot.multiplyTo(scaled);
}

/// @addr{0x807D171C}
bool KartModel::itemHandPosition(bool forward, f32 itemRadius, EGG::Vector3f &pos,
        EGG::Vector3f &kartPos) const {
    constexpr f32 SLOPE_FACTOR = 5.0f;

    if (!m_hasItemAnchors) {
        return false;
    }

    const EGG::Vector3f &anchor = forward ? m_itemAnchors.throwForward : m_itemAnchors.dropBehind;
    EGG::Vector3f local(0.0f, anchor.y, anchor.z);

    if (!forward) {
        // A drop starts further back and higher the bigger the item and the kart are, less so at
        // speed and on a slope.
        f32 speedRatio = move()->speedRatioCapped();
        f32 divisor = SLOPE_FACTOR * (1.0f + componentZAxis().y);
        if (!(divisor >= 1.0f)) {
            divisor = 1.0f;
        }
        f32 reach = (itemRadius + collide()->boundingRadius()) / divisor;
        f32 back = (1.0f - speedRatio) * reach;
        local.y = local.y + back;
        local.z = local.z - back;
    }

    const EGG::Vector3f &s = scale();
    f32 scaleY = s.y;
    if (!(scaleY >= 1.0f)) {
        scaleY = 1.0f;
    }
    local.x = local.x * s.x;
    local.y = local.y * scaleY;
    local.z = local.z * s.z;

    pos = m_driverMatrix.ps_multVector(local);
    kartPos = KartObjectProxy::pos();
    return true;
}

/// @addr{0x807CB198}
/// @rename
void KartModel::FUN_807CB198() {
    m_somethingRight = false;
    m_somethingLeft = false;
    auto &status = KartObjectProxy::status();

    bool turnInput = status.onBit(Kart::eStatus::StickLeft, Kart::eStatus::StickRight);
    if (state()->isDrifting() || (status.onBit(Kart::eStatus::ChargingSSMT) && turnInput)) {
        if (move()->hopStickX() == 1) {
            m_somethingLeft = true;
        } else {
            if (move()->hopStickX() == -1) {
                m_somethingRight = true;
            } else if (status.offBit(Kart::eStatus::StickLeft)) {
                m_somethingRight = true;
            } else {
                m_somethingLeft = true;
            }
        }
    }
}

/// @addr{0x807CB530}
/// @rename
void KartModel::FUN_807CB530() {
    FUN_807CB198();
    vf_1c();
}

/// @addr{0x807C7828}
/// @rename
void KartModel::FUN_807C7828(u8 /*playerIdx*/, bool /*isBike*/) {
    m_isInsideDrift =
            param()->stats().driftType == Kart::KartParam::Stats::DriftType::Inside_Drift_Bike;
}

/// @addr{0x807C7364}
KartModelKart::KartModelKart() = default;

/// @addr{0x807CDD08}
KartModelKart::~KartModelKart() = default;

/// @addr{0x807CDCCC}
KartModelBike::KartModelBike() = default;

/// @addr{0x807D3F58}
KartModelBike::~KartModelBike() = default;

} // namespace Kinoko::Render
