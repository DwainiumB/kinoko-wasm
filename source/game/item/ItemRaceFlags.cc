#include "ItemRaceFlags.hh"

#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"

namespace Kinoko::Item {

/// @addr{0x807BD1D0}
/// @details The game also stores a per-course item scale (+0x18) and a course flag (+0x1C) here;
/// only the model code reads them, so they are not ported.
void ItemRaceFlags::Init() {
    using GameMode = System::RaceConfig::Scenario::GameMode;

    const auto &scenario = System::RaceConfig::Instance()->raceScenario();
    s_playerCount = scenario.playerCount;
    s_online = false;
    s_versus = false;
    s_battle = false;
    s_timeTrial = false;
    s_raceFrame = 0;
    s_raceFrameMod2 = 0;
    s_raceFrameMod4 = 0;
    s_raceFrameMod8 = 0;

    switch (scenario.gameMode) {
    case GameMode::Grand_Prix:
    case GameMode::Mission:
    case GameMode::Awards:
    case GameMode::Credits:
        break;
    case GameMode::Versus:
        s_versus = true;
        break;
    case GameMode::Battle:
        s_battle = true;
        break;
    case GameMode::Online_Private_Versus:
    case GameMode::Online_Public_Versus:
        s_online = true;
        break;
    case GameMode::Online_Public_Battle:
    case GameMode::Online_Private_Battle:
        s_online = true;
        s_battle = true;
        break;
    default:
        // Time Trial, ghost race, mode 6 and anything out of range.
        s_timeTrial = true;
        break;
    }

    // Kinoko has no mode flags (team races).
    s_teams = false;
}

/// @addr{0x807BD718}
System::Random &ItemRaceFlags::Random() {
    auto *raceMgr = System::RaceManager::Instance();
    return s_online || s_timeTrial ? raceMgr->random2() : raceMgr->random();
}

u8 ItemRaceFlags::s_playerCount = 0;
bool ItemRaceFlags::s_online = false;
bool ItemRaceFlags::s_versus = false;
bool ItemRaceFlags::s_battle = false;
bool ItemRaceFlags::s_timeTrial = false;
bool ItemRaceFlags::s_teams = false;
s32 ItemRaceFlags::s_raceFrame = 0;
u32 ItemRaceFlags::s_raceFrameMod2 = 0;
u32 ItemRaceFlags::s_raceFrameMod4 = 0;
u32 ItemRaceFlags::s_raceFrameMod8 = 0;

/// @addr{0x807BD340}
void ItemRaceFlags::CalcRaceFrame() {
    u32 frame = System::RaceManager::Instance()->timer();
    s_raceFrame = static_cast<s32>(frame);
    s_raceFrameMod2 = frame & 1;
    s_raceFrameMod4 = frame & 3;
    s_raceFrameMod8 = frame & 7;
}

} // namespace Kinoko::Item
