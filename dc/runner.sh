#!/usr/bin/env bash

sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x1 ./run.sbatch      #1
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x3 ./run.sbatch      #2
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x7 ./run.sbatch      #3
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0xF ./run.sbatch      #4
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x1F ./run.sbatch     #5
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x3F ./run.sbatch     #6
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x7F ./run.sbatch     #7
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0xFF ./run.sbatch     #8
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x1FF ./run.sbatch    #9
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x3FF ./run.sbatch    #10
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x7FF ./run.sbatch    #11
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0xFFF ./run.sbatch    #12
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x1FFF ./run.sbatch   #13
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x3FFF ./run.sbatch   #14
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0x7FFF ./run.sbatch   #15
sbatch  --nodes=2 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #16

sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x1FF ./run.sbatch    #18
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x3FF ./run.sbatch    #20
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x7FF ./run.sbatch    #22
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0xFFF ./run.sbatch    #24
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x1FFF ./run.sbatch   #26
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x3FFF ./run.sbatch   #28
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0x7FFF ./run.sbatch   #30
sbatch  --nodes=3 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #32

sbatch  --nodes=4 --export=TGT_MASK=0x1,PERF_MASK=0xFFF ./run.sbatch    #36
sbatch  --nodes=4 --export=TGT_MASK=0x1,PERF_MASK=0x1FFF ./run.sbatch   #39
sbatch  --nodes=4 --export=TGT_MASK=0x1,PERF_MASK=0x3FFF ./run.sbatch   #42
sbatch  --nodes=4 --export=TGT_MASK=0x1,PERF_MASK=0x7FFF ./run.sbatch   #45
sbatch  --nodes=4 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #48

sbatch  --nodes=5 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #64
sbatch  --nodes=6 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #80
sbatch  --nodes=7 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #96
sbatch  --nodes=8 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #112
sbatch  --nodes=9 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch   #128
sbatch  --nodes=10 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #144
sbatch  --nodes=11 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #160
sbatch  --nodes=12 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #176
sbatch  --nodes=13 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #192
sbatch  --nodes=17 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #256
sbatch  --nodes=21 --export=TGT_MASK=0x1,PERF_MASK=0xFFFF ./run.sbatch  #320




