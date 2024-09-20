/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */
#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse proc/stat and get time spent processing system mode and user mode
 *
 * \param core Core which will be queried
 * \param usr Holds time [USER_HZ] spent processing in user mode
 * \param sys Holds time [USER_HZ] spent processing in system mode
 * \param irq Holds time [USER_HZ] spent processing interrupts
 *
 * \return 0 on success -1 on fail
 */
int app_get_proc_stat(unsigned int core, uint64_t *usr, uint64_t *sys, uint64_t *irq);

#ifdef __cplusplus
}
#endif

/**
 * Get isolated CPU core mask.
 */
const char *scheduler_get_isolated_core_mask(void);

/**
 * Set isolated CPU core mask.
 */
bool scheduler_set_isolated_core_mask(struct spdk_cpuset isolated_core_mask);

#endif /* EVENT_INTERNAL_H */