#include "DriverItemAnchorTool.hh"

#include "abstract/File.hh"
#include "game/render/DriverItemAnchor.hh"

#include <egg/core/Decomp.hh>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Kinoko::Host {

namespace {

u32 BigU32(const u8 *p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
            (static_cast<u32>(p[2]) << 8) | p[3];
}

/// @brief Finds a file by name in an uncompressed U8 archive (a flat scan of the node table).
const u8 *FindInU8(const std::vector<u8> &archive, const char *wanted, size_t &size) {
    if (archive.size() < 0x20 || BigU32(archive.data()) != 0x55AA382D) {
        return nullptr;
    }

    u32 nodesOff = BigU32(archive.data() + 4);
    const u8 *nodes = archive.data() + nodesOff;
    u32 nodeCount = BigU32(nodes + 8); // the root node's size is the node count
    const char *names = reinterpret_cast<const char *>(nodes + nodeCount * 12);

    for (u32 i = 1; i < nodeCount; ++i) {
        const u8 *node = nodes + i * 12;
        u32 typeAndName = BigU32(node);
        if (typeAndName >> 24) {
            continue; // a directory
        }

        const char *name = names + (typeAndName & 0xFFFFFF);
        if (strcmp(name, wanted) == 0) {
            u32 dataOff = BigU32(node + 4);
            size = BigU32(node + 8);
            if (dataOff + size > archive.size()) {
                return nullptr;
            }
            return archive.data() + dataOff;
        }
    }

    return nullptr;
}

bool LoadArchive(const std::filesystem::path &path, std::vector<u8> &out) {
    size_t size = 0;
    std::string pathStr = path.string();
    u8 *data = Abstract::File::LoadHost(pathStr.c_str(), size);
    if (!data) {
        return false;
    }

    if (size >= 0x10 && memcmp(data, "Yaz0", 4) == 0) {
        out.resize(EGG::Decomp::GetExpandSize(data));
        EGG::Decomp::DecodeSZS(data, out.data());
    } else {
        out.assign(data, data + size);
    }

    delete[] data;
    return true;
}

} // namespace

bool GenerateDriverItemAnchors(const char *kartDir, const char *outPath) {
    using namespace Render::DriverItemAnchor;

    std::vector<TableEntry> entries;
    std::vector<u8> archive;
    size_t skipped = 0;

    for (const auto &dirEntry : std::filesystem::directory_iterator(kartDir)) {
        if (!dirEntry.is_regular_file()) {
            continue;
        }

        std::string stem = dirEntry.path().stem().string();
        std::string ext = dirEntry.path().extension().string();
        if (ext != ".szs") {
            continue;
        }

        // Multiplayer variants (_2, _4) hold the same driver model at lower detail.
        if (stem.size() > 2 && stem[stem.size() - 2] == '_' &&
                (stem.back() == '2' || stem.back() == '4')) {
            continue;
        }

        size_t dash = stem.find('-');
        if (dash == std::string::npos) {
            continue;
        }

        std::string vehicleName = stem.substr(0, dash);
        std::string code = stem.substr(dash + 1);

        s32 vehicle = -1;
        for (size_t i = 0; i < std::size(VEHICLE_NAMES); ++i) {
            if (vehicleName == VEHICLE_NAMES[i]) {
                vehicle = static_cast<s32>(i);
                break;
            }
        }

        if (vehicle < 0) {
            ++skipped;
            continue;
        }

        std::vector<Character> characters;
        for (int c = 0; c < static_cast<int>(Character::Max); ++c) {
            Character character = static_cast<Character>(c);
            if (ArchiveCharacter(character) != character) {
                continue;
            }
            const char *archiveCode = ArchiveCode(character);
            if (archiveCode && code == archiveCode) {
                characters.push_back(character);
            }
        }

        if (characters.empty()) {
            ++skipped;
            continue;
        }

        if (!LoadArchive(dirEntry.path(), archive)) {
            ++skipped;
            continue;
        }

        size_t brresSize = 0;
        const u8 *brres = FindInU8(archive, "driver_model.brres", brresSize);
        Render::DriverItemAnchors anchors;
        if (!brres || !Compute(brres, brresSize, anchors)) {
            fprintf(stderr, "item-anchors: no usable driver model in %s\n", stem.c_str());
            ++skipped;
            continue;
        }

        for (Character character : characters) {
            TableEntry e{};
            e.vehicle = static_cast<u8>(vehicle);
            e.character = static_cast<u8>(character);
            e.throwForward[0] = anchors.throwForward.x;
            e.throwForward[1] = anchors.throwForward.y;
            e.throwForward[2] = anchors.throwForward.z;
            e.dropBehind[0] = anchors.dropBehind.x;
            e.dropBehind[1] = anchors.dropBehind.y;
            e.dropBehind[2] = anchors.dropBehind.z;
            entries.push_back(e);
        }

        const auto &t = anchors.throwForward;
        const auto &d = anchors.dropBehind;
        printf("%-24s throw (%.4f %.4f %.4f) drop (%.4f %.4f %.4f)\n", stem.c_str(),
                static_cast<double>(t.x), static_cast<double>(t.y), static_cast<double>(t.z),
                static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z));
    }

    if (entries.empty()) {
        fprintf(stderr, "item-anchors: no kart archives found in %s\n", kartDir);
        return false;
    }

    std::filesystem::path out(outPath);
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path());
    }

    std::ofstream file(out, std::ios::binary);
    TableHeader header{};
    memcpy(header.magic, "KDIA", 4);
    header.version = 1;
    header.count = static_cast<u32>(entries.size());
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(entries.data()),
            static_cast<std::streamsize>(entries.size() * sizeof(TableEntry)));

    printf("item-anchors: wrote %zu entries to %s (%zu archives skipped)\n", entries.size(), outPath,
            skipped);
    return true;
}

} // namespace Kinoko::Host
