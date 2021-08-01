#!/usr/bin/env bash

sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFFFFFF  ./run_nvmeperf.sbatch
sbatch  --nodes=20 --export=TGT_MASK=0xFFFF,PERF_MASK=0xFFFFFFFFFFFF  ./run_nvmeperf.sbatch


# var=1
# for i in $(seq 1 6)
# do
#     MASK=$(printf '0x%X\n' $var)
#     sbatch  --nodes=8 --export=TGT_MASK=$MASK,PERF_MASK=0xFFFF ./run_nvmeperf_swx_ray.sbatch  #320
# #    sbatch  --nodes=21 --export=TGT_MASK=$MASK,PERF_MASK=0xFFFF ./run_nvmeperf.sbatch  #320
#     var=$(( $(( var << 1 ))|1 ))
# done

	 
