#include "MapdataJugemPoint.hh"

#include "game/system/CourseMap.hh"
#include "game/system/RaceConfig.hh"
#include "game/system/map/MapdataItemPoint.hh"

#include <egg/math/Math.hh>
#include <egg/math/Quat.hh>

#include <array>

namespace Kinoko::System {

/// @addr{0x805183A8}
MapdataJugemPoint::MapdataJugemPoint(const SData *data) : m_rawData(data) {
    EGG::RamStream stream = EGG::RamStream(data, sizeof(SData));
    read(stream);

    // The game keeps the rotation in radians and the point's rotated -Z and -X axes for
    // respawnPos().
    EGG::Quatf rot = EGG::Quatf::FromRPY(m_rot.x * DEG2RAD, m_rot.y * DEG2RAD, m_rot.z * DEG2RAD);
    m_backDir = rot.rotateVector(-EGG::Vector3f::ez);
    m_leftDir = rot.rotateVector(-EGG::Vector3f::ex);
    m_itemPointIdx = 0;
}

void MapdataJugemPoint::read(EGG::Stream &stream) {
    m_pos.read(stream);
    m_rot.read(stream);
}

/// @addr{0x805184FC}
/// @brief Finds the item route point nearest ahead of this point, inside a 15 degree cone around
/// its facing, so that a player put back on the track here continues the item route from it. Falls
/// back to point 0. The game links the nearest enemy route point the same way (+0x28) for the CPU
/// AI; Kinoko has no enemy route.
void MapdataJugemPoint::linkNearestItemPoint() {
    constexpr f32 CONE_HALF_ANGLE_FIDX =
            10.6667f; ///< 15 degrees in the sine table's 256-per-turn units.
    constexpr f32 NO_DISTANCE = 10000000.0f;

    EGG::Quatf rot = EGG::Quatf::FromRPY(m_rot.x * DEG2RAD, m_rot.y * DEG2RAD, m_rot.z * DEG2RAD);
    EGG::Vector3f forward = rot.rotateVector(EGG::Vector3f::ez);
    EGG::Vector3f right = rot.rotateVector(EGG::Vector3f::ex);
    EGG::Vector3f up = rot.rotateVector(EGG::Vector3f::ey);
    f32 coneTan =
            EGG::Mathf::SinFIdx(CONE_HALF_ANGLE_FIDX) / EGG::Mathf::CosFIdx(CONE_HALF_ANGLE_FIDX);

    const auto *courseMap = CourseMap::Instance();
    u8 nearest = MapdataItemPoint::NONE;
    f32 nearestAhead = NO_DISTANCE;

    for (u16 i = 0; i < courseMap->getItemPointCount(); ++i) {
        EGG::Vector3f delta = courseMap->getItemPoint(i)->pos() - m_pos;
        f32 ahead = forward.dot(delta);
        if (ahead < 0.0f) {
            continue;
        }

        f32 coneRadius = ahead * coneTan;
        f32 sideways = right.dot(delta);
        f32 upward = up.dot(delta);
        if (sideways * sideways + upward * upward >= coneRadius * coneRadius) {
            continue;
        }

        if (nearest != MapdataItemPoint::NONE && ahead >= nearestAhead) {
            continue;
        }

        nearestAhead = ahead;
        nearest = static_cast<u8>(i);
    }

    m_itemPointIdx = nearest == MapdataItemPoint::NONE ? 0 : nearest;
}

/// @addr{0x8051896C}
/// @brief Where a player is put back on the track: the point itself in Time Trials and ghost races,
/// otherwise shifted 150 units to the point's left or right (450 for players 3 and 4) and, from
/// player 5 on, 300 or 600 units behind it, so that players respawning together do not land on each
/// other.
EGG::Vector3f MapdataJugemPoint::respawnPos(u8 playerIdx) const {
    constexpr f32 SIDE_STEP = 150.0f;
    constexpr f32 BACK_STEP = 300.0f;
    /// {steps to the left, steps back} per player index (0x8088FA08).
    static constexpr std::array<std::array<s8, 2>, 12> PLAYER_STEPS = {{
            {-1, 0},
            {1, 0},
            {-3, 0},
            {3, 0},
            {-1, 1},
            {1, 1},
            {-3, 1},
            {3, 1},
            {-1, 2},
            {1, 2},
            {-3, 2},
            {3, 2},
    }};

    using GameMode = RaceConfig::Scenario::GameMode;
    GameMode gameMode = RaceConfig::Instance()->raceScenario().gameMode;
    if (gameMode == GameMode::Time_Trial || gameMode == GameMode::Ghost_Race || playerIdx == 0xFF) {
        return m_pos;
    }

    ASSERT(playerIdx < PLAYER_STEPS.size());
    const auto &steps = PLAYER_STEPS[playerIdx];
    EGG::Vector3f side = m_leftDir * (SIDE_STEP * static_cast<f32>(steps[0]));
    EGG::Vector3f back = m_backDir * (BACK_STEP * static_cast<f32>(steps[1]));
    return m_pos + (side + back);
}

/// @addr{Inlined at 0x805130C4}
MapdataJugemPointAccessor::MapdataJugemPointAccessor(const MapSectionHeader *header)
    : MapdataAccessorBase<MapdataJugemPoint, MapdataJugemPoint::SData>(header) {
    init(reinterpret_cast<const MapdataJugemPoint::SData *>(m_sectionHeader + 1),
            parse<u16>(m_sectionHeader->count));
}

MapdataJugemPointAccessor::~MapdataJugemPointAccessor() = default;

} // namespace Kinoko::System
