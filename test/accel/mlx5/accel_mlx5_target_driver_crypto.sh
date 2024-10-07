#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

allowed_devices=${1:-"mlx5_0"}

MALLOC_BDEV_SIZE=256
MALLOC_BLOCK_SIZE=512
app_sock=/var/tmp/bdev.sock
bdevperf=$rootdir/build/examples/bdevperf
nvmeperf=$rootdir/build/bin/spdk_nvme_perf
testdma=$rootdir/test/dma/test_dma/test_dma

function gen_bdevperf_json() {

	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_nvme_attach_controller",
		          "params": {
		            "name": "Nvme0",
		            "trtype": "$TEST_TRANSPORT",
		            "adrfam": "IPv4",
		            "traddr": "$NVMF_FIRST_TARGET_IP",
		            "trsvcid": "$NVMF_PORT",
		            "subnqn": "nqn.2016-06.io.spdk:cnode0"
		          }
		        },
		        {
		          "method": "bdev_set_options",
		          "params": {
		            "bdev_auto_examine": false
		          }
		        },
		        {
		          "method": "bdev_wait_for_examine"
		        }
		      ]
		    }
		  ]
		}
	JSON
}

validate_crypto_umr_stats() {
	rpc_sock=$1
	stats=$($rpc_py -s $rpc_sock accel_mlx5_dump_stats -l total)

	val=$(echo $stats | jq -r '.total.umrs.crypto_umrs')
	if [ "$val" == 0 ]; then
		echo "Unexpected number of crypto_umrs: $val, expected > 0"
		return 1
	fi
	val=$(echo $stats | jq -r '.total.umrs.sig_umrs')
	if [ "$val" != 0 ]; then
		echo "Unexpected number of sig_umrs: $val, expected 0"
		return 1
	fi
	val=$(echo $stats | jq -r '.total.rdma.total')
	if [ "$val" != 0 ]; then
		echo "Unexpected number of RDMA operations: $val, expected 0"
		return 1
	fi
	val=$(echo $stats | jq -r '.total.tasks.crypto_mkey')
	if [ $val != 0 ] && [ $val != $(echo $stats | jq -r '.total.tasks.total') ]; then
		echo "Unexpected number of tasks operations: $val, expected > 0 and no other tasks"
		return 1
	fi
}

if [ "$TEST_TRANSPORT" != "rdma" ]; then
	exit 0
fi

nvmftestinit
nvmfappstart -m 0xf0 --wait-for-rpc

$rpc_py mlx5_scan_accel_module --enable-driver --allowed-devs $allowed_devices
$rpc_py bdev_set_options --disable-auto-examine
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS --in-capsule-data-size 0
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py accel_crypto_key_create -c AES_XTS -k 00112233445566778899001122334455 -e 11223344556677889900112233445500 -n test_dek
$rpc_py bdev_crypto_create Malloc0 Crypto0 -n test_dek
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Crypto0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

sleep 1

$bdevperf --json <(gen_bdevperf_json) -q 64 -o 4096 -t 5 -w randrw -M 50 -m 0xf -r $app_sock
$bdevperf --json <(gen_bdevperf_json) -q 64 -o 65536 -t 5 -w verify -m 0xf -r $app_sock
$nvmeperf -q 64 -o 4096 -O 500 -w randrw -M 50 -t 5 -r "trtype:$TEST_TRANSPORT  traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT adrfam:ipv4" -c 0xf

# Test mkey corruption. From my observation, HW doesn't verify the mkey when registering UMR, so it allows us
# to send an invalid mkey to the target. Later target receives a WC with "remote access error" status and handles
# requests which are in the process of transferring data to/from accel
$testdma --json <(gen_bdevperf_json) -q 64 -o 4096 -t 10 -w randrw -M 50 -m 0xf -r $app_sock -b "Nvme0n1" -f -x translate -Y 50000 &
testdma_pid=$!
waitforlisten $testdma_pid $app_sock
sleep 5
validate_crypto_umr_stats $DEFAULT_RPC_ADDR
sleep 1
wait $testdma_pid || true

## By killing the target, we trigger qpair disconnect with outstanding IOs and test that nvme_rdma<->accel_mlx5
## interaction works well. No hang or crash expected.
$bdevperf --json <(gen_bdevperf_json) -q 64 -o 4096 -t 60 -w rw -M 50 -m 0xf -r $app_sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid $app_sock
sleep 5
validate_crypto_umr_stats $DEFAULT_RPC_ADDR
sleep 1
killprocess $nvmfpid
wait $bdev_perf_pid || true

nvmfappstart -m 0x20 --wait-for-rpc
# Test small qp size and number of MRs
$rpc_py mlx5_scan_accel_module --enable-driver --allowed-devs $allowed_devices --qp-size 16 --cq-size 8 --num-requests 16
$rpc_py bdev_set_options --disable-auto-examine
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS --in-capsule-data-size 0
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py accel_crypto_key_create -c AES_XTS -k 00112233445566778899001122334455 -e 11223344556677889900112233445500 -n test_dek
$rpc_py bdev_crypto_create Malloc0 Crypto0 -n test_dek
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Crypto0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$bdevperf --json <(gen_bdevperf_json) -q 128 -o 4096 -t 10 -w verify -m 0x2 -r $app_sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid $app_sock
sleep 5
validate_crypto_umr_stats $DEFAULT_RPC_ADDR
sleep 1
killprocess $nvmfpid
wait $bdev_perf_pid || true

nvmftestfini

trap - SIGINT SIGTERM EXIT
