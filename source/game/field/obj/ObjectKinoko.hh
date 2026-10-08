#pragma once

#include "game/field/obj/ObjectKCL.hh"

namespace Kinoko::Field {

enum class KinokoType : u16 {
    Light = 0,
    Dark = 1,
};

/// @brief The base class for a mushroom object with jump pad properties
class ObjectKinoko : public ObjectKCL {
public:
    ObjectKinoko(const System::MapdataGeoObj &params);
    ~ObjectKinoko() override;

    void calc() override;

    /// @addr {0x80807DAC}
    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    /// @addr{0x80807D8C}
    [[nodiscard]] const char *getKclName() const override {
        return m_type == KinokoType::Light ? "kinoko_r" : "kinoko_d_r";
    }
    virtual void calcOscillation() = 0;

protected:
    KinokoType m_type;
    const EGG::Vector3f m_objPos; // the initial position of the object
    const EGG::Vector3f m_objRot; // the initial rotation of the object
    s16 m_pulseFrame;
    s16 m_restFrame;
    f32 m_pulseFalloff;
    u16 m_oscFrame;
};

/// @brief Mushrooms which oscillate up and down. The stem does not move.
/// @details This represents the first two mushrooms on MG, even though they don't oscillate up or
/// down.
class ObjectKinokoUd : public ObjectKinoko {
public:
    ObjectKinokoUd(const System::MapdataGeoObj &params);
    ~ObjectKinokoUd() override;

    void calcOscillation() override;

    /// @addr{0x80807DFC}
    /// @details The base game does check for the light type, however since m_type never gets set
    /// it'll always be 0 which means it always returns "kinoko_r"
    [[nodiscard]] const char *getKclName() const override {
        return "kinoko_r";
    }

    /// @addr{0x80807DF8}
    void calcScale(u32) override {}

private:
    u16 m_waitFrame;
    s16 m_period;
    s16 m_waitDuration;
    s16 m_amplitude;
    f32 m_angFreq;
};

/// @brief Mushrooms which bend in a certain direction
/// @details This functionality didn't get used in the base game?
class ObjectKinokoBend : public ObjectKinoko {
public:
    ObjectKinokoBend(const System::MapdataGeoObj &params);
    ~ObjectKinokoBend() override;
    void calcOscillation() override;

    /// @addr{0x80807D88}
    void calcScale(u32) override {}

private:
    s16 m_currentFrame;
    s16 m_period;
    f32 m_amplitude;
    f32 m_angFreq;
};

/// @brief Mushroom Gorge's giant bridge-platform mushroom caps (object id KinokoT1). Bobs
/// vertically on a fixed cycle; does NOT pulse in scale.
/// @details Not decompiled -- no public source documents this object's real class or address.
/// An earlier version of this class guessed it shared ObjectKinoko's scale-pulse animation (based
/// only on a community description calling it a "moving bouncy mushroom decoration"); that guess
/// was WRONG, disproven by a real Dolphin memory capture synchronized against a recorded video of
/// the actual motion (see project notes, 2026-09-27): the object's scale never leaves 1.0, but its
/// world Y position genuinely oscillates, smoothly and precisely, with a 500-unit amplitude --
/// fitted directly from that capture (the real cosine curve matched the fit within ~2.5%, i.e. ~13
/// units on the 500-unit swing). It uses the exact same mathematical form as
/// ObjectKinokoUd::calcOscillation() (posY = objPos.y + amplitude * (cos(angFreq*frame)+1) * 0.5).
///
/// The 4 real placements on this course are NOT synchronized -- each has a different real period
/// (measured directly, one at a time, via the same memory-capture technique): rotY=+9.7 deg ->
/// 182 frames, rotY=-3.85 deg -> ~163 frames, rotY=-33.65 deg -> ~137 frames (the 4th, rotY=+80.55
/// deg, couldn't be pinned down directly -- its animating field wasn't findable at the same relative
/// offset, likely because the large rotation redistributes it across multiple matrix cells). Those
/// 3 points fit a roughly linear relationship between period and the placement's own Y rotation
/// (this course's KMP settings for every KinokoT1 placement are all zero, ruling out a
/// settings-driven period or amplitude), so PERIOD below derives it from rot().y at load instead of
/// a single shared constant -- this reproduces the real desync (each instance ticks at its own real
/// rate) without needing a per-instance lookup table, though it's a fitted approximation, not a
/// decompiled formula, and unverified for the 4th (extreme-rotation) placement. The starting phase
/// (m_oscFrame = 0 on load) is also an assumption, not verified against a real race start -- but
/// since each instance's period differs, they drift apart over time regardless. If a real decompiled
/// reference for this object ever surfaces, replace this fitted formula with it.
class ObjectKinokoT1 : public ObjectKCL {
public:
    ObjectKinokoT1(const System::MapdataGeoObj &params);
    ~ObjectKinokoT1() override;

    void calc() override;

    [[nodiscard]] u32 loadFlags() const override {
        return 1;
    }

    [[nodiscard]] const char *getKclName() const override {
        return "kinoko_r";
    }

    void calcScale(u32) override {}

private:
    const EGG::Vector3f m_objPos;
    const u16 m_period;
    u16 m_oscFrame;
};

/// @brief The class for a mushroom object with normal road properties, for the most part
class ObjectKinokoNm : public ObjectKCL {
public:
    ObjectKinokoNm(const System::MapdataGeoObj &params);
    ~ObjectKinokoNm() override;

    /// @addr{0x80827A74}
    [[nodiscard]] const char *getKclName() const override {
        return m_type == KinokoType::Light ? "kinoko_g" : "kinoko_d_g";
    }

private:
    KinokoType m_type;
};

} // namespace Kinoko::Field
