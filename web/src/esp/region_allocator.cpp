#include "region_allocator.h"

#include <cstring>

namespace cardenza_web::heap {

// Every block starts with its header; a free block keeps its list links in its payload. Sizes are
// whole blocks (header included), multiples of 8; bit 0 of `size` is "used". A block handed out
// with a larger alignment is preceded, inside its own block, by an 8-byte tag whose bit 1 is set and
// whose size field is the distance back to the real payload.
struct RegionAllocator::Header {
    uint32_t size;
    uint32_t previousSize;  // of the block before it in memory; 0 for the first
};

struct RegionAllocator::FreeLinks {
    Header* next;
    Header* previous;
};

namespace {
constexpr uint32_t kUsed = 1u;
constexpr uint32_t kAlignedTag = 2u;
constexpr uint32_t kFlags = 7u;
constexpr std::size_t kMinimumBlock = 8 + 2 * sizeof(void*) <= 16 ? 16 : 24;

std::size_t RoundUp(std::size_t value, std::size_t to) { return (value + to - 1) & ~(to - 1); }
}  // namespace

RegionAllocator::RegionAllocator(void* start, std::size_t bytes)
{
    uintptr_t begin = RoundUp(reinterpret_cast<uintptr_t>(start), 8);
    uintptr_t end = (reinterpret_cast<uintptr_t>(start) + bytes) & ~static_cast<uintptr_t>(7);
    start_ = begin;
    end_ = end;
    if (end <= begin + kMinimumBlock + kHeaderBytes) return;
    // One free block and a used sentinel header at the end, so Next() of the last block stops there.
    const std::size_t blockBytes = end - begin - kHeaderBytes;
    auto* block = reinterpret_cast<Header*>(begin);
    block->size = static_cast<uint32_t>(blockBytes);
    block->previousSize = 0;
    auto* sentinel = reinterpret_cast<Header*>(begin + blockBytes);
    sentinel->size = kUsed;
    sentinel->previousSize = static_cast<uint32_t>(blockBytes);
    total_ = free_ = minimumFree_ = blockBytes;
    Insert(block);
}

int RegionAllocator::ClassOf(std::size_t bytes)
{
    int index = 0;
    while (bytes > 1 && index < kClasses - 1)
    {
        bytes >>= 1;
        ++index;
    }
    return index;
}

RegionAllocator::Header* RegionAllocator::First() const { return reinterpret_cast<Header*>(start_); }

RegionAllocator::Header* RegionAllocator::Next(Header* block) const
{
    return reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(block) + (block->size & ~kFlags));
}

RegionAllocator::Header* RegionAllocator::Previous(Header* block) const
{
    if (block->previousSize == 0) return nullptr;
    return reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(block) - block->previousSize);
}

void RegionAllocator::Insert(Header* block)
{
    const int index = ClassOf(block->size & ~kFlags);
    auto* links = reinterpret_cast<FreeLinks*>(block + 1);
    links->previous = nullptr;
    links->next = lists_[index];
    if (lists_[index] != nullptr) reinterpret_cast<FreeLinks*>(lists_[index] + 1)->previous = block;
    lists_[index] = block;
    ++freeBlocks_;
}

void RegionAllocator::Remove(Header* block)
{
    const int index = ClassOf(block->size & ~kFlags);
    auto* links = reinterpret_cast<FreeLinks*>(block + 1);
    if (links->previous != nullptr) reinterpret_cast<FreeLinks*>(links->previous + 1)->next = links->next;
    else lists_[index] = links->next;
    if (links->next != nullptr) reinterpret_cast<FreeLinks*>(links->next + 1)->previous = links->previous;
    --freeBlocks_;
}

// Cuts `block` (free, not in a list) to `bytes`; the rest becomes a free block in its list.
RegionAllocator::Header* RegionAllocator::Split(Header* block, std::size_t bytes)
{
    const std::size_t size = block->size & ~kFlags;
    if (size < bytes + kMinimumBlock) return block;
    auto* rest = reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(block) + bytes);
    rest->size = static_cast<uint32_t>(size - bytes);
    rest->previousSize = static_cast<uint32_t>(bytes);
    block->size = static_cast<uint32_t>(bytes) | (block->size & kFlags);
    Next(rest)->previousSize = rest->size & ~kFlags;
    // Free neighbours are always merged: the block after a free block is used, so the rest stands alone.
    Insert(rest);
    return block;
}

// Merges a free block (not in a list) with free neighbours; returns the merged block (not in a list).
RegionAllocator::Header* RegionAllocator::Merge(Header* block)
{
    Header* next = Next(block);
    if ((next->size & kUsed) == 0)
    {
        Remove(next);
        block->size += next->size;
        Next(block)->previousSize = block->size & ~kFlags;
    }
    Header* previous = Previous(block);
    if (previous != nullptr && (previous->size & kUsed) == 0)
    {
        Remove(previous);
        previous->size += block->size & ~kFlags;
        Next(previous)->previousSize = previous->size & ~kFlags;
        block = previous;
    }
    return block;
}

void RegionAllocator::Used(std::size_t bytes)
{
    free_ -= bytes;
    if (free_ < minimumFree_) minimumFree_ = free_;
}

void* RegionAllocator::Allocate(std::size_t bytes, std::size_t alignment)
{
    if (total_ == 0) return nullptr;
    if (alignment < 8) alignment = 8;
    if (bytes == 0) bytes = 1;
    if (bytes > total_) return nullptr;
    const std::size_t extra = alignment > 8 ? alignment + kHeaderBytes : 0;
    std::size_t need = RoundUp(bytes, 8) + kHeaderBytes + extra;
    if (need < kMinimumBlock) need = kMinimumBlock;
    Header* found = nullptr;
    for (int index = ClassOf(need); index < kClasses && found == nullptr; ++index)
    {
        for (Header* block = lists_[index]; block != nullptr; block = reinterpret_cast<FreeLinks*>(block + 1)->next)
        {
            if ((block->size & ~kFlags) >= need)
            {
                found = block;
                break;
            }
        }
    }
    if (found == nullptr) return nullptr;
    Remove(found);
    Split(found, need);
    found->size |= kUsed;
    Used(found->size & ~kFlags);
    ++allocatedBlocks_;
    uintptr_t payload = reinterpret_cast<uintptr_t>(found + 1);
    if (alignment > 8)
    {
        uintptr_t aligned = RoundUp(payload + kHeaderBytes, alignment);
        auto* tag = reinterpret_cast<Header*>(aligned - kHeaderBytes);
        tag->size = static_cast<uint32_t>(aligned - payload) | kAlignedTag | kUsed;
        tag->previousSize = 0;
        payload = aligned;
    }
    return reinterpret_cast<void*>(payload);
}

void RegionAllocator::Free(void* pointer)
{
    if (pointer == nullptr || !Owns(pointer)) return;
    uintptr_t payload = reinterpret_cast<uintptr_t>(pointer);
    auto* tag = reinterpret_cast<Header*>(payload - kHeaderBytes);
    if ((tag->size & kAlignedTag) != 0) payload -= tag->size & ~kFlags;
    auto* block = reinterpret_cast<Header*>(payload - kHeaderBytes);
    if ((block->size & kUsed) == 0) return;  // freed twice
    block->size &= ~kFlags;
    free_ += block->size;
    --allocatedBlocks_;
    Insert(Merge(block));
}

void* RegionAllocator::Reallocate(void* pointer, std::size_t bytes)
{
    if (pointer == nullptr) return Allocate(bytes);
    if (bytes == 0)
    {
        Free(pointer);
        return nullptr;
    }
    const std::size_t have = UsableSize(pointer);
    auto* tag = reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(pointer) - kHeaderBytes);
    const bool aligned = (tag->size & kAlignedTag) != 0;
    if (!aligned)
    {
        auto* block = tag;
        const std::size_t need = RoundUp(bytes, 8) + kHeaderBytes < kMinimumBlock ? kMinimumBlock : RoundUp(bytes, 8) + kHeaderBytes;
        const std::size_t size = block->size & ~kFlags;
        if (need <= size)
        {
            // Shrink: give the tail back.
            if (size >= need + kMinimumBlock)
            {
                block->size = static_cast<uint32_t>(need) | kUsed;
                auto* rest = reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(block) + need);
                rest->size = static_cast<uint32_t>(size - need);
                rest->previousSize = static_cast<uint32_t>(need);
                Next(rest)->previousSize = rest->size;
                free_ += rest->size;
                Insert(Merge(rest));
            }
            return pointer;
        }
        Header* next = Next(block);
        if ((next->size & kUsed) == 0 && size + next->size >= need)
        {
            Remove(next);
            const std::size_t merged = size + next->size;
            Used(next->size);
            block->size = static_cast<uint32_t>(merged) | kUsed;
            Next(block)->previousSize = static_cast<uint32_t>(merged);
            if (merged >= need + kMinimumBlock)
            {
                block->size = static_cast<uint32_t>(need) | kUsed;
                auto* rest = reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(block) + need);
                rest->size = static_cast<uint32_t>(merged - need);
                rest->previousSize = static_cast<uint32_t>(need);
                Next(rest)->previousSize = rest->size;
                free_ += rest->size;
                Insert(rest);
            }
            return pointer;
        }
    }
    void* moved = Allocate(bytes);
    if (moved == nullptr) return nullptr;
    std::memcpy(moved, pointer, have < bytes ? have : bytes);
    Free(pointer);
    return moved;
}

bool RegionAllocator::Owns(const void* pointer) const
{
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return address > start_ && address < end_;
}

std::size_t RegionAllocator::UsableSize(const void* pointer) const
{
    if (pointer == nullptr || !Owns(pointer)) return 0;
    uintptr_t payload = reinterpret_cast<uintptr_t>(pointer);
    const auto* tag = reinterpret_cast<const Header*>(payload - kHeaderBytes);
    std::size_t offset = 0;
    if ((tag->size & kAlignedTag) != 0)
    {
        offset = tag->size & ~kFlags;
        payload -= offset;
    }
    const auto* block = reinterpret_cast<const Header*>(payload - kHeaderBytes);
    return (block->size & ~kFlags) - kHeaderBytes - offset;
}

std::size_t RegionAllocator::LargestFreeBlock() const
{
    for (int index = kClasses - 1; index >= 0; --index)
    {
        if (lists_[index] == nullptr) continue;
        std::size_t largest = 0;
        for (Header* block = lists_[index]; block != nullptr; block = reinterpret_cast<FreeLinks*>(block + 1)->next)
            if ((block->size & ~kFlags) > largest) largest = block->size & ~kFlags;
        return largest > kHeaderBytes ? largest - kHeaderBytes : 0;
    }
    return 0;
}

bool RegionAllocator::Check() const
{
    if (total_ == 0) return true;
    Header* block = First();
    uint32_t previous = 0;
    std::size_t seen = 0;
    while (reinterpret_cast<uintptr_t>(block) < start_ + total_)
    {
        const std::size_t size = block->size & ~kFlags;
        if (size < kMinimumBlock || (size & 7) != 0 || block->previousSize != previous) return false;
        seen += size;
        if (seen > total_) return false;
        previous = static_cast<uint32_t>(size);
        block = Next(block);
    }
    return seen == total_ && block->size == kUsed;
}

}  // namespace cardenza_web::heap
