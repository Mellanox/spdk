#!/usr/bin/env bash
#SBATCH -J spdk_dc
#SBATCH -p jazz

#echo "TEST TEST ${PERF_MASK}, ${TGT_MASK}"
export PREFIX=/labhome/denisn/dc
export SPDK_DIR=$PREFIX/spdk
#export SPDK_DIR=$PREFIX/spdk_release
export SPDK_BIN_DIR=$SPDK_DIR/install/bin
export RDMA_CORE_DIR=$PREFIX/rdma-core/build/lib
export PERF_MASK=${PERF_MASK:=0x3F}
export TGT_MASK=${TGT_MASK:=0x3F}
export SERVERS_NUMBER=${SERVERS_NUMBER:=1}

((CLIENTS_NUMBER=$SLURM_JOB_NUM_NODES-$SERVERS_NUMBER))
export CLIENTS_NUMBER
export NODES=($(scontrol show hostnames $SLURM_NODELIST))
export SERVER_NODES=${NODES[@]: 0:$SERVERS_NUMBER}
export SERVER_NODELIST=${SERVER_NODES// /,}
export CLIENT_NODES=${NODES[@]: $SERVERS_NUMBER}
export CLIENT_NODELIST=${CLIENT_NODES// /,}
export OUTPUT_DIR=${SLURM_SUBMIT_DIR}/job.${SLURM_JOBID}
mkdir ${OUTPUT_DIR}

INPUT_LOG=${OUTPUT_DIR}/input.log

echo "== Starting run at $(date)" | tee -a $INPUT_LOG
echo "== Job ID: ${SLURM_JOBID}" | tee -a $INPUT_LOG
echo "== Totally nodes: ${SLURM_JOB_NODES}" | tee -a $INPUT_LOG
echo "== Node list: ${SLURM_NODELIST}" | tee -a $INPUT_LOG
echo "== Submit dir. : ${SLURM_SUBMIT_DIR}" | tee -a $INPUT_LOG
echo "== Servers number: ${SERVERS_NUMBER}" | tee -a $INPUT_LOG
echo "== SPDK_DIR: ${SPDK_DIR}" | tee -a $INPUT_LOG
echo "== RDMA_CORE_DIR: ${RDMA_CORE_DIR}" | tee -a $INPUT_LOG
echo "== TGT_MASK: ${TGT_MASK}" | tee -a $INPUT_LOG
echo "== PERF_MASK: ${PERF_MASK}" | tee -a $INPUT_LOG



echo "== Server nodelist: ${SERVER_NODELIST}" | tee -a $INPUT_LOG
echo "== Client nodelist: ${CLIENT_NODELIST}" | tee -a $INPUT_LOG



echo "== Servers running..." | tee -a $INPUT_LOG
echo "srun -l -O --nodes ${SERVERS_NUMBER#} --ntasks ${SERVERS_NUMBER} --nodelist=${SERVER_NODELIST} --export=ALL ./server_run.sh &" | tee -a $INPUT_LOG
srun -l -O --nodes ${SERVERS_NUMBER} --ntasks ${SERVERS_NUMBER} --nodelist=${SERVER_NODELIST} --export=ALL ./server_run.sh &

echo "== Clients running... " | tee -a $INPUT_LOG
echo "srun -l --nodes ${CLIENTS_NUMBER} --ntasks ${CLIENTS_NUMBER} --nodelist=${CLIENT_NODELIST} --export=ALL ./bdevperf_run.sh" | tee -a $INPUT_LOG
srun -l --nodes ${CLIENTS_NUMBER} --ntasks ${CLIENTS_NUMBER} --nodelist=${CLIENT_NODELIST} --export=ALL ./bdevperf_run.sh 


echo "== Servers stopping... " | tee -a $INPUT_LOG
echo "srun -l -O --nodes ${SERVERS_NUMBER} --ntasks ${SERVERS_NUMBER} --nodelist=${SERVER_NODELIST} --export=ALL ./stop_tgt.sh" | tee -a $INPUT_LOG
srun -l -O --nodes ${SERVERS_NUMBER} --ntasks ${SERVERS_NUMBER} --nodelist=${SERVER_NODELIST} --export=ALL ./stop_tgt.sh

### Just to make sure ones more
pdsh -w ${SERVER_NODELIST} sudo $SPDK_DIR/scripts/rpc.py  spdk_kill_instance 9
pdsh -w ${CLIENT_NODELIST} sudo $SPDK_DIR/scripts/rpc.py  spdk_kill_instance 9
#wait
