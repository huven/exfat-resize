/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_TEST_ALLOCATOR_H
#define EXFAT_RESIZE_TEST_ALLOCATOR_H

#include "exfat_resize.h"

#include <stdbool.h>
#include <stddef.h>

enum { TEST_ALLOCATOR_CAPACITY = 16 };

enum test_allocator_error {
	TEST_ALLOCATOR_OK,
	TEST_ALLOCATOR_ZERO_SIZE,
	TEST_ALLOCATOR_REGISTRY_FULL,
	TEST_ALLOCATOR_UNKNOWN_POINTER,
	TEST_ALLOCATOR_SIZE_MISMATCH,
};

struct test_allocation {
	void *pointer;
	size_t size;
	bool in_use;
};

struct test_allocator {
	struct test_allocation allocations[TEST_ALLOCATOR_CAPACITY];
	size_t allocation_attempts;
	size_t successful_allocations;
	size_t largest_requested_size;
	size_t deallocation_calls;
	size_t fail_on_attempt;
	/* First tracker or callback-contract error; allocation failure alone is not an error. */
	enum test_allocator_error error;
};

void test_allocator_set_fail_on_attempt(struct test_allocator *allocator, size_t attempt);

void *test_allocator_allocate(struct test_allocator *allocator, size_t size);
void test_allocator_deallocate(struct test_allocator *allocator, void *memory, size_t size);

size_t test_allocator_live_count(const struct test_allocator *allocator);
size_t test_allocator_live_bytes(const struct test_allocator *allocator);
int test_allocator_is_clean(const struct test_allocator *allocator);

struct exfat_resize_allocator test_allocator_callbacks(struct test_allocator *allocator);

#endif
