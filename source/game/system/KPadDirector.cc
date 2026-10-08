#include "KPadDirector.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::System {

/// @brief The number of players in the race, or 1 before the race is configured.
size_t KPadDirector::playerCount() const {
    const auto *config = RaceConfig::Instance();
    return config ? std::max<size_t>(1, config->raceScenario().playerCount) : 1;
}

/// @addr{0x805238F0}
void KPadDirector::calc() {
    calcPads();
    for (size_t i = 0; i < playerCount(); ++i) {
        m_playerInputs[i].calc();
    }
}

/// @addr{0x805237E8}
void KPadDirector::calcPads() {
    m_ghostController->calc();
    for (size_t i = 0; i < playerCount(); ++i) {
        m_hostControllers[i]->calc();
    }
}

/// @addr{0x80523690}
void KPadDirector::reset() {
    for (size_t i = 0; i < playerCount(); ++i) {
        m_playerInputs[i].reset();
    }
}

/// @addr{0x80524580}
void KPadDirector::startGhostProxies() {
    m_playerInputs[0].startGhostProxy();
}

/// @addr{0x805245DC}
void KPadDirector::endGhostProxies() {
    m_playerInputs[0].endGhostProxy();
}

/// @addr{0x8052453C}
void KPadDirector::setGhostPad(const u8 *inputs, bool driftIsAuto) {
    m_playerInputs[0].setGhostController(m_ghostController, inputs, driftIsAuto);
}

void KPadDirector::setHostPad(bool driftIsAuto, size_t idx) {
    m_playerInputs[idx].setHostController(m_hostControllers[idx], driftIsAuto);
}

/// @addr{0x8052313C}
KPadDirector *KPadDirector::CreateInstance() {
    ASSERT(!s_instance);
    return s_instance = EGG::egg_new<KPadDirector>();
}

/// @addr{0x8052318C}
void KPadDirector::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

/// @addr{0x805232F0}
KPadDirector::KPadDirector() {
    m_ghostController = EGG::egg_new<KPadGhostController>();
    for (auto &controller : m_hostControllers) {
        controller = EGG::egg_new<KPadHostController>();
    }
}

/// @addr{0x805231DC}
KPadDirector::~KPadDirector() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("KPadDirector instance not explicitly handled!");
    }
}

KPadDirector *KPadDirector::s_instance = nullptr; ///< @addr{0x809BD70C}

} // namespace Kinoko::System
