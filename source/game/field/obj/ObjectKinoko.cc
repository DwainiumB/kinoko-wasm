#include "ObjectKinoko.hh"

namespace Kinoko::Field {

/// @addr{0x8080761C}
ObjectKinoko::ObjectKinoko(const System::MapdataGeoObj &params)
    : ObjectKCL(params), m_objPos(pos()), m_objRot(rot()) {
    m_type = static_cast<KinokoType>(params.setting(0));

    m_restFrame = 0;
    m_pulseFrame = 1; // (-1*-1) % PULSE_DURATION;
    m_pulseFalloff = 0.1f;
}

/// @addr{0x80807A54}
ObjectKinoko::~ObjectKinoko() = default;

/// @addr{0x8080782C}
void ObjectKinoko::calc() {
    constexpr s16 REST_DURATION = 10;
    constexpr s16 PULSE_DURATION = 40;
    constexpr f32 PULSE_SCALE = 0.0008f;
    constexpr f32 PULSE_FREQ = 6.0f * F_PI / 40.0f;

    if (m_restFrame == 0) {
        ++m_pulseFrame;
    }
    if (m_pulseFrame == PULSE_DURATION) {
        ++m_restFrame;
    }
    if (m_restFrame > REST_DURATION) {
        m_restFrame = 0;
    }
    if (m_pulseFrame > PULSE_DURATION) {
        m_pulseFrame = 0;
    }

    m_pulseFalloff = PULSE_SCALE * static_cast<f32>(PULSE_DURATION - m_pulseFrame);
    setScale(m_pulseFalloff * EGG::Mathf::sin(PULSE_FREQ * static_cast<f32>(m_pulseFrame)) + 1.0f);
    calcOscillation();
}

/// @addr{0x80807950}
ObjectKinokoUd::ObjectKinokoUd(const System::MapdataGeoObj &params) : ObjectKinoko(params) {
    m_waitFrame = 0;
    m_oscFrame = params.setting(3);
    m_waitDuration = params.setting(4);
    m_amplitude = params.setting(1);
    m_period = std::max<u16>(params.setting(2), 2);
    m_angFreq = F_TAU / static_cast<f32>(m_period);
}

/// @addr{0x80807E1C}
ObjectKinokoUd::~ObjectKinokoUd() = default;

/// @addr{0x80807A54}
void ObjectKinokoUd::calcOscillation() {
    f32 posY = m_objPos.y +
            static_cast<f32>(m_amplitude) *
                    (EGG::Mathf::cos(m_angFreq * static_cast<f32>(m_oscFrame)) + 1.0f) * 0.5f;
    setPos(EGG::Vector3f(pos().x, posY, pos().z));

    if (m_waitFrame == 0) {
        ++m_oscFrame;
    }
    if (m_oscFrame == (m_period / 2)) {
        ++m_waitFrame;
    }
    if (m_waitFrame > m_waitDuration) {
        m_waitFrame = 0;
    }
    if (m_oscFrame > m_period) {
        m_oscFrame = 0;
    }
}

/// @addr{0x80807B7C}
ObjectKinokoBend::ObjectKinokoBend(const System::MapdataGeoObj &params) : ObjectKinoko(params) {
    m_currentFrame = params.setting(3);
    m_amplitude = static_cast<f32>(params.setting(1)) * DEG2RAD;
    m_period = std::max<u16>(params.setting(2), 2);
    m_angFreq = F_TAU / static_cast<f32>(m_period);
}

/// @addr{0x80807DB4}
ObjectKinokoBend::~ObjectKinokoBend() = default;

/// @addr{0x80807C98}
void ObjectKinokoBend::calcOscillation() {
    const f32 s = EGG::Mathf::sin(m_angFreq * static_cast<f32>(m_currentFrame));
    EGG::Vector3f rot = m_objRot + (EGG::Vector3f::ez * s) * m_amplitude;

    calcTransform();
    setRot(transform().multVector33(rot));

    if (++m_currentFrame >= m_period) {
        m_currentFrame = 0;
    }
}

namespace {
/// Fitted from 3 real, individually memory-captured placements (rotY -> period): (9.7, 182),
/// (-3.85, 163), (-33.65, 137) -- see ObjectKinokoT1's class comment (ObjectKinoko.hh). Not a
/// decompiled formula.
u16 FitKinokoT1Period(f32 rotYDegrees) {
    f32 period = 166.0f + 1.05f * rotYDegrees;
    if (period < 60.0f) {
        period = 60.0f;
    } else if (period > 400.0f) {
        period = 400.0f;
    }
    return static_cast<u16>(period);
}
} // namespace

ObjectKinokoT1::ObjectKinokoT1(const System::MapdataGeoObj &params)
    : ObjectKCL(params), m_objPos(pos()), m_period(FitKinokoT1Period(rot().y)), m_oscFrame(0) {}

ObjectKinokoT1::~ObjectKinokoT1() = default;

void ObjectKinokoT1::calc() {
    // Fitted from a real memory capture, not decompiled -- see the class comment (ObjectKinoko.hh).
    constexpr f32 AMPLITUDE = 500.0f;
    const f32 angFreq = F_TAU / static_cast<f32>(m_period);

    f32 posY = m_objPos.y +
            AMPLITUDE * (EGG::Mathf::cos(angFreq * static_cast<f32>(m_oscFrame)) + 1.0f) * 0.5f;
    setPos(EGG::Vector3f(pos().x, posY, pos().z));

    if (++m_oscFrame >= m_period) {
        m_oscFrame = 0;
    }
}

ObjectKinokoNm::ObjectKinokoNm(const System::MapdataGeoObj &params) : ObjectKCL(params) {
    m_type = static_cast<KinokoType>(params.setting(0));
}

/// @addr{0x80827A9C}
ObjectKinokoNm::~ObjectKinokoNm() = default;

} // namespace Kinoko::Field
