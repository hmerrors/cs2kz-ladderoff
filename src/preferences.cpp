#include "preferences.h"

#include <fstream>
#include <string>
#include <system_error>
#include <Windows.h>

namespace ladderoff::preferences
{
static std::filesystem::path File(const std::filesystem::path &directory, std::uint64_t steamId)
{
	return directory / (std::to_string(steamId) + ".txt");
}

bool Load(const std::filesystem::path &directory, std::uint64_t steamId)
{
	if (!steamId)
	{
		return false;
	}
	std::ifstream in(File(directory, steamId), std::ios::binary);
	char state = 0, lineEnd = 0, extra = 0;
	if (!in.get(state) || (state != '0' && state != '1'))
	{
		return false;
	}
	if (in.get(lineEnd))
	{
		if (lineEnd != '\n' || in.get(extra))
		{
			return false;
		}
	}
	return state == '1';
}

bool Save(const std::filesystem::path &directory, std::uint64_t steamId, bool enabled)
{
	if (!steamId)
	{
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	if (ec)
	{
		return false;
	}
	auto path = File(directory, steamId);
	auto temporary = path;
	temporary += L".tmp";
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		out << (enabled ? "1\n" : "0\n");
		out.flush();
		if (!out)
		{
			return false;
		}
	}
	return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
}
