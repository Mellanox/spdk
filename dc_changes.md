# Architecture changes DC
## SRQ on initiatiator side

## Build
	* through configuration option
		"${CONFIG[RDMA_PROV]}" == "mlx5_dv_dc" 
		
## Tools\Example support
	* examples/nvme/perf/perf.c

```diff	
	+static uint32_t g_srq_depth;
	-	while ((op = getopt(argc, argv, "a:c:e:i:lo:q:r:k:s:t:w:C:DGHILM:NP:RT:U:V")) != -1) {
    +	while ((op = getopt(argc, argv, "a:c:e:i:lo:q:r:k:s:t:w:C:DGHILM:NP:RT:U:VS:")) != -1) {

```


##  Header's changes

* include/spdk/nvme.h

```diff
+struct spdk_nvme_transport_opts {
+	/* SRQ depth for RDMA transport. If zero SRQ is not used */
+	uint32_t srq_depth;
+};
+	void (*get_opts)(struct spdk_nvme_transport_opts *opts);
+	void (*set_opts)(const struct spdk_nvme_transport_opts *opts);
+void nvme_transport_get_opts(const char *transport_name,
+			     struct spdk_nvme_transport_opts *opts);
+void nvme_transport_set_opts(const char *transport_name,
+			     const struct spdk_nvme_transport_opts *opts)

```

* include/spdk/nvmf_spec.h
```diff
@@ -457,14 +457,18 @@ struct spdk_nvmf_rdma_request_private_data {
 	uint16_t	hrqsize;	/* host receive queue size */
 	uint16_t	hsqsize;	/* host send queue size */
 	uint16_t	cntlid;		/* controller id */
-	uint8_t		reserved[22];
+	uint32_t        dctn;
+	uint32_t        assigned_id;
+	uint8_t		reserved[14];
 };
 SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_request_private_data) == 32, "Incorrect size");
 
 struct spdk_nvmf_rdma_accept_private_data {
 	uint16_t	recfmt; /* record format */
 	uint16_t	crqsize;	/* controller receive queue size */
-	uint8_t		reserved[28];
+	uint32_t        dctn;
+	uint32_t        assigned_id;
+	uint8_t		reserved[20];
 };

```

* include/spdk_internal/rdma.h

```diff
+struct spdk_rdma_poller_context;
+struct spdk_rdma_poller_context *
+spdk_rdma_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr);
+
+int spdk_rdma_qp_set_poller_context(struct spdk_rdma_qp *spdk_rdma_qp,
+                                            struct spdk_rdma_poller_context *poller_ctx);
+uint32_t spdk_rdma_send_qp_num(struct spdk_rdma_qp *spdk_rdma_qp);
+uint32_t spdk_rdma_recv_qp_num(struct spdk_rdma_qp *spdk_rdma_qp);
+
+struct ibv_pd *spdk_rdma_qp_pd(struct spdk_rdma_qp *spdk_rdma_qp);
+int spdk_rdma_query_qp_dci(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
+			   enum ibv_qp_attr_mask attr_mask,
+			   struct ibv_qp_init_attr *init_attr);
+int spdk_rdma_query_qp_dct(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
+			   enum ibv_qp_attr_mask attr_mask,
+			   struct ibv_qp_init_attr *init_attr);
+
+void spdk_rdma_qp_set_remote_dctn(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dctn);
+uint32_t spdk_rdma_qp_get_local_dctn(struct spdk_rdma_qp *spdk_rdma_qp);
+bool spdk_rdma_is_corresponded_qp(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_wc *wc);
+void spdk_rdma_qp_set_remote_dci(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dci_qp_num);
+struct ibv_qp *spdk_rdma_receive_qp(struct spdk_rdma_qp *spdk_rdma_qp);
+struct ibv_qp *spdk_rdma_send_qp(struct spdk_rdma_qp *spdk_rdma_qp);
+uint32_t spdk_rdma_generate_qpair_id(struct spdk_rdma_qp *spdk_rdma_qp);
+void spdk_rdma_qp_assign_id(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t assigned_id);
+void spdk_rdma_notify_qp_on_send_completion(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t wrs_released);
+int spdk_rdma_qp_get_qpn_reservation(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t *qpn_reservation);
+void spdk_rdma_qp_reset(struct spdk_rdma_qp *spdk_rdma_qp);

```

* lib/nvme/nvme_rdma.c

```diff
+struct spdk_rdma_poller_context;
 struct nvme_rdma_poller {
 	struct ibv_context		*device;
 	struct ibv_cq			*cq;
+	struct ibv_pd			*pd;
+	struct ibv_srq			*srq;
+	struct nvme_rdma_resources	*resources;
+	struct spdk_rdma_poller_context *ctx;
 	int				required_num_wc;
 	int				current_num_wc;
 	STAILQ_ENTRY(nvme_rdma_poller)	link;
```
 

 ```C
 nvme_rdma_qpair_process_cm_event
 nvme_rdma_qpair_init
 nvme_rdma_qpair_submit_sends
 nvme_rdma_submit_recvs
 nvme_rdma_queue_recv_wr - SRQ related only changes
 nvme_rdma_post_recv - SRQ related only changes
 nvme_rdma_unregister_rsps - SRQ related only changes
 nvme_rdma_free_rsps - SRQ related only changes
 nvme_rdma_alloc_rsps - SRQ related only changes
 nvme_rdma_register_rsps - SRQ related only changes
 nvme_rdma_create_resources - SRQ related only changes
 nvme_rdma_destroy_resources - SRQ related only changes
 nvme_rdma_register_reqs - 
 nvme_rdma_connect
  _nvme_rdma_ctrlr_connect_qpair
 nvme_rdma_get_key
 nvme_rdma_ctrlr_create_qpair
 nvme_rdma_ctrlr_disconnect_qpair
 nvme_rdma_ctrlr_delete_io_qpair
 nvme_rdma_request_ready - SRQ related only changes
 nvme_rdma_cq_process_completions
 nvme_rdma_poller_create - SRQ related only changes
 nvme_rdma_poll_group_free_pollers - SRQ related only changes
 nvme_rdma_poll_group_get_qpair_by_wc
 nvme_rdma_poll_group_connect_qpair
 nvme_rdma_poll_group_process_completions - SRQ related only changes
 ```
 
 On another diff
 ```C
 
 ```

* lib/nvmf/rdma.c

```C
@@ -394,6 +420,9 @@ struct spdk_nvmf_rdma_qpair {
 
 	/* Indicate that nvmf_rdma_close_qpair is called */
 	bool					to_close;
+	uint32_t                                remote_dctn; /*FIXME - find how to keep it until we createg rdma_qp*/
+	uint32_t                                assigned_id; /*FIXME - the same as ^^^^^^ */
+	uint32_t                                in_pending_send_state;
 };

```

```C
nvmf_rdma_update_ibv_state
nvmf_rdma_qpair_initialize
request_transfer_in
request_transfer_out
nvmf_rdma_event_accept
nvmf_rdma_connect
nvmf_rdma_destroy_drained_qpair
get_rdma_qpair_from_wc
_poller_submit_recvs
_qp_reset_failed_sends
_poller_submit_sends
nvmf_rdma_poller_poll
```


* module/bdev/nvme/bdev_nvme.c
* module/bdev/nvme/bdev_nvme_rpc.c
* scripts/rpc.py
* scripts/rpc/bdev.py

```diff
+struct spdk_dc_mlx5_dv_poller_context {
+	struct spdk_rdma_poller_context common;
+	struct ibv_qp *qp_dci;
+	struct ibv_qp_ex *qp_dci_qpex;
+	struct mlx5dv_qp_ex *qp_dci_mqpex;
+
+	struct ibv_qp *srq; /*FIXME now owning. Just pointer to SRQ*/
+	struct ibv_qp *qp_dct;
+	bool   activated;
+	struct spdk_dc_mlx5_dv_qp *current_qp;
+
+	uint32_t qpair_counter;
+
+	CIRCLEQ_HEAD(, qp_list_entry) qps;
+	struct qp_list_entry *current_qpe; /*FIXME join with *current_qp*/
+	uint32_t registered_qp;
+	uint32_t available_in_dci;
+	uint32_t max_send_wr;
+	uint32_t quota;
+	bool send_started;
+};
+
+struct spdk_dc_mlx5_dv_qp {
+	struct spdk_rdma_qp common;
+	struct spdk_dc_mlx5_dv_poller_context *poller_ctx;
+	uint32_t remote_dctn;
+	uint32_t remote_qp_id;
+	uint32_t assigned_id;
+	uint64_t remote_dc_key;
+	struct ibv_ah *ah;
+	/* we don't expect concurent usage of spdk_dc_mlx5_dv_qp */
+	__be32 dctn;
+	struct ibv_send_wr *bad_wr;
+	struct ibv_send_wr *not_sent_yet;
+	uint32_t qpn_reservation;
+	uint32_t wrs_sent;
+};
```
