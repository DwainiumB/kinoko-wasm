#pragma once

#include <Common.hh>

#include <cstdio>

namespace Kinoko::Host {

/// Number of checkpoints ahead of the player written into every state row.
constexpr int PATH_LOOKAHEAD = 12;

/// @brief Writes the CSV header for the rows produced by WriteStateRow.
/// @param prefix Optional text written before the header (used by the drive protocol).
void WriteStateHeader(FILE *file, const char *prefix = "");

/// @brief Writes one CSV row describing player 0 after the frame that just ran.
/// @details Used by both the replay dump (tools/dump_ghosts.py) and the closed-loop drive mode
/// (tools/drive_policy.py), so a policy sees identical columns in training and in the loop.
/// The path columns hold, for PATH_LOOKAHEAD checkpoints starting at the player's current one
/// (following the first branch), the world x/z of the middle and of both edges.
void WriteStateRow(FILE *file, u32 frame, const char *prefix = "");

} // namespace Kinoko::Host
