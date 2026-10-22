#include <ISmmPlugin.h>

#include "ladder.h"
#include "hud.h"
#include "preferences.h"

#include "engine2/igameeventsystem.h"
#include "engine2/igameresourceservice.h"
#include "engine2/isource2server.h"
#include "engine2/ivengineserver2.h"
#include "entity2/entitysystem.h"
#include "entity2/entityinstance.h"
#include "ehandle.h"
#include "icvar.h"
#include "networksystem/inetworkmessages.h"
#include "schemasystem/schemasystem.h"
#include "steam/steamclientpublic.h"
#include "recipientfilter.h"
#include "shareddefs.h"
#include "usermessages.pb.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <Windows.h>

PLUGIN_GLOBALVARS();

namespace ladderoff
{
struct Field
{
	int offset {-1};

	template<class T> T *At(void *object) const
	{
		return object && offset >= 0 ? reinterpret_cast<T *>(static_cast<char *>(object) + offset) : nullptr;
	}
};

static Field FindField(CSchemaSystem *schemas, const char *type, const char *name)
{
	auto scope = schemas->FindTypeScopeForModule("server.dll");
	auto declared = scope ? scope->FindDeclaredClass(type).Get() : nullptr;
	if (declared)
	{
		for (int index = 0; index < declared->m_nFieldCount; ++index)
		{
			auto &field = declared->m_pFields[index];
			if (!std::strcmp(field.m_pszName, name))
			{
				return {field.m_nSingleInheritanceOffset};
			}
		}
	}
	return {};
}

struct Player
{
	ladder::State ladderState;
	std::uint64_t steamId {};
	std::uint32_t pawnHandle {};
	double nextSave {}, nextHud {};
	bool enabled {}, loaded {}, dirty {}, hudVisible {}, hudZero {};
};

class Plugin final : public ISmmPlugin, public IMetamodListener
{
public:
	Plugin()
		: gameFrameHook(&ISource2Server::GameFrame, this, nullptr, &Plugin::FramePost),
		  dispatchHook(&ICvar::DispatchConCommand, this, &Plugin::DispatchPre, nullptr),
		  disconnectHook(&ISource2GameClients::ClientDisconnect, this, &Plugin::DisconnectPre, nullptr),
		  activeHook(&ISource2GameClients::ClientActive, this, nullptr, &Plugin::ActivePost)
	{
	}

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	bool Unload(char *, size_t) override;
	void OnLevelShutdown() override;
	void OnLevelInit(const char *, const char *, const char *, const char *, bool, bool) override;
	bool Pause(char *, size_t) override;
	bool Unpause(char *, size_t) override;

	const char *GetAuthor() override { return "KZLadderOff contributors"; }
	const char *GetName() override { return "KZLadderOff"; }
	const char *GetDescription() override { return "Read-only CS2KZ ladder contact candidate"; }
	const char *GetURL() override { return ""; }
	const char *GetLicense() override { return "AGPL-3.0"; }
	const char *GetVersion() override { return "0.1.3-candidate"; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "KZLadderOff"; }

private:
	KHook::Return<void> FramePost(ISource2Server *, bool simulating, bool, bool);
	KHook::Return<void> DispatchPre(ICvar *, ConCommandRef, const CCommandContext &, const CCommand &);
	KHook::Return<void> DisconnectPre(ISource2GameClients *, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
	KHook::Return<void> ActivePost(ISource2GameClients *, CPlayerSlot, bool, const char *, uint64);

	void Reset(int slot, bool notifyClient);
	void Hide(int slot);
	void Frame();
	void Identity(int slot, double now);
	void Toggle(int slot);
	void Chat(int slot, const char *text);
	CEntityInstance *Pawn(int slot) const;
	bool Alive(CEntityInstance *pawn) const;

	KHook::Virtual<ISource2Server, void, bool, bool, bool> gameFrameHook;
	KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext &, const CCommand &> dispatchHook;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *> disconnectHook;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, bool, const char *, uint64> activeHook;
	IVEngineServer2 *engine {};
	ISource2Server *game {};
	ISource2GameClients *clients {};
	IGameEventSystem *events {};
	INetworkMessages *messages {};
	INetworkMessageInternal *textMessage {};
	CSchemaSystem *schemas {};
	Field controllerPawn, lifeState, teamNumber;
	std::filesystem::path preferenceDirectory;
	std::array<Player, 64> players;
	bool running {}, paused {};
};

bool Plugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool)
{
	PLUGIN_SAVEVARS();
	// Both Metamod's loader and the game are named server.dll. Resolve the
	// game's full path so RTTI and HUD signatures inspect the correct image.
	auto serverPath = std::filesystem::u8path(Plat_GetGameDirectory()) / "csgo/bin/win64/server.dll";
	HMODULE serverModule = GetModuleHandleW(serverPath.c_str());
	if (!serverModule)
	{
		V_snprintf(error, maxlen, "KZLadderOff: game server module unavailable");
		return false;
	}
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetServerFactory, game, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetServerFactory, clients, ISource2GameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, schemas, CSchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, events, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, messages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	controllerPawn = FindField(schemas, "CCSPlayerController", "m_hPlayerPawn");
	lifeState = FindField(schemas, "CBaseEntity", "m_lifeState");
	teamNumber = FindField(schemas, "CBaseEntity", "m_iTeamNum");
	if (controllerPawn.offset < 0 || lifeState.offset < 0 || teamNumber.offset < 0)
	{
		V_snprintf(error, maxlen, "KZLadderOff: required player schema is missing");
		return false;
	}
	if (!ladder::Init(schemas, serverModule, error, maxlen))
	{
		return false;
	}
	textMessage = messages->FindNetworkMessagePartial("TextMsg");
	preferenceDirectory = std::filesystem::u8path(Plat_GetGameDirectory()) / "csgo/addons/kzladderoff/data";
	if (!hud::Init(engine, events, messages, serverModule))
	{
		V_snprintf(error, maxlen, "KZLadderOff: native HUD message unavailable");
		return false;
	}
	ismm->AddListener(this, this);
	gameFrameHook.Add(game);
	dispatchHook.Add(g_pCVar);
	disconnectHook.Add(clients);
	activeHook.Add(clients);
	running = true;
	Msg("[KZLadderOff] Loaded candidate detector; verify 0off behavior on additional ladders.\n");
	return true;
}

bool Plugin::Unload(char *, size_t)
{
	running = false;
	gameFrameHook.Remove(game);
	dispatchHook.Remove(g_pCVar);
	disconnectHook.Remove(clients);
	activeHook.Remove(clients);
	for (int slot = 0; slot < 64; ++slot)
	{
		Reset(slot, true);
	}
	hud::Cleanup();
	return true;
}

bool Plugin::Pause(char *, size_t)
{
	paused = true;
	for (int slot = 0; slot < 64; ++slot)
	{
		Hide(slot);
		ladder::Reset(players[slot].ladderState);
	}
	return true;
}

bool Plugin::Unpause(char *, size_t)
{
	paused = false;
	return true;
}

void Plugin::OnLevelShutdown()
{
	for (int slot = 0; slot < 64; ++slot)
	{
		Hide(slot);
		ladder::Reset(players[slot].ladderState);
		players[slot].pawnHandle = 0;
	}
}

void Plugin::OnLevelInit(const char *, const char *, const char *, const char *, bool, bool)
{
	for (auto &player : players)
	{
		ladder::Reset(player.ladderState);
		player.pawnHandle = 0;
	}
}

void Plugin::Hide(int slot)
{
	auto &player = players[slot];
	if (player.hudVisible)
	{
		hud::Clear(slot);
		player.hudVisible = false;
	}
}

void Plugin::Reset(int slot, bool notifyClient)
{
	auto &player = players[slot];
	if (player.dirty && player.steamId && !preferences::Save(preferenceDirectory, player.steamId, player.enabled))
	{
		Warning("[KZLadderOff] Could not save setting for %llu\n", static_cast<unsigned long long>(player.steamId));
	}
	// Clear the transport's per-slot display state even after the client left.
	// Its Connected check avoids sending to a disconnected slot.
	if (notifyClient || player.hudVisible)
	{
		hud::Clear(slot);
	}
	player = Player {};
}

CEntityInstance *Plugin::Pawn(int slot) const
{
	auto entities = GameEntitySystem();
	auto controller = entities ? entities->GetEntityInstance(CEntityIndex(slot + 1)) : nullptr;
	if (!controller || std::strcmp(controller->GetClassname(), "cs_player_controller"))
	{
		return nullptr;
	}
	auto handle = controllerPawn.At<CEntityHandle>(controller);
	auto pawn = handle ? handle->Get() : nullptr;
	return pawn && !std::strcmp(pawn->GetClassname(), "player") ? pawn : nullptr;
}

bool Plugin::Alive(CEntityInstance *pawn) const
{
	auto life = lifeState.At<std::uint8_t>(pawn);
	auto team = teamNumber.At<int>(pawn);
	return life && *life == 0 && team && (*team == 2 || *team == 3);
}

void Plugin::Identity(int slot, double now)
{
	auto &player = players[slot];
	CPlayerSlot playerSlot(slot);
	if (!engine->IsClientFullyAuthenticated(playerSlot))
	{
		return;
	}
	auto id = engine->GetClientSteamID(playerSlot);
	std::uint64_t steamId = id ? id->ConvertToUint64() : 0;
	if (!steamId)
	{
		return;
	}
	if (player.steamId && player.steamId != steamId)
	{
		Reset(slot, true);
	}
	if (!player.loaded)
	{
		player.steamId = steamId;
		player.loaded = true;
		if (!player.dirty)
		{
			player.enabled = preferences::Load(preferenceDirectory, steamId);
		}
	}
	if (player.dirty && now >= player.nextSave)
	{
		if (preferences::Save(preferenceDirectory, steamId, player.enabled))
		{
			player.dirty = false;
		}
		player.nextSave = now + 5.0;
	}
}

void Plugin::Frame()
{
	if (!running || paused || !GameEntitySystem())
	{
		return;
	}
	auto globals = engine->GetServerGlobals();
	if (!globals)
	{
		return;
	}
	double now = globals->curtime;
	for (int slot = 0; slot < 64; ++slot)
	{
		auto &player = players[slot];
		if (player.loaded && !player.enabled && !player.dirty)
		{
			continue;
		}
		auto pawn = Pawn(slot);
		if (!pawn)
		{
			if (player.pawnHandle)
			{
				Hide(slot);
				ladder::Reset(player.ladderState);
				player.pawnHandle = 0;
			}
			continue;
		}
		Identity(slot, now);
		std::uint32_t handle = pawn->GetRefEHandle().ToPackedInt();
		if (player.pawnHandle != handle)
		{
			Hide(slot);
			ladder::Reset(player.ladderState);
			player.pawnHandle = handle;
		}
		if (!player.enabled || !Alive(pawn))
		{
			Hide(slot);
			ladder::Reset(player.ladderState);
			continue;
		}
		auto result = ladder::Sample(player.ladderState, pawn, now);
		if (result == ladder::Result::Unknown)
		{
			Hide(slot);
			continue;
		}
		bool zero = result == ladder::Result::Zero;
		if (!player.hudVisible || player.hudZero != zero || now >= player.nextHud)
		{
			hud::Show(slot, zero);
			player.hudVisible = true;
			player.hudZero = zero;
			player.nextHud = now + 0.5;
		}
	}
}

void Plugin::Chat(int slot, const char *text)
{
	if (!textMessage)
	{
		textMessage = messages->FindNetworkMessagePartial("TextMsg");
		if (!textMessage) return;
	}
	auto *raw = textMessage->AllocateMessage();
	if (!raw)
	{
		return;
	}
	auto *message = raw->ToPB<CUserMessageTextMsg>();
	message->set_dest(HUD_PRINTTALK);
	message->add_param(text);
	CRecipientFilter recipient;
	recipient.AddRecipient(CPlayerSlot(slot));
	events->PostEventAbstract(0, false, &recipient, textMessage, message, 0);
	messages->DeallocateNetMessageAbstract(textMessage, message);
}

void Plugin::Toggle(int slot)
{
	if (slot < 0 || slot >= 64 || !Pawn(slot))
	{
		return;
	}
	auto &player = players[slot];
	player.enabled = !player.enabled;
	player.dirty = true;
	player.nextSave = 0;
	if (!player.enabled)
	{
		Hide(slot);
		ladder::Reset(player.ladderState);
	}
	Chat(slot, player.enabled ? "梯子 0off 检测已开启" : "梯子 0off 检测已关闭");
	auto globals = engine->GetServerGlobals();
	Identity(slot, globals ? globals->curtime : 0);
}

KHook::Return<void> Plugin::FramePost(ISource2Server *, bool simulating, bool, bool)
{
	if (simulating)
	{
		Frame();
	}
	return {KHook::Action::Ignore};
}

KHook::Return<void> Plugin::DispatchPre(ICvar *, ConCommandRef, const CCommandContext &context, const CCommand &args)
{
	if (running && !paused && args.ArgC() >= 2 && (!V_stricmp(args[0], "say") || !V_stricmp(args[0], "say_team"))
		&& !V_stricmp(args[1], "!ladderoff"))
	{
		Toggle(context.GetPlayerSlot().Get());
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

KHook::Return<void> Plugin::DisconnectPre(ISource2GameClients *, CPlayerSlot slot, ENetworkDisconnectionReason, const char *, uint64, const char *)
{
	if (running && slot.Get() >= 0 && slot.Get() < 64)
	{
		Reset(slot.Get(), false);
	}
	return {KHook::Action::Ignore};
}

KHook::Return<void> Plugin::ActivePost(ISource2GameClients *, CPlayerSlot slot, bool, const char *, uint64)
{
	if (running && slot.Get() >= 0 && slot.Get() < 64)
	{
		Reset(slot.Get(), false);
	}
	return {KHook::Action::Ignore};
}
}

ladderoff::Plugin plugin;
PLUGIN_EXPOSE(ladderoff::Plugin, plugin);
