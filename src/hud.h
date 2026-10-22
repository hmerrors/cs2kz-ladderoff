#pragma once

class IVEngineServer2;
class IGameEventSystem;
class INetworkMessages;

namespace ladderoff::hud
{
// Returns false only when the engine cannot send even the plain-text fallback.
bool Init(IVEngineServer2 *engine, IGameEventSystem *events, INetworkMessages *messages, void *serverModule);
bool HasColor();
void Show(int slot, bool zeroOff);
void Clear(int slot);
void Cleanup();
} // namespace ladderoff::hud
