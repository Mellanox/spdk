#!/usr/bin/env bash
sbatch  --nodes=20 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  # 304




