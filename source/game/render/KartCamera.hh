#pragma once

#include "game/kart/KartMove.hh"
#include "game/kart/KartObjectManager.hh"

namespace Kinoko::Render {

/// @brief Tracks the current state of the front and backwards player cameras.
class KartCameraState {
    friend class KartCamera;

public:
    KartCameraState() {
        m_pos.setZero();
        m_bigAirHeight = 0.0f;
        m_1c = 0.0f;
        m_prevPos.setZero();
        m_dist = 0.0f;
        m_bigAirFallPitch = 0.0f;
        m_targetPos.setZero();
    }

    ~KartCameraState() = default;

    /// @addr{0x805A1C3C}
    void init() {
        m_dist = 0.0f;
        m_bigAirFallPitch = 0.0f;
    }

private:
    EGG::Vector3f m_pos;
    f32 m_bigAirHeight; ///< Additional camera height applied after 20 frames of airtime
    f32 m_1c;           ///< TODO: Seems to be related to boost-induced pitch
    EGG::Vector3f m_prevPos;
    f32 m_dist;                ///< Distance away from TODO
    f32 m_bigAirFallPitch;     ///< Height applied once you start falling after 20 frames of airtime
    EGG::Vector3f m_targetPos; ///< The position the camera looks towards
};

/// @brief Manager class for the forward and backwards cameras.
/// @details Responsible for setting the camera state and performing camera collision checks.
class KartCamera {
    friend class Host::Context;

public:
    /// @addr{0x805A2034}
    void init() {
        auto *param = Kart::KartObjectManager::Instance()->object(0)->param();
        m_camParams = &param->camera();

        initPos();
    }

    void calc();

    /// @brief The forward camera's eye position, the point it looks at, and its parameters (for
    /// hosts that draw the race).
    [[nodiscard]] const EGG::Vector3f &forwardPos() const {
        return m_forwardCamera.m_pos;
    }

    [[nodiscard]] const EGG::Vector3f &forwardTarget() const {
        return m_forwardCamera.m_targetPos;
    }

    [[nodiscard]] const Kart::KartParam::KartCameraParam *camParams() const {
        return m_camParams;
    }

    /// @brief The field of view the game is using right now: it widens while boosting and eases
    /// back afterwards.
    /// @details Measured from the real game (camera log of a Standard Kart M run): +0.1 of the
    /// remaining gap per frame towards fov + 6 while boosting, then fov + 0.97 of the excess per
    /// frame once the boost ends.
    [[nodiscard]] f32 fov() const {
        return m_fov;
    }

    /// @brief Height of the point the camera looks at above the kart: targetPosY - 20 + the pitch
    /// of the smoothed forward direction in degrees (measured from the real game; fits to within
    /// 0.2 units).
    /// @brief Internal camera state, for comparing against the real game: {pitch deg, drift yaw
    /// deg, hop pos y, pitch factor m_1c, big-air height, big-air fall pitch}.
    void debugState(f32 *out) const {
        out[0] = m_pitchDeg;
        out[1] = m_driftYaw;
        out[2] = m_hopPosY;
        out[3] = m_forwardCamera.m_1c;
        out[4] = m_forwardCamera.m_bigAirHeight;
        out[5] = m_forwardCamera.m_bigAirFallPitch;
    }

    [[nodiscard]] f32 targetOffsetY() const {
        return m_camParams->targetPosY - 20.0f + m_pitchDeg;
    }

    KartCamera();
    ~KartCamera();

    static KartCamera *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] static KartCamera *Instance() {
        return s_instance;
    }

private:
    /// @addr{0x805A2B84}
    void calcForward(f32 t, const Kart::KartObjectProxy *proxy) {
        m_forward = Interpolate(t, m_forward, proxy->move()->smoothedForward());
        m_right = EGG::Vector3f::ey.perpInPlane(m_forward, true);
    }

    void calcDriftOffset(const Kart::KartObjectProxy *proxy);
    void calcFov(const Kart::KartObjectProxy *proxy);
    void calcCamera(f32 param1, f32 param2, f32 param3, KartCameraState &state, bool isBackwards,
            const Kart::KartObjectProxy *proxy, const EGG::Vector3f &targetPos) const;
    void calcAirtimeHeight(KartCameraState &state, const Kart::KartObjectProxy *proxy) const;
    void initPos();

    void calcCollision(KartCameraState &state, bool isRear) const;

    /// @addr{0x805A2C34}
    static EGG::Vector3f Interpolate(f32 t, const EGG::Vector3f &v0, const EGG::Vector3f &v1) {
        return v0 + (v1 - v0) * t;
    }

    f32 m_driftYaw; ///< Rotation induced when drifting
    f32 m_hopPosY;  ///< Induces a downwards camera position offset
    f32 m_fov;      ///< Current field of view (degrees), see fov()
    f32 m_pitchDeg; ///< Pitch of the smoothed forward direction (degrees), see targetOffsetY()
    EGG::Vector3f m_forward;
    EGG::Vector3f m_right;

    /// @warning Kinoko assumes the use of the 16:9 camera. It is possible for a time trial
    /// desync to occur due to the difference in camera distance between 4:3 and 16:9. This
    /// varying distance can cause an ObjectKCL transformation matrix to be updated for one
    /// camera's collision check but not the other. As far as we know, this can only occur on
    /// the DS Delfino Square @ref Field::ObjectTownBridge, and there are no naturally occurring
    /// desyncs of this type.
    const Kart::KartParam::KartCameraParam *m_camParams;

    KartCameraState m_forwardCamera;  ///< Forward camera state
    KartCameraState m_backwardCamera; ///< Rear camera state

    static KartCamera *s_instance;
};

} // namespace Kinoko::Render
