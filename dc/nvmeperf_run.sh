#!/usr/bin/env bash

PERF_LOG=$OUTPUT_DIR/perf.${SLURM_NODEID}.log


PORT=4420
QUEUE_DEPTH=32
IO_SIZE=131072
RW=randread
PERF_TIME=60
#QUEUE_DEPTH=1
#IO_SIZE=4096
#IO_SIZE=2048



echo "Initiator's Servers number: ${SERVERS_NUMBER}" | tee -a $PERF_LOG
echo "Initiator's SPDK_DIR: ${SPDK_DIR}" | tee -a $PERF_LOG
echo "Initiator's RDMA_CORE_DIR: ${RDMA_CORE_DIR}" | tee -a $PERF_LOG
echo "Initiator's TGT_MASK: ${TGT_MASK}" | tee -a $PERF_LOG
echo "Initiator's PERF_MASK: ${PERF_MASK}" | tee -a $PERF_LOG
echo "Initiator's QUEUE_DEPTH: ${QUEUE_DEPTH}" | tee -a $PERF_LOG
echo "Initiator's IO_SIZE: ${IO_SIZE}" | tee -a $PERF_LOG

. ./utils.sh

function run_nvmeperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}
    echo "sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_BIN_DIR/spdk_nvme_perf \
	 --srq-depth 128 -r \"trtype:rdma adrfam:ipv4 traddr:$ADDR trsvcid:$PORT\" \
	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $NVME_PERF_EXTRA_OPTS 2>&1 | tee -a $PERF_LOG"

    #--srq-depth 128 -r "trtype:rdma adrfam:ipv4 traddr:$ADDR trsvcid:$PORT" \
    
    sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_BIN_DIR/spdk_nvme_perf \
	 --srq-depth 128 -r "trtype:rdma adrfam:ipv4 traddr:$ADDR trsvcid:$PORT" \
	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $NVME_PERF_EXTRA_OPTS 2>&1 | tee -a $PERF_LOG

    # sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_BIN_DIR/spdk_nvme_perf \
    # 	 -r "trtype:rdma adrfam:ipv4 traddr:$ADDR trsvcid:$PORT" \
    # 	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
    # 	 $NVME_PERF_EXTRA_OPTS 2>&1 | tee -a $PERF_LOG

}


function basic_test() {
    sudo env HUGEMEM=8192 $SPDK_DIR/scripts/setup.sh
    kill_if spdk_tgt
    kill_if bdevperf
    if [ -n "$NEED_IP_RESET" ]; then
	reset_ip
    fi
    #sudo hugeadm --pool-pages-min DEFAULT:4G
    wait_for_n_servers $SERVERS_NUMBER
    get_servers_list
    echo "Got servers list: ${SERVERS[@]}" | tee -a $PERF_LOG
    for ADDR in "${SERVERS[@]}"; do
	run_nvmeperf
    done

}

basic_test
