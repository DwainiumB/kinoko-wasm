#pragma once

#include "game/kart/KartObjectProxy.hh"

namespace Kinoko {

namespace Host {
class Context;
} // namespace Host

} // namespace Kinoko

namespace Kinoko::Kart {

enum class Action {
    None = -1,
    UNK_0 = 0,
    UNK_1 = 1,
    UNK_2 = 2,
    UNK_3 = 3,
    UNK_4 = 4,
    UNK_5 = 5,
    UNK_6 = 6,
    UNK_7 = 7,
    UNK_8 = 8,
    UNK_9 = 9,
    LightningHit = 10, ///< Shrunk by lightning: a spin. Start @addr{0x8056804C}.
    PowSpin = 11,      ///< A POW block's last shake on the ground. Start @addr{0x80568064}.
    UNK_12 = 12,
    MegaSquash = 13, ///< Squashed by a racer in a mega mushroom. Start @addr{0x80569818}.
    UNK_14 = 14,
    UNK_15 = 15,
    UNK_16 = 16,
    PowHit = 17, ///< Hit by a POW block. Start @addr{0x8056804C}.
    Max = 18,
};

class KartAction : KartObjectProxy {
    friend class Host::Context;

public:
    enum class eFlags {
        Landing = 0,
        PowDodged = 1, ///< The POW spin was dodged with a trick input: a light spin.
        LargeFlip = 2,
        Rotating = 3,
        LandingFromFlip = 5,
    };
    typedef EGG::TBitFlag<u32, eFlags> Flags;

    KartAction();
    ~KartAction();

    void init();
    void calc();
    void calcVehicleSpeed();
    bool start(Action action, bool hitsMega = false);
    void startRotation(size_t idx);

    void setHitDepth(const EGG::Vector3f &hitDepth) {
        m_hitDepth = hitDepth;
    }

    void setTranslation(const EGG::Vector3f &v) {
        m_translation = v;
    }

    [[nodiscard]] const Flags &flags() const {
        return m_flags;
    }

    [[nodiscard]] Action currentAction() const {
        return m_currentAction;
    }

    [[nodiscard]] u32 frame() const {
        return m_frame;
    }

    /// @addr{0x805674F4}
    void setPowAffected(bool affected) {
        m_powAffected = affected;
    }

    /// @addr{0x8056754C}
    /// @brief A POW block's last shake hits us this frame.
    [[nodiscard]] bool isPowStriking() const {
        return s_powStrike && m_powAffected;
    }

    bool dodgePow();

    static void StartPowStrike(u8 stage);
    static void CalcPowStrike();
    static void ResetPowStrike();

private:
    /// @brief Parameters specific to an action ID.
    struct ActionParams {
        f32 startSpeedMult;
        f32 calcSpeedMult;
        s16 priority;
    };

    struct RotationParams {
        f32 initialAngleIncrement;
        f32 minAngleIncrement;
        f32 minMultiplier;
        f32 initialMultiplierDecrement;
        f32 slowdownThreshold;
        f32 finalAngle;
    };

    // The player index sent into StartActionFunc is assumed to be cosmetic
    typedef void (KartAction::*StartActionFunc)();
    typedef bool (KartAction::*CalcActionFunc)();
    typedef void (KartAction::*EndActionFunc)(bool arg);

    void calcSideFromHitDepth();
    void calcSideFromHitDepthAndTranslation();

    void end();

    bool calcCurrentAction();
    void calcEndAction(bool endArg);
    bool calcRotation();
    void calcUp();
    void calcLanding();
    void startLaunch(f32 extVelScalar, f32 extVelKart, f32 extVelBike, f32 numRotations,
            u32 param6);
    void activateCrush(u16 timer);

    void applyStartSpeed();
    void setRotation(size_t idx);

    /* ================================ *
     *     START FUNCTIONS
     * ================================ */

    void startStub();
    void startAction0();
    void startAction1();
    void startAction2();
    void startAction3();
    void startAction4();
    void startAction6();
    void startLightningAction();
    void startAction5();
    void startLargeFlipAction();
    void startAction9();
    void startLongPressAction();
    void startShortPressAction();
    void startMegaSquashAction();
    void startSpinShrinkAction();
    void startPowAction();

    /* ================================ *
     *     CALC FUNCTIONS
     * ================================ */

    bool calcStub();
    bool calcAction1();
    bool calcLaunchAction();
    bool calcAction4();
    bool calcLargeFlipAction();
    bool calcPressAction();
    bool calcPowAction();

    /* ================================ *
     *     END FUNCTIONS
     * ================================ */

    void endStub(bool arg);
    void endAction1(bool arg);
    void endLaunchAction(bool arg);

    EGG::Vector3f m_side;
    Action m_currentAction;
    f32 m_rotationDirection;
    f32 m_targetRot;
    EGG::Vector3f m_hitDepth;
    EGG::Vector3f m_rotAxis;
    EGG::Vector3f m_translation;

    f32 m_velPitch;
    f32 m_pitch;
    f32 m_deltaPitch;
    f32 m_flipPhase;
    s32 m_groundStartLaunchTimer;

    StartActionFunc m_onStart;
    CalcActionFunc m_onCalc;
    EndActionFunc m_onEnd;

    EGG::Quatf m_rotation;
    const ActionParams *m_actionParams;
    u32 m_frame;
    u32 m_crushTimer;
    Flags m_flags;
    f32 m_currentAngle;
    f32 m_angleIncrement;
    f32 m_multiplier;
    f32 m_multiplierDecrement;
    f32 m_finalAngle;
    const RotationParams *m_rotationParams;
    EGG::Vector3f m_up;
    u16 m_framesFlipping;
    s16 m_priority;
    bool m_powAffected; ///< +0xF8 Set at each POW shake: we are among its targets.

    static bool s_powStrike;     ///< @addr{0x809C1898}
    static s16 s_powStrikeTimer; ///< @addr{0x809C189A}
    static u8 s_powStage;        ///< @addr{0x809C189C}
    static const ActionParams s_powSpinLateParams;

    static constexpr size_t MAX_ACTION = static_cast<size_t>(Action::Max);

    static const std::array<ActionParams, MAX_ACTION> s_actionParams;
    static const std::array<RotationParams, 5> s_rotationParams;

    static const std::array<StartActionFunc, MAX_ACTION> s_onStart;
    static const std::array<CalcActionFunc, MAX_ACTION> s_onCalc;
    static const std::array<EndActionFunc, MAX_ACTION> s_onEnd;
};

} // namespace Kinoko::Kart
