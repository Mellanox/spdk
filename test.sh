#!/usr/bin/env bash

# Adjust paths according to your setup or add new setup
if [ "1" == $SETUP ]; then
    # Test setup via SNAP on swx-astra05/06
    # Bluefield
    BF_HOSTNAME=swx-astra05-bf2
    SNAP_SSH=${SNAP_SSH:-$BF_HOSTNAME}
    SNIC_SNAP_PATH=${SNIC_SNAP_PATH:-$PWD/../nvmx}
    SNAP_BIN_PATH=${SNAP_BIN_PATH:-$SNIC_SNAP_PATH/build-$BF_HOSTNAME/service}
    SNIC_SPDK_PATH=${SNIC_SPDK_PATH:-$PWD}
    SNAP_SPDK_LIB_PATH=${SNAP_SPDK_LIB_PATH:-$SNIC_SPDK_PATH/install-$BF_HOSTNAME/lib}
    SNAP_DPDK_LIB_PATH=${SNAP_DPDK_LIB_PATH:-$SNIC_SPDK_PATH/dpdk/build/lib}
    LIBXLIO=${LIBXLIO:-$PWD/../libxlio/install-$BF_HOSTNAME/lib/libxlio.so}
    #SNIC_RDMA_CORE_PATH=${SNIC_RDMA_CORE_PATH:-$PWD/../rdma-core/build/lib}
    SNAP_ENV_OPTS="LD_LIBRARY_PATH=$SNAP_SPDK_LIB_PATH:$SNAP_DPDK_LIB_PATH:$SNIC_RDMA_CORE_PATH"
    #SNAP_ENV_OPTS="LD_PRELOAD=/usr/lib/gcc/aarch64-linux-gnu/11/libasan.so $SNAP_ENV_OPTS"
    # Host
    SPDK_MASTER_PATH=${SPDK_MASTER_PATH:-$PWD/../spdk-master}
    PCI_ADDR=${PCI_ADDR:-0000:9e:00.2}
    PCI_VF_ADDRS=(0000:9e:04.5 0000:9e:04.6)

    ###### run fio on Host
    PERF_SSH=${PERF_SSH:-}
    PERF_SPDK_PATH=${PERF_SPDK_PATH:-$SPDK_MASTER_PATH}

    ###### run bdevperf on BF
    #PERF_SSH=${PERF_SSH:-$BF_HOSTNAME}
    #PERF_SPDK_PATH=${PERF_SPDK_PATH:-$PWD}

    PERF_BIN_PATH=${PERF_BIN_PATH:-$PERF_SPDK_PATH/install-$HOSTNAME/bin}
    FIO=${FIO:-$PWD/../fio/install-$HOSTNAME/bin/fio}
    # Target
    TGT_MASK=${TGT_MASK:-0x3F000000}
    TGT_HOST=${TGT_HOST:-swx-astra06}
    TGT_ADDR=${TGT_ADDR:-1.1.16.1}
    TGT_PORT=${TGT_PORT:-4420}
    #TGT_SECOND_ADDR=${TGT_SECOND_ADDR:-2.2.102.1}
    #TGT_SECOND_PORT=${TGT_SECOND_PORT:-4421}
    TGT_SPDK_PATH=${TGT_SPDK_PATH:-$SPDK_MASTER_PATH}
    TGT_BIN_PATH=${TGT_BIN_PATH:-$TGT_SPDK_PATH/install-$TGT_HOST/bin}
elif [ "perf" == $SETUP ]; then
    # Reference performance test setup on spdk-03/04
    # Bluefield
    BF_HOSTNAME=spdk-03-bf1
    SNAP_SSH=${SNAP_SSH:-$BF_HOSTNAME}
    SNIC_SNAP_PATH=${SNIC_SNAP_PATH:-$PWD/../nvmx}
    SNAP_BIN_PATH=${SNAP_BIN_PATH:-$SNIC_SNAP_PATH/build-$BF_HOSTNAME/service}
    SNIC_SPDK_PATH=${SNIC_SPDK_PATH:-$PWD}
    SNAP_SPDK_LIB_PATH=${SNAP_SPDK_LIB_PATH:-$SNIC_SPDK_PATH/install-$BF_HOSTNAME/lib}
    SNAP_DPDK_LIB_PATH=${SNAP_DPDK_LIB_PATH:-$SNIC_SPDK_PATH/dpdk/build/lib}
    LIBXLIO=${LIBXLIO:-$PWD/../libxlio/install-$BF_HOSTNAME/lib/libxlio.so}
    SNIC_RDMA_CORE_PATH=${SNIC_RDMA_CORE_PATH:-$PWD/../rdma-core/build/lib}
    SNAP_ENV_OPTS="LD_LIBRARY_PATH=$SNAP_SPDK_LIB_PATH:$SNAP_DPDK_LIB_PATH:$SNIC_RDMA_CORE_PATH"
    # Host
    SPDK_MASTER_PATH=${SPDK_MASTER_PATH:-$PWD/../spdk-master}
    PCI_ADDR=${PCI_ADDR:-0000:85:00.2}
    PERF_SSH=${PERF_SSH:-}
    PERF_SPDK_PATH=${PERF_SPDK_PATH:-$SPDK_MASTER_PATH}
    PERF_BIN_PATH=${PERF_BIN_PATH:-$PERF_SPDK_PATH/install-$HOSTNAME/bin}
    FIO=${FIO:-$PWD/../fio/install-$HOSTNAME/bin/fio}
    # Target
    TGT_MASK=${TGT_MASK:-0x3F0}
    TGT_HOST=${TGT_HOST:-spdk-04}
    TGT_ADDR=${TGT_ADDR:-1.1.4.1}
    TGT_PORT=${TGT_PORT:-4420}
    TGT_SPDK_PATH=${TGT_SPDK_PATH:-$SPDK_MASTER_PATH}
    TGT_BIN_PATH=${TGT_BIN_PATH:-$TGT_SPDK_PATH/install-$TGT_HOST/bin}
else
    echo "Unknown setup"
    exit 1
fi

TGT_LIBXLIO=${TGT_LIBXLIO:-}

XLIO_OPTS="
XLIO_STATS_FILE=/tmp/xlio.stat
XLIO_STATS_FD_NUM=1000
XLIO_RING_ALLOCATION_LOGIC_TX=30
XLIO_RING_ALLOCATION_LOGIC_RX=30
XLIO_QP_COMPENSATION_LEVEL=8
XLIO_STRQ_STRIDES_COMPENSATION_LEVEL=32768
XLIO_FORK=0
XLIO_SPEC=latency
XLIO_INTERNAL_THREAD_AFFINITY=0x01
XLIO_TX_WRE=1024
XLIO_RX_WRE_BATCHING=1
XLIO_THREAD_MODE=1
XLIO_TX_WRE_BATCHING=128
XLIO_TSO=1
XLIO_RX_POLL=-1
XLIO_RX_PREFETCH_BYTES_BEFORE_POLL=256
XLIO_MEM_ALLOC_TYPE=1
XLIO_AVOID_SYS_CALLS_ON_TCP_FD=1
XLIO_CQ_KEEP_QP_FULL=0
XLIO_CQ_AIM_INTERVAL_MSEC=0
XLIO_CQ_AIM_MAX_COUNT=64
XLIO_CQ_MODERATION_ENABLE=1
XLIO_PROGRESS_ENGINE_INTERVAL=0
XLIO_SELECT_POLL_OS_FORCE=1
XLIO_SELECT_POLL_OS_RATIO=1
XLIO_SELECT_SKIP_OS=1
XLIO_TCP_NODELAY=0
XLIO_RING_ALLOCATION_LOGIC_TX=20
XLIO_RING_ALLOCATION_LOGIC_RX=20
XLIO_TCP_ABORT_ON_CLOSE=1
XLIO_RX_BUFS=4096
XLIO_STRQ_STRIDE_SIZE_BYTES=64
XLIO_STRQ_NUM_STRIDES=2048
XLIO_RX_WRE=4
XLIO_SKIP_POLL_IN_RX=2
XLIO_RING_DEV_MEM_TX=1024
XLIO_GRO_STREAMS_MAX=0
XLIO_LRO=on
XLIO_TX_SEGS_TCP=600000
XLIO_TX_BUFS=30000
XLIO_BUFFER_BATCHING_MODE=0
"

SOCK_IMPL=${SOCK_IMPL:-xlio}
TCP=${TCP:-NVDA_TCP}
DATA_DGST=${DATA_DGST:-}
FIO_JOBS=${FIO_JOBS:-8}
FIO_CPUS=${FIO_CPUS:-1-8}
MDTS=${MDTS:-4}
SUBSYS=${SUBSYS:-1}
PATHS=${PATHS:-1}
CONNECT_TIMEOUT=${CONNECT_TIMEOUT:-500000}

TGT_MASK=${TGT_MASK:-0xFF}
SNAP_MASK=${SNAP_MASK:-0xFF}
PERF_TIME=${PERF_TIME:-60}
WARMUP_TIME=${WARMUP_TIME:-5}
PERF_MASKS=${PERF_MASKS:-0x10}
QUEUE_DEPTHS=${QUEUE_DEPTHS:-1 2 4 8 16 32 64 128}
IO_SIZES=${IO_SIZES:-4096 8192 16384 32768 65536 131072}
RW=${RW:-randread}
REPEAT=${REPEAT:-1}
#NVME_PERF_EXTRA_OPTS="-T xlio -T nvme"
#BDEV_PERF_EXTRA_OPTS="-L xlio -L nvme"
SSH_EXTRA_OPTS=${SSH_EXTRA_OPTS:-}
BDEV_NULL_OPTS="8192 512"
MULTIPATH_OPTS="-p active_active -s round_robin -r 16"
SNAP_ENV_OPTS="$SNAP_ENV_OPTS NVME_EMU_PROVIDER=dpa LIBSNAP_DPA_DIR=$SNIC_SNAP_PATH/subprojects/core/dpa_app"
#ACCEL_OPTS="--qp-size 128 --num-requests 1024"
#SOCK_EXTRA_OPTS="--disable-recv-pipe"
SNAP_DEBUG=${SNAP_DEBUG:-}

function ssh_prefix() {
    local SSH=""
    local HOST=$1
    if [ -n "$HOST" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $HOST"
    fi
    echo "$SSH"
}

function rpc_tgt() {
    $(ssh_prefix $TGT_HOST) sudo $TGT_SPDK_PATH/scripts/rpc.py -v $@ >> rpc_tgt.log 2>&1
}

function rpc_tgt_batch() {
    echo -e "$@" | $(ssh_prefix $TGT_HOST) sudo $TGT_SPDK_PATH/scripts/rpc.py -v >> rpc_tgt.log 2>&1
}

function rpc_perf() {
    $(ssh_prefix $PERF_SSH) sudo $PERF_SPDK_PATH/scripts/rpc.py -s /var/tmp/bdevperf.sock -v $@ 2>&1 | tee -a perf.log
}

function rpc_perf_batch() {
    echo -e "$@" | $(ssh_prefix $PERF_SSH) sudo $PERF_SPDK_PATH/scripts/rpc.py -s /var/tmp/bdevperf.sock -v >> perf.log 2>&1
}

function rpc_snap_spdk() {
    $(ssh_prefix $SNAP_SSH) sudo $SNIC_SPDK_PATH/scripts/rpc.py -v $@ 2>&1 | tee -a rpc_snap.log > /dev/null
}

function rpc_snap_spdk_batch() {
    echo -e "$@" | $(ssh_prefix $SNAP_SSH) sudo $SNIC_SPDK_PATH/scripts/rpc.py -v 2>&1 | tee -a rpc_snap.log > /dev/null
}

function rpc_snap_snap() {
    $(ssh_prefix $SNAP_SSH) sudo $SNIC_SNAP_PATH/snap_rpc.py $@ 2>&1 | tee -a rpc_snap.log > /dev/null
}

function rpc_snap_snap_batch() {
    echo -e "$@" | $(ssh_prefix $SNAP_SSH) sudo $SNIC_SNAP_PATH/snap_rpc.py 2>&1 | tee -a rpc_snap.log > /dev/null
}

function start_tgt() {
    if [ -n "$TGT_LIBXLIO" ]; then
	$(ssh_prefix $TGT_HOST) sudo $XLIO_OPTS LD_PRELOAD=$TGT_LIBXLIO $TGT_BIN_PATH/spdk_tgt -m $TGT_MASK 2>&1 | tee tgt.log > /dev/null &
    else
	$(ssh_prefix $TGT_HOST) sudo $TGT_BIN_PATH/spdk_tgt -m $TGT_MASK --wait-for-rpc 2>&1 | tee tgt.log > /dev/null &
    fi
    for i in $(seq 10); do
	if rpc_tgt spdk_get_version; then
	    return
	fi
	sleep 1
    done
}

function stop_tgt() {
    local SIGNAL=${1:-15}
    rpc_tgt spdk_kill_instance $SIGNAL
    sleep 3
}

function config_tgt_add_path1() {
	rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode1
}

function config_tgt_rm_path1() {
	rpc_tgt nvmf_subsystem_remove_listener -t tcp -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode1
}

function config_tgt_add_path2() {
	rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_SECOND_ADDR -f ipv4 -s $TGT_SECOND_PORT nqn.2016-06.io.spdk:cnode1
}

function config_tgt_rm_path2() {
	rpc_tgt nvmf_subsystem_remove_listener -t tcp -a $TGT_SECOND_ADDR -f ipv4 -s $TGT_SECOND_PORT nqn.2016-06.io.spdk:cnode1
}

function config_tgt_multipath() {
    #rpc_tgt sock_impl_set_options -i posix --enable-zerocopy-send
    rpc_tgt framework_start_init
    rpc_tgt nvmf_create_transport -t tcp -n 2048 -b 128
    rpc_tgt nvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode1
    rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode1
    if [ -n "$TGT_SECOND_PORT" ]; then
	rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_SECOND_ADDR -f ipv4 -s $TGT_SECOND_PORT nqn.2016-06.io.spdk:cnode1
    fi
    rpc_tgt bdev_null_create Null0 $BDEV_NULL_OPTS
    rpc_tgt nvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode1 Null0
    rpc_tgt save_config
}

function config_tgt() {
    local CONFIG=""

    rpc_tgt framework_start_init
    CONFIG="$CONFIG\nnvmf_create_transport -t tcp -n 8192 -b 128"
    for ((i=0;i<$SUBSYS;i++))
    do
	CONFIG="$CONFIG\nnvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode$i"
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nnvmf_subsystem_add_listener -t tcp -a $TGT_ADDR -f ipv4 -s $((TGT_PORT+j)) nqn.2016-06.io.spdk:cnode$i"
	    if [ -n "$TGT_SECOND_PORT" ]; then
		CONFIG="$CONFIG\nnvmf_subsystem_add_listener -t tcp -a $TGT_SECOND_ADDR -f ipv4 -s $((TGT_SECOND_PORT+j)) nqn.2016-06.io.spdk:cnode$i"
	    fi
	done
	if [ -n "$VERIFY" ]; then
		CONFIG="$CONFIG\nbdev_malloc_create -b Null$i 8192 512"
	else
		CONFIG="$CONFIG\nbdev_null_create Null$i $BDEV_NULL_OPTS"
	fi
	CONFIG="$CONFIG\nnvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode$i Null$i"
    done
    rpc_tgt_batch "$CONFIG"
    rpc_tgt save_config
}

function config_tgt_delay() {
    #rpc_tgt sock_impl_set_options -i posix #--disable-zerocopy-send-server --enable-quickack --disable-recv-pipe
    rpc_tgt framework_start_init
    rpc_tgt nvmf_create_transport -t tcp -n 8192 -b 512 --max-queue-depth 512
    for ((i=0;i<$SUBSYS;i++))
    do
	rpc_tgt nvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode$i
	rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode$i
	if [ -n "$TGT_SECOND_PORT" ]; then
		rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_SECOND_ADDR -f ipv4 -s $TGT_SECOND_PORT nqn.2016-06.io.spdk:cnode$i
	fi
	rpc_tgt bdev_null_create Null0 $BDEV_NULL_OPTS
	rpc_tgt bdev_delay_create -b Null0 -d Delay$i -r 300 -t 300 -w 300 -n 300
	rpc_tgt nvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode$i Delay$i
    done
    rpc_tgt save_config
}

function start_snap_service() {
    $(ssh_prefix $SNAP_SSH) sudo systemctl start mlnx_snap
}

function stop_snap_service() {
    $(ssh_prefix $SNAP_SSH) sudo systemctl stop mlnx_snap
}

function start_snap() {
    $(ssh_prefix $SNAP_SSH) sudo $XLIO_OPTS \
	 $SNAP_ENV_OPTS \
	 nice --20 $SNAP_BIN_PATH/snap_service -m $SNAP_MASK -u --wait-for-rpc \
	 2>&1 | tee snap.log &
    SNAP_PID=$!
    echo "SNAP PID is $SNAP_PID" 2>&1 | tee -a snap.log
    for i in $(seq 10); do
	if rpc_snap_spdk spdk_get_version; then
	    return
	fi
	sleep 1
    done
}

function stop_snap() {
    local SIGNAL=${1:-15}
    rpc_snap_spdk spdk_kill_instance $SIGNAL
    echo "Waiting for SNAP: $SNAP_PID"
    wait $SNAP_PID 
}

function snap_enable_debug() {
    if [ -z "$SNAP_DEBUG" ]; then return; fi

    local CONFIG=""
    CONFIG="$CONFIG\nlog_set_flag bdev"
    CONFIG="$CONFIG\nlog_set_flag bdev_nvme"
    CONFIG="$CONFIG\nlog_set_flag nvme"
    CONFIG="$CONFIG\nlog_set_flag vbdev_crypto"
    #CONFIG="$CONFIG\nlog_set_flag sock"
    #CONFIG="$CONFIG\nlog_set_flag xlio"
    CONFIG="$CONFIG\nlog_set_flag accel"
    CONFIG="$CONFIG\nlog_set_flag accel_mlx5"
    CONFIG="$CONFIG\nlog_set_flag mlx5"
    CONFIG="$CONFIG\nlog_set_level DEBUG"
    CONFIG="$CONFIG\nlog_set_print_level DEBUG"
    rpc_snap_spdk_batch "$CONFIG"
}

function config_snap() {
    local CONFIG=""

    snap_enable_debug
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL $SOCK_EXTRA_OPTS"
    CONFIG="$CONFIG\nmlx5_scan_accel_module $ACCEL_OPTS"
    rpc_snap_spdk_batch "$CONFIG"

    rpc_snap_spdk framework_start_init

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"

    for ((i=0;i<$SUBSYS;i++))
    do
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $((TGT_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    if [ -n "$TGT_SECOND_PORT" ]; then
		    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
			    -a $TGT_SECOND_ADDR -s $((TGT_SECOND_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    fi
	done
	if [ -n "$MULTIPATH_OPTS" ]; then
	    CONFIG="$CONFIG\nbdev_nvme_set_multipath_policy -b Nvme${i}n1 $MULTIPATH_OPTS"
	fi
    done

    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap -nn 2048 -mnan 2048 --model_number Mellanox_NVMe_SNAP"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap --pf_id 0 --num_queues 31 --mdts $MDTS"
    for ((i=0;i<$SUBSYS;i++))
    do
	    CONFIG="$CONFIG\nspdk_bdev_create Nvme${i}n1"
	    nsid=$((i+1))
	    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap --bdev_name Nvme${i}n1 --nsid $nsid --uuid $(uuidgen -r)"
	    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl1 --nsid $nsid"
    done

    rpc_snap_snap_batch "$CONFIG"
}

function config_snap_crypto() {
    local CONFIG=""

    snap_enable_debug
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL $SOCK_EXTRA_OPTS"
    CONFIG="$CONFIG\nmlx5_scan_accel_module $ACCEL_OPTS"
    CONFIG="$CONFIG\nframework_start_init"
    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"
    CONFIG="$CONFIG\naccel_crypto_key_create --name Key0 --cipher AES_XTS \
		  --key 00112233445566778899001122334455 \
		  --key2 11223344556677889900112233445500 --tweak-offset 0"
    for ((i=0;i<$SUBSYS;i++))
    do
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $((TGT_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    if [ -n "$TGT_SECOND_PORT" ]; then
		    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
			    -a $TGT_SECOND_ADDR -s $((TGT_SECOND_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    fi
	done
	if [ -n "$MULTIPATH_OPTS" ]; then
	    CONFIG="$CONFIG\nbdev_nvme_set_multipath_policy -b Nvme${i}n1 $MULTIPATH_OPTS"
	fi
	CONFIG="$CONFIG\nbdev_crypto_create --key-name Key0 Nvme${i}n1 Crypto${i}"
    done
    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap --model_number Mellanox_NVMe_SNAP"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap --pf_id 0 --num_queues 31 --mdts $MDTS"
    for ((i=0;i<$SUBSYS;i++))
    do
	    CONFIG="$CONFIG\nspdk_bdev_create Crypto${i}"
	    nsid=$((i+1))
	    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap --bdev_name Crypto${i} --nsid $nsid --uuid $(uuidgen -r)"
	    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl1 --nsid $nsid"
    done
    rpc_snap_snap_batch "$CONFIG"
}

function config_snap_crypto_sw() {
    local CONFIG=""

    snap_enable_debug
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL $SOCK_EXTRA_OPTS"
    CONFIG="$CONFIG\naccel_assign_opc -o encrypt -m software"
    CONFIG="$CONFIG\naccel_assign_opc -o decrypt -m software"
    CONFIG="$CONFIG\nframework_start_init"
    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"
    CONFIG="$CONFIG\naccel_crypto_key_create --name Key0 --cipher AES_XTS \
		  --key 00112233445566778899001122334455 \
		  --key2 11223344556677889900112233445500"
    for ((i=0;i<$SUBSYS;i++))
    do
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $((TGT_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    if [ -n "$TGT_SECOND_PORT" ]; then
		    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
			    -a $TGT_SECOND_ADDR -s $((TGT_SECOND_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    fi
	done
	if [ -n "$MULTIPATH_OPTS" ]; then
	    CONFIG="$CONFIG\nbdev_nvme_set_multipath_policy -b Nvme${i}n1 $MULTIPATH_OPTS"
	fi
	CONFIG="$CONFIG\nbdev_crypto_create --key-name Key0 Nvme${i}n1 Crypto${i}"
    done
    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap --model_number Mellanox_NVMe_SNAP"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap --pf_id 0 --num_queues 31 --mdts $MDTS"
    for ((i=0;i<$SUBSYS;i++))
    do
	    CONFIG="$CONFIG\nspdk_bdev_create Crypto${i}"
	    nsid=$((i+1))
	    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap --bdev_name Crypto${i} --nsid $nsid --uuid $(uuidgen -r)"
	    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl1 --nsid $nsid"
    done
    rpc_snap_snap_batch "$CONFIG"
}


function config_snap_vfs() {
    local CONFIG=""

    snap_enable_debug
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL $SOCK_EXTRA_OPTS"
    CONFIG="$CONFIG\nmlx5_scan_accel_module $ACCEL_OPTS"
    rpc_snap_spdk_batch "$CONFIG"

    rpc_snap_spdk framework_start_init

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"

    for ((i=0;i<$SUBSYS;i++))
    do
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $((TGT_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    if [ -n "$TGT_SECOND_PORT" ]; then
		    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
			    -a $TGT_SECOND_ADDR -s $((TGT_SECOND_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	    fi
	done
	if [ -n "$MULTIPATH_OPTS" ]; then
	    CONFIG="$CONFIG\nbdev_nvme_set_multipath_policy -b Nvme${i}n1 $MULTIPATH_OPTS"
	fi
    done

    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap --model_number Mellanox_NVMe_SNAP"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap --pf_id 0 --ctrl NVMeCtrlMain0 --num_queues 31 --mdts $MDTS"
    for ((i=0;i<$SUBSYS;i++))
    do
	CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap:cnode${i}"
	CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap:cnode${i} --pf_id 0 --vf_id $i --ctrl NVMeCtrl${i} --num_queues 31 --mdts $MDTS"
	CONFIG="$CONFIG\nspdk_bdev_create Nvme${i}n1"
	nsid=1
	CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap:cnode${i} --bdev_name Nvme${i}n1 --nsid $nsid --uuid $(uuidgen -r)"
	CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl${i} --nsid $nsid"
    done

    rpc_snap_snap_batch "$CONFIG"
}

function config_snap_vfs_qos_demo() {
    local CONFIG=""

    snap_enable_debug
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL $SOCK_EXTRA_OPTS"
    CONFIG="$CONFIG\nmlx5_scan_accel_module $ACCEL_OPTS"
    rpc_snap_spdk_batch "$CONFIG"

    rpc_snap_spdk framework_start_init

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"

    for ((i=0;i<4;i++)); do
	CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme${i} -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $TGT_PORT -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
    done

    CONFIG="$CONFIG\nbdev_group_create vf0"
    CONFIG="$CONFIG\nbdev_group_create vf1"
    CONFIG="$CONFIG\nbdev_group_add_bdev vf0 Nvme0n1"
    CONFIG="$CONFIG\nbdev_group_add_bdev vf0 Nvme1n1"
    CONFIG="$CONFIG\nbdev_group_add_bdev vf1 Nvme2n1"
    CONFIG="$CONFIG\nbdev_group_add_bdev vf1 Nvme3n1"
    if [ -n "$QOS" ]; then
		VF0_QOS_LIMIT=$(echo $QOS | cut -d ":" -f 1)
		VF1_QOS_LIMIT=$(echo $QOS | cut -d ":" -f 2)
		CONFIG="$CONFIG\nbdev_group_set_qos_limit --rw-ios-per-sec $VF0_QOS_LIMIT vf0"
		CONFIG="$CONFIG\nbdev_group_set_qos_limit --rw-ios-per-sec $VF1_QOS_LIMIT vf1"
    fi
    rpc_snap_spdk_batch "$CONFIG"

    CONFIG=""
    # PF
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap --model_number Mellanox_NVMe_SNAP"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap --pf_id 0 --ctrl NVMeCtrlMain0 --num_queues 31 --mdts $MDTS"
    # VF 0
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap:cnode0"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap:cnode0 --pf_id 0 --vf_id 0 --ctrl NVMeCtrl0 --num_queues 31 --mdts $MDTS"
    CONFIG="$CONFIG\nspdk_bdev_create Nvme0n1"
    CONFIG="$CONFIG\nspdk_bdev_create Nvme1n1"
    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap:cnode0 --bdev_name Nvme0n1 --nsid 1 --uuid $(uuidgen -r)"
    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap:cnode0 --bdev_name Nvme1n1 --nsid 2 --uuid $(uuidgen -r)"
    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl0 --nsid 1"
    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl0 --nsid 2"
    # VF 1
    CONFIG="$CONFIG\nnvme_subsystem_create --nqn nqn.2020-12.mlnx.snap:cnode1"
    CONFIG="$CONFIG\nnvme_controller_create --nqn nqn.2020-12.mlnx.snap:cnode1 --pf_id 0 --vf_id 1 --ctrl NVMeCtrl1 --num_queues 31 --mdts $MDTS"
    CONFIG="$CONFIG\nspdk_bdev_create Nvme2n1"
    CONFIG="$CONFIG\nspdk_bdev_create Nvme3n1"
    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap:cnode1 --bdev_name Nvme2n1 --nsid 1 --uuid $(uuidgen -r)"
    CONFIG="$CONFIG\nnvme_namespace_create --nqn nqn.2020-12.mlnx.snap:cnode1 --bdev_name Nvme3n1 --nsid 2 --uuid $(uuidgen -r)"
    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl1 --nsid 1"
    CONFIG="$CONFIG\nnvme_controller_attach_ns --ctrl NVMeCtrl1 --nsid 2"
    rpc_snap_snap_batch "$CONFIG"
}

function run_nvmeperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}

    if [ -n "$PERF_SSH" ]; then
	$(ssh_prefix $PERF_SSH) sudo $PERF_ENV_OPTS $XLIO_OPTS $PERF_BIN_PATH/spdk_nvme_perf \
	     -S $SOCK_IMPL -r \"trtype:$TCP adrfam:ipv4 traddr:$ADDR trsvcid:$PORT\" \
	     -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -M 50 -t $PERF_TIME \
	     $NVME_PERF_EXTRA_OPTS 2>&1 | tee perf.log&
    else
	sudo $PERF_ENV_OPTS $XLIO_OPTS $PERF_BIN_PATH/spdk_nvme_perf \
	     -S $SOCK_IMPL -r "trtype:$TCP adrfam:ipv4 traddr:$ADDR trsvcid:$PORT" \
	     -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -M 50 -t $PERF_TIME \
	     $NVME_PERF_EXTRA_OPTS 2>&1 | tee perf.log&
    fi

    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
}

function run_nvmeperf_snap() {
    local ADDR=${ADDR:-$PCI_ADDR}

    sudo $PERF_BIN_PATH/spdk_nvme_perf \
	 -r "trtype:pcie traddr:$ADDR" \
	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -M 50 -t $PERF_TIME \
	 -A 4096 $NVME_PERF_EXTRA_OPTS 2>&1 | tee perf.log&
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
}

function wait_nvmeperf() {
    echo "Waiting for perf $PERF_PID"
    wait $PERF_PID
}

function report_nvmeperf() {
    local OUT=$(grep "Total" perf.log | awk '{ print $3 " | " $4 " | " $5 " | " }')
    echo "| $TEST | $PERF_MASK | $TGT_MASK | $SNAP_MASK | $RW | $IO_SIZE | $QUEUE_DEPTH | $OUT" >> report.log
}

function run_fio_nvme() {
    local DEV=${DEV:-Snap}
    local ADDR=${ADDR:-$PCI_ADDR}

    sudo LD_PRELOAD=$PERF_SPDK_PATH/build/fio/spdk_nvme $FIO --stats=1 --thread=1 \
	 --group_reporting=1 --ioengine=spdk_nvme --direct=1 --norandommap \
         --name=$DEV --filename="trtype=PCIe traddr=${ADDR//:/.}" \
         --readwrite=$RW --bs=$IO_SIZE --iodepth=$QUEUE_DEPTH --ramp_time=20 \
         --numjobs=$FIO_JOBS --cpus_allowed=$FIO_CPUS --cpus_allowed_policy=split \
         --iomem_align=4096 --time_based=1 --runtime=${PERF_TIME}s 2>&1 | tee fio.log &

    FIO_PID=$!
    echo "Fio PID is $FIO_PID" 2>&1 | tee -a perf.log
}

function generate_fio_config_nvme_pci() {
    cat <<EOF > fio_spdk_conf.json
{
  "subsystems": [ {
    "subsystem": "bdev",
    "config": [
        {
          "method": "bdev_set_options",
          "params": {
            "bdev_auto_examine": false
          }
        },
        {
          "method": "bdev_nvme_attach_controller",
          "params": {
            "trtype": "pcie",
            "name":"Nvme0",
            "traddr":"$PCI_ADDR"
          }
        } ]
  } ]
}
EOF
}

function generate_fio_config_nvme_pci_vfs() {
    cat <<EOF > fio_spdk_conf.json
{
  "subsystems": [ {
    "subsystem": "bdev",
    "config": [
EOF
    for ((i=0;i<$((SUBSYS-1));i++))
    do
	cat <<EOF >> fio_spdk_conf.json
    {
      "method": "bdev_nvme_attach_controller",
      "params": {
        "trtype": "pcie",
        "name":"Nvme${i}",
        "traddr":"${PCI_VF_ADDRS[$i]}"
      }
    },
EOF
    done

    cat <<EOF >> fio_spdk_conf.json
    {
      "method": "bdev_nvme_attach_controller",
      "params": {
        "trtype": "pcie",
        "name":"Nvme$((SUBSYS-1))",
        "traddr":"${PCI_VF_ADDRS[$((SUBSYS-1))]}"
      }
    } ]
  } ]
}
EOF
}

function generate_fio_config_nvme_pci_vfs_qos_demo() {
    cat <<EOF > fio_spdk_conf.json
{
  "subsystems": [ {
    "subsystem": "bdev",
    "config": [
    {
      "method": "bdev_nvme_attach_controller",
      "params": {
        "trtype": "pcie",
        "name":"Nvme0",
        "traddr":"${PCI_VF_ADDRS[0]}"
      }
    },
    {
      "method": "bdev_nvme_attach_controller",
      "params": {
        "trtype": "pcie",
        "name":"Nvme1",
        "traddr":"${PCI_VF_ADDRS[1]}"
      }
    } ]
  } ]
}
EOF
}

function generate_fio_job() {
    cat <<EOF > fio.job
[global]
direct=1
thread=1
ioengine=spdk_bdev
spdk_json_conf=fio_spdk_conf.json
norandommap
stats=1
group_reporting
time_based
runtime=$PERF_TIME
ramp_time=$WARMUP_TIME
numjobs=$FIO_JOBS
cpus_allowed=$FIO_CPUS
cpus_allowed_policy=split
rw=$RW
bs=$IO_SIZE
iodepth=$QUEUE_DEPTH

[job1]
EOF

    for ((i=1;i<=$SUBSYS;i++)); do
	echo "filename=Nvme0n$i" >> fio.job
    done
    echo "file_service_type=random:16" >> fio.job
}


function generate_fio_job_vfs() {
    cat <<EOF > fio.job
[global]
direct=1
thread=1
ioengine=spdk_bdev
spdk_json_conf=fio_spdk_conf.json
norandommap
stats=1
group_reporting
time_based
runtime=$PERF_TIME
ramp_time=$WARMUP_TIME
numjobs=$FIO_JOBS
cpus_allowed=$FIO_CPUS
cpus_allowed_policy=split
rw=$RW
bs=$IO_SIZE
iodepth=$QUEUE_DEPTH

EOF

    for ((i=0;i<$SUBSYS;i++)); do
	echo "[job${i}]" >> fio.job
	echo "filename=Nvme${i}n1" >> fio.job
	echo "file_service_type=random:16" >> fio.job
    done
}


function generate_fio_job_vfs_qos_demo() {
    cat <<EOF > fio.job
[global]
direct=1
thread=1
ioengine=spdk_bdev
spdk_json_conf=fio_spdk_conf.json
norandommap
stats=1
#group_reporting
time_based
runtime=$PERF_TIME
#ramp_time=$WARMUP_TIME
numjobs=$FIO_JOBS
cpus_allowed=$FIO_CPUS
cpus_allowed_policy=split
rw=$RW
bs=$IO_SIZE
iodepth=$QUEUE_DEPTH

[job1]
filename=Nvme0n1
file_service_type=random:16
ramp_time=4

[job2]
filename=Nvme0n2
file_service_type=random:16
startdelay=1
ramp_time=3

[job3]
filename=Nvme1n1
file_service_type=random:16
startdelay=2
ramp_time=2

[job4]
filename=Nvme1n2
file_service_type=random:16
startdelay=3
ramp_time=1
EOF
}

function run_fio_bdev() {
    local FIO_CONF_GENERATOR=${FIO_CONF_GENERATOR:-generate_fio_config_nvme_pci}
    local FIO_JOB_GENERATOR=${FIO_JOB_GENERATOR:-generate_fio_job}
    $FIO_CONF_GENERATOR
    $FIO_JOB_GENERATOR
    $(ssh_prefix $PERF_SSH) sudo LD_PRELOAD=$PERF_SPDK_PATH/build/fio/spdk_bdev $FIO fio.job \
	 --output-format=json --output=fio_result.json \
	 $FIO_EXTRA_OPTS 2>&1 | tee fio.log &
    FIO_PID=$!
    echo "Fio PID is $FIO_PID" 2>&1 | tee -a fio.log

}

function wait_fio() {
    echo "Waiting for fio $FIO_PID"
    wait $FIO_PID
}

function report_fio() {
    local IOPS_R=$(jq ".jobs[0].read.iops / 1000 | round" fio_result.json)
    local BW_R=$(jq ".jobs[0].read.bw / 1024 | round" fio_result.json)
    local LAT_AVG_R=$(jq ".jobs[0].read.clat_ns.mean / 1000 | round" fio_result.json)
    local LAT9900_R=$(jq ".jobs[0].read.clat_ns.percentile[\"99.000000\"] / 100 | round / 10" fio_result.json)
    local LAT9999_R=$(jq ".jobs[0].read.clat_ns.percentile[\"99.990000\"] / 100 | round / 10" fio_result.json)

    local IOPS_W=$(jq ".jobs[0].write.iops / 1000 | round" fio_result.json)
    local BW_W=$(jq ".jobs[0].write.bw / 1024 | round" fio_result.json)
    local LAT_AVG_W=$(jq ".jobs[0].write.clat_ns.mean / 1000 | round" fio_result.json)
    local LAT9900_W=$(jq ".jobs[0].write.clat_ns.percentile[\"99.000000\"] / 100 | round / 10" fio_result.json)
    local LAT9999_W=$(jq ".jobs[0].write.clat_ns.percentile[\"99.990000\"] / 100 | round / 10" fio_result.json)

    echo -n "| $TEST | $FIO_JOBS@$FIO_CPUS | $TGT_MASK | $SNAP_MASK | $SUBSYS | $RW | $IO_SIZE | $QUEUE_DEPTH | " >> report.log
    case "$RW" in
	*"read")
	    echo "$IOPS_R | $BW_R | $LAT_AVG_R | $LAT9900_R | $LAT9999_R" >> report.log
	    ;;
	*"write")
	    echo "$IOPS_W | $BW_W | $LAT_AVG_W | $LAT9900_W | $LAT9999_W" >> report.log
	    ;;
	*"rw")
	    echo "$IOPS_R+$IOPS_W | $BW_R+$BW_W | $LAT_AVG_R/$LAT_AVG_W | $LAT9900_R/$LAT9900_W | $LAT9999_R/$LAT9999_W" >> report.log
	    ;;
    esac
}

function run_bdevperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}
    local CONFIG=""

    $(ssh_prefix $PERF_SSH) sudo $PERF_ENV_OPTS $XLIO_OPTS $PERF_SPDK_PATH/build/examples/bdevperf \
	 -r /var/tmp/bdevperf.sock --wait-for-rpc -m $PERF_MASK -C \
	 -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $BDEV_PERF_EXTRA_OPTS 2>&1 | tee perf.log &
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
    sleep 3
    CONFIG="$CONFIG\nsock_set_default_impl -i $SOCK_IMPL"
    CONFIG="$CONFIG\nsock_impl_set_options -i $SOCK_IMPL \
	     --enable-zerocopy-recv \
	     --disable-zerocopy-send-server \
	     --disable-zerocopy-send-client \
	     $SOCK_EXTRA_OPTS"

    CONFIG="$CONFIG\nmlx5_scan_accel_module --qp-size 1024 --num-requests 8192"
    CONFIG="$CONFIG\nframework_start_init"
    rpc_perf_batch "$CONFIG"

    CONFIG=""
    CONFIG="$CONFIG\naccel_get_module_info"
    CONFIG="$CONFIG\naccel_get_opc_assignments"
    CONFIG="$CONFIG\nbdev_nvme_set_options -k 0"

    for ((i=0;i<$SUBSYS;i++))
    do
	for ((j=0; j<PATHS; j++)); do
	    CONFIG="$CONFIG\nbdev_nvme_attach_controller $BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS -b Nvme$i -t $TCP -f ipv4 \
		    -a $TGT_ADDR -s $((TGT_PORT + j)) -n nqn.2016-06.io.spdk:cnode$i -x multipath $DATA_DGST --fabrics-timeout $CONNECT_TIMEOUT"
	done
	if [ -n "$MULTIPATH_OPTS" ]; then
	    CONFIG="$CONFIG\nbdev_nvme_set_multipath_policy -b Nvme${i}n1 $MULTIPATH_OPTS"
	fi
    done

    rpc_perf_batch "$CONFIG"

    $(ssh_prefix $PERF_SSH) sudo PYTHONPATH="$PYTHONPATH:$PERF_SPDK_PATH/scripts" \
	 $PERF_SPDK_PATH/examples/bdev/bdevperf/bdevperf.py \
	 -s /var/tmp/bdevperf.sock -t 3600 perform_tests 2>&1 | tee -a perf.log &
    RPC_TASK_PID=$!
    echo "RPC task PID is $RPC_TASK_PID" 2>&1 | tee -a perf.log
}

function wait_bdevperf() {
    echo "Waiting for RPC task $RPC_TASK_PID"
    wait $RPC_TASK_PID
    rpc_perf spdk_kill_instance 15
    sleep 3
    echo "Waiting for perf $PERF_PID"
    wait $PERF_PID
}

function report_bdevperf() {
    local OUT=$(grep "Total" perf.log | tail -2 | head -1 | awk '{ print $4 " | " $8 " | " }')
    echo "| $TEST | $PERF_MASK | $TGT_MASK | $SNAP_MASK | $RW | $IO_SIZE | $QUEUE_DEPTH | $OUT" >> report.log
}

function basic_test_nvme() {
    start_tgt
    config_tgt
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_nvmeperf # > /dev/null 2>&1
		    wait_nvmeperf
		    report_nvmeperf
		done
	    done
	done
    done
    stop_tgt
}

function basic_test_nvme_snap_service() {
    start_tgt
    config_tgt
    start_snap_service
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_nvmeperf_snap # > /dev/null 2>&1
		    wait_nvmeperf
		    report_nvmeperf
		    rpc_snap_spdk bdev_nvme_get_transport_statistics
		    rpc_snap_spdk bdev_get_iostat
		done
	    done
	done
    done
    stop_snap_service
    stop_tgt
}

function basic_test_nvme_snap() {
    start_tgt
    config_tgt
    start_snap
    config_snap
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_nvmeperf_snap # > /dev/null 2>&1
		    wait_nvmeperf
		    report_nvmeperf
		    rpc_snap_spdk bdev_nvme_get_transport_statistics
		    rpc_snap_spdk bdev_get_iostat
		done
	    done
	done
    done
    stop_snap
    stop_tgt
}

function basic_test_fio_snap() {
    local TGT_CONFIG=${TGT_CONFIG:-config_tgt}
    local SNAP_CONFIG=${SNAP_CONFIG:-config_snap}
    local RWS=$RW
    local RW=""
    local NUM_SUBSYS=$SUBSYS
    local SUBSYS=""
	local PER_REPEAT_REPORTER=${PER_REPEAT_REPORTER:-report_fio}

    for SUBSYS in $NUM_SUBSYS; do
	start_tgt
	$TGT_CONFIG
	start_snap
	if [ -n "$SNAP_DEBUG" ]; then
	    echo "You have 10 seconds to attach debugger"
	    echo 'gdb -p $(pidof snap_service)'
	    sleep 10
	fi
	$SNAP_CONFIG
	for PERF_MASK in $PERF_MASKS; do
	    for RW in $RWS; do
 		for QUEUE_DEPTH in $QUEUE_DEPTHS; do
		    for IO_SIZE in $IO_SIZES; do
			for REP in $(seq $REPEAT); do
			    run_fio_bdev # > /dev/null 2>&1
			    wait_fio
			    $PER_REPEAT_REPORTER
			    rpc_snap_spdk bdev_nvme_get_transport_statistics
			    rpc_snap_spdk bdev_get_iostat
			done
		    done
		done
	    done
	done
	stop_snap
	stop_tgt
    done
}

function basic_test_fio_snap_multipath() {
    start_tgt
    config_tgt_multipath
    start_snap
    config_snap_multipath
    for PERF_MASK in $PERF_MASKS; do
        for QUEUE_DEPTH in $QUEUE_DEPTHS; do
            for IO_SIZE in $IO_SIZES; do
        	for REP in $(seq $REPEAT); do
        	    run_fio_nvme # > /dev/null 2>&1
        	    wait_fio
        	    report_fio
        	done
            done
        done
    done
    stop_snap
    stop_tgt
}

function basic_test_bdev() {
    start_tgt
    config_tgt
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_bdevperf # > /dev/null 2>&1
		    wait_bdevperf
		    report_bdevperf
		done
	    done
	done
    done
    stop_tgt
}

function test_bdev_perf() {
    SOCK_IMPL=xlio \
	     BDEV_PERF_EXTRA_OPTS="-Z $BDEV_PERF_EXTRA_OPTS" \
	     PERF_ENV_OPTS="LD_LIBRARY_PATH=$PWD/install-$BF_HOSTNAME/lib:$PWD/dpdk/build/lib SPDK_XLIO_PATH=$LIBXLIO $PERF_ENV_OPTS" \
             basic_test_bdev
}

function test_perf_snap4() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local ACCEL_OPTS="--qp-size 256 --num-requests 4096"
    #local FIO_EXTRA_OPTS="--log_flags=all"
    if [ -n "$VERIFY" ]; then
	local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
	local RW=randwrite
	local FIO_JOBS=1
	local QUEUE_DEPTHS=1
    fi

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_delay() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local ACCEL_OPTS="--qp-size 256 --num-requests 512"
    local TGT_CONFIG=config_tgt_delay
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 basic_test_fio_snap
}

function test_perf_snap4_digest() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local ACCEL_OPTS="--qp-size 64 --num-requests 512"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_no_mem_domain() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_delay_no_mem_domain() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local TGT_CONFIG=config_tgt_delay
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 basic_test_fio_snap
}

function test_perf_snap4_no_mem_domain_digest() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN=1"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_tcp_mem_domain() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_USE_TCP_MEM_DOMAIN=1 \
	  SNAP_DMA_Q_OPMODE=0"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_tcp_mem_domain_digest() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_USE_TCP_MEM_DOMAIN=1 \
	  SNAP_DMA_Q_OPMODE=0"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local ACCEL_OPTS="--qp-size 512 --num-requests 4096 --allowed-crypto-devs mlx5_2"
    if [ -n "$VERIFY" ]; then
	local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
	local RW=randwrite
	local FIO_JOBS=1
	local QUEUE_DEPTHS=1
    fi

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_delay_crypto() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local ACCEL_OPTS="--qp-size 512 --num-requests 4096 --allowed-crypto-devs mlx5_2"
    local TGT_CONFIG=config_tgt_delay

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_multiblock() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local ACCEL_OPTS="--qp-size 512 --num-requests 4096 --allowed-crypto-devs mlx5_2"
    if [ -n "$VERIFY" ]; then
	local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
	local RW=randwrite
	local FIO_JOBS=1
	local QUEUE_DEPTHS=1
    fi

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_delay_crypto_multiblock() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local ACCEL_OPTS="--qp-size 512 --num-requests 4096 --allowed-crypto-devs mlx5_2"
    local TGT_CONFIG=config_tgt_delay

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_digest() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local ACCEL_OPTS="--qp-size 512 --num-requests 4096 --allowed-crypto-devs mlx5_2"
    if [ -n "$VERIFY" ]; then
	local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
	local RW=randwrite
	local FIO_JOBS=1
	local QUEUE_DEPTHS=1
    fi

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_4k() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local BDEV_NULL_OPTS="8192 4096"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_4k_digest() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto
    local BDEV_NULL_OPTS="8192 4096"
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_sw() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN=1 \
	  SPDK_NVDA_TCP_DISABLE_ACCEL_SEQ=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto_sw
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function test_perf_snap4_crypto_digest_sw() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1 \
	  SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN=1 \
	  SPDK_NVDA_TCP_DISABLE_ACCEL_SEQ=1"
    local BDEV_NVME_ATTACH_CONTROLLER_EXTRA_OPTS="--hdgst --ddgst"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local SNAP_CONFIG=config_snap_crypto_sw
    #local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
    #local FIO_EXTRA_OPTS="--log_flags=all"

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

# Run as SETUP=<your setup> ./test.sh set_trust_level
function set_trust_level() {
    sudo /etc/init.d/openibd stop
    sleep 10
    $(ssh_prefix $SNAP_SSH) sudo devlink dev eswitch set pci/0000:03:00.0 mode legacy
    $(ssh_prefix $SNAP_SSH) sudo devlink dev eswitch set pci/0000:03:00.1 mode legacy
    # These lines seems to be not required but are present in original instruction
    #$(ssh_prefix $SNAP_SSH) sudo sh -c "'echo none > /sys/class/net/p0/compat/devlink/encap'"
    #$(ssh_prefix $SNAP_SSH) sudo sh -c "'echo none > /sys/class/net/p1/compat/devlink/encap'"
    $(ssh_prefix $SNAP_SSH) sudo devlink dev eswitch set pci/0000:03:00.0 mode switchdev
    $(ssh_prefix $SNAP_SSH) sudo devlink dev eswitch set pci/0000:03:00.1 mode switchdev
    $(ssh_prefix $SNAP_SSH) sudo mlxreg -d /dev/mst/mt41692_pciconf0 --reg_name VHCA_TRUST_LEVEL --yes --indexes "vhca_id=0x0,all_vhca=0x1" --set "trust_level=0x1"
    $(ssh_prefix $SNAP_SSH) sudo mlxreg -d /dev/mst/mt41692_pciconf0.1 --reg_name VHCA_TRUST_LEVEL --yes --indexes "vhca_id=0x0,all_vhca=0x1" --set "trust_level=0x1"
    sudo /etc/init.d/openibd start
    sleep 10
    $(ssh_prefix $SNAP_SSH) sudo /opt/mellanox/iproute2/sbin/mlxdevm port add pci/0000:03:00.0 flavour pcisf pfnum 0 sfnum 0
    $(ssh_prefix $SNAP_SSH) sudo /opt/mellanox/iproute2/sbin/mlxdevm port add pci/0000:03:00.1 flavour pcisf pfnum 1 sfnum 0
    $(ssh_prefix $SNAP_SSH) sudo /opt/mellanox/iproute2/sbin/mlxdevm port function set pci/0000:03:00.0/360480 state active
    $(ssh_prefix $SNAP_SSH) sudo /opt/mellanox/iproute2/sbin/mlxdevm port function set pci/0000:03:00.1/426016 state active
    $(ssh_prefix $SNAP_SSH) sudo sh -c "'echo mlx5_core.sf.2 > /sys/bus/auxiliary/drivers/mlx5_core.sf_cfg/unbind'"
    $(ssh_prefix $SNAP_SSH) sudo sh -c "'echo mlx5_core.sf.3 > /sys/bus/auxiliary/drivers/mlx5_core.sf_cfg/unbind'"
    $(ssh_prefix $SNAP_SSH) sudo sh -c "'echo mlx5_core.sf.2 > /sys/bus/auxiliary/drivers/mlx5_core.sf/bind'"
    $(ssh_prefix $SNAP_SSH) sudo sh -c "'echo mlx5_core.sf.3 > /sys/bus/auxiliary/drivers/mlx5_core.sf/bind'"
}

function test_perf_snap4_vfs() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local FIO_SPDK_CONF="$PWD/fio_spdk_conf.json"
    local FIO_BDEV_JOBS_CONF="$PWD/fio_bdev_jobs"
    local ACCEL_OPTS="--qp-size 256 --num-requests 4096"
    local SNAP_CONFIG=config_snap_vfs
    local FIO_CONF_GENERATOR=generate_fio_config_nvme_pci_vfs
    local FIO_JOB_GENERATOR=generate_fio_job_vfs
    #local FIO_EXTRA_OPTS="--log_flags=all"
    if [ -n "$VERIFY" ]; then
	local FIO_EXTRA_OPTS="--verify=crc32c --verify_backlog=1"
	local RW=randwrite
	local FIO_JOBS=1
	local QUEUE_DEPTHS=1
    fi

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function iops_by_dev_name() {
	local FIO_RES_FNAME=${FIO_RES_FNAME:-fio_result.json}
	DEV_NAME=$1
	IOPS=0
	for iops in $(cat $FIO_RES_FNAME | jq --arg dev_name $DEV_NAME '.jobs[] | select(."job options".filename==$dev_name)' | jq .read.iops);
	do
		IIOPS=$(echo $iops | cut -d '.' -f 1)
		IOPS=$(expr $IOPS + $IIOPS)
	done
	echo $IOPS
}

function test_perf_snap4_vfs_qos_demo_post_repeat() {
	rpc_snap_spdk bdev_get_iostat -b Nvme0n1 -c
	rpc_snap_spdk bdev_get_iostat -b Nvme1n1 -c
	rpc_snap_spdk bdev_get_iostat -b Nvme2n1 -c
	rpc_snap_spdk bdev_get_iostat -b Nvme3n1 -c

	BD0=$(iops_by_dev_name Nvme0n1)
	BD1=$(iops_by_dev_name Nvme0n2)
	BD2=$(iops_by_dev_name Nvme1n1)
	BD3=$(iops_by_dev_name Nvme1n2)

	GR0=$(expr $BD0 + $BD1)
	GR1=$(expr $BD2 + $BD3)

	TOTAL=$(expr $GR0 + $GR1)

	echo "-----------------":
	printf "IOPs reported:\n"
	printf "\ttotal:     %s\n" $TOTAL
	printf "\tper group: %s %s\n" $GR0 $GR1
	printf "\tper pdev:  %s %s %s %s\n" $BD0 $BD1 $BD2 $BD3
	echo "-----------------":
}

function test_perf_snap4_vfs_qos_demo() {
    local EXTRA_SNAP_OPTS="SPDK_XLIO_PATH=$LIBXLIO \
	  SNAP4_RDMA_ZCOPY_ENABLE=1 \
	  SNAP4_TCP_XLIO_ENABLE=1 \
	  MLX5_SHUT_UP_BF=1"
    local ACCEL_OPTS="--qp-size 256 --num-requests 4096"
    local SNAP_CONFIG=config_snap_vfs_qos_demo
    local FIO_CONF_GENERATOR=generate_fio_config_nvme_pci_vfs_qos_demo
    local FIO_JOB_GENERATOR=generate_fio_job_vfs_qos_demo
    local PER_REPEAT_REPORTER=test_perf_snap4_vfs_qos_demo_post_repeat

    SNAP_ENV_OPTS="$SNAP_ENV_OPTS $EXTRA_SNAP_OPTS" \
		 SOCK_IMPL=xlio \
		 SOCK_EXTRA_OPTS="--enable-zerocopy-recv --enable-zerocopy-send-client" \
		 basic_test_fio_snap
}

function cleanup() {
    sudo kill -9 $(pidof fio)
    stop_snap 9
    stop_tgt 9
}

trap cleanup SIGINT

if [ -n "$1" ]; then
    tests="$@"
else
    tests="test1 \
	   test2 \
	   test3 \
	   test4 \
	   test5 \
	   test6 \
	   test7 \
	   test8 \
	   test9 \
	   test10 \
	   test11 \
	   test12 \
	   test13 \
	   test14"
fi

rm -rf rpc.log rpc_snap.log rpc_tgt.log perf.log tgt.log snap.log report.log fio.log
echo "| Test | Perf CPU | TGT CPU | SNAP CPU | Subsys | RW | IO size | QD | KIOPS | BW | Lat_avg | Lat_99 | Lat_99.99 |" >> report.log
echo "Running tests: $tests"
for t in $tests; do
    TEST="$t"
    echo "===== Start $t ====="
    $t | tee test.log
    echo "===== End $t ====="
    echo ""
done

cat report.log
