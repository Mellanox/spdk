/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"
#include "common/lib/ut_multithread.c"
#include "accel/mlx5/accel_mlx5.c"
#include "unit/lib/json_mock.c"

DEFINE_STUB_V(spdk_memory_domain_destroy, (struct spdk_memory_domain *domain));
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "UT_DMA");
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *domain), SPDK_DMA_DEVICE_TYPE_RDMA);
DEFINE_STUB(spdk_memory_domain_update_notification_subscribe, int, (void *user_ctx,
		spdk_memory_domain_update_notification_cb user_cb), 0);
DEFINE_STUB(spdk_mlx5_crypto_keytag_create, int, (struct spdk_mlx5_crypto_dek_create_attr *attr,
		struct spdk_mlx5_crypto_keytag **out), 0);
DEFINE_STUB_V(spdk_mlx5_crypto_keytag_destroy, (struct spdk_mlx5_crypto_keytag *keytag));
DEFINE_STUB(spdk_mlx5_cq_poll_completions, int, (struct spdk_mlx5_cq *cq,
		struct spdk_mlx5_cq_completion *comp, int max_completions), 0);
DEFINE_STUB(spdk_mlx5_qp_create, int, (struct ibv_pd *pd, struct spdk_mlx5_cq *cq,
				       struct spdk_mlx5_qp_attr *qp_attr, struct spdk_mlx5_qp **qp_out), 0);
DEFINE_STUB(spdk_mlx5_qp_get_verbs_qp, struct ibv_qp *, (struct spdk_mlx5_qp *qp), 0);
DEFINE_STUB(spdk_mlx5_qp_connect_loopback, int, (struct spdk_mlx5_qp *qp), 0);
DEFINE_STUB_V(spdk_mlx5_qp_destroy, (struct spdk_mlx5_qp *qp));
DEFINE_STUB_V(spdk_mlx5_qp_complete_send, (struct spdk_mlx5_qp *qp));
DEFINE_STUB(spdk_mlx5_cq_create, int, (struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
				       struct spdk_mlx5_cq **cq_out), 0);
DEFINE_STUB(spdk_mlx5_cq_destroy, int, (struct spdk_mlx5_cq *cq), 0);
DEFINE_STUB(spdk_mlx5_qp_set_error_state, int, (struct spdk_mlx5_qp *qp), 0);
DEFINE_STUB(spdk_memory_domain_update_notification_unsubscribe, int, (void *user_ctx), 0);
DEFINE_STUB(spdk_mlx5_crypto_devs_allow, int, (const char *const dev_names[], size_t devs_count),
	    0);
DEFINE_STUB_V(spdk_accel_module_finish, (void));
DEFINE_STUB(spdk_mlx5_crypto_devs_get, struct ibv_context **, (int *dev_num), NULL);
DEFINE_STUB(spdk_mlx5_device_query_caps, int, (struct ibv_context *context,
		struct spdk_mlx5_device_caps *caps), 0);
DEFINE_STUB_V(spdk_mlx5_crypto_devs_release, (struct ibv_context **rdma_devs));
DEFINE_STUB(spdk_memory_domain_translate_data, int, (struct spdk_memory_domain *src_domain,
		void *src_domain_ctx,
		struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
		void *addr, size_t len, struct spdk_memory_domain_translation_result *result), 0);
DEFINE_STUB(spdk_memory_domain_transfer_data, int, (struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt,
		struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt,
		struct spdk_memory_domain_translation_result *src_translation,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg), 0);
DEFINE_STUB(spdk_memory_domain_get_user_context, void *, (struct spdk_memory_domain *domain,
		size_t *ctx_size), NULL);
DEFINE_STUB(spdk_rdma_utils_get_translation, int, (struct spdk_rdma_utils_mem_map *map,
		void *address,
		size_t length, struct spdk_rdma_utils_memory_translation *translation), 0);
DEFINE_STUB(spdk_mlx5_crypto_get_dek_data, int, (struct spdk_mlx5_crypto_keytag *keytag,
		struct ibv_pd *pd, struct spdk_mlx5_crypto_dek_data *data), 0);
DEFINE_STUB(spdk_mlx5_umr_configure_crypto, int, (struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr,
		struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_umr_configure, int, (struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr, uint64_t wr_id, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_qp_rdma_read, int, (struct spdk_mlx5_qp *qp, struct ibv_sge *sge,
		uint32_t sge_count,
		uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_qp_rdma_write, int, (struct spdk_mlx5_qp *qp, struct ibv_sge *sge,
		uint32_t sge_count,
		uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_umr_configure_sig_crypto, int, (struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr,
		struct spdk_mlx5_umr_sig_attr *sig_attr,
		struct spdk_mlx5_umr_crypto_attr *crypto_attr,
		uint64_t wr_id, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_qp_set_psv, int, (struct spdk_mlx5_qp *dv_qp, uint32_t psv_index,
					uint32_t crc_seed, uint64_t wr_id, uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_umr_configure_sig, int, (struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr, struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id,
		uint32_t flags), 0);
DEFINE_STUB(spdk_mlx5_create_psv, struct spdk_mlx5_psv *, (struct ibv_pd *pd), NULL);
DEFINE_STUB(spdk_mlx5_destroy_psv, int, (struct spdk_mlx5_psv *psv), 0);
DEFINE_STUB(spdk_mlx5_mkey_pool_init, int, (struct spdk_mlx5_mkey_pool_param *params,
		struct ibv_pd *pd), 0);
DEFINE_STUB(spdk_mlx5_mkey_pool_destroy, int, (uint32_t flags, struct ibv_pd *pd), 0);
DEFINE_STUB(spdk_mlx5_mkey_pool_get_ref, struct spdk_mlx5_mkey_pool *, (struct ibv_pd *pd,
		uint32_t flags), NULL);
DEFINE_STUB_V(spdk_mlx5_mkey_pool_put_ref, (struct spdk_mlx5_mkey_pool *pool));
DEFINE_STUB(spdk_mlx5_mkey_pool_get_bulk, int, (struct spdk_mlx5_mkey_pool *pool,
		struct spdk_mlx5_mkey_pool_obj **mkeys,
		uint32_t mkeys_count), 0);
DEFINE_STUB_V(spdk_mlx5_mkey_pool_put_bulk, (struct spdk_mlx5_mkey_pool *pool,
		struct spdk_mlx5_mkey_pool_obj **mkeys,
		uint32_t mkeys_count));
DEFINE_STUB(spdk_mlx5_mkey_pool_find_mkey_by_id, struct spdk_mlx5_mkey_pool_obj *, (void *ch,
		uint32_t mkey_id), NULL);
DEFINE_STUB_V(spdk_mlx5_umr_implementer_register, (bool registered));
DEFINE_STUB(spdk_mempool_create_ctor, struct spdk_mempool *, (const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id, spdk_mempool_obj_cb_t *obj_init,
		void *obj_init_arg), NULL);
DEFINE_STUB(spdk_mempool_obj_iter, uint32_t, (struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb,
		void *obj_cb_arg), 0);
DEFINE_STUB_V(spdk_accel_module_list_add, (struct spdk_accel_module_if *accel_module));
DEFINE_STUB_V(spdk_accel_task_complete, (struct spdk_accel_task *accel_task, int status));
DEFINE_STUB(spdk_accel_sequence_next_task, struct spdk_accel_task *, (struct spdk_accel_task *task),
	    NULL);
DEFINE_STUB_V(spdk_accel_sequence_continue, (struct spdk_accel_sequence *seq));
DEFINE_STUB(spdk_accel_get_memory_domain, struct spdk_memory_domain *, (void), NULL);
DEFINE_STUB(spdk_accel_get_opcode_name, const char *, (enum spdk_accel_opcode opcode), "pewpew");
DEFINE_STUB(spdk_accel_sequence_first_task, struct spdk_accel_task *,
	    (struct spdk_accel_sequence *seq), NULL);
DEFINE_STUB_V(spdk_accel_driver_register, (struct spdk_accel_driver *driver));
DEFINE_STUB(spdk_accel_set_driver, int, (const char *name), 0);
DEFINE_STUB(spdk_rdma_utils_get_memory_domain, struct spdk_memory_domain *,
	    (struct ibv_pd *pd, enum spdk_dma_device_type type), NULL);
DEFINE_STUB_V(spdk_rdma_utils_put_memory_domain, (struct spdk_memory_domain *domain));
DEFINE_STUB(spdk_rdma_utils_create_mem_map, struct spdk_rdma_utils_mem_map *, (struct ibv_pd *pd,
		struct spdk_nvme_rdma_hooks *hooks, int accel_flags), 0);
DEFINE_STUB_V(spdk_rdma_utils_free_mem_map, (struct spdk_rdma_utils_mem_map **_map));
DEFINE_STUB(spdk_rdma_utils_get_pd, struct ibv_pd *, (struct ibv_context *context), NULL);
DEFINE_STUB_V(spdk_rdma_utils_put_pd, (struct ibv_pd *pd));

static int
test_setup(void)
{
	return 0;
}

static int
test_cleanup(void)
{
	return 0;
}

static void
test_accel_mlx5_get_copy_task_count(void)
{
	uint32_t num_ops;

	struct iovec src1[1] = { { .iov_len = 4096 } };
	struct iovec dst1[1] = { { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src1, SPDK_COUNTOF(src1), dst1, SPDK_COUNTOF(dst1));
	CU_ASSERT(num_ops == 1);

	struct iovec src2[1] = { { .iov_len = 8192 } };
	struct iovec dst2[2] = { { .iov_len = 4096 }, { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src2, SPDK_COUNTOF(src2), dst2, SPDK_COUNTOF(dst2));
	CU_ASSERT(num_ops == 2);

	struct iovec src3[2] = { { .iov_len = 4096 }, { .iov_len = 4096 } };
	struct iovec dst3[1] = { { .iov_len = 8192 } };
	num_ops = accel_mlx5_get_copy_task_count(src3, SPDK_COUNTOF(src3), dst3, SPDK_COUNTOF(dst3));
	CU_ASSERT(num_ops == 1);

	struct iovec src4[16] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }
	};
	struct iovec dst4[1] = { { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src4, SPDK_COUNTOF(src4), dst4, SPDK_COUNTOF(dst4));
	CU_ASSERT(num_ops == 1);

	struct iovec src5[17] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 128 },
		{ .iov_len = 128 }
	};
	struct iovec dst5[1] = { { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src5, SPDK_COUNTOF(src5), dst5, SPDK_COUNTOF(dst5));
	CU_ASSERT(num_ops == 2);

	struct iovec src6[18] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 128 },
		{ .iov_len = 128 }, { .iov_len = 4096 }
	};
	struct iovec dst6[2] = { { .iov_len = 4096 }, { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src6, SPDK_COUNTOF(src6), dst6, SPDK_COUNTOF(dst6));
	CU_ASSERT(num_ops == 3);

	struct iovec src7[32] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }
	};
	struct iovec dst7[2] = { { .iov_len = 4096 }, { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src7, SPDK_COUNTOF(src7), dst7, SPDK_COUNTOF(dst7));
	CU_ASSERT(num_ops == 2);

	struct iovec src8[17] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 4096 }
	};
	struct iovec dst8[3] = { { .iov_len = 4096 }, { .iov_len = 2048 }, { .iov_len = 2048 } };
	num_ops = accel_mlx5_get_copy_task_count(src8, SPDK_COUNTOF(src8), dst8, SPDK_COUNTOF(dst8));
	CU_ASSERT(num_ops == 3);

	struct iovec src9[16] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 4352 }
	};
	struct iovec dst9[3] = { { .iov_len = 4096 }, { .iov_len = 2048 }, { .iov_len = 2048 } };
	num_ops = accel_mlx5_get_copy_task_count(src9, SPDK_COUNTOF(src9), dst9, SPDK_COUNTOF(dst9));
	CU_ASSERT(num_ops == 3);

	struct iovec src10[16] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 4352 }
	};
	struct iovec dst10[4] = { { .iov_len = 4096 }, { .iov_len = 2048 }, { .iov_len = 1792 }, { .iov_len = 256 } };
	num_ops = accel_mlx5_get_copy_task_count(src10, SPDK_COUNTOF(src10), dst10, SPDK_COUNTOF(dst10));
	CU_ASSERT(num_ops == 4);

	struct iovec src11[18] = { { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 4096 }, { .iov_len = 4096 }
	};
	struct iovec dst11[4] = { { .iov_len = 4096 }, { .iov_len = 2048 }, { .iov_len = 1792 }, { .iov_len = 4352 } };
	num_ops = accel_mlx5_get_copy_task_count(src11, SPDK_COUNTOF(src11), dst11, SPDK_COUNTOF(dst11));
	CU_ASSERT(num_ops == 4);

	struct iovec src12[24] = { { .iov_len = 512 }, { .iov_len = 512 }, { .iov_len = 512 }, { .iov_len = 512 },
		{ .iov_len = 512 }, { .iov_len = 512 }, { .iov_len = 512 }, { .iov_len = 512 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 },
		{ .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }, { .iov_len = 256 }
	};
	struct iovec dst12[2] = { { .iov_len = 4096 }, { .iov_len = 4096 } };
	num_ops = accel_mlx5_get_copy_task_count(src12, SPDK_COUNTOF(src12), dst12, SPDK_COUNTOF(dst12));
	CU_ASSERT(num_ops == 2);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("accel_mlx5", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_accel_mlx5_get_copy_task_count);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
