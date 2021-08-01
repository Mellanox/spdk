#!/usr/bin/env bash

var=1
for i in $(seq 1 6)
do
    MASK=$(printf '0x%X\n' $var)
    sbatch  --nodes=21 --export=TGT_MASK=$MASK,PERF_MASK=0xFFFF ./run.sbatch  #320
#    sbatch  --nodes=21 --export=TGT_MASK=$MASK,PERF_MASK=0xFFFF ./run_nvmeperf.sbatch  #320
    var=$(( $(( var << 1 ))|1 ))
done

	 
