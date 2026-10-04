/* SPDX-License-Identifier: MIT */
#ifndef EXFAT_RESIZE_SHRINK_FIXTURE_H
#define EXFAT_RESIZE_SHRINK_FIXTURE_H
#include "support/exfat_fixture.h"

/* Known files and all system streams straddle or lie beyond the 2000-cluster target. */
int shrink_fixture_initialize(struct exfat_fixture *fixture, uint32_t sectors_per_cluster);
int shrink_fixture_verify(struct exfat_fixture *fixture, uint32_t cluster_count);
int shrink_fixture_set_fat(struct exfat_fixture *fixture, uint32_t cluster, uint32_t value);
int shrink_fixture_set_bit(struct exfat_fixture *fixture, uint32_t cluster, int allocated);
uint32_t shrink_fixture_fat(struct exfat_fixture *fixture, uint32_t cluster);
uint64_t shrink_fixture_target(const struct exfat_fixture *fixture);
#endif
