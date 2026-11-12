#include "DriverItemAnchor.hh"

#include "abstract/File.hh"

#include <egg/core/ExpHeap.hh>

#include <egg/math/Math.hh>
#include <egg/math/Matrix.hh>

#include <bit>
#include <cstring>
#include <vector>

namespace Kinoko::Render {

namespace {

/// @brief Big-endian reads into a BRRES buffer.
struct Reader {
    const u8 *d;
    size_t n;

    [[nodiscard]] bool has(size_t o, size_t len) const {
        return o + len <= n;
    }

    [[nodiscard]] u32 u32At(size_t o) const {
        return parse<u32>(*reinterpret_cast<const u32 *>(d + o));
    }

    [[nodiscard]] u16 u16At(size_t o) const {
        return parse<u16>(*reinterpret_cast<const u16 *>(d + o));
    }

    [[nodiscard]] s32 s32At(size_t o) const {
        return static_cast<s32>(u32At(o));
    }

    [[nodiscard]] f32 f32At(size_t o) const {
        return std::bit_cast<f32>(u32At(o));
    }

    [[nodiscard]] const char *str(size_t o) const {
        return reinterpret_cast<const char *>(d + o);
    }
};

/// @brief A nw4r ResDic: `u32 size, u32 count`, then 16-byte entries; entry 0 is the sentinel.
/// Name and data offsets are relative to the dictionary itself.
struct DicEntry {
    const char *name;
    size_t data;
};

u32 DicCount(const Reader &r, size_t dic) {
    return r.has(dic, 8) ? r.u32At(dic + 4) : 0;
}

DicEntry DicAt(const Reader &r, size_t dic, u32 idx) {
    size_t e = dic + 8 + static_cast<size_t>(idx + 1) * 0x10;
    s32 nameOff = r.s32At(e + 8);
    s32 dataOff = r.s32At(e + 0xC);
    return {nameOff ? r.str(dic + nameOff) : "", dataOff ? dic + dataOff : 0};
}

size_t DicFind(const Reader &r, size_t dic, const char *name) {
    u32 count = DicCount(r, dic);
    for (u32 i = 0; i < count; ++i) {
        DicEntry e = DicAt(r, dic, i);
        if (e.data && strcmp(e.name, name) == 0) {
            return e.data;
        }
    }

    return 0;
}

/// @addr{0x8004C140}
/// @brief The dictionary index of a name, or -1. The game walks the dictionary's radix tree; the
/// result is the same as a linear search.
s32 DicIndexOf(const Reader &r, size_t dic, const char *name) {
    u32 count = DicCount(r, dic);
    for (u32 i = 0; i < count; ++i) {
        DicEntry e = DicAt(r, dic, i);
        if (e.data && strcmp(e.name, name) == 0) {
            return static_cast<s32>(i);
        }
    }

    return -1;
}

// ---------------------------------------------------------------------------------------------
// CHR0 track evaluation (nw4r::g3d::ResAnmChr::GetAnmResult, 0x80055540 and its decoders).
// ---------------------------------------------------------------------------------------------

constexpr f32 ONE = 1.0f;
constexpr f32 TWO = 2.0f;
constexpr f32 THREE = 3.0f;

/// @brief The cubic between two keys, as the game computes it (0x8005428C): a reciprocal estimate
/// refined once, then fused multiply-adds in this exact order.
f32 Hermite(f32 frame, f32 f0, f32 v0, f32 t0, f32 f1, f32 v1, f32 t1) {
    f32 d = frame - f0;
    f32 span = f1 - f0;
    f32 inv = EGG::Mathf::fres(span);
    f32 twice = inv + inv;
    f32 sq = inv * inv;
    inv = fma(-span, sq, twice);
    f32 t = d * inv;
    f32 tMinus1 = fma(d, inv, -ONE);
    f32 c1 = fma(TWO, t, -THREE);
    f32 a = t * t1;
    f32 b = d * tMinus1;
    f32 dv = v0 - v1;
    f32 c = c1 * dv;
    f32 e = fma(tMinus1, t0, a);
    c = t * c;
    c = fma(t, c, v0);
    return fma(b, e, c);
}

/// @brief Format 1: 4-byte keys, `frame:8 | quantized value:12 | tangent:12`, with a float step
/// and base (0x80054270).
f32 EvalFormat1(const Reader &r, size_t data, f32 frame) {
    u32 count = r.u16At(data);
    f32 step = r.f32At(data + 8);
    f32 base = r.f32At(data + 0xC);
    size_t keys = data + 0x10;

    auto keyFrame = [&](u32 i) { return static_cast<f32>(r.u32At(keys + 4 * i) >> 24); };
    auto keyValue = [&](u32 i) {
        f32 q = static_cast<f32>((r.u32At(keys + 4 * i) >> 12) & 0xFFF);
        return fma(step, q, base);
    };
    auto keyTangent = [&](u32 i) {
        s32 raw = static_cast<s32>(r.u32At(keys + 4 * i) << 20) >> 20;
        return static_cast<f32>(raw) / 32.0f;
    };

    if (frame <= keyFrame(0)) {
        return keyValue(0);
    }

    if (keyFrame(count - 1) <= frame) {
        return keyValue(count - 1);
    }

    u32 k = 0;
    while (k + 1 < count && keyFrame(k + 1) <= frame) {
        ++k;
    }

    if (frame == keyFrame(k)) {
        return keyValue(k);
    }

    return Hermite(frame, keyFrame(k), keyValue(k), keyTangent(k), keyFrame(k + 1), keyValue(k + 1),
            keyTangent(k + 1));
}

/// @brief Format 2: 6-byte keys, `frame:16/32, quantized value:16, tangent:16/256` (0x800540F0).
f32 EvalFormat2(const Reader &r, size_t data, f32 frame) {
    u32 count = r.u16At(data);
    f32 step = r.f32At(data + 8);
    f32 base = r.f32At(data + 0xC);
    size_t keys = data + 0x10;

    auto keyFrame = [&](u32 i) {
        return static_cast<f32>(static_cast<s16>(r.u16At(keys + 6 * i))) / 32.0f;
    };
    auto keyValue = [&](u32 i) {
        return fma(step, static_cast<f32>(r.u16At(keys + 6 * i + 2)), base);
    };
    auto keyTangent = [&](u32 i) {
        return static_cast<f32>(static_cast<s16>(r.u16At(keys + 6 * i + 4))) / 256.0f;
    };

    if (frame <= keyFrame(0)) {
        return keyValue(0);
    }

    if (keyFrame(count - 1) <= frame) {
        return keyValue(count - 1);
    }

    u32 k = 0;
    while (k + 1 < count && keyFrame(k + 1) <= frame) {
        ++k;
    }

    if (frame == keyFrame(k)) {
        return keyValue(k);
    }

    return Hermite(frame, keyFrame(k), keyValue(k), keyTangent(k), keyFrame(k + 1), keyValue(k + 1),
            keyTangent(k + 1));
}

/// @brief Format 3: 12-byte float keys `frame, value, tangent` (0x80053FB0).
f32 EvalFormat3(const Reader &r, size_t data, f32 frame) {
    u32 count = r.u16At(data);
    size_t keys = data + 8;

    auto keyFrame = [&](u32 i) { return r.f32At(keys + 12 * i); };
    auto keyValue = [&](u32 i) { return r.f32At(keys + 12 * i + 4); };
    auto keyTangent = [&](u32 i) { return r.f32At(keys + 12 * i + 8); };

    if (frame <= keyFrame(0)) {
        return keyValue(0);
    }

    if (keyFrame(count - 1) <= frame) {
        return keyValue(count - 1);
    }

    u32 k = 0;
    while (k + 1 < count && keyFrame(k + 1) <= frame) {
        ++k;
    }

    if (frame == keyFrame(k)) {
        return keyValue(k);
    }

    return Hermite(frame, keyFrame(k), keyValue(k), keyTangent(k), keyFrame(k + 1), keyValue(k + 1),
            keyTangent(k + 1));
}

/// @brief Rotation formats 4 to 6: one value per frame (8-bit or 16-bit with a float scale and
/// bias, or plain floats). Only whole frames are needed here.
f32 EvalPerFrame(const Reader &r, size_t data, u32 fmt, f32 frame) {
    if (frame != static_cast<f32>(static_cast<u32>(frame))) {
        PANIC("Driver item anchor: per-frame animation sampled between frames");
    }

    u32 idx = static_cast<u32>(frame);
    if (fmt == 6) {
        return r.f32At(data + 4 * idx);
    }

    f32 scale = r.f32At(data);
    f32 bias = r.f32At(data + 4);
    f32 raw = fmt == 4 ? static_cast<f32>(r.d[data + 8 + idx]) :
                         static_cast<f32>(r.u16At(data + 8 + 2 * idx));
    return fma(scale, raw, bias);
}

/// @brief One channel of a track: a constant in the slot when fixed, else keyed data at the
/// slot's offset from the track.
f32 EvalChannel(const Reader &r, size_t track, size_t slot, u32 fmt, bool fixed, f32 frame) {
    if (fmt == 0 || fixed) {
        return r.f32At(slot);
    }

    size_t data = track + r.u32At(slot);
    switch (fmt) {
    case 1:
        return EvalFormat1(r, data, frame);
    case 2:
        return EvalFormat2(r, data, frame);
    case 3:
        return EvalFormat3(r, data, frame);
    case 4:
    case 5:
    case 6:
        return EvalPerFrame(r, data, fmt, frame);
    default:
        PANIC("Driver item anchor: unknown animation format %u", fmt);
    }
}

/// @addr{0x80085940}
/// @brief nw4r::math::MTX34RotXYZFIdx: a rotation matrix from three angles in 1/256 turns, with
/// the game's sine table and paired-single operation order.
void RotXYZFIdx(EGG::Matrix34f &m, f32 fx, f32 fy, f32 fz) {
    constexpr f32 WRAP = 65536.0f;

    auto sinCos = [](f32 fidx, f32 &s, f32 &c) {
        f32 a = EGG::Mathf::abs(fidx);
        while (a > WRAP) {
            a -= WRAP;
        }

        u32 whole = a >= 65535.0f ? 65535 : static_cast<u32>(a);
        f32 frac = a - static_cast<f32>(static_cast<u16>(whole));
        const auto &e = EGG::Mathf::sSinCosTbl[whole & 0xFF];
        s = fma(e.sinDt, frac, e.sinVal);
        c = fma(e.cosDt, frac, e.cosVal);
        if (fidx < 0.0f) {
            s = -s;
        }
    };

    f32 sx, cx, sy, cy, sz, cz;
    sinCos(fx, sx, cx);
    sinCos(fy, sy, cy);
    sinCos(fz, sz, cz);

    f32 szcy = sz * cy;
    f32 czcy = cz * cy;
    f32 sxsz = sx * sz;
    f32 cxsz = cx * sz;
    f32 sxcz = sx * cz;
    f32 cxcz = cx * cz;

    m[0, 0] = czcy;
    m[0, 1] = fma(sxcz, sy, -cxsz);
    m[0, 2] = fma(cxcz, sy, sxsz);
    m[0, 3] = 0.0f;
    m[1, 0] = szcy;
    m[1, 1] = fma(sxsz, sy, cxcz);
    m[1, 2] = fma(cxsz, sy, -sxcz);
    m[1, 3] = 0.0f;
    m[2, 0] = -sy;
    m[2, 1] = sx * cy;
    m[2, 2] = cx * cy;
    m[2, 3] = 0.0f;
}

/// @brief The posed SRT of one track at one frame: the game's `ChrAnmResult`.
struct AnmResult {
    u32 flags; ///< The track code's low 13 bits, plus bit 31 when there is a rotation.
    EGG::Vector3f scale;
    EGG::Matrix34f mtx; ///< Rotation, with the translation in the fourth column.
};

/// @addr{0x80055540}
void GetAnmResult(const Reader &r, size_t chr0, s32 trackIdx, f32 frame, AnmResult &res) {
    constexpr f32 DEG_TO_FIDX = std::bit_cast<f32>(0x3F360B61u); // 256 / 360

    size_t dic = chr0 + r.s32At(chr0 + 0x10);
    size_t track = DicAt(r, dic, static_cast<u32>(trackIdx)).data;
    u32 code = r.u32At(track + 4);
    size_t slot = track + 8;

    res.flags = code & 0x1FFF;
    res.scale = EGG::Vector3f(ONE, ONE, ONE);
    res.mtx = EGG::Matrix34f::ident;

    bool hasScale = code & 0x400000;
    bool hasRot = code & 0x800000;
    bool hasTrans = code & 0x1000000;

    if (hasScale) {
        // 0x80053D00
        u32 fmt = (code >> 25) & 3;
        bool iso = code & 0x10;
        f32 x = EvalChannel(r, track, slot, fmt, code & 0x2000, frame);
        slot += 4;
        f32 y = x;
        f32 z = x;
        if (!iso) {
            y = EvalChannel(r, track, slot, fmt, code & 0x4000, frame);
            slot += 4;
            z = EvalChannel(r, track, slot, fmt, code & 0x8000, frame);
            slot += 4;
        }
        res.scale = EGG::Vector3f(x, y, z);
    }

    if (hasRot) {
        // 0x80054460
        u32 fmt = (code >> 27) & 7;
        f32 x = EvalChannel(r, track, slot, fmt, code & 0x10000, frame);
        f32 y = EvalChannel(r, track, slot + 4, fmt, code & 0x20000, frame);
        f32 z = EvalChannel(r, track, slot + 8, fmt, code & 0x40000, frame);
        slot += 12;
        RotXYZFIdx(res.mtx, DEG_TO_FIDX * x, DEG_TO_FIDX * y, DEG_TO_FIDX * z);
        res.flags |= 0x80000000;
    }

    if (hasTrans) {
        // 0x80054ED0
        u32 fmt = (code >> 30) & 3;
        res.mtx[0, 3] = EvalChannel(r, track, slot, fmt, code & 0x80000, frame);
        res.mtx[1, 3] = EvalChannel(r, track, slot + 4, fmt, code & 0x100000, frame);
        res.mtx[2, 3] = EvalChannel(r, track, slot + 8, fmt, code & 0x200000, frame);
    }
}

// ---------------------------------------------------------------------------------------------
// The driver skeleton as the kart model holds it: 36 named slots (0x807C7E7C).
// ---------------------------------------------------------------------------------------------

constexpr const char *SKELETON_SLOT_NAMES[] = {
        "skl_root",
        "leg_l1",
        "leg_l2",
        "ankle_l1",
        "leg_r1",
        "leg_r2",
        "ankle_r1",
        "spin",
        "arm_l1",
        "arm_l2",
        "wrist_l1",
        "arm_r1",
        "arm_r2",
        "wrist_r1",
        "face_1",
        "cap1",
        "mouth_1",
        "ca_mouth_1",
        "yo_mouth_1",
        "tang_1",
        "tang_2",
        "tang_3",
        "tie_1",
        "wing_l1",
        "wing_r1",
        "tail_1",
        "tail_2",
        "tail_3",
        "skirt_1",
        "d_hair_1",
        "d_hair_2",
        "p_hair_1",
        "p_hair_2",
        "unknown_1",
        "unknown_2",
        "unknown_3",
};
constexpr size_t SLOT_COUNT = 36;
constexpr size_t NAMED_SLOTS = 33;      ///< Bones with other names go into slots 33 to 35.
constexpr size_t RIGHT_WRIST_SLOT = 13; ///< The slot the anchors are read from (+0x534).
constexpr f32 ANCHOR_FRAME = 4.0f;      ///< Both anchors pose the skeleton at frame 4.

struct Node {
    size_t bone = 0; ///< ResNodeData offset in the BRRES, 0 when the slot is empty.
    s32 parent = -1;
    s32 child = -1;
    s32 next = -1;
    EGG::Matrix34f mtx = EGG::Matrix34f::ident;
    EGG::Vector3f scale = EGG::Vector3f(ONE, ONE, ONE);
};

/// @addr{0x807CAE8C}
/// @brief Poses one node and its subtree: its track evaluated at `frame`, the translation scaled
/// by the parent's scale, then concatenated onto the parent's matrix.
void PoseSubtree(const Reader &r, Node *nodes, s32 idx, const EGG::Matrix34f *parentMtx,
        const EGG::Vector3f *parentScale, size_t chr0, f32 frame) {
    Node &node = nodes[idx];
    size_t dic = chr0 + r.s32At(chr0 + 0x10);
    const char *name = r.str(node.bone + r.s32At(node.bone + 8));
    s32 trackIdx = DicIndexOf(r, dic, name);
    if (trackIdx < 0) {
        if (node.parent >= 0) {
            PANIC("Driver item anchor: bone %s has no animation track", name);
        }
        trackIdx = 0;
    }

    AnmResult res;
    GetAnmResult(r, chr0, trackIdx, frame, res);

    // 0x80055600: the node's local matrix.
    if (!(res.flags & 0x20)) {
        node.mtx = res.mtx;
        if (res.flags & 0x40) {
            node.mtx[0, 3] = 0.0f;
            node.mtx[1, 3] = 0.0f;
            node.mtx[2, 3] = 0.0f;
        }
    } else {
        node.mtx = EGG::Matrix34f::ident;
        if (!(res.flags & 0x40)) {
            node.mtx[0, 3] = res.mtx[0, 3];
            node.mtx[1, 3] = res.mtx[1, 3];
            node.mtx[2, 3] = res.mtx[2, 3];
        }
    }

    // 0x800555C0: the node's scale, applied to its children's translations.
    node.scale = (res.flags & 8) ? EGG::Vector3f(ONE, ONE, ONE) : res.scale;

    if (parentMtx) {
        node.mtx[0, 3] = node.mtx[0, 3] * parentScale->x;
        node.mtx[1, 3] = node.mtx[1, 3] * parentScale->y;
        node.mtx[2, 3] = node.mtx[2, 3] * parentScale->z;
        node.mtx = parentMtx->multiplyTo(node.mtx);
    }

    if (node.child >= 0) {
        PoseSubtree(r, nodes, node.child, &node.mtx, &node.scale, chr0, frame);
    }

    if (node.next >= 0) {
        PoseSubtree(r, nodes, node.next, parentMtx, parentScale, chr0, frame);
    }
}

} // namespace

namespace DriverItemAnchor {

bool Compute(const u8 *brres, size_t size, DriverItemAnchors &out) {
    Reader r{brres, size};
    if (size < 0x10 || memcmp(brres, "bres", 4) != 0) {
        return false;
    }

    size_t root = r.u16At(0xC);
    size_t rootDic = root + 8;
    size_t models = DicFind(r, rootDic, "3DModels(NW4R)");
    size_t anims = DicFind(r, rootDic, "AnmChr(NW4R)");
    if (!models || !anims || DicCount(r, models) == 0) {
        return false;
    }

    // The driver skeleton is the model named "model"; some archives carry extra models (hair).
    size_t mdl0 = DicFind(r, models, "model");
    if (!mdl0) {
        mdl0 = DicAt(r, models, 0).data;
    }
    size_t throwForward = DicFind(r, anims, "throw_f");
    size_t dropBehind = DicFind(r, anims, "throw_b");
    if (!mdl0 || !throwForward || !dropBehind) {
        return false;
    }

    // 0x807C7E7C: bones into their named slots, unknown ones into the spare slots.
    Node nodes[SLOT_COUNT];
    size_t boneDic = mdl0 + r.s32At(mdl0 + 0x14);
    u32 boneCount = DicCount(r, boneDic);
    size_t spare = NAMED_SLOTS;
    for (u32 i = 0; i < boneCount; ++i) {
        size_t bone = DicAt(r, boneDic, i).data;
        if (!bone) {
            continue;
        }

        const char *name = r.str(bone + r.s32At(bone + 8));
        bool placed = false;
        for (size_t slot = 0; slot < SLOT_COUNT; ++slot) {
            if (strcmp(name, SKELETON_SLOT_NAMES[slot]) == 0) {
                nodes[slot].bone = bone;
                placed = true;
                break;
            }
        }

        if (!placed && spare < SLOT_COUNT) {
            nodes[spare++].bone = bone;
        }
    }

    auto slotOf = [&](size_t bone) -> s32 {
        if (!bone) {
            return -1;
        }
        for (size_t slot = 0; slot < SLOT_COUNT; ++slot) {
            if (nodes[slot].bone == bone) {
                return static_cast<s32>(slot);
            }
        }
        return -1;
    };

    s32 rootSlot = -1;
    for (size_t slot = 0; slot < SLOT_COUNT; ++slot) {
        Node &node = nodes[slot];
        if (!node.bone) {
            continue;
        }

        auto link = [&](size_t field) -> s32 {
            s32 off = r.s32At(node.bone + field);
            return off ? slotOf(node.bone + off) : -1;
        };
        node.parent = link(0x5C);
        node.child = link(0x60);
        node.next = link(0x64);

        if (rootSlot < 0) {
            rootSlot = static_cast<s32>(slot);
        }
    }

    if (rootSlot < 0) {
        return false;
    }

    while (nodes[rootSlot].parent >= 0) {
        rootSlot = nodes[rootSlot].parent;
    }

    auto anchor = [&](size_t chr0) {
        for (auto &node : nodes) {
            node.mtx = EGG::Matrix34f::ident;
            node.scale = EGG::Vector3f(ONE, ONE, ONE);
        }
        PoseSubtree(r, nodes, rootSlot, nullptr, nullptr, chr0, ANCHOR_FRAME);
        return nodes[RIGHT_WRIST_SLOT].mtx.translation();
    };

    out.throwForward = anchor(throwForward);
    out.dropBehind = anchor(dropBehind);
    return true;
}

Character ArchiveCharacter(Character character) {
    switch (character) {
    case Character::Peach_Biker_Outfit:
        return Character::Peach;
    case Character::Daisy_Biker_Outfit:
        return Character::Daisy;
    case Character::Rosalina_Biker_Outfit:
        return Character::Rosalina;
    case Character::Small_Mii:
        return Character::Small_Mii_Outfit_A_Male;
    case Character::Medium_Mii:
        return Character::Medium_Mii_Outfit_A_Male;
    case Character::Large_Mii:
        return Character::Large_Mii_Outfit_A_Male;
    default:
        return character;
    }
}

const char *ArchiveCode(Character character) {
    static constexpr const char *CODES[] = {
            "mr",
            "bpc",
            "wl",
            "kp",
            "bds",
            "ka",
            "bmr",
            "lg",
            "ko",
            "dk",
            "ys",
            "wr",
            "blg",
            "kk",
            "nk",
            "ds",
            "pc",
            "ca",
            "dd",
            "kt",
            "jr",
            "bk",
            "fk",
            "rs",
            // Miis: small / medium / large, outfits A and B, male / female. Outfit C has no
            // archive of its own and uses outfit A's.
            "sa_mii_m",
            "sa_mii_f",
            "sb_mii_m",
            "sb_mii_f",
            "sa_mii_m",
            "sa_mii_f",
            "ma_mii_m",
            "ma_mii_f",
            "mb_mii_m",
            "mb_mii_f",
            "ma_mii_m",
            "ma_mii_f",
            "la_mii_m",
            "la_mii_f",
            "lb_mii_m",
            "lb_mii_f",
            "la_mii_m",
            "la_mii_f",
    };

    size_t idx = static_cast<size_t>(ArchiveCharacter(character));
    return idx < std::size(CODES) ? CODES[idx] : nullptr;
}

bool Lookup(Vehicle vehicle, Character character, DriverItemAnchors &out) {
    // The table outlives the game heap (the test suite rebuilds the heap per case), so the entries
    // are copied out of the loaded file once.
    static bool s_loaded = false;
    static std::vector<TableEntry> s_entries;

    if (!s_loaded) {
        s_loaded = true;
        size_t size = 0;
        u8 *file = Abstract::File::TryLoad(TABLE_PATH, size);
        if (file && size >= sizeof(TableHeader)) {
            TableHeader header;
            memcpy(&header, file, sizeof(header));
            if (memcmp(header.magic, "KDIA", 4) == 0 && header.version == 1 &&
                    sizeof(TableHeader) + header.count * sizeof(TableEntry) <= size) {
                s_entries.resize(header.count);
                memcpy(s_entries.data(), file + sizeof(TableHeader),
                        header.count * sizeof(TableEntry));
            }
        }
        if (file) {
            EGG::egg_free(file);
        }
    }

    u8 v = static_cast<u8>(vehicle);
    u8 c = static_cast<u8>(ArchiveCharacter(character));
    for (const TableEntry &e : s_entries) {
        if (e.vehicle == v && e.character == c) {
            out.throwForward =
                    EGG::Vector3f(e.throwForward[0], e.throwForward[1], e.throwForward[2]);
            out.dropBehind = EGG::Vector3f(e.dropBehind[0], e.dropBehind[1], e.dropBehind[2]);
            return true;
        }
    }

    return false;
}

} // namespace DriverItemAnchor

} // namespace Kinoko::Render
