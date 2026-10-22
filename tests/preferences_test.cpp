#include "preferences.h"

#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	const std::filesystem::path directory = std::filesystem::u8path(argv[1]);
	constexpr std::uint64_t first = 76561198000000001ULL;
	constexpr std::uint64_t second = 76561198000000002ULL;
	if (ladderoff::preferences::Load(directory, first)) return 3;
	if (!ladderoff::preferences::Save(directory, first, true)) return 4;
	if (!ladderoff::preferences::Load(directory, first)) return 5;
	if (!ladderoff::preferences::Save(directory, second, false)) return 6;
	if (ladderoff::preferences::Load(directory, second)) return 7;
	if (!ladderoff::preferences::Load(directory, first)) return 8;
	{
		std::ofstream corrupt(directory / (std::to_string(first) + ".txt"), std::ios::binary | std::ios::trunc);
		corrupt << "invalid\n";
	}
	if (ladderoff::preferences::Load(directory, first)) return 9;
	if (!ladderoff::preferences::Save(directory, first, false)) return 10;
	if (ladderoff::preferences::Load(directory, first)) return 11;
	return 0;
}
