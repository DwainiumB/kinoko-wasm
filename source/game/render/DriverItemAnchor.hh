#pragma once

#include <Common.hh>

#include <egg/math/Vector.hh>

namespace Kinoko::Render {

/// @brief Where the driver's right hand is, in the driver model's space, while an item is held
/// out: once in the throw-forward pose and once in the drop-behind pose. The game computes both at
/// kart setup (`PlayerModel_init` 0x807C8758, fields +0x6C0 and +0x6CC of the kart model) by posing
/// the driver skeleton with the "throw_f" / "throw_b" animations at frame 4 and reading the
/// `wrist_r1` node. Thrown and dropped items start from these points (0x807D171C).
struct DriverItemAnchors {
    EGG::Vector3f
            throwForward;     ///< `wrist_r1` in the "throw_f" pose at frame 4 (kart model +0x6C0).
    EGG::Vector3f dropBehind; ///< `wrist_r1` in the "throw_b" pose at frame 4 (kart model +0x6CC).
};

namespace DriverItemAnchor {

/// @brief The per vehicle / character table, generated from the player's own kart archives with
/// `kinoko_host item-anchors <Race/Kart dir> <this file>`. Kinoko reads it from the file system
/// root, next to Race/Common.szs.
constexpr const char *TABLE_PATH = "/Race/Kart/driverItemAnchors.bin";

/// @brief Computes both anchors from an uncompressed `driver_model.brres`, with the game's own
/// float operations (nw4r CHR0 evaluation, fidx rotation matrices, paired-single concatenation).
/// @return false when the archive has no usable skeleton or animation.
[[nodiscard]] bool Compute(const u8 *brres, size_t size, DriverItemAnchors &out);

/// @brief Looks a vehicle / character pair up in the table (loaded once, cached).
/// @return false when there is no table or no entry.
[[nodiscard]] bool Lookup(Vehicle vehicle, Character character, DriverItemAnchors &out);

/// @brief The character whose kart archive a character uses (biker outfits share the normal
/// character's archive; Miis without an outfit use outfit A).
[[nodiscard]] Character ArchiveCharacter(Character character);

/// @brief The archive name suffix (`Race/Kart/<vehicle>-<code>.szs`) for a character, or nullptr.
[[nodiscard]] const char *ArchiveCode(Character character);

/// @brief The on-disk table layout.
struct TableHeader {
    char magic[4]; ///< "KDIA"
    u32 version;   ///< 1
    u32 count;
    u32 reserved;
};

struct TableEntry {
    u8 vehicle;
    u8 character;
    u16 reserved;
    f32 throwForward[3];
    f32 dropBehind[3];
};

} // namespace DriverItemAnchor

} // namespace Kinoko::Render
