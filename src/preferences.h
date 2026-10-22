#pragma once

#include <cstdint>
#include <filesystem>

namespace ladderoff::preferences
{
// Missing, unreadable and malformed files all mean the safe default: off.
bool Load(const std::filesystem::path &directory, std::uint64_t steamId);
bool Save(const std::filesystem::path &directory, std::uint64_t steamId, bool enabled);
}
