
DEST_DIR=./RAY
JOB_DIR=./job.76296
PERF_MASK=0xF
for i in $(seq 1 16)
do
    srun -l --nodes ${CLIENTS_NUMBER} --ntasks ${CLIENTS_NUMBER} --nodelist=${CLIENT_NODELIST} --export=ALL ./nvmeperf_run.sh
    cp -R ${JOB_DIR} ${DEST_DIR}/JOB.${i}
    rm ${JOB_DIR}/*.log
    PERF_MASK=${PERF_MASK}F
done
