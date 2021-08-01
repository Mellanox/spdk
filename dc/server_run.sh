#!/usr/bin/env bash

PORT=4420

TGT_LOG=$OUTPUT_DIR/spdk_tgt.${SLURM_NODEID}.log

echo "Server's Servers number: ${SERVERS_NUMBER}" | tee -a $TGT_LOG
echo "Server's SPDK_DIR: ${SPDK_DIR}" | tee -a $TGT_LOG
echo "Server's RDMA_CORE_DIR: ${RDMA_CORE_DIR}" | tee -a $TGT_LOG
echo "Server's TGT_MASK: ${TGT_MASK}" | tee -a $TGT_LOG
echo "Server's PERF_MASK: ${PERF_MASK}" | tee -a $TGT_LOG
echo "Server's IFACE: ${SERVER_IFACE}" | tee -a $TGT_LOG
echo "Server's Need IP ${NEED_IP_RESET}" | tee -a $TGT_LOG
. ./utils.sh

echo "Server is going to setup HUGEMEM=8182 with setup.sh"
sudo env HUGEMEM=8192 $SPDK_DIR/scripts/setup.sh
#sudo hugeadm --pool-pages-min DEFAULT:4G
echo "Server says that setup have been done"
kill_if spdk_tgt
kill_if bdevperf
echo "Server says that cleenup finished"
sleep 1

if [ -n "$NEED_IP_RESET" ]; then
    reset_ip
fi

echo "sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_DIR/install/bin/spdk_tgt -m $TGT_MASK 2>&1 | tee -a $TGT_LOG &"
sudo env LD_LIBRARY_PATH=$RDMA_CORE_DIR $SPDK_DIR/install/bin/spdk_tgt -m $TGT_MASK 2>&1 | tee -a $TGT_LOG &
echo "Waiting for 3s after spdk_tgt run ..." | tee -a $TGT_LOG
sleep 3
TGT_PID=$(pidof spdk_tgt)
echo "spdk_tgt pid: $TGT_PID" | tee -a $TGT_LOG
sudo $SPDK_DIR/scripts/rpc.py  bdev_null_create Null0 8192 512 2>&1 | tee -a $TGT_LOG
sudo $SPDK_DIR/scripts/rpc.py  nvmf_create_transport -t rdma 2>&1 | tee -a $TGT_LOG
#$SPDK_DIR/scripts/rpc.py  nvmf_create_transport -t rdma -u 131072 | tee -a $TGT_LOG
sudo $SPDK_DIR/scripts/rpc.py  nvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode1 2>&1 | tee -a $TGT_LOG
echo "sudo $SPDK_DIR/scripts/rpc.py  nvmf_subsystem_add_listener -t rdma -a $IP_ADDRESS -f ipv4 -s 4420 nqn.2016-06.io.spdk:cnode1 2>&1 | tee -a $TGT_LOG"  | tee -a $TGT_LOG
sudo $SPDK_DIR/scripts/rpc.py  nvmf_subsystem_add_listener -t rdma -a $IP_ADDRESS -f ipv4 -s 4420 nqn.2016-06.io.spdk:cnode1 2>&1 | tee -a $TGT_LOG
sudo $SPDK_DIR/scripts/rpc.py  nvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode1 Null0 2>&1 | tee -a $TGT_LOG

sleep 1
touch ${OUTPUT_DIR}/$IP_ADDRESS.server

wait $TGT_PID
