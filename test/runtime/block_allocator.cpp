#include "common.h"

#include "internal/block_allocator.h"
#include "internal/pointer_table.h"

using namespace Halide::Runtime::Internal;

namespace {

size_t allocated_region_memory = 0;
size_t allocated_block_memory = 0;

int allocate_block(void *user_context, MemoryBlock *block) {
    block->handle = allocate_system(user_context, block->size);
    allocated_block_memory += block->size;

    debug(user_context) << "Test : allocate_block ("
                        << "block=" << (void *)(block) << " "
                        << "block_size=" << int32_t(block->size) << " "
                        << "allocated_block_memory=" << int32_t(allocated_block_memory) << " "
                        << ") !\n";

    return halide_error_code_success;
}

int deallocate_block(void *user_context, MemoryBlock *block) {
    deallocate_system(user_context, block->handle);
    allocated_block_memory -= block->size;

    debug(user_context) << "Test : deallocate_block ("
                        << "block=" << (void *)(block) << " "
                        << "block_size=" << int32_t(block->size) << " "
                        << "allocated_block_memory=" << int32_t(allocated_block_memory) << " "
                        << ") !\n";

    return halide_error_code_success;
}

int allocate_region(void *user_context, MemoryRegion *region) {
    region->handle = (void *)1;
    allocated_region_memory += region->size;

    debug(user_context) << "Test : allocate_region ("
                        << "region=" << (void *)(region) << " "
                        << "region_size=" << int32_t(region->size) << " "
                        << "allocated_region_memory=" << int32_t(allocated_region_memory) << " "
                        << ") !\n";

    return halide_error_code_success;
}

int deallocate_region(void *user_context, MemoryRegion *region) {
    region->handle = (void *)0;
    allocated_region_memory -= region->size;

    debug(user_context) << "Test : deallocate_region ("
                        << "region=" << (void *)(region) << " "
                        << "region_size=" << int32_t(region->size) << " "
                        << "allocated_region_memory=" << int32_t(allocated_region_memory) << " "
                        << ") !\n";

    return halide_error_code_success;
}

}  // end namespace

int main(int argc, char **argv) {
    void *user_context = (void *)1;

    SystemMemoryAllocatorFns system_allocator = {allocate_system, deallocate_system};
    MemoryBlockAllocatorFns block_allocator = {allocate_block, deallocate_block};
    MemoryRegionAllocatorFns region_allocator = {allocate_region, deallocate_region};

    // test class interface
    {
        BlockAllocator::Config config = {0};
        config.minimum_block_size = 1024;

        BlockAllocator::MemoryAllocators allocators = {system_allocator, block_allocator, region_allocator};
        BlockAllocator *instance = BlockAllocator::create(user_context, config, allocators);

        MemoryRequest request = {0};
        request.size = sizeof(int);
        request.alignment = sizeof(int);
        request.properties.visibility = MemoryVisibility::DefaultVisibility;
        request.properties.caching = MemoryCaching::DefaultCaching;
        request.properties.usage = MemoryUsage::DefaultUsage;

        MemoryRegion *r1 = instance->reserve(user_context, request);
        halide_abort_if_false(user_context, r1 != nullptr);
        halide_abort_if_false(user_context, allocated_block_memory == config.minimum_block_size);
        halide_abort_if_false(user_context, allocated_region_memory == request.size);

        MemoryRegion *r2 = instance->reserve(user_context, request);
        halide_abort_if_false(user_context, r2 != nullptr);
        halide_abort_if_false(user_context, allocated_block_memory == config.minimum_block_size);
        halide_abort_if_false(user_context, allocated_region_memory == (2 * request.size));

        instance->reclaim(user_context, r1);
        halide_abort_if_false(user_context, allocated_region_memory == (1 * request.size));

        MemoryRegion *r3 = instance->reserve(user_context, request);
        halide_abort_if_false(user_context, r3 != nullptr);
        halide_abort_if_false(user_context, allocated_block_memory == config.minimum_block_size);
        halide_abort_if_false(user_context, allocated_region_memory == (2 * request.size));
        instance->retain(user_context, r3);
        halide_abort_if_false(user_context, allocated_region_memory == (2 * request.size));
        instance->release(user_context, r3);
        halide_abort_if_false(user_context, allocated_region_memory == (2 * request.size));
        instance->reclaim(user_context, r3);

        instance->destroy(user_context);
        debug(user_context) << "Test : block_allocator::destroy ("
                            << "allocated_block_memory=" << int32_t(allocated_block_memory) << " "
                            << "allocated_region_memory=" << int32_t(allocated_region_memory) << " "
                            << ") !\n";

        halide_abort_if_false(user_context, allocated_block_memory == 0);
        halide_abort_if_false(user_context, allocated_region_memory == 0);

        BlockAllocator::destroy(user_context, instance);

        debug(user_context) << "Test : block_allocator::destroy ("
                            << "allocated_system_memory=" << int32_t(allocated_system_memory) << " "
                            << ") !\n";

        halide_abort_if_false(user_context, allocated_system_memory == 0);
    }

    // allocation stress test
    {
        BlockAllocator::Config config = {0};
        config.minimum_block_size = 1024;

        BlockAllocator::MemoryAllocators allocators = {system_allocator, block_allocator, region_allocator};
        BlockAllocator *instance = BlockAllocator::create(user_context, config, allocators);

        MemoryRequest request = {0};
        request.size = sizeof(int);
        request.alignment = sizeof(int);
        request.properties.visibility = MemoryVisibility::DefaultVisibility;
        request.properties.caching = MemoryCaching::DefaultCaching;
        request.properties.usage = MemoryUsage::DefaultUsage;

        static size_t test_allocations = 1000;
        PointerTable pointers(user_context, test_allocations, system_allocator);
        for (size_t n = 0; n < test_allocations; ++n) {
            size_t count = n % 32;
            count = count > 1 ? count : 1;
            request.size = count * sizeof(int);
            MemoryRegion *region = instance->reserve(user_context, request);
            pointers.append(user_context, region);
        }

        for (size_t n = 0; n < pointers.size(); ++n) {
            MemoryRegion *region = static_cast<MemoryRegion *>(pointers[n]);
            instance->reclaim(user_context, region);
        }
        halide_abort_if_false(user_context, allocated_region_memory == 0);

        pointers.destroy(user_context);
        instance->destroy(user_context);
        halide_abort_if_false(user_context, allocated_block_memory == 0);

        BlockAllocator::destroy(user_context, instance);
        halide_abort_if_false(user_context, allocated_system_memory == 0);
    }

    // reuse stress test
    {
        BlockAllocator::Config config = {0};
        config.minimum_block_size = 1024;

        BlockAllocator::MemoryAllocators allocators = {system_allocator, block_allocator, region_allocator};
        BlockAllocator *instance = BlockAllocator::create(user_context, config, allocators);

        MemoryRequest request = {0};
        request.size = sizeof(int);
        request.alignment = sizeof(int);
        request.properties.visibility = MemoryVisibility::DefaultVisibility;
        request.properties.caching = MemoryCaching::DefaultCaching;
        request.properties.usage = MemoryUsage::DefaultUsage;

        size_t total_allocation_size = 0;
        static size_t test_allocations = 1000;
        PointerTable pointers(user_context, test_allocations, system_allocator);
        for (size_t n = 0; n < test_allocations; ++n) {
            size_t count = n % 32;
            count = count > 1 ? count : 1;
            request.size = count * sizeof(int);
            total_allocation_size += request.size;
            MemoryRegion *region = instance->reserve(user_context, request);
            pointers.append(user_context, region);
        }

        for (size_t n = 0; n < pointers.size(); ++n) {
            MemoryRegion *region = static_cast<MemoryRegion *>(pointers[n]);
            instance->release(user_context, region);  // release but don't destroy
        }
        pointers.clear(user_context);
        halide_abort_if_false(user_context, allocated_region_memory >= total_allocation_size);

        // reallocate and reuse
        for (size_t n = 0; n < test_allocations; ++n) {
            size_t count = n % 32;
            count = count > 1 ? count : 1;
            request.size = count * sizeof(int);
            MemoryRegion *region = instance->reserve(user_context, request);
            pointers.append(user_context, region);
        }

        pointers.destroy(user_context);
        instance->destroy(user_context);
        halide_abort_if_false(user_context, allocated_block_memory == 0);

        BlockAllocator::destroy(user_context, instance);
        halide_abort_if_false(user_context, allocated_system_memory == 0);
    }

    print(user_context) << "Success!\n";
    return 0;
}
