#include "Option.hh"

#include <cstring>

namespace Kinoko::Host::Option {

std::optional<EOption> CheckFlag(const char *arg) {
    ASSERT(arg);
    if (arg[0] != '-') {
        return std::nullopt;
    }

    // Verbose flag
    if (arg[1] == '-') {
        const char *verbose_arg = &arg[2];

        if (strcmp(verbose_arg, "suite") == 0) {
            return EOption::Suite;
        }

        if (strcmp(verbose_arg, "ghost") == 0) {
            return EOption::Ghost;
        }

        if (strcmp(verbose_arg, "krkg") == 0) {
            return EOption::KRKG;
        }

        if (strcmp(verbose_arg, "framecount") == 0) {
            return EOption::TargetFrame;
        }

        if (strcmp(verbose_arg, "task") == 0) {
            return EOption::Task;
        }

        if (strcmp(verbose_arg, "dump") == 0) {
            return EOption::Dump;
        }

        if (strcmp(verbose_arg, "course") == 0) {
            return EOption::Course;
        }

        if (strcmp(verbose_arg, "character") == 0) {
            return EOption::Character;
        }

        if (strcmp(verbose_arg, "vehicle") == 0) {
            return EOption::Vehicle;
        }

        if (strcmp(verbose_arg, "maxframes") == 0) {
            return EOption::MaxFrames;
        }

        return EOption::Invalid;
    } else {
        switch (arg[1]) {
        case 'S':
        case 's':
            return EOption::Suite;
        case 'G':
        case 'g':
            return EOption::Ghost;
        case 'K':
        case 'k':
            return EOption::KRKG;
        case 'F':
        case 'f':
            return EOption::TargetFrame;
        case 'T':
        case 't':
            return EOption::Task;
        case 'D':
        case 'd':
            return EOption::Dump;
        default:
            return EOption::Invalid;
        }
    }

    // This is unreachable
    return std::nullopt;
}

} // namespace Kinoko::Host::Option
