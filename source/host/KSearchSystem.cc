#include "KSearchSystem.hh"

#include "host/FrameState.hh"
#include "host/Option.hh"
#include "host/SceneCreatorDynamic.hh"

#include <game/system/KPadDirector.hh>
#include <game/system/RaceManager.hh>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace Kinoko {

namespace {

#ifdef _WIN32
extern "C" char __ImageBase;
#else
// Only Windows (PE) exposes the module base address; elsewhere this debug-only value is a constant.
char __ImageBase = 0;
#endif

/// Debug: the ExpHeap used block containing heap offset `off`, as "block=<offset> size=<n> vt=<rva
/// of first word>".
std::string DescribeBlock(const u8 *heap, size_t off) {
    constexpr size_t HEAD = 24;
    for (size_t b = off & ~size_t(3); b + 4 > 4 && off - b < (8u << 20); b -= 4) {
        u16 sig;
        u32 size;
        std::memcpy(&sig, heap + b, 2);
        std::memcpy(&size, heap + b + 4, 4);
        if (sig == 0x5544 && b + HEAD + size > off && b + HEAD <= off + HEAD) {
            uintptr_t first;
            std::memcpy(&first, heap + b + HEAD, 8);
            const uintptr_t image = reinterpret_cast<uintptr_t>(&__ImageBase);
            char buf[128];
            snprintf(buf, sizeof(buf), "block=%zu size=%u +%zu first=%s0x%llx", b, size,
                    off - b - HEAD, first - image < (64u << 20) ? "rva:" : "",
                    static_cast<unsigned long long>(
                            first - image < (64u << 20) ? first - image : first));
            return buf;
        }
        if (b == 0) {
            break;
        }
    }
    return "no block";
}

/// Snapshot table. It must live outside the game heap: operator new is overridden to allocate from
/// the heap that LOAD overwrites, so a std::unordered_map (or any new'd object) would be wiped or
/// corrupted by a restore. Static storage plus malloc are both outside it.
constexpr int MAX_SNAPSHOTS = 1024;

struct Slot {
    Host::Context *context;
    u32 frame;
};

Slot s_slots[MAX_SNAPSHOTS];

Slot &SlotFor(const char *arg) {
    int id = std::atoi(arg);
    if (id < 0 || id >= MAX_SNAPSHOTS) {
        PANIC("Snapshot id out of range: %d", id);
    }

    return s_slots[id];
}

void Release(Slot &slot) {
    if (slot.context) {
        slot.context->~Context();
        free(slot.context);
        slot.context = nullptr;
    }
}

} // namespace

void KSearchSystem::init() {
    auto *sceneCreator = EGG::egg_new<Host::SceneCreatorDynamic>();
    m_sceneMgr = EGG::egg_new<EGG::SceneManager>(sceneCreator);

    System::RaceConfig::RegisterInitCallback(OnInit, nullptr);
    m_sceneMgr->changeScene(0);
}

void KSearchSystem::calc() {
    m_sceneMgr->calc();
}

void KSearchSystem::applyInput(int buttons, float stickX, float stickY, int trick) {
    auto *padDir = System::KPadDirector::Instance();
    auto *host = padDir ? padDir->hostController() : nullptr;
    if (host) {
        host->setInputs(static_cast<u16>(buttons), EGG::Vector2f(stickX, stickY),
                static_cast<System::Trick>(trick));
    }
}

bool KSearchSystem::run() {
    Host::WriteStateHeader(stdout, "COLS ");
    fflush(stdout);

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (std::strncmp(line, "STEP ", 5) == 0) {
            int buttons = 0;
            int trick = 0;
            float stickX = 0.0f;
            float stickY = 0.0f;
            if (sscanf(line + 5, "%d %f %f %d", &buttons, &stickX, &stickY, &trick) != 4) {
                PANIC("Malformed STEP: %s", line);
            }

            applyInput(buttons, stickX, stickY, trick);
            calc();
            Host::WriteStateRow(stdout, m_frame++, "ROW ");
            if (System::RaceManager::Instance()->stage() ==
                    System::RaceManager::Stage::FinishGlobal) {
                reportEnd();
            }
        } else if (std::strncmp(line, "SAVE ", 5) == 0) {
            Slot &slot = SlotFor(line + 5);
            Release(slot);
            // Only the first megabyte of the heap ever changes during a race
            // (tools/state_extent.py), so that is all a snapshot keeps unless the caller asks for
            // more: `SAVE <id> [bytes]`.
            const char *sizeArg = std::strchr(line + 5, ' ');
            size_t bytes = sizeArg ? static_cast<size_t>(std::atoll(sizeArg + 1)) : (1u << 20);
            void *mem = malloc(sizeof(Host::Context));
            slot.context = new (mem) Host::Context(bytes);
            slot.frame = m_frame;
            puts("OK");
            fflush(stdout);
        } else if (std::strncmp(line, "LOAD ", 5) == 0) {
            Slot &slot = SlotFor(line + 5);
            if (!slot.context) {
                PANIC("Unknown snapshot: %s", line);
            }

            Host::Context::SetActiveContext(*slot.context);
            m_frame = slot.frame;
            puts("OK");
            fflush(stdout);
        } else if (std::strncmp(line, "DIRTY ", 6) == 0) {
            // Debug: how much of the game heap differs from snapshot <id>?
            Slot &slot = SlotFor(line + 6);
            if (!slot.context) {
                PANIC("Unknown snapshot: %s", line);
            }

            size_t perMiB[MEMORY_SPACE_SIZE / (1 << 20)];
            size_t bytes = 0;
            size_t last = 0;
            size_t pages = slot.context->CountDifferingPages(perMiB, &bytes, &last);
            printf("DIRTY pages=%zu bytes=%zu last=%zu perMiB=", pages, bytes, last);
            for (size_t i = 0; i < MEMORY_SPACE_SIZE / (1 << 20); ++i) {
                printf("%zu%s", perMiB[i], i + 1 < MEMORY_SPACE_SIZE / (1 << 20) ? "," : "\n");
            }
            fflush(stdout);
        } else if (std::strncmp(line, "DUMP ", 5) == 0) {
            // Debug (GPU port): `DUMP <bytes> <path>` writes the heap base address, the image base
            // and the first <bytes> of the game heap, for word-by-word comparison with a GPU race
            // (tools/gpu_spike/heap_diff.py).
            char path[200] = {};
            size_t bytes = 0;
            if (sscanf(line + 5, "%zu %199s", &bytes, path) != 2 || bytes > MEMORY_SPACE_SIZE) {
                PANIC("Malformed DUMP: %s", line);
            }
            FILE *f = fopen(path, "wb");
            u64 header[2] = {reinterpret_cast<u64>(EGG::SceneManager::RootHeap()),
                    reinterpret_cast<u64>(&__ImageBase)};
            fwrite(header, sizeof(header), 1, f);
            fwrite(EGG::SceneManager::RootHeap(), 1, bytes, f);
            fclose(f);
            puts("OK");
            fflush(stdout);
        } else if (std::strncmp(line, "PTRSCAN ", 8) == 0) {
            // Debug (GPU port planning): which 8-byte words of the game heap hold pointers into the
            // heap? A GPU race would copy only the first R bytes per race and share the rest, so a
            // pointer stored at or beyond R that points below R would make all races share one
            // object. `PTRSCAN <R>`.
            size_t r = static_cast<size_t>(std::atoll(line + 8));
            const auto *heap = reinterpret_cast<const u8 *>(EGG::SceneManager::RootHeap());
            const uintptr_t base = reinterpret_cast<uintptr_t>(heap);
            size_t lastNonZero = 0, lowLow = 0, lowHigh = 0, highLow = 0, highHigh = 0;
            for (size_t off = 0; off + 8 <= MEMORY_SPACE_SIZE; off += 8) {
                uintptr_t v;
                std::memcpy(&v, heap + off, 8);
                if (v != 0) {
                    lastNonZero = off;
                }
                if (v < base || v >= base + MEMORY_SPACE_SIZE) {
                    continue;
                }
                size_t target = v - base;
                if (off < r) {
                    (target < r ? lowLow : lowHigh)++;
                } else if (target < r) {
                    if (highLow < 20) {
                        printf("PTR high->low at=%zu (%s) target=%zu (%s)\n", off,
                                DescribeBlock(heap, off).c_str(), target,
                                DescribeBlock(heap, target).c_str());
                    }
                    highLow++;
                } else {
                    highHigh++;
                }
            }
            printf("PTRSCAN r=%zu used=%zu lowToLow=%zu lowToHigh=%zu highToLow=%zu "
                   "highToHigh=%zu\n",
                    r, lastNonZero + 8, lowLow, lowHigh, highLow, highHigh);
            fflush(stdout);
        } else if (std::strncmp(line, "FREE ", 5) == 0) {
            Release(SlotFor(line + 5));
            puts("OK");
            fflush(stdout);
        } else if (std::strncmp(line, "QUIT", 4) == 0) {
            break;
        } else {
            PANIC("Unknown command: %s", line);
        }
    }

    return true;
}

void KSearchSystem::parseOptions(int argc, char **argv) {
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
        default:
            PANIC("Invalid flag for search mode!");
            break;
        }
    }
}

KSearchSystem *KSearchSystem::CreateInstance() {
    ASSERT(!s_instance);
    s_instance = EGG::egg_new<KSearchSystem>();
    return static_cast<KSearchSystem *>(s_instance);
}

void KSearchSystem::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

KSearchSystem::KSearchSystem()
    : m_sceneMgr(nullptr), m_courseId(8), m_characterId(22), m_vehicleId(32), m_frame(0) {}

KSearchSystem::~KSearchSystem() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("KSearchSystem instance not explicitly handled!");
    }

    EGG::egg_delete(m_sceneMgr);
}

void KSearchSystem::reportEnd() const {
    const auto *race = System::RaceManager::Instance();
    const auto &player = race->player();
    const auto &t = player.raceTimer();
    int timeMs = static_cast<int>(t.min) * 60000 + static_cast<int>(t.sec) * 1000 +
            static_cast<int>(t.mil);

    printf("END finished=1 frames=%u timeMs=%d completion=%d\n", m_frame, timeMs,
            static_cast<int>(player.raceCompletion() * 10000.0f));
    fflush(stdout);
}

void KSearchSystem::OnInit(System::RaceConfig *config, void * /* arg */) {
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
