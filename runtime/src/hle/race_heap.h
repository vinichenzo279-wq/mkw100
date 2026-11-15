#pragma once

#include <cstdint>

struct CpuContext;

// race_heap.cpp: the race scene's heap fallback past 12 players.

// The current scene's EGG::ExpHeap* at a GameScene field (0xC88 structsMem1, 0xC8C structsMem2,
// 0xC90 structsDebug), 0 without a scene.
uint32_t RaceHeap_CurrentSceneHeap(uint32_t field);

// Free bytes the fallback still has behind the EGG::ExpHeap `egg` in races of more than 12 players
// (0 otherwise, and for heaps it does not cover), for pack code that checks free space before it
// allocates. Calls guest code through `ctx` and restores it.
uint32_t RaceHeap_FallbackFree(CpuContext* ctx, uint32_t egg);
