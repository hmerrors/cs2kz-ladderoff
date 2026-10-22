#pragma once

#include <cstddef>
#include <cstdint>

#include "mathlib/vector.h"

class CEntityInstance;
class CSchemaSystem;

namespace ladder
{
enum class Result
{
	Unknown,
	Zero,
	NonZero
};

struct State
{
	CEntityInstance *pawn = nullptr;
	uint32_t pawnHandle = 0;
	void *shape = nullptr;
	Vector normal = {};
	Vector planePoint = {};
	Vector lastOrigin = {};
	Vector mins = {};
	Vector maxs = {};
	double lastSampleTime = 0.0;
	double nextProbeTime = 0.0;
	bool valid = false;
};

bool Init(CSchemaSystem *schema, void *serverModule, char *error, size_t errorLength);
void Reset(State &state);
Result Sample(State &state, CEntityInstance *pawn, double now);
} // namespace ladder
