#include "ladder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "bspflags.h"
#include "entity2/entityinstance.h"
#include "gametrace.h"
#include "schemasystem/schemasystem.h"

namespace ladder
{
namespace
{
struct Offsets
{
	int moveType = -1;
	int movementServices = -1;
	int ladderNormal = -1;
	int body = -1;
	int sceneNode = -1;
	int origin = -1;
	int collision = -1;
	int mins = -1;
	int maxs = -1;
	int solidType = -1;
	int attribute = -1;
	int interactsWith = -1;
	int hierarchyId = -1;
};

Offsets g_offsets;
void **g_navVtable = nullptr;
bool g_ready = false;
int g_navStage = 0;

// CNavPhysicsInterface's private ABI follows the CS2KZ server implementation
// (KZGlobalTeam/cs2kz-metamod, AGPL-3.0). The game/SDK expose no public Nav
// trace interface; reject an unknown vtable layout instead of guessing one.
using TraceLineFn = void (*)(void *, const Vector &, const Vector &, CTraceFilter *, CGameTrace *);
using TraceShapeFn = void (*)(void *, const Ray_t &, const Vector &, const Vector &, CTraceFilter *, CGameTrace *);

struct Section
{
	const uint8_t *begin = nullptr;
	size_t size = 0;
	bool Contains(const void *address, size_t bytes = 1) const
	{
		auto p = reinterpret_cast<uintptr_t>(address);
		auto first = reinterpret_cast<uintptr_t>(begin);
		return p >= first && p - first <= size && bytes <= size - (p - first);
	}
};

void Error(char *buffer, size_t length, const char *message)
{
	if (buffer && length)
	{
		std::snprintf(buffer, length, "%s", message);
	}
}

const uint8_t *FindBytes(Section section, const void *needle, size_t length, const uint8_t *after = nullptr)
{
	if (!section.begin || !length || length > section.size)
	{
		return nullptr;
	}
	const uint8_t *start = after ? after + 1 : section.begin;
	if (!section.Contains(start, length))
	{
		return nullptr;
	}
	for (const uint8_t *p = start; p <= section.begin + section.size - length; ++p)
	{
		if (std::memcmp(p, needle, length) == 0)
		{
			return p;
		}
	}
	return nullptr;
}

bool FindNavVtable(void *serverModule, void ***out)
{
	g_navStage = 1;
	auto *base = reinterpret_cast<const uint8_t *>(serverModule);
	if (!base || reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return false;
	}
	auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
	{
		return false;
	}
	auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
	{
		return false;
	}
	// The trace overload order in this game build differs from the order in
	// CS2KZ's header. Never call a private vtable against an untested build.
	if (nt->FileHeader.TimeDateStamp != 0x6ad7b41c || nt->OptionalHeader.SizeOfImage != 0x2572000)
	{
		g_navStage = 6;
		return false;
	}
	const size_t imageSize = nt->OptionalHeader.SizeOfImage;
	Section data, rdata, text;
	auto *headers = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		const auto &header = headers[i];
		const size_t size = header.Misc.VirtualSize;
		if (!size || header.VirtualAddress > imageSize || size > imageSize - header.VirtualAddress)
		{
			continue;
		}
		Section current {base + header.VirtualAddress, size};
		if (std::memcmp(header.Name, ".data", 5) == 0) data = current;
		if (std::memcmp(header.Name, ".rdata", 6) == 0) rdata = current;
		if (std::memcmp(header.Name, ".text", 5) == 0) text = current;
	}
	if (!data.begin || !rdata.begin || !text.begin)
	{
		return false;
	}
	g_navStage = 2;

	// MSVC x64 RTTI: TypeDescriptor name is 16 bytes after its start. A
	// CompleteObjectLocator contains the TypeDescriptor RVA at offset 12,
	// and the vtable's preceding pointer references that locator.
	constexpr char name[] = ".?AVCNavPhysicsInterface@@";
	const uint8_t *typeName = FindBytes(data, name, sizeof(name));
	if (!typeName || !data.Contains(typeName - 16, 16) || FindBytes(data, name, sizeof(name), typeName))
	{
		return false;
	}
	g_navStage = 3;
	const auto typeRva = static_cast<uint32_t>(typeName - 16 - base);
	const uint8_t *match = nullptr;
	void **found = nullptr;
	while ((match = FindBytes(rdata, &typeRva, sizeof(typeRva), match)))
	{
		if (!rdata.Contains(match - 12, 24)) continue;
		const uint8_t *locator = match - 12;
		uint32_t signature, offset, cdOffset, classRva, selfRva;
		std::memcpy(&signature, locator, 4);
		std::memcpy(&offset, locator + 4, 4);
		std::memcpy(&cdOffset, locator + 8, 4);
		std::memcpy(&classRva, locator + 16, 4);
		std::memcpy(&selfRva, locator + 20, 4);
		if (signature != 1 || offset != 0 || cdOffset != 0 || selfRva != locator - base || classRva >= imageSize)
		{
			continue;
		}
		g_navStage = 4;
		const uint8_t *reference = nullptr;
		while ((reference = FindBytes(rdata, &locator, sizeof(locator), reference)))
		{
			if (!rdata.Contains(reference + sizeof(void *), 5 * sizeof(void *))) continue;
			auto **candidate = reinterpret_cast<void **>(const_cast<uint8_t *>(reference + sizeof(void *)));
			bool valid = true;
			for (int slot = 0; slot <= 4; ++slot)
			{
				if (!text.Contains(candidate[slot])) valid = false;
			}
			if (!valid) continue;
			g_navStage = 5;
			if (found && found != candidate) return false;
			found = candidate;
		}
	}
	*out = found;
	return found != nullptr;
}

bool Resolve(CSchemaSystemTypeScope *scope, const char *className, const char *fieldName, size_t fieldSize, int &result)
{
	auto *klass = scope->FindDeclaredClass(className).Get();
	if (!klass || klass->m_nSize <= 0 || !klass->m_pFields) return false;
	for (unsigned i = 0; i < klass->m_nFieldCount; ++i)
	{
		const auto &field = klass->m_pFields[i];
		if (field.m_pszName && std::strcmp(field.m_pszName, fieldName) == 0)
		{
			const int offset = field.m_nSingleInheritanceOffset;
			if (offset < 0 || static_cast<size_t>(offset) + fieldSize > static_cast<size_t>(klass->m_nSize)) return false;
			result = offset;
			return true;
		}
	}
	return false;
}

template <typename T> T Read(const void *object, int offset)
{
	T value {};
	std::memcpy(&value, static_cast<const uint8_t *>(object) + offset, sizeof(value));
	return value;
}

bool Finite(const Vector &v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float Dot(const Vector &a, const Vector &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool UnitNormal(Vector &normal)
{
	if (!Finite(normal)) return false;
	const float lengthSquared = Dot(normal, normal);
	if (lengthSquared < 0.25f || lengthSquared > 2.25f) return false;
	normal *= 1.0f / std::sqrt(lengthSquared);
	return std::abs(normal.z) < 0.7f;
}

Vector Support(const Vector &normal, const Vector &mins, const Vector &maxs)
{
	return {normal.x >= 0.0f ? mins.x : maxs.x,
			normal.y >= 0.0f ? mins.y : maxs.y,
			normal.z >= 0.0f ? mins.z : maxs.z};
}

bool LadderHit(const CGameTrace &trace, const Vector &normal)
{
	if (trace.m_bStartInSolid || !std::isfinite(trace.m_flFraction) || trace.m_flFraction < 0.0f || trace.m_flFraction >= 1.0f
		|| !trace.m_hShape || !(trace.m_ShapeAttributes.m_nInteractsAs & CONTENTS_LADDER) || !Finite(trace.m_vHitNormal))
	{
		return false;
	}
	Vector hitNormal = trace.m_vHitNormal;
	return UnitNormal(hitNormal) && Dot(hitNormal, normal) >= 0.99f;
}

void TraceLine(const Vector &start, const Vector &end, CTraceFilter &filter, CGameTrace &trace)
{
	reinterpret_cast<TraceLineFn>(g_navVtable[1])(&g_navVtable, start, end, &filter, &trace);
}

void TraceShape(const Vector &mins, const Vector &maxs, const Vector &start, const Vector &end, CTraceFilter &filter, CGameTrace &trace)
{
	Ray_t ray(mins, maxs);
	reinterpret_cast<TraceShapeFn>(g_navVtable[3])(&g_navVtable, ray, start, end, &filter, &trace);
}

void SetFilter(CTraceFilter &filter)
{
	filter.EnableInteractsAsLayer(LAYER_INDEX_CONTENTS_PLAYER);
	filter.m_nObjectSetMask = RNQUERY_OBJECTS_ALL;
	filter.m_bHitSolid = true;
	filter.m_bHitSolidRequiresGenerateContacts = true;
	filter.m_bHitTrigger = false;
	filter.m_bShouldIgnoreDisabledPairs = true;
	filter.m_bIgnoreIfBothInteractWithHitboxes = false;
	filter.m_bForceHitEverything = false;
	filter.m_bUnknown = true;
}

bool PointOnLadder(CEntityInstance *pawn, uint16_t hierarchy, const Vector &origin, const Vector &mins, const Vector &maxs,
					  const Vector &normal, CGameTrace &trace)
{
	// Probe the middle of an axis-aligned face. A zero normal component must
	// use the hull center, otherwise the ray needlessly probes a side edge.
	Vector face {normal.x > 1e-4f ? mins.x : normal.x < -1e-4f ? maxs.x : (mins.x + maxs.x) * 0.5f,
			 normal.y > 1e-4f ? mins.y : normal.y < -1e-4f ? maxs.y : (mins.y + maxs.y) * 0.5f,
			 (mins.z + maxs.z) * 0.5f};
	Vector start = origin + face + normal * 0.5f;
	Vector end = start - normal * 20.0f;
	CTraceFilter filter(pawn, nullptr, hierarchy, CONTENTS_LADDER, COLLISION_GROUP_PLAYER, true);
	SetFilter(filter);
	TraceLine(start, end, filter, trace);
	return LadderHit(trace, normal);
}

double ULP(float x)
{
	if (!std::isfinite(x)) return std::numeric_limits<double>::infinity();
	return static_cast<double>(std::nextafterf(std::abs(x), std::numeric_limits<float>::infinity()) - std::abs(x));
}

double NumericalTolerance(const Vector &origin, const Vector &point, const Vector &normal)
{
	return std::max(0.0005, 2.0 * (std::abs(normal.x) * std::max(ULP(origin.x), ULP(point.x))
		+ std::abs(normal.y) * std::max(ULP(origin.y), ULP(point.y))
		+ std::abs(normal.z) * std::max(ULP(origin.z), ULP(point.z))));
}
} // namespace

bool Init(CSchemaSystem *schema, void *serverModule, char *error, size_t errorLength)
{
	g_ready = false;
	g_navVtable = nullptr;
	g_offsets = {};
	if (!schema)
	{
		Error(error, errorLength, "SchemaSystem unavailable");
		return false;
	}
	auto *scope = schema->FindTypeScopeForModule("server.dll");
	if (!scope
		|| !Resolve(scope, "CBaseEntity", "m_MoveType", sizeof(uint8_t), g_offsets.moveType)
		|| !Resolve(scope, "CBasePlayerPawn", "m_pMovementServices", sizeof(void *), g_offsets.movementServices)
		|| !Resolve(scope, "CCSPlayer_MovementServices", "m_vecLadderNormal", sizeof(Vector), g_offsets.ladderNormal)
		|| !Resolve(scope, "CBaseEntity", "m_CBodyComponent", sizeof(void *), g_offsets.body)
		|| !Resolve(scope, "CBodyComponent", "m_pSceneNode", sizeof(void *), g_offsets.sceneNode)
		|| !Resolve(scope, "CGameSceneNode", "m_vecAbsOrigin", sizeof(Vector), g_offsets.origin)
		|| !Resolve(scope, "CBaseEntity", "m_pCollision", sizeof(void *), g_offsets.collision)
		|| !Resolve(scope, "CCollisionProperty", "m_vecMins", sizeof(Vector), g_offsets.mins)
		|| !Resolve(scope, "CCollisionProperty", "m_vecMaxs", sizeof(Vector), g_offsets.maxs)
		|| !Resolve(scope, "CCollisionProperty", "m_nSolidType", sizeof(uint8_t), g_offsets.solidType)
		|| !Resolve(scope, "CCollisionProperty", "m_collisionAttribute", 1, g_offsets.attribute)
		|| !Resolve(scope, "VPhysicsCollisionAttribute_t", "m_nInteractsWith", sizeof(uint64_t), g_offsets.interactsWith)
		|| !Resolve(scope, "VPhysicsCollisionAttribute_t", "m_nHierarchyId", sizeof(uint16_t), g_offsets.hierarchyId))
	{
		Error(error, errorLength, "Required server.dll schema field unavailable");
		return false;
	}
	if (!FindNavVtable(serverModule, &g_navVtable))
	{
		if (g_navStage == 6)
		{
			Error(error, errorLength, "Unsupported CS2 server.dll build; trace ABI not verified");
		}
		else
		{
			std::snprintf(error, errorLength, "Validated CNavPhysicsInterface RTTI vtable unavailable (stage %d)", g_navStage);
		}
		return false;
	}
	g_ready = true;
	Error(error, errorLength, "");
	return true;
}

void Reset(State &state)
{
	state = {};
}

Result Sample(State &state, CEntityInstance *pawn, double now)
{
	if (!g_ready || !pawn || !pawn->m_pEntity || !std::isfinite(now))
	{
		Reset(state);
		return Result::Unknown;
	}
	const uint32_t handle = pawn->GetRefEHandle().ToInt();
	if (state.pawn != pawn || state.pawnHandle != handle)
	{
		Reset(state);
		state.pawn = pawn;
		state.pawnHandle = handle;
	}
	const uint8_t moveType = Read<uint8_t>(pawn, g_offsets.moveType);
	if (moveType != MOVETYPE_LADDER && moveType != MOVETYPE_WALK)
	{
		Reset(state);
		return Result::Unknown;
	}
	auto *movement = Read<void *>(pawn, g_offsets.movementServices);
	auto *body = Read<void *>(pawn, g_offsets.body);
	auto *collision = Read<void *>(pawn, g_offsets.collision);
	if (!movement || !body || !collision || Read<uint8_t>(collision, g_offsets.solidType) != SOLID_BBOX)
	{
		Reset(state);
		return Result::Unknown;
	}
	auto *node = Read<void *>(body, g_offsets.sceneNode);
	if (!node)
	{
		Reset(state);
		return Result::Unknown;
	}
	Vector origin = Read<Vector>(node, g_offsets.origin);
	Vector mins = Read<Vector>(collision, g_offsets.mins);
	Vector maxs = Read<Vector>(collision, g_offsets.maxs);
	if (!Finite(origin) || !Finite(mins) || !Finite(maxs) || maxs.x - mins.x < 16.0f || maxs.x - mins.x > 48.0f
		|| maxs.y - mins.y < 16.0f || maxs.y - mins.y > 48.0f || maxs.z - mins.z < 32.0f || maxs.z - mins.z > 90.0f)
	{
		Reset(state);
		return Result::Unknown;
	}
	const auto *attribute = static_cast<const uint8_t *>(collision) + g_offsets.attribute;
	const uint64_t interactsWith = Read<uint64_t>(attribute, g_offsets.interactsWith);
	const uint16_t hierarchy = Read<uint16_t>(attribute, g_offsets.hierarchyId);
	if (!interactsWith)
	{
		Reset(state);
		return Result::Unknown;
	}
	if (state.valid)
	{
		const float elapsed = static_cast<float>(std::max(0.0, now - state.lastSampleTime));
		const float permittedTravel = std::max(48.0f, 2000.0f * elapsed);
		if ((origin - state.lastOrigin).LengthSqr() > permittedTravel * permittedTravel
			|| (mins - state.mins).LengthSqr() > 1e-8f || (maxs - state.maxs).LengthSqr() > 1e-8f)
		{
			state.valid = false;
		}
	}
	Vector normal = state.normal;
	if (moveType == MOVETYPE_LADDER)
	{
		normal = Read<Vector>(movement, g_offsets.ladderNormal);
		if (!UnitNormal(normal))
		{
			state.valid = false;
			return Result::Unknown;
		}
		if (state.valid && Dot(normal, state.normal) < 0.99f) state.valid = false;
	}
	else if (!state.valid)
	{
		return Result::Unknown;
	}
	if (!state.valid)
	{
		if (moveType != MOVETYPE_LADDER || now < state.nextProbeTime) return Result::Unknown;
		CTraceFilter filter(pawn, nullptr, hierarchy, interactsWith | CONTENTS_LADDER, COLLISION_GROUP_PLAYER, true);
		SetFilter(filter);
		CGameTrace hit;
		TraceShape(mins, maxs, origin, origin - normal * 12.0f, filter, hit);
		if (!LadderHit(hit, normal))
		{
			state.nextProbeTime = now + 0.05;
			return Result::Unknown;
		}
		state.shape = hit.m_hShape;
		state.normal = normal;
		state.mins = mins;
		state.maxs = maxs;
		state.valid = true;
	}
	CGameTrace point;
	if (!PointOnLadder(pawn, hierarchy, origin, mins, maxs, state.normal, point)
		|| point.m_hShape != state.shape)
	{
		state.valid = false;
		state.nextProbeTime = now + 0.05;
		return Result::Unknown;
	}
	const Vector planePoint = point.m_bExactHitPoint ? point.m_vHitPoint : point.m_vEndPos;
	if (!Finite(planePoint))
	{
		state.valid = false;
		return Result::Unknown;
	}
	// AABB support in -normal, then signed distance to the observed ladder
	// plane. Point Trace is repeated while displaying to avoid a cached
	// infinite plane producing false positives after leaving the ladder edge.
	const Vector closest = origin + Support(state.normal, mins, maxs);
	const double gap = static_cast<double>(state.normal.x) * (closest.x - planePoint.x)
		+ static_cast<double>(state.normal.y) * (closest.y - planePoint.y)
		+ static_cast<double>(state.normal.z) * (closest.z - planePoint.z);
	const double tolerance = NumericalTolerance(origin, planePoint, state.normal);
	state.planePoint = planePoint;
	state.lastOrigin = origin;
	state.lastSampleTime = now;
	if (!std::isfinite(gap) || gap < -tolerance || gap > 12.0)
	{
		state.valid = false;
		return Result::Unknown;
	}
	// A confirmed 0off climb measured a 1/16-unit AABB-to-ladder gap:
	// the hull trace stops 1/32 unit from the surface, while the player
	// origin remains another 1/32 unit behind that position. Allow only
	// float rounding beyond the observed contact separation.
	constexpr double contactSeparation = 1.0 / 16.0;
	return gap <= contactSeparation + tolerance ? Result::Zero : Result::NonZero;
}
} // namespace ladder
