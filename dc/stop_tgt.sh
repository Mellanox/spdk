#!/usr/bin/env bash

STOP_LOG=$OUTPUT_DIR/stop.${SLURM_NODEID}.log

PID=$(pidof spdk_tgt)

HUGE_MB=$(sudo cat /proc/$PID/smaps | grep -B 11 'KernelPageSize:     2048 kB' | grep "^Size:" | awk 'BEGIN{sum=0}{sum+=$2}END{print sum/1024}')
HWM_MB=$(cat /proc/$PID/status | grep -e VmHWM | awk '{print $2/1024}')

echo "== TGT Memory usage =="
echo "Huge pages: ${HUGE_MB} MB, HWM: ${HWM_MB}"
echo "Huge pages: ${HUGE_MB} MB, HWM: ${HWM_MB}" > $STOP_LOG


sudo $SPDK_DIR/scripts/rpc.py  spdk_kill_instance 15
sleep 3
