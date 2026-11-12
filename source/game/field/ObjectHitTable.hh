#pragma once

#include "game/field/obj/ObjectId.hh"

#include "game/kart/KartCollide.hh"

#include <span>

namespace Kinoko::Field {

/// @brief One of the GeoHitTable files: per object slot a header and `fieldCount` reactions. The
/// kart tables have four columns (a kart in no special state, in a star, a mega mushroom or a
/// bullet bill; ObjectDirector::KartHitColumn), the item tables (GeoHitTableItem.bin,
/// GeoHitTableItemObj.bin) one per item object kind.
class ObjectHitTable {
public:
    ObjectHitTable(const char *filename);
    ~ObjectHitTable();

    Kart::Reaction reaction(s16 i) const;
    [[nodiscard]] Kart::Reaction kartReaction(s16 i, s16 column) const;
    [[nodiscard]] s16 reaction(s16 i, s16 column) const;
    s16 slot(ObjectId id) const;

private:
    s16 m_count;
    s16 m_fieldCount;
    owning_span<s16>
            m_reactions; ///< `m_count` rows of `m_fieldCount` values, the headers left out.
    const s16 *m_slots;
};

} // namespace Kinoko::Field
