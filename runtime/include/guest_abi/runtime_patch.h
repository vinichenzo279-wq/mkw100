#pragma once

#include <cstdint>

// mkw100: game instructions a mod rewrites while running (translator Mods.RuntimePatchSites,
// Insane Kart Wii). Translated code at such a site reads the word guest memory holds there and runs
// the variant compiled for it; recomp_mod_loader.cpp keeps the words the mod may write there.
extern "C" uint32_t MKW100_RuntimeWord(uint32_t address);
// After a call into a module hook that returns to LR + K (translator Mods.LrAdjustingHooks): the
// LR it left when that lies inside the calling function [start, end), else 0 (the hook returned
// out of it).
extern "C" uint32_t MKW100_LocalReturn(uint32_t lr, uint32_t start, uint32_t end);
// The immediate of the instruction at a run-time immediate site, sign- or zero-extended.
extern "C" uint32_t MKW100_RuntimeImmS(uint32_t address);
extern "C" uint32_t MKW100_RuntimeImmU(uint32_t address);
