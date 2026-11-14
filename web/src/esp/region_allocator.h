#pragma once

// An allocator over one region of memory, like one of ESP-IDF's heaps (TLSF there): segregated
// free lists by size class, first fit within a class, boundary tags to merge neighbours on free.
// It gives the web target's internal RAM and PSRAM their real limits, fragmentation and largest
// free blocks. Pure: tested on the host (tests/web).

#include <cstddef>
#include <cstdint>

namespace cardenza_web::heap {

class RegionAllocator {
public:
    // [start, start + bytes) must stay valid while the allocator is used.
    RegionAllocator(void* start, std::size_t bytes);

    void* Allocate(std::size_t bytes, std::size_t alignment = 8);
    void Free(void* block);
    // Grows or shrinks in place when it can, else moves; null (and the old block kept) when it cannot.
    void* Reallocate(void* block, std::size_t bytes);
    bool Owns(const void* pointer) const;
    std::size_t UsableSize(const void* block) const;

    std::size_t TotalBytes() const { return total_; }
    std::size_t FreeBytes() const { return free_; }
    std::size_t MinimumFreeBytes() const { return minimumFree_; }
    std::size_t LargestFreeBlock() const;
    std::size_t AllocatedBlocks() const { return allocatedBlocks_; }
    std::size_t FreeBlocks() const { return freeBlocks_; }
    std::size_t AllocatedBytes() const { return total_ - free_; }
    // Walks every block: false when a header is damaged (a write past the end of a block).
    bool Check() const;
    uintptr_t Start() const { return start_; }
    uintptr_t End() const { return end_; }

    static constexpr std::size_t kHeaderBytes = 8;

private:
    struct Header;
    struct FreeLinks;
    static constexpr int kClasses = 32;
    static int ClassOf(std::size_t bytes);
    Header* First() const;
    Header* Next(Header* block) const;
    Header* Previous(Header* block) const;
    void Insert(Header* block);
    void Remove(Header* block);
    Header* Split(Header* block, std::size_t bytes);
    Header* Merge(Header* block);
    void Used(std::size_t bytes);

    uintptr_t start_ = 0;
    uintptr_t end_ = 0;
    std::size_t total_ = 0;
    std::size_t free_ = 0;
    std::size_t minimumFree_ = 0;
    std::size_t allocatedBlocks_ = 0;
    std::size_t freeBlocks_ = 0;
    Header* lists_[kClasses] = {};
};

}  // namespace cardenza_web::heap
