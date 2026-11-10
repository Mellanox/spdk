/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * OCP Datacenter NVMe SSD Specification definitions
 */

#ifndef SPDK_NVME_OCP_H
#define SPDK_NVME_OCP_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/assert.h"

/** UUID selecting the OCP log pages and features through the UUID index */
#define SPDK_NVME_OCP_UUID \
	0xc1, 0x94, 0xd5, 0x5b, 0xe0, 0x94, 0x47, 0x94, 0xa2, 0x1d, 0x29, 0x99, 0x8f, 0x56, 0xbe, 0x6f

enum spdk_nvme_ocp_log_page {
	/** SMART / Health Information Extended (\ref spdk_nvme_ocp_smart_log_page) */
	SPDK_NVME_OCP_LOG_SMART		= 0xc0,
};

/** Log Page GUID of the SMART / Health Information Extended log page */
#define SPDK_NVME_OCP_SMART_LOG_GUID \
	0xc5, 0xaf, 0x10, 0x28, 0xea, 0xbf, 0xf2, 0xa4, 0x9c, 0x4f, 0x6f, 0x7c, 0xc9, 0x14, 0xd5, 0xaf

/**
 * Log Page Version of the SMART / Health Information Extended log page, named after the OCP
 * specification revision that introduced it
 */
enum spdk_nvme_ocp_smart_log_version {
	SPDK_NVME_OCP_SMART_LOG_VERSION_1_0	= 0x2,
	SPDK_NVME_OCP_SMART_LOG_VERSION_2_0	= 0x3,
	SPDK_NVME_OCP_SMART_LOG_VERSION_2_5	= 0x4,
	SPDK_NVME_OCP_SMART_LOG_VERSION_2_6	= 0x5,
};

/**
 * SMART / Health Information Extended log page. Fields marked with an OCP revision were added in
 * that revision and are only valid if the Log Page Version is at least the revision's
 * \ref spdk_nvme_ocp_smart_log_version.
 */
struct spdk_nvme_ocp_smart_log_page {
	/* Physical Media Units Written and Read are in bytes. */
	uint64_t	physical_media_units_written[2];
	uint64_t	physical_media_units_read[2];

	struct {
		uint64_t	raw : 48;
		uint64_t	normalized : 16;
	} bad_user_nand_blocks;

	struct {
		uint64_t	raw : 48;
		uint64_t	normalized : 16;
	} bad_system_nand_blocks;

	uint64_t	xor_recovery_count;
	uint64_t	uncorrectable_read_error_count;
	uint64_t	soft_ecc_error_count;

	struct {
		uint32_t	detected_errors;
		uint32_t	corrected_errors;
	} end_to_end_correction_counts;

	uint64_t	system_data_percent_used : 8;
	uint64_t	refresh_counts : 56;

	struct {
		uint32_t	maximum;
		uint32_t	minimum;
	} user_data_erase_counts;

	struct {
		uint8_t		events;
		uint8_t		current_status;
	} thermal_throttling;

	/* OCP 2.0. Point and Minor are unaligned little-endian 16-bit values. */
	struct {
		uint8_t		errata;
		uint8_t		point[2];
		uint8_t		minor[2];
		uint8_t		major;
	} dssd_specification_version;

	uint64_t	pcie_correctable_error_count;
	uint32_t	incomplete_shutdowns;
	uint8_t		reserved116[4];
	uint8_t		percent_free_blocks;
	uint8_t		reserved121[7];
	uint16_t	capacitor_health;

	/* Errata revisions (a, b, c, etc.) in ASCII, or 0h if there are none */
	uint8_t		nvme_base_errata_version;		/* OCP 2.0 */
	uint8_t		nvm_command_set_errata_version;		/* OCP 2.5 */
	uint8_t		nvme_over_pcie_errata_version;		/* OCP 2.6 */
	uint8_t		nvme_mi_errata_version;			/* OCP 2.6 */

	uint8_t		reserved134[2];
	uint64_t	unaligned_io;
	uint64_t	security_version_number;
	uint64_t	total_nuse;
	uint64_t	plp_start_count[2];
	/* Endurance Estimate is in bytes. */
	uint64_t	endurance_estimate[2];
	uint64_t	pcie_link_retraining_count;		/* OCP 2.0 */
	uint64_t	power_state_change_count;		/* OCP 2.0 */
	uint8_t		lowest_permitted_firmware_revision[8];	/* OCP 2.5 */

	/* OCP 2.6 */
	uint16_t	total_media_dies;
	uint16_t	media_die_failure_tolerance;
	uint16_t	media_dies_offline;
	/* Max Temperature Recorded is in degrees Celsius. */
	uint8_t		max_temperature_recorded;
	uint8_t		reserved223;
	uint64_t	nand_avg_erase_count;
	uint32_t	command_timeouts;

	struct {
		uint32_t	raw;
		uint8_t		normalized;
		uint8_t		reserved[3];
	} system_area_program_fail_count;

	struct {
		uint32_t	raw;
		uint8_t		normalized;
		uint8_t		reserved[3];
	} system_area_uncorrectable_read_count;

	struct {
		uint32_t	raw;
		uint8_t		normalized;
		uint8_t		reserved[3];
	} system_area_erase_fail_count;

	/* Max Peak Power Capability and Current Max Average Power are in Watts. */
	uint16_t	max_peak_power_capability;
	uint16_t	current_max_average_power;
	/* Lifetime Power Consumed is in kWh. */
	uint8_t		lifetime_power_consumed[6];
	uint8_t		dssd_firmware_revision[8];
	uint8_t		dssd_firmware_build_uuid[16];
	uint8_t		dssd_firmware_build_label[64];

	uint8_t		reserved358[136];
	uint16_t	log_page_version;
	uint8_t		log_page_guid[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ocp_smart_log_page) == 512, "Incorrect size");

#ifdef __cplusplus
}
#endif

#endif
