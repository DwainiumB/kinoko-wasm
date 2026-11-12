#pragma once

#include "game/system/map/MapdataAccessorBase.hh"

#include <egg/math/Vector.hh>

namespace Kinoko::System {

/// @brief This class represents the course's respawn positions.
class MapdataJugemPoint {
public:
    struct SData {
        EGG::Vector3f pos;
        EGG::Vector3f rot;
        u16 id;
        s16 range;
    };
    static_assert(sizeof(SData) == 0x1c);

    MapdataJugemPoint(const SData *data);
    void read(EGG::Stream &stream);

    void linkNearestItemPoint();
    [[nodiscard]] EGG::Vector3f respawnPos(u8 playerIdx) const;

    /// @beginGetters
    const EGG::Vector3f &pos() const {
        return m_pos;
    }

    const EGG::Vector3f &rot() const {
        return m_rot;
    }

    /// @brief The item route point a player continues from after respawning here (+0x29 in the
    /// game).
    [[nodiscard]] u8 itemPointIdx() const {
        return m_itemPointIdx;
    }
    /// @endGetters

private:
    [[maybe_unused]] const SData *m_rawData;
    EGG::Vector3f m_pos;
    EGG::Vector3f m_rot;     ///< Degrees, as in the file.
    EGG::Vector3f m_backDir; ///< The point's -Z axis (+0x10 in the game): players 5 to 12 are
                             ///< placed behind the point.
    EGG::Vector3f m_leftDir; ///< The point's -X axis (+0x1C): players alternate sides along it.
    u8 m_itemPointIdx;       ///< Set by linkNearestItemPoint().
};

class MapdataJugemPointAccessor
    : public MapdataAccessorBase<MapdataJugemPoint, MapdataJugemPoint::SData> {
public:
    MapdataJugemPointAccessor(const MapSectionHeader *header);
    ~MapdataJugemPointAccessor() override;
};

} // namespace Kinoko::System
