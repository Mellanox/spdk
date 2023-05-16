/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MLX5_H
#define SPDK_MLX5_H

#include "spdk/likely.h"

#include <infiniband/mlx5dv.h>

#define SPDK_MLX5_VENDOR_ID_MELLANOX 0x2c9

struct spdk_mlx5_crypto_dek;
struct spdk_mlx5_crypto_keytag;

struct spdk_mlx5_crypto_dek_create_attr {
	/* Data Encryption Key in binary form */
	char *dek;
	/* Length of the dek */
	size_t dek_len;
};

/**
 * Specify which devices are allowed to be used for crypto operation.
 *
 * If the user doesn't call this function then all devices which support crypto will be used.
 * This function copies devices names, in order to free allocated memory, the user must call
 * this function with either NULL \b dev_names or with \b devs_count equal 0. That method can
 * also be used to allow all devices.
 *
 * Subsequent calls with non-NULL \b dev_names and non-zero \b devs_count overwrite previously set
 * values.
 *
 * This function is not thread safe.
 *
 * \param dev_names Array of devices names which are allowed to be used for crypto operations
 * \param devs_count Size of \b devs_count array
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_devs_allow(const char * const dev_names[], size_t devs_count);

/**
 * Return a NULL terminated array of devices which support crypto operation on Nvidia NICs
 *
 * \param dev_num The size of the array or 0
 * \return Array of contexts. This array must be released with \b spdk_mlx5_crypto_devs_release
 */
struct ibv_context **spdk_mlx5_crypto_devs_get(int *dev_num);

/**
 * Releases array of devices allocated by \b spdk_mlx5_crypto_devs_get
 *
 * \param rdma_devs Array of device to be released
 */
void spdk_mlx5_crypto_devs_release(struct ibv_context **rdma_devs);

/**
 * Create a keytag which contains DEKs per each crypto device in the system
 *
 * \param attr Crypto attributes
 * \param out Keytag
 * \return 0 on success, negated errno of failure
 */
int spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
				   struct spdk_mlx5_crypto_keytag **out);

/**
 * Destroy a keytag created using \b spdk_mlx5_crypto_keytag_create
 *
 * \param keytag Keytag pointer
 */
void spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag);

/**
 * Fills attributes used to register UMR with crypto operation
 *
 * \param attr_out Configured UMR attributes
 * \param keytag Keytag with DEKs
 * \param pd Protection Domain which is going to be used to register UMR. This function will find a DEK in \b keytag with the same PD
 * \param block_size Logical block size
 * \param iv Initialization vector or tweak. Usually that is logical block address
 * \param encrypt_on_tx If set, memory data will be encrypted during TX and wire data will be decrypted during RX. If not set, memory data will be decrypted during TX and wire data will be encrypted during RX.
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_set_attr(struct mlx5dv_crypto_attr *attr_out,
			      struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
			      uint32_t block_size, uint64_t iv, bool encrypt_on_tx);

/**
 * Get low level devx obj id which represents the DEK
 *
 * \param keytag Keytag with DEKs
 * \param pd Protection Domain which is going to be used to register UMR.
 * \param dek_obj_id Output value
 * \return 0 on success, negated errno on failure
 */
int
spdk_mlx5_crypto_get_dek_obj_id(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd, uint32_t *dek_obj_id);

/* low level cq view, suitable for the direct polling, adapted from struct mlx5dv_cq */
struct spdk_mlx5_hw_cq {
	uint64_t cq_addr;
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	uint32_t ci;
	uint32_t cq_num;
};

struct spdk_mlx5_cq {
	struct spdk_mlx5_hw_cq hw;
	struct ibv_cq *verbs_cq;
};

struct spdk_mlx5_cq_attr {
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	void *cq_context;
	struct ibv_comp_channel *comp_channel;
	int comp_vector;
};

struct spdk_mlx5_hw_qp {
	uint64_t dbr_addr;
	uint64_t sq_addr;
	uint64_t sq_bf_addr;
	uint32_t sq_wqe_cnt;
	uint16_t sq_pi;
	uint32_t sq_tx_db_nc;
	uint32_t qp_num;
};

struct spdk_mlx5_qp_attr {
	struct ibv_qp_cap cap;
	bool sigall;
	/* If set then CQ_UPDATE will be cleared for every ctrl WQE and only last ctlr WQE before ringing the doorbell
	 * will be updated with CQ_UPDATE flag */
	bool siglast;
};

struct mlx5_qp_completion {
	uint64_t wr_id;
	/* Number of unsignaled completions before this one. Used to track qp overflow */
	uint32_t completions;
};

struct spdk_mlx5_qp {
	struct spdk_mlx5_hw_qp hw;
	struct mlx5_qp_completion *completions;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct ibv_qp *verbs_qp;
	uint16_t nonsignaled_outstanding;
	uint16_t max_sge;
	uint16_t tx_available;
	uint16_t tx_flags;
	uint16_t tx_revert_flags;
	uint16_t last_pi;
	bool tx_need_ring_db;
	bool aes_xts_inc_64;
	/* If set, HW expects tweak in big endian
	 * Otherwise, in little endian */
	bool aes_xts_tweak_be;
};

/* QP + CQ */
struct spdk_mlx5_dma_qp {
	struct spdk_mlx5_cq cq;
	struct spdk_mlx5_qp qp;
};

struct spdk_mlx5_cq_completion {
	uint64_t wr_id;
	int status;
};

struct spdk_mlx5_indirect_mkey {
	struct mlx5dv_devx_obj *devx_obj;
	uint32_t mkey;
	uint64_t addr;
};

enum {
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_WIRE_SIGNATURE    = 0x0,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE  = 0x1,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE          = 0x2,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY        = 0x3,
};

struct spdk_mlx5_umr_crypto_attr {
    	/* MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE to encrypt
    	 * MLX5_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE to decrypt */
	uint8_t enc_order;
	uint8_t bs_selector;
	uint8_t tweak_offset;
	uint32_t dek_obj_id;
    	uint64_t xts_iv;
    	uint64_t keytag;
};

struct spdk_mlx5_umr_attr {
	struct mlx5_wqe_data_seg *klm;
	uint32_t dv_mkey; /* mkey to configure */
	uint32_t umr_len;
	uint16_t klm_count;
};

int spdk_mlx5_dma_qp_create(struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
			    struct spdk_mlx5_qp_attr *qp_attr, void *context, struct spdk_mlx5_dma_qp **qp_out);
void spdk_mlx5_dma_qp_destroy(struct spdk_mlx5_dma_qp *dma_qp);
int spdk_mlx5_dma_qp_poll_completions(struct spdk_mlx5_dma_qp *dma_qp,
				      struct spdk_mlx5_cq_completion *comp, int max_completions);

/**
 * Prefetch \b wqe_count building blocks into cache
 *
 * @param dma_qp
 * @param wqe_count
 */
//TODO: use more "intelligent" interface like - num_umrs, num_writes, etc
static inline void spdk_mlx5_dma_qp_prefetch_sq(struct spdk_mlx5_dma_qp *dma_qp, uint32_t wqe_count)
{
	struct spdk_mlx5_hw_qp *hw = &dma_qp->qp.hw;
	uint32_t to_end, pi, i;
	char *sq;

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	sq = (char *)hw->sq_addr + pi * MLX5_SEND_WQE_BB;
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	if (spdk_likely(to_end >= wqe_count * MLX5_SEND_WQE_BB)) {
		for (i = 0; i < wqe_count; i++) {
			__builtin_prefetch(sq);
			sq += MLX5_SEND_WQE_BB;
		}
	} else {
		for (i = 0; i < wqe_count; i++) {
			__builtin_prefetch(sq);
			to_end -= MLX5_SEND_WQE_BB;
			if (to_end == 0) {
				sq = (char *)hw->sq_addr;
				to_end = hw->sq_wqe_cnt * MLX5_SEND_WQE_BB;
			} else {
				sq += MLX5_SEND_WQE_BB;
			}
		}
	}
}

/**
 *
 * @param qp
 * @param klm values in BE format
 * @param klm_count
 * @param dstaddr
 * @param rkey
 * @param wrid
 * param flags MLX5_WQE_CTRL_CQ_UPDATE to have a signaled completion or 0
 * @return
 */
int spdk_mlx5_dma_qp_rdma_write(struct spdk_mlx5_dma_qp *qp, struct mlx5_wqe_data_seg *klm,
				uint32_t klm_count, uint64_t dstaddr, uint32_t rkey,
				uint64_t wrid, uint32_t flags);

int spdk_mlx5_dma_qp_rdma_read(struct spdk_mlx5_dma_qp *qp, struct mlx5_wqe_data_seg *klm,
				uint32_t klm_count, uint64_t dstaddr, uint32_t rkey,
				uint64_t wrid, uint32_t flags);


int spdk_mlx5_umr_configure_crypto(struct spdk_mlx5_dma_qp *dma_qp, struct spdk_mlx5_umr_attr *umr_attr,
				   struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags);

int spdk_mlx5_umr_configure(struct spdk_mlx5_dma_qp *dma_qp, struct spdk_mlx5_umr_attr *umr_attr,
				uint64_t wr_id, uint32_t flags);

struct mlx5_devx_mkey_attr {
	uint64_t addr;
	uint64_t size;
	uint32_t log_entity_size;
	uint32_t relaxed_ordering_write: 1;
	uint32_t relaxed_ordering_read: 1;
	struct ibv_sge *sg;
	uint32_t sg_count;
	/* Size of bsf in octowords. If 0 then bsf is disabled */
	uint32_t bsf_octowords;
	bool crypto_en;
};

struct spdk_mlx5_relaxed_ordering_caps {
	bool relaxed_ordering_write_pci_enabled;
	bool relaxed_ordering_write;
	bool relaxed_ordering_read;
	bool relaxed_ordering_write_umr;
	bool relaxed_ordering_read_umr;
};

struct spdk_mlx5_crypto_caps {
	/* crypto supported or not */
	bool crypto;
	bool wrapped_crypto_operational;
	bool wrapped_crypto_going_to_commissioning;
	bool wrapped_import_method_aes_xts;
	bool single_block_le_tweak;
	bool multi_block_be_tweak;
	bool multi_block_le_tweak;
	bool tweak_inc_64;
};

int spdk_mlx5_query_crypto_caps(struct ibv_context *context, struct spdk_mlx5_crypto_caps *caps);

/**
* spdk_mlx5_query_relaxed_ordering_caps() - Query for Relaxed-Ordering
	*				       capabilities.
* @context: ibv_context to query.
* @caps: relaxed-ordering capabilities (output)
*
* Relaxed Ordering is a feature that improves performance by disabling the
	* strict order imposed on PCIe writes/reads. Applications that can handle
* this lack of strict ordering can benefit from it and improve performance.
*
* The function queries for the below capabilities:
* - relaxed_ordering_write_pci_enabled: relaxed_ordering_write is supported by
*     the device and also enabled in PCI.
* - relaxed_ordering_write: relaxed_ordering_write is supported by the device
*     and can be set in Mkey Context when creating Mkey.
* - relaxed_ordering_read: relaxed_ordering_read can be set in Mkey Context
	*     when creating Mkey.
* - relaxed_ordering_write_umr: relaxed_ordering_write can be modified by UMR.
* - relaxed_ordering_read_umr: relaxed_ordering_read can be modified by UMR.
*
* Return:
* 0 or -errno on error
*/
int spdk_mlx5_query_relaxed_ordering_caps(struct ibv_context *context,
		struct spdk_mlx5_relaxed_ordering_caps *caps);

struct spdk_mlx5_indirect_mkey *spdk_mlx5_create_indirect_mkey(struct ibv_pd *pd,
		struct mlx5_devx_mkey_attr *attr);
int spdk_mlx5_destroy_indirect_mkey(struct spdk_mlx5_indirect_mkey *mkey);
#endif /* SPDK_MLX5_H */
