#include "hud.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <engine2/igameeventsystem.h>
#include <engine2/ivengineserver2.h>
#include <gameevents.pb.h>
#include <igameevents.h>
#include <networksystem/inetworkmessages.h>
#include <networksystem/netmessage.h>
#include <recipientfilter.h>
#include <shareddefs.h>
#include <usermessages.pb.h>

namespace ladderoff::hud
{
namespace
{
constexpr int kMaxPlayers = 64;
constexpr auto kRefresh = std::chrono::milliseconds(750);
constexpr const char *kGreen = "<font color='#55DD55' class='fontSize-s'>0 OFF</font>";
constexpr const char *kRed = "<font color='#FF5555' class='fontSize-s'>NOT 0 OFF</font>";

struct SlotState
{
	int shown = -1;
	std::chrono::steady_clock::time_point sent {};
};

IVEngineServer2 *g_engine = nullptr;
IGameEventSystem *g_events = nullptr;
INetworkMessages *g_messages = nullptr;
IGameEventManager2 *g_manager = nullptr;
INetworkMessageInternal *g_htmlMessage = nullptr;
INetworkMessageInternal *g_textMessage = nullptr;
IGameEvent *g_htmlEvent = nullptr;
std::array<SlotState, kMaxPlayers> g_slots;

// CS2KZ's current Windows GameEventManager signature identifies a RIP-relative
// reference to the manager. The native per-client event transport follows the
// public SDK interfaces used by FemboyKZ/mm-cs2menus; no source is copied.
IGameEventManager2 *FindEventManager(void *serverModule)
{
	if (!serverModule)
	{
		return nullptr;
	}
	auto *base = reinterpret_cast<const std::uint8_t *>(serverModule);
	auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 4096)
	{
		return nullptr;
	}
	auto *pe = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (pe->Signature != IMAGE_NT_SIGNATURE || pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
		|| pe->FileHeader.NumberOfSections == 0 || pe->FileHeader.NumberOfSections > 32)
	{
		return nullptr;
	}
	const std::size_t imageSize = pe->OptionalHeader.SizeOfImage;
	if (imageSize < 11 || imageSize > 256u * 1024u * 1024u)
	{
		return nullptr;
	}

	const std::uint8_t *hit = nullptr;
	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(pe);
	for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i)
	{
		if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
		{
			continue;
		}
		const std::size_t start = section[i].VirtualAddress;
		const std::size_t size = section[i].Misc.VirtualSize;
		if (start >= imageSize || size > imageSize - start || size < 11)
		{
			continue;
		}
		for (std::size_t j = 0; j <= size - 11; ++j)
		{
			const std::uint8_t *p = base + start + j;
			if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && p[7] == 0x49
				&& p[8] == 0x8B && p[9] == 0x04 && p[10] == 0x38)
			{
				if (hit)
				{
					return nullptr;
				}
				hit = p;
			}
		}
	}
	if (!hit)
	{
		return nullptr;
	}
	std::int32_t displacement;
	std::memcpy(&displacement, hit + 3, sizeof(displacement));
	const auto begin = reinterpret_cast<std::uintptr_t>(base);
	const auto end = begin + imageSize;
	const auto target = reinterpret_cast<std::uintptr_t>(hit + 7) + static_cast<std::intptr_t>(displacement);
	if (target < begin || target > end - sizeof(void *))
	{
		return nullptr;
	}
	const auto vtable = *reinterpret_cast<const void *const *>(target);
	const auto tableAddress = reinterpret_cast<std::uintptr_t>(vtable);
	if (tableAddress < begin || tableAddress > end - sizeof(void *))
	{
		return nullptr;
	}
	return reinterpret_cast<IGameEventManager2 *>(target);
}

bool Connected(int slot)
{
	return g_engine && slot >= 0 && slot < kMaxPlayers && g_engine->GetPlayerNetInfo(CPlayerSlot(slot));
}

void EnsureMessages()
{
	if (!g_messages) return;
	if (!g_textMessage) g_textMessage = g_messages->FindNetworkMessagePartial("TextMsg");
	if (g_manager && !g_htmlMessage) g_htmlMessage = g_messages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
	if (g_manager && g_htmlMessage && !g_htmlEvent)
	{
		g_htmlEvent = g_manager->CreateEvent("show_survival_respawn_status", true);
	}
}

bool SendHtml(int slot, const char *content, int duration)
{
	if (!g_manager || !g_htmlEvent || !g_htmlMessage || !g_events || !g_messages)
	{
		return false;
	}
	g_htmlEvent->SetString("loc_token", content);
	g_htmlEvent->SetInt("duration", duration);
	g_htmlEvent->SetInt("userid", -1);
	auto *raw = g_htmlMessage->AllocateMessage();
	if (!raw)
	{
		return false;
	}
	auto *data = raw->ToPB<CMsgSource1LegacyGameEvent>();
	const bool ok = g_manager->SerializeEvent(g_htmlEvent, data);
	if (ok)
	{
		CReliableSingleUserRecipientFilter filter{CPlayerSlot(slot)};
		g_events->PostEventAbstract(0, false, &filter, g_htmlMessage, data, 0);
	}
	g_messages->DeallocateNetMessageAbstract(g_htmlMessage, data);
	return ok;
}

bool SendPlain(int slot, const char *content)
{
	if (!g_textMessage || !g_events || !g_messages)
	{
		return false;
	}
	auto *raw = g_textMessage->AllocateMessage();
	if (!raw)
	{
		return false;
	}
	auto *data = raw->ToPB<CUserMessageTextMsg>();
	data->set_dest(HUD_PRINTCENTER);
	data->add_param(content);
	CReliableSingleUserRecipientFilter filter{CPlayerSlot(slot)};
	g_events->PostEventAbstract(0, false, &filter, g_textMessage, data, 0);
	g_messages->DeallocateNetMessageAbstract(g_textMessage, data);
	return true;
}
} // namespace

bool Init(IVEngineServer2 *engine, IGameEventSystem *events, INetworkMessages *messages, void *serverModule)
{
	Cleanup();
	g_engine = engine;
	g_events = events;
	g_messages = messages;
	if (!engine || !events || !messages)
	{
		return false;
	}
	g_manager = FindEventManager(serverModule);
	EnsureMessages();
	return true;
}

bool HasColor()
{
	EnsureMessages();
	return g_manager && g_htmlMessage && g_htmlEvent;
}

void Show(int slot, bool zeroOff)
{
	EnsureMessages();
	if (!Connected(slot))
	{
		return;
	}
	auto &state = g_slots[slot];
	const int desired = zeroOff ? 1 : 0;
	const auto now = std::chrono::steady_clock::now();
	if (state.shown == desired && now - state.sent < kRefresh)
	{
		return;
	}
	const char *plain = zeroOff ? "0 OFF" : "NOT 0 OFF";
	if ((HasColor() && SendHtml(slot, zeroOff ? kGreen : kRed, 2)) || SendPlain(slot, plain))
	{
		state.shown = desired;
		state.sent = now;
	}
}

void Clear(int slot)
{
	if (slot < 0 || slot >= kMaxPlayers)
	{
		return;
	}
	auto &state = g_slots[slot];
	if (state.shown < 0)
	{
		return;
	}
	state = {};
	if (Connected(slot))
	{
		if (!HasColor() || !SendHtml(slot, "<font></font>", 1))
		{
			SendPlain(slot, "");
		}
	}
}

void Cleanup()
{
	for (int slot = 0; slot < kMaxPlayers; ++slot)
	{
		Clear(slot);
	}
	if (g_htmlEvent && g_manager)
	{
		g_manager->FreeEvent(g_htmlEvent);
	}
	g_htmlEvent = nullptr;
	g_htmlMessage = nullptr;
	g_textMessage = nullptr;
	g_manager = nullptr;
	g_messages = nullptr;
	g_events = nullptr;
	g_engine = nullptr;
}
} // namespace ladderoff::hud
