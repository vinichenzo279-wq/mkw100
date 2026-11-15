#pragma once

// mkw100: Retro Rewind with Item Fever's items (experimental, docs/mkw100/rr-item-fever.md).

#include <cstdint>

struct CpuContext;

namespace RrItemFever {

// Called every Raceinfo::Update; does nothing unless MKW100_TEST_GIVE_ITEM is set.
void TestGiveItem(CpuContext* ctx, uint32_t raceinfo);

} // namespace RrItemFever
