/* SPDX-License-Identifier: MIT */

#include "support/test_allocator.h"

#include <stdlib.h>

static void record_error(struct test_allocator *allocator, enum test_allocator_error error)
{
	if (allocator->error == TEST_ALLOCATOR_OK)
		allocator->error = error;
}

void test_allocator_set_fail_on_attempt(struct test_allocator *allocator, size_t attempt)
{
	allocator->fail_on_attempt = attempt;
}

void *test_allocator_allocate(struct test_allocator *allocator, size_t size)
{
	size_t available = TEST_ALLOCATOR_CAPACITY;
	void *memory;
	size_t index;

	++allocator->allocation_attempts;
	if (size > allocator->largest_requested_size)
		allocator->largest_requested_size = size;
	if (size == 0) {
		record_error(allocator, TEST_ALLOCATOR_ZERO_SIZE);
		return NULL;
	}
	if (allocator->allocation_attempts == allocator->fail_on_attempt)
		return NULL;

	for (index = 0; index < TEST_ALLOCATOR_CAPACITY; ++index) {
		if (!allocator->allocations[index].in_use) {
			available = index;
			break;
		}
	}
	if (available == TEST_ALLOCATOR_CAPACITY) {
		record_error(allocator, TEST_ALLOCATOR_REGISTRY_FULL);
		return NULL;
	}

	memory = malloc(size);
	if (memory == NULL)
		return NULL;
	allocator->allocations[available].pointer = memory;
	allocator->allocations[available].size = size;
	allocator->allocations[available].in_use = true;
	++allocator->successful_allocations;
	return memory;
}

void test_allocator_deallocate(struct test_allocator *allocator, void *memory, size_t size)
{
	size_t index;

	++allocator->deallocation_calls;
	for (index = 0; index < TEST_ALLOCATOR_CAPACITY; ++index) {
		struct test_allocation *allocation = &allocator->allocations[index];

		if (!allocation->in_use || allocation->pointer != memory)
			continue;
		if (allocation->size != size) {
			record_error(allocator, TEST_ALLOCATOR_SIZE_MISMATCH);
			return;
		}
		allocation->pointer = NULL;
		allocation->size = 0;
		allocation->in_use = false;
		free(memory);
		return;
	}
	record_error(allocator, TEST_ALLOCATOR_UNKNOWN_POINTER);
}

size_t test_allocator_live_count(const struct test_allocator *allocator)
{
	size_t count = 0;
	size_t index;

	for (index = 0; index < TEST_ALLOCATOR_CAPACITY; ++index) {
		if (allocator->allocations[index].in_use)
			++count;
	}
	return count;
}

size_t test_allocator_live_bytes(const struct test_allocator *allocator)
{
	size_t bytes = 0;
	size_t index;

	for (index = 0; index < TEST_ALLOCATOR_CAPACITY; ++index) {
		if (allocator->allocations[index].in_use)
			bytes += allocator->allocations[index].size;
	}
	return bytes;
}

int test_allocator_is_clean(const struct test_allocator *allocator)
{
	return allocator->error == TEST_ALLOCATOR_OK && test_allocator_live_count(allocator) == 0;
}

static void *allocate_callback(void *context, size_t size)
{
	return test_allocator_allocate(context, size);
}

static void deallocate_callback(void *context, void *memory, size_t size)
{
	test_allocator_deallocate(context, memory, size);
}

struct exfat_resize_options test_allocator_options(struct test_allocator *allocator)
{
	struct exfat_resize_options options;

	options.allocator.context = allocator;
	options.allocator.allocate = allocate_callback;
	options.allocator.deallocate = deallocate_callback;
	return options;
}
