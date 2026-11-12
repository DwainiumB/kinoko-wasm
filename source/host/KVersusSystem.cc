#include "KVersusSystem.hh"

#include "host/SceneCreatorDynamic.hh"

#include <game/item/ItemDirector.hh>
#include <game/kart/KartCollide.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartState.hh>
#include <game/system/KPadDirector.hh>
#include <game/system/RaceManager.hh>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace Kinoko {

namespace {
// printf takes doubles; make the float -> double widening explicit (-Wdouble-promotion)
inline double D(f32 v) {
    return static_cast<double>(v);
}
} // namespace

void KVersusSystem::init() {
    ASSERT(!m_karts.empty() && m_karts.size() <= 12);

    loadInputs();

    auto *sceneCreator = EGG::egg_new<Host::SceneCreatorDynamic>();
    m_sceneMgr = EGG::egg_new<EGG::SceneManager>(sceneCreator);

    System::RaceConfig::RegisterInitCallback(OnInit, nullptr);
    m_sceneMgr->changeScene(0);
}

void KVersusSystem::calc() {
    m_sceneMgr->calc();
}

bool KVersusSystem::run() {
    FILE *out = m_outPath ? fopen(m_outPath, "w") : stdout;
    if (!out) {
        PANIC("Cannot open %s", m_outPath);
    }

    fprintf(out, "frame,raceFrame,player,posX,posY,posZ,rotX,rotY,rotZ,rotW,speed,"
                 "playerBumpTimer,extVelX,extVelY,extVelZ,angVel0Z,slipstream,item,itemCount,"
                 "effects,rank,action,roulette,rouletteItem,itemPoint,status0,status1,status2,"
                 "status3,status4,rng1\n");

    FILE *items = m_itemsPath ? fopen(m_itemsPath, "w") : nullptr;
    if (items) {
        fprintf(items, "frame,raceFrame,kind,idx,owner,trailing,flags,posX,posY,posZ,velX,velY,"
                       "velZ,scale,hitRadius,inBoxCol\n");
    }

    if (auto *race = System::RaceManager::Instance()) {
        for (u32 i = 0; i < m_rng1Skip; ++i) {
            race->random().next();
        }
    }

    while (m_frameIdx < m_maxFrames) {
        applyInputs();
        calc();
        writeRows(out);
        if (items) {
            writeItemRows(items);
        }
        ++m_frameIdx;
    }

    if (out != stdout) {
        fclose(out);
    }

    if (items) {
        fclose(items);
    }

    return true;
}

void KVersusSystem::parseOptions(int argc, char **argv) {
    for (int i = 0; i + 1 < argc; i += 2) {
        const char *flag = argv[i];
        const char *value = argv[i + 1];

        if (strcmp(flag, "--course") == 0) {
            m_courseId = std::atoi(value);
        } else if (strcmp(flag, "--kart") == 0) {
            KartChoice kart = {0, 0, false};
            char drift[8] = {};
            if (sscanf(value, "%d:%d:%7s", &kart.character, &kart.vehicle, drift) < 2) {
                PANIC("Expected --kart <character>:<vehicle>[:auto], got %s", value);
            }

            kart.driftIsAuto = strcmp(drift, "auto") == 0;
            m_karts.push_back(kart);
        } else if (strcmp(flag, "--inputs") == 0) {
            m_inputPath = value;
        } else if (strcmp(flag, "--out") == 0) {
            m_outPath = value;
        } else if (strcmp(flag, "--items") == 0) {
            m_itemsPath = value;
        } else if (strcmp(flag, "--frames") == 0) {
            m_maxFrames = static_cast<u32>(std::atoi(value));
        } else if (strcmp(flag, "--cc") == 0) {
            using EngineClass = System::RaceConfig::Scenario::EngineClass;
            int cc = std::atoi(value);
            m_engineClass = cc == 50 ? EngineClass::CC50 :
                    (cc == 100 ? EngineClass::CC100 : EngineClass::CC150);
        } else if (strcmp(flag, "--rng1-skip") == 0) {
            m_rng1Skip = static_cast<u32>(std::atoi(value));
        } else if (strcmp(flag, "--seed") == 0) {
            char *end = nullptr;
            m_seed1 = static_cast<u32>(strtoul(value, &end, 0));
            m_seed2 = end && *end == ':' ? static_cast<u32>(strtoul(end + 1, nullptr, 0)) : 0;
        } else if (strcmp(flag, "--item-mode") == 0) {
            int mode = std::atoi(value);
            if (mode < 0 || mode > 3) {
                PANIC("Expected --item-mode 0..3, got %s", value);
            }

            m_itemMode = static_cast<System::RaceConfig::Scenario::ItemMode>(mode);
        } else if (strcmp(flag, "--human") == 0) {
            m_humans.clear();
            for (const char *p = value; *p;) {
                m_humans.push_back(static_cast<u8>(std::atoi(p)));
                const char *comma = strchr(p, ',');
                p = comma ? comma + 1 : p + strlen(p);
            }
        } else if (strcmp(flag, "--start") == 0) {
            for (const char *p = value; *p;) {
                m_startPlacements.push_back(static_cast<u8>(std::atoi(p)));
                const char *comma = strchr(p, ',');
                p = comma ? comma + 1 : p + strlen(p);
            }
        } else {
            PANIC("Invalid flag for versus mode: %s", flag);
        }
    }
}

KVersusSystem *KVersusSystem::CreateInstance() {
    ASSERT(!s_instance);
    s_instance = EGG::egg_new<KVersusSystem>();
    return static_cast<KVersusSystem *>(s_instance);
}

void KVersusSystem::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

KVersusSystem::KVersusSystem()
    : m_sceneMgr(nullptr), m_courseId(8),
      m_engineClass(System::RaceConfig::Scenario::EngineClass::CC150), m_humans{0},
      m_seed1(0x74A1B095), m_seed2(0),
      m_itemMode(System::RaceConfig::Scenario::ItemMode::Balanced), m_inputPath(nullptr),
      m_outPath(nullptr), m_itemsPath(nullptr),
      m_maxFrames(1000), m_frameIdx(0), m_nextInput(0) {}

KVersusSystem::~KVersusSystem() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("KVersusSystem instance not explicitly handled!");
    }

    EGG::egg_delete(m_sceneMgr);
}

void KVersusSystem::loadInputs() {
    if (!m_inputPath) {
        return;
    }

    FILE *file = fopen(m_inputPath, "r");
    if (!file) {
        PANIC("Cannot open %s", m_inputPath);
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        unsigned frame = 0, player = 0, buttons = 0;
        float stickX = 0.0f, stickY = 0.0f;
        int trick = 0;
        int item = 0;
        if (sscanf(line, "item %u %u %d", &frame, &player, &item) == 3) {
            m_inputs.push_back({frame, player, 0, 0.0f, 0.0f, 0, item});
        } else if (sscanf(line, "%u %u %u %f %f %d", &frame, &player, &buttons, &stickX, &stickY,
                           &trick) == 6) {
            m_inputs.push_back(
                    {frame, player, static_cast<u16>(buttons), stickX, stickY, trick, -1});
        }
    }

    fclose(file);
    std::stable_sort(m_inputs.begin(), m_inputs.end(),
            [](const Input &a, const Input &b) { return a.frame < b.frame; });
}

/// @brief Applies every input line up to the race frame about to be simulated (the race timer
/// after this calc). Controllers keep their last input.
void KVersusSystem::applyInputs() {
    auto *padDir = System::KPadDirector::Instance();
    const auto *race = System::RaceManager::Instance();
    u32 nextRaceFrame = (race ? race->timer() : 0) + 1;
    while (m_nextInput < m_inputs.size() && m_inputs[m_nextInput].frame <= nextRaceFrame) {
        const Input &input = m_inputs[m_nextInput++];
        if (input.item >= 0) {
            auto *itemDir = Item::ItemDirector::Instance();
            if (itemDir && input.player < m_karts.size()) {
                // "item <frame> <player> 20" (none) empties the inventory, e.g. where the game
                // used an item Kinoko has no object for.
                auto &inventory = itemDir->kartItem(input.player).inventory();
                auto id = static_cast<Item::ItemId>(input.item);
                if (id == Item::ItemId::NONE) {
                    inventory.clear();
                } else {
                    inventory.setItem(id);
                }
            }
            continue;
        }

        auto *controller = padDir ? padDir->hostController(input.player) : nullptr;
        if (controller && input.player < m_karts.size()) {
            controller->setInputs(input.buttons, EGG::Vector2f(input.stickX, input.stickY),
                    static_cast<System::Trick>(input.trick));
        }
    }
}

/// @brief One row per item object taken out of its pool (held or on the course).
void KVersusSystem::writeItemRows(FILE *out) const {
    auto *itemDir = Item::ItemDirector::Instance();
    const auto *race = System::RaceManager::Instance();
    if (!itemDir) {
        return;
    }

    for (size_t k = 0; k < static_cast<size_t>(Item::ItemObjKind::Count); ++k) {
        auto &holder = itemDir->holder(static_cast<Item::ItemObjKind>(k));
        for (s32 i = 0; i < holder.spawnedCount(); ++i) {
            const Item::ItemObj *obj = holder.obj(i);
            const EGG::Vector3f &pos = obj->pos();
            const EGG::Vector3f &vel = obj->vel();
            fprintf(out, "%u,%u,%zu,%d,%u,%d,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n",
                    m_frameIdx, race ? race->timer() : 0u, k, i, obj->owner(),
                    obj->isTrailing() ? 1 : 0, obj->flags().getDirect(), D(pos.x), D(pos.y),
                    D(pos.z), D(vel.x), D(vel.y), D(vel.z), D(obj->scale()),
                    D(obj->hitRadius()), obj->isInBoxCol() ? 1 : 0);
        }
    }
}

void KVersusSystem::writeRows(FILE *out) const {
    const auto *kartMgr = Kart::KartObjectManager::Instance();
    const auto *race = System::RaceManager::Instance();
    if (!kartMgr) {
        return;
    }

    for (size_t i = 0; i < m_karts.size(); ++i) {
        const Kart::KartObject *kart = kartMgr->object(i);
        const EGG::Vector3f &pos = kart->pos();
        const EGG::Quatf &rot = kart->fullRot();
        const EGG::Vector3f &extVel = kart->dynamics()->extVel();
        const auto &status = kart->status();
        int slipstream = status.onBit(Kart::eStatus::SlipstreamBoost) ?
                2 :
                (status.onBit(Kart::eStatus::SlipstreamCharge) ? 1 : 0);
        // effects: 1 star, 2 mega, 4 shocked (shrunk), 8 inked, 16 crushed, 32 thunder cloud
        int effects = (status.onBit(Kart::eStatus::InStar) ? 1 : 0) |
                (status.onBit(Kart::eStatus::InMega) ? 2 : 0) |
                (status.onBit(Kart::eStatus::Shocked) ? 4 : 0) |
                (status.onBit(Kart::eStatus::Inked) ? 8 : 0) |
                (status.onBit(Kart::eStatus::Crushed) ? 16 : 0) |
                (status.onBit(Kart::eStatus::ThunderCloud) ? 32 : 0);
        // The game's five status words (KartState +0x04..+0x14): bit n is word n / 32, bit n % 32.
        std::array<u32, 5> statusWords = {};
        for (size_t bit = 0; bit < static_cast<size_t>(Kart::eStatus::FlagMax); ++bit) {
            if (status.onBit(static_cast<Kart::eStatus>(bit))) {
                statusWords[bit / 32] |= 1u << (bit % 32);
            }
        }
        const auto &kartItem = Item::ItemDirector::Instance()->kartItem(i);
        const auto &inventory = kartItem.inventory();
        const auto &roulette = kartItem.roulette();
        fprintf(out,
                "%u,%u,%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,"
                "%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%016llx\n",
                m_frameIdx, race ? race->timer() : 0u, i, D(pos.x), D(pos.y), D(pos.z), D(rot.v.x),
                D(rot.v.y), D(rot.v.z), D(rot.w), D(kart->move()->speed()),
                kart->collide()->playerBumpTimer(), D(extVel.x), D(extVel.y), D(extVel.z),
                D(kart->dynamics()->angVel0().z), slipstream,
                static_cast<int>(inventory.id()), inventory.currentCount(), effects,
                race ? race->player(i).rank() : 0,
                static_cast<int>(kart->action()->currentAction()),
                static_cast<int>(roulette.state()), static_cast<int>(roulette.shownItem()),
                static_cast<int>(kartItem.itemPoint()), statusWords[0], statusWords[1],
                statusWords[2], statusWords[3], statusWords[4],
                race ? static_cast<unsigned long long>(
                               const_cast<System::RaceManager *>(race)->random().state()) :
                       0ull);
    }
}

void KVersusSystem::OnInit(System::RaceConfig *config, void * /* arg */) {
    auto *self = Instance();
    auto &scenario = config->raceScenario();
    scenario.course = static_cast<Course>(self->m_courseId);
    scenario.playerCount = static_cast<u8>(self->m_karts.size());
    scenario.engineClass = self->m_engineClass;
    scenario.gameMode = System::RaceConfig::Scenario::GameMode::Versus;
    scenario.itemMode = self->m_itemMode;
    scenario.seed1 = self->m_seed1;
    scenario.seed2 = self->m_seed2;

    for (size_t i = 0; i < self->m_karts.size(); ++i) {
        auto &player = scenario.players[i];
        bool human = std::find(self->m_humans.begin(), self->m_humans.end(), i) !=
                self->m_humans.end();
        player.type = human ? System::RaceConfig::Player::Type::Local :
                              System::RaceConfig::Player::Type::Cpu;
        player.character = static_cast<Character>(self->m_karts[i].character);
        player.vehicle = static_cast<Vehicle>(self->m_karts[i].vehicle);
        player.driftIsAuto = self->m_karts[i].driftIsAuto;
        player.startPlacement = i < self->m_startPlacements.size() ? self->m_startPlacements[i] : 0;
    }
}

} // namespace Kinoko
