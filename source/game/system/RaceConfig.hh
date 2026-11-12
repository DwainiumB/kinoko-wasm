#pragma once

#include "game/system/GhostFile.hh"

#include <functional>

namespace Kinoko {

namespace Host {

class Context;

} // namespace Host

namespace System {

/// @addr{0x809BD728}
/// @brief Initializes the player with parameters specified in the provided ghost file.
/// @details In the base game, this class is responsible for managing the race and menu scenarios.
/// The menu scenario mostly pertains to character and vehicle selection in the menus prior to
/// starting a race. In Kinoko, we don't have these menus, so we initialize the race directly
/// through this class.
class RaceConfig : EGG::Disposer {
    friend class Host::Context;

public:
    struct Player {
    public:
        enum class Type {
            Local = 0,  // Inputs managed by ML algorithm
            Cpu = 1,    // Added in Kinoko: inputs written by the host program each frame (a CPU
                        // driver)
            Ghost = 3,  // Inputs managed by ghost
            Remote = 4, // Another console's racer online; Kinoko cannot drive one yet
            None = 5,
        };

        Character character;
        Vehicle vehicle;
        Type type;
        bool driftIsAuto;
        /// 1-based place on the starting grid in a VS race (the game's menus set it; offset 0x109
        /// in the game's player struct). 0 means player index + 1.
        u8 startPlacement;
    };

    struct Scenario {
    public:
        /// @brief Offset 0xB50 in the game's scenario (0xB70 in RaceConfig).
        enum class GameMode {
            Grand_Prix = 0,
            Versus = 1,
            Time_Trial = 2,
            Battle = 3,
            Mission = 4,
            Ghost_Race = 5,
            Online_Private_Versus = 7,
            Online_Public_Versus = 8,
            Online_Public_Battle = 9,
            Online_Private_Battle = 10,
            Awards = 11,
            Credits = 12,
        };

        /// @brief Which items the boxes give (offset 0xB60 in the game's scenario).
        enum class ItemMode {
            Balanced = 0,
            Frantic = 1,
            Strategic = 2,
            None = 3,
        };

        void init();
        void initRng();

        /// @brief The race's engine class (offset 0xB6C in the game's RaceConfig). It scales
        /// speeds through KartMove's engine speed factor.
        enum class EngineClass {
            CC50 = 0,
            CC100 = 1,
            CC150 = 2,
            Battle = 3,
        };

        std::array<Player, 12> players;
        u8 playerCount;
        Course course;
        EngineClass engineClass;
        GameMode gameMode;
        ItemMode itemMode;
        /// @brief Seeds of RaceManager's two random generators (offsets 0xB74 and 0xB78 in the
        /// game's scenario). The game takes them from the clock outside Time Trials; Kinoko's
        /// host sets them so that every client of a race draws the same numbers.
        u32 seed1;
        u32 seed2;
    };

    typedef std::function<void(RaceConfig *, void *)> InitCallback;

    void init();
    void initRace();
    void initControllers();
    void initGhost();

    [[nodiscard]] const Scenario &raceScenario() const {
        return m_raceScenario;
    }

    [[nodiscard]] Scenario &raceScenario() {
        return m_raceScenario;
    }

    void setGhost(const u8 *rkg) {
        m_ghost = rkg;
    }

    static void RegisterInitCallback(const InitCallback &callback, void *arg) {
        s_onInitCallback = callback;
        s_onInitCallbackArg = arg;
    }

    static RaceConfig *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] static RaceConfig *Instance() {
        return s_instance;
    }

private:
    EGG_NEW_DELETE_FRIEND

    RaceConfig();
    ~RaceConfig() override;

    Scenario m_raceScenario;
    RawGhostFile m_ghost;

    static RaceConfig *s_instance; ///< @addr{0x809BD728}
    static InitCallback s_onInitCallback;
    static void *s_onInitCallbackArg;
};

} // namespace System

} // namespace Kinoko
