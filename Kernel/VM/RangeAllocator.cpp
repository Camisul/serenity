/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/BinarySearch.h>
#include <AK/QuickSort.h>
#include <Kernel/Debug.h>
#include <Kernel/Random.h>
#include <Kernel/Thread.h>
#include <Kernel/VM/RangeAllocator.h>

#define VM_GUARD_PAGES

namespace Kernel {

RangeAllocator::RangeAllocator()
{
}

void RangeAllocator::initialize_with_range(VirtualAddress base, size_t size)
{
    m_total_range = { base, size };
    m_available_ranges.append({ base, size });
#if VRA_DEBUG
    ScopedSpinLock lock(m_lock);
    dump();
#endif
}

void RangeAllocator::initialize_from_parent(const RangeAllocator& parent_allocator)
{
    ScopedSpinLock lock(parent_allocator.m_lock);
    m_total_range = parent_allocator.m_total_range;
    m_available_ranges = parent_allocator.m_available_ranges;
}

RangeAllocator::~RangeAllocator()
{
}

void RangeAllocator::dump() const
{
    ASSERT(m_lock.is_locked());
    dbgln("RangeAllocator({})", this);
    for (auto& range : m_available_ranges) {
        dbgln("    {:x} -> {:x}", range.base().get(), range.end().get() - 1);
    }
}

Vector<Range, 2> Range::carve(const Range& taken)
{
    Vector<Range, 2> parts;
    if (taken == *this)
        return {};
    if (taken.base() > base())
        parts.append({ base(), taken.base().get() - base().get() });
    if (taken.end() < end())
        parts.append({ taken.end(), end().get() - taken.end().get() });

    if constexpr (VRA_DEBUG) {
        dbgln("VRA: carve: take {:x}-{:x} from {:x}-{:x}",
            taken.base().get(),
            taken.end().get() - 1,
            base().get(),
            end().get() - 1);

        for (size_t i = 0; i < parts.size(); ++i)
            dbgln("        {:x}-{:x}", parts[i].base().get(), parts[i].end().get() - 1);
    }

    return parts;
}

void RangeAllocator::carve_at_index(int index, const Range& range)
{
    ASSERT(m_lock.is_locked());
    auto remaining_parts = m_available_ranges[index].carve(range);
    ASSERT(remaining_parts.size() >= 1);
    m_available_ranges[index] = remaining_parts[0];
    if (remaining_parts.size() == 2)
        m_available_ranges.insert(index + 1, move(remaining_parts[1]));
}

Range RangeAllocator::allocate_anywhere(size_t size, size_t alignment)
{
    if (!size)
        return {};

#ifdef VM_GUARD_PAGES
    // NOTE: We pad VM allocations with a guard page on each side.
    size_t effective_size = size + PAGE_SIZE * 2;
    size_t offset_from_effective_base = PAGE_SIZE;
#else
    size_t effective_size = size;
    size_t offset_from_effective_base = 0;
#endif

    ScopedSpinLock lock(m_lock);
    for (size_t i = 0; i < m_available_ranges.size(); ++i) {
        auto& available_range = m_available_ranges[i];
        // FIXME: This check is probably excluding some valid candidates when using a large alignment.
        if (available_range.size() < (effective_size + alignment))
            continue;

        FlatPtr initial_base = available_range.base().offset(offset_from_effective_base).get();
        FlatPtr aligned_base = round_up_to_power_of_two(initial_base, alignment);

        Range allocated_range(VirtualAddress(aligned_base), size);
        if (available_range == allocated_range) {
            dbgln<VRA_DEBUG>("VRA: Allocated perfect-fit anywhere({}, {}): {}", size, alignment, allocated_range.base().get());
            m_available_ranges.remove(i);
            return allocated_range;
        }
        carve_at_index(i, allocated_range);
        if constexpr (VRA_DEBUG) {
            dbgln<VRA_DEBUG>("VRA: Allocated anywhere({}, {}): {}", size, alignment, allocated_range.base().get());
            dump();
        }
        return allocated_range;
    }
    klog() << "VRA: Failed to allocate anywhere: " << size << ", " << alignment;
    return {};
}

Range RangeAllocator::allocate_specific(VirtualAddress base, size_t size)
{
    if (!size)
        return {};

    Range allocated_range(base, size);
    ScopedSpinLock lock(m_lock);
    for (size_t i = 0; i < m_available_ranges.size(); ++i) {
        auto& available_range = m_available_ranges[i];
        if (!available_range.contains(base, size))
            continue;
        if (available_range == allocated_range) {
            m_available_ranges.remove(i);
            return allocated_range;
        }
        carve_at_index(i, allocated_range);

        if constexpr (VRA_DEBUG) {
            dbgln("VRA: Allocated specific({}): {}", size, available_range.base().get());
            dump();
        }

        return allocated_range;
    }
    dbgln("VRA: Failed to allocate specific range: {}({})", base, size);
    return {};
}

void RangeAllocator::deallocate(Range range)
{
    ScopedSpinLock lock(m_lock);
    ASSERT(m_total_range.contains(range));
    ASSERT(range.size());
    ASSERT(range.base() < range.end());

    if constexpr (VRA_DEBUG) {
        dbgln("VRA: Deallocate: {}({})", range.base().get(), range.size());
        dump();
    }

    ASSERT(!m_available_ranges.is_empty());

    size_t nearby_index = 0;
    auto* existing_range = binary_search(
        m_available_ranges.span(),
        range,
        &nearby_index,
        [](auto& a, auto& b) { return a.base().get() - b.end().get(); });

    size_t inserted_index = 0;
    if (existing_range) {
        existing_range->m_size += range.size();
        inserted_index = nearby_index;
    } else {
        m_available_ranges.insert_before_matching(
            Range(range), [&](auto& entry) {
                return entry.base() >= range.end();
            },
            nearby_index, &inserted_index);
    }

    if (inserted_index < (m_available_ranges.size() - 1)) {
        // We already merged with previous. Try to merge with next.
        auto& inserted_range = m_available_ranges[inserted_index];
        auto& next_range = m_available_ranges[inserted_index + 1];
        if (inserted_range.end() == next_range.base()) {
            inserted_range.m_size += next_range.size();
            m_available_ranges.remove(inserted_index + 1);
            return;
        }
    }
#if VRA_DEBUG
    dbgln("VRA: After deallocate");
    dump();
#endif
}

}
