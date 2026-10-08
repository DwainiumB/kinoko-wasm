#include "KDriveSystem.hh"

#include "host/FrameState.hh"
#include "host/Option.hh"
#include "host/SceneCreatorDynamic.hh"

#include <game/system/KPadDirector.hh>
#include <game/system/RaceManager.hh>

#include <cstdio>
#include <cstdlib>

namespace Kinoko {

void KDriveSystem::init() {
    auto *sceneCreator = EGG::egg_new<Host::SceneCreatorDynamic>();
    m_sceneMgr = EGG::egg_new<EGG::SceneManager>(sceneCreator);

    System::RaceConfig::RegisterInitCallback(OnInit, nullptr);
    m_sceneMgr->changeScene(0);
}

/// @brief Reads the next input line from stdin and applies it to the host controller.
/// @return False if stdin was closed, in which case the controlling process is gone.
bool KDriveSystem::readInput() {
    char line[256];
    if (!fgets(line, sizeof(line), stdin)) {
        return false;
    }

    int buttons = 0;
    int trick = 0;
    float stickX = 0.0f;
    float stickY = 0.0f;
    if (sscanf(line, "%d %f %f %d", &buttons, &stickX, &stickY, &trick) != 4) {
        PANIC("Malformed input line: %s", line);
    }

    auto *padDir = System::KPadDirector::Instance();
    auto *host = padDir ? padDir->hostController() : nullptr;
    if (host) {
        host->setInputs(static_cast<u16>(buttons), EGG::Vector2f(stickX, stickY),
                static_cast<System::Trick>(trick));
    }

    return true;
}

void KDriveSystem::calc() {
    m_sceneMgr->calc();
}

bool KDriveSystem::run() {
    Host::WriteStateHeader(stdout, "COLS ");
    fflush(stdout);

    while (!calcEnd()) {
        if (!readInput()) {
            return false;
        }

        calc();
        Host::WriteStateRow(stdout, m_frameIdx++, "ROW ");
    }

    reportEnd();
    return true;
}

void KDriveSystem::parseOptions(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        std::optional<Host::EOption> flag = Host::Option::CheckFlag(argv[i]);
        if (!flag || *flag == Host::EOption::Invalid) {
            WARN("Expected a flag! Got: %s", argv[i]);
            continue;
        }

        ASSERT(i + 1 < argc);
        switch (*flag) {
        case Host::EOption::Course:
            m_courseId = std::atoi(argv[++i]);
            break;
        case Host::EOption::Character:
            m_characterId = std::atoi(argv[++i]);
            break;
        case Host::EOption::Vehicle:
            m_vehicleId = std::atoi(argv[++i]);
            break;
        case Host::EOption::MaxFrames:
            m_maxFrames = static_cast<u32>(std::atoi(argv[++i]));
            break;
        default:
            PANIC("Invalid flag for drive mode!");
            break;
        }
    }
}

KDriveSystem *KDriveSystem::CreateInstance() {
    ASSERT(!s_instance);
    s_instance = EGG::egg_new<KDriveSystem>();
    return static_cast<KDriveSystem *>(s_instance);
}

void KDriveSystem::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

KDriveSystem::KDriveSystem()
    : m_sceneMgr(nullptr), m_courseId(8), m_characterId(22), m_vehicleId(32), m_maxFrames(9000),
      m_frameIdx(0) {}

KDriveSystem::~KDriveSystem() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("KDriveSystem instance not explicitly handled!");
    }

    EGG::egg_delete(m_sceneMgr);
}

bool KDriveSystem::calcEnd() const {
    const auto *race = System::RaceManager::Instance();
    if (race && race->stage() == System::RaceManager::Stage::FinishGlobal) {
        return true;
    }

    return m_frameIdx >= m_maxFrames;
}

void KDriveSystem::reportEnd() const {
    const auto *race = System::RaceManager::Instance();
    const auto &player = race->player();
    const bool finished = race->stage() == System::RaceManager::Stage::FinishGlobal;
    const auto &t = player.raceTimer();
    int timeMs = static_cast<int>(t.min) * 60000 + static_cast<int>(t.sec) * 1000 +
            static_cast<int>(t.mil);

    printf("END finished=%d frames=%u timeMs=%d completion=%d\n", finished ? 1 : 0, m_frameIdx,
            finished ? timeMs : -1, static_cast<int>(player.raceCompletion() * 10000.0f));
    fflush(stdout);
}

void KDriveSystem::OnInit(System::RaceConfig *config, void * /* arg */) {
    auto *self = Instance();
    auto &scenario = config->raceScenario();
    scenario.course = static_cast<Course>(self->m_courseId);
    scenario.playerCount = 1;

    auto &player = scenario.players[0];
    player.type = System::RaceConfig::Player::Type::Local;
    player.character = static_cast<Character>(self->m_characterId);
    player.vehicle = static_cast<Vehicle>(self->m_vehicleId);
    player.driftIsAuto = false;
}

} // namespace Kinoko
