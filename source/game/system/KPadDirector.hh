#pragma once

#include "game/system/KPadController.hh"

namespace Kinoko {

namespace Host {

class Context;

} // namespace Host

namespace System {

/// @brief The highest level abstraction for controller processing.
/// @addr{0x809BD70C}
class KPadDirector : EGG::Disposer {
    friend class Host::Context;

public:
    void calc();
    void calcPads();

    /// @addr{0x80523724}
    void clear() {}

    void reset();
    void startGhostProxies();
    void endGhostProxies();

    /// @brief The input state of player idx. Players are numbered as in RaceConfig; player 0 is the local one.
    [[nodiscard]] const KPadPlayer &playerInput(size_t idx = 0) const {
        return m_playerInputs[idx];
    }

    /// @brief The externally driven controller of player idx (the local player, or a CPU driven by the host).
    [[nodiscard]] KPadHostController *hostController(size_t idx = 0) {
        return m_hostControllers[idx];
    }

    void setGhostPad(const u8 *inputs, bool driftIsAuto);
    void setHostPad(bool driftIsAuto, size_t idx = 0);

    static KPadDirector *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] static KPadDirector *Instance() {
        return s_instance;
    }

private:
    EGG_NEW_DELETE_FRIEND

    KPadDirector();
    ~KPadDirector() override;

    static constexpr size_t MAX_PLAYERS = 12;

    [[nodiscard]] size_t playerCount() const;

    std::array<KPadPlayer, MAX_PLAYERS> m_playerInputs;
    KPadGhostController *m_ghostController; ///< Drives player 0 when it is a ghost.
    std::array<KPadHostController *, MAX_PLAYERS> m_hostControllers;

    static KPadDirector *s_instance; ///< @addr{0x809BD70C}
};

} // namespace System

} // namespace Kinoko
