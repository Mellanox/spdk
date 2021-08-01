#!/usr/bin/env bash

STANDALONE=false

PREFIX=${PREFIX:-/labhome/denisn/dc}
SPDK_DIR=${SPDK_DIR:-$PREFIX/spdk}
SPDK_BIN_DIR=${SPDK_BIN_DIR:-$SPDK_DIR/install/bin}
RDMA_CORE_DIR=${RDMA_CORE_DIR:-$PREFIX/rdma-core/build/lib}
PERF_MASK=${PERF_MASK:-0x3F}
SERVERS_NUMBER=${SERVERS_NUMBER:-1}

OUTPUT_DIR=${OUTPUT_DIR:-.}

INPUT_LOG=${OUTPUT_DIR}/input.log
PERF_LOG=$OUTPUT_DIR/perf.${SLURM_NODEID}.log


PORT=4420
QUEUE_DEPTH=32
IO_SIZE=131072
RW=randread
PERF_TIME=30


echo "Initiator's Servers number: ${SERVERS_NUMBER}"
echo "Initiator's SPDK_DIR: ${SPDK_DIR}"
echo "Initiator's RDMA_CORE_DIR: ${RDMA_CORE_DIR}"
echo "Initiator's PERF_MASK: ${PERF_MASK}"

. ./utils.sh



function rpc_perf() {
    sudo $SPDK_DIR/scripts/rpc.py -s /var/tmp/bdevperf.sock $@ 2>&1 | tee -a $PERF_LOG
}

function run_bdevperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}

    sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_DIR/test/bdev/bdevperf/bdevperf \
	 -r /var/tmp/bdevperf.sock --wait-for-rpc -m $PERF_MASK -C \
	 -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME -z \
	 $BDEV_PERF_EXTRA_OPTS 2>&1 | tee -a $PERF_LOG &
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a $PERF_LOG
    sleep 3
#    rpc_perf sock_set_default_impl 
#    rpc_perf sock_impl_set_options --enable-zerocopy-recv --disable-zerocopy-send --disable-recv-pipe
    rpc_perf framework_start_init
##!!!!    rpc_perf bdev_nvme_transport_set_options -s 200 -t RDMA

    controller_number=0

    for ADDR in "${SERVERS[@]}"; do
	echo "Attaching to ${ADDR}..."
	rpc_perf bdev_nvme_attach_controller -b Nvme$controller_number -t RDMA -f ipv4 -a $ADDR -s $PORT \
		 -n nqn.2016-06.io.spdk:cnode1
	((controller_number+=1))
    done
    sudo PYTHONPATH="$PYTHONPATH:$SPDK_DIR/scripts" $SPDK_DIR/test/bdev/bdevperf/bdevperf.py \
	 -s /var/tmp/bdevperf.sock -t 3600 perform_tests 2>&1 | tee -a $PERF_LOG &
    RPC_TASK_PID=$!
    echo "RPC task PID is $RPC_TASK_PID" 2>&1 | tee -a $PERF_LOG
}


function wait_bdevperf() {
    echo "Waiting for RPC task $RPC_TASK_PID"
    wait $RPC_TASK_PID
    rpc_perf spdk_kill_instance 15
    sleep 3
    echo "Waiting for perf $PERF_PID"
    wait $PERF_PID
}


function basic_test() {
    sudo env HUGEMEM=8192 $SPDK_DIR/scripts/setup.sh
    kill_if spdk_tgt
    kill_if bdevperf
    #sudo hugeadm --pool-pages-min DEFAULT:4G
    if [ $STANDALONE = false ]; then
	wait_for_n_servers $SERVERS_NUMBER
	get_servers_list
    fi
    echo "Got servers list: $SERVERS";

    run_bdevperf
    wait_bdevperf
}

while getopts s: flag
do
    case "${flag}" in
        s) SERVERS=${OPTARG}; STANDALONE=true;
    esac
done
basic_test
