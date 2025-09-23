#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"
bdevperf_rpc_sock=/var/tmp/bdevperf.sock
backend_rpc_sock="/var/tmp/nvmf_backend.sock"
backend_rpc="$rpc_py -s $backend_rpc_sock"

function nvmf_backend_start() {
	"${NVMF_TARGET_NS_CMD[@]}" $SPDK_BIN_DIR/nvmf_tgt "$@" -r $backend_rpc_sock &
	nvmf_backend_pid=$!
	waitforlisten $nvmf_backend_pid $backend_rpc_sock
	trap 'killprocess $nvmf_backend_pid' SIGINT SIGTERM EXIT
	$backend_rpc nvmf_create_transport $NVMF_TRANSPORT_OPTS
	$backend_rpc bdev_null_create null0 4096 4096
	$backend_rpc nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a
	$backend_rpc nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 null0 --bypass-bdev
	$backend_rpc nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
}

nvmftestinit

nvmf_backend_start -m 0x30

nvmfappstart -m 0xc

$rpc_py bdev_nvme_set_options --transport-retry-count 7 --bdev-retry-count 3 --ctrlr-loss-timeout-sec -1 --reconnect-delay-sec 1
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py bdev_nvme_attach_controller -b nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 nvme0n1 --bypass-bdev
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# start normal IO for a few seconds
$rootdir/build/examples/bdevperf -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w randrw -M 50 -t 30 -m 0x3 -f &
bdevperf_pid=$!

trap 'killprocess $bdevperf_pid; killprocess $nvmf_backend_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
run_test_pid=$!

sleep 3

# Force kill target to test error path with bdev bypass
kill -9 $nvmf_backend_pid

#re-assign backend rpc sock since the previous one might be still used
backend_rpc_sock="/var/tmp/nvmf_backend2.sock"
backend_rpc="$rpc_py -s $backend_rpc_sock"

sleep 2
nvmf_backend_start -m 0x30

# IO traffic must restart after backend target restart
sleep 6

killprocess $bdevperf_pid
killprocess $nvmfpid
nvmfpid=""
killprocess $nvmf_backend_pid

trap - SIGINT SIGTERM EXIT

nvmftestfini
