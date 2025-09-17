#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit
nvmfappstart

subnqn="nqn.2024-07.io.spdk:cnode0"
perf="$SPDK_BIN_DIR/spdk_nvme_perf"
perf_transport_opt="trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT}"
perf_opt=(-q 32 -w randread -r "$perf_transport_opt")
small_cache_size=64
large_cache_size=32

# configure small and large buffer caches, enable pool selection. A pool (small/large) is selected based on the IO size.
$rpc_py bdev_malloc_create -b Malloc0 32 512
$rpc_py nvmf_create_transport "$NVMF_TRANSPORT_OPTS" --small-buf-cache-size $small_cache_size --large-buf-cache-size $large_cache_size --enable-pool-selection
$rpc_py nvmf_create_subsystem "$subnqn" -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns "$subnqn" Malloc0
$rpc_py nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

#only small pool must be used
$perf "${perf_opt[@]}" -o 4096 -t 1 "${NO_HUGE[@]}"
iobufstat=$($rpc_py iobuf_get_stats)

large_cache_count=$(echo $iobufstat | jq -r ".[] | select(.module == "\"nvmf_${TEST_TRANSPORT^^}\"") | .large_pool.cache")
if [[ $large_cache_count -gt 0 ]]; then
	exit 1
fi
small_cache_count=$(echo $iobufstat | jq -r ".[] | select(.module == "\"nvmf_${TEST_TRANSPORT^^}\"") | .small_pool.cache")
if [[ $small_cache_count -eq 0 ]]; then
	exit 1
fi

#only large pool must be used
$perf "${perf_opt[@]}" -o 131072 -t 1 "${NO_HUGE[@]}"
iobufstat=$($rpc_py iobuf_get_stats)

large_cache_count=$(echo $iobufstat | jq -r ".[] | select(.module == "\"nvmf_${TEST_TRANSPORT^^}\"") | .large_pool.cache")
if [[ $large_cache_count -eq 0 ]]; then
	exit 1
fi
# some amount of small buffers are used to init controller, so we can't check if small_cache_count was not changed since last perf run

trap - SIGINT SIGTERM EXIT
nvmftestfini
