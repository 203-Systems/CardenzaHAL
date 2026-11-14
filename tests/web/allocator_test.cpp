// The web target's RAM regions (web/src/esp/region_allocator.h): random allocations, frees and
// reallocations with alignments, checked block by block; everything returns to one free block.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "region_allocator.h"

using cardenza_web::heap::RegionAllocator;

int main()
{
    static unsigned char memory[300 * 1024];
    RegionAllocator allocator(memory, sizeof(memory));
    const size_t total = allocator.FreeBytes();
    assert(allocator.LargestFreeBlock() + RegionAllocator::kHeaderBytes == total);
    std::mt19937 random(1);
    std::vector<std::pair<void*, size_t>> live;
    for (int step = 0; step < 200000; ++step)
    {
        if (live.empty() || random() % 3 != 0)
        {
            const size_t bytes = random() % 2000 + 1;
            const size_t alignment = random() % 5 == 0 ? (16u << (random() % 5)) : 8;
            void* block = allocator.Allocate(bytes, alignment);
            if (block == nullptr) continue;
            assert(reinterpret_cast<uintptr_t>(block) % alignment == 0);
            assert(allocator.UsableSize(block) >= bytes);
            std::memset(block, 0xAB, bytes);
            live.push_back({block, bytes});
        }
        else
        {
            const size_t index = random() % live.size();
            if (random() % 4 == 0)
            {
                const size_t bytes = random() % 3000 + 1;
                void* moved = allocator.Reallocate(live[index].first, bytes);
                if (moved != nullptr)
                {
                    live[index] = {moved, bytes};
                    std::memset(moved, 0xCD, bytes);
                }
            }
            else
            {
                allocator.Free(live[index].first);
                live[index] = live.back();
                live.pop_back();
            }
        }
        if (step % 1000 == 0) assert(allocator.Check());
        assert(total - allocator.FreeBytes() == allocator.AllocatedBytes());
        assert(allocator.MinimumFreeBytes() <= allocator.FreeBytes());
    }
    for (auto& block : live) allocator.Free(block.first);
    assert(allocator.Check());
    assert(allocator.FreeBytes() == total && allocator.FreeBlocks() == 1);
    // A request larger than the region, and one that fits only after merging.
    assert(allocator.Allocate(total + 1) == nullptr);
    void* a = allocator.Allocate(total / 2);
    void* b = allocator.Allocate(total / 3);
    assert(a != nullptr && b != nullptr && allocator.Allocate(total / 2) == nullptr);
    allocator.Free(a);
    allocator.Free(b);
    assert(allocator.Allocate(total - 64) != nullptr);
    std::printf("allocator tests passed\n");
    return 0;
}
