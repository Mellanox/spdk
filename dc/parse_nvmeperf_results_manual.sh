#!/usr/bin/env bash

DIR_NAME=$1

function count_bits() {
    local num=$1   
    while [[ $num -gt 0 ]]
    do
	let temp=$num%2
	if [[ $temp -eq 1 ]]
	then
            let count=$count+1
	fi
	let num=$num/2
    done
    echo $count
}

function hex_to_dec() {
    local num=${1:2} # removing 0x prefix from hexnumber
    echo "obase=10; ibase=16; $num" | bc
}

function count_bits_hex() {
    local num=$1    
    if [ "$num" = "0xFFFFFFFFFFFFFFFF" ]
    then
	echo 64
	#ugly hack
    else

	local dec_num=$(hex_to_dec $num)
	local bits_count=$(count_bits $dec_num)
	echo $bits_count
    fi
}

DIR_PATTERN="./JOB.*"
echo "CORES_ON_SERVERS CORES_ON_PERFS IOPS MiB/s Latency,us Latency(stddev)"
for DIR_NAME in $DIR_PATTERN
do
    PERF_MASK=$(grep PERF_MASK ${DIR_NAME}/perf.0.log | awk '{print $3}')
    TGT_MASK=$(grep TGT_MASK ${DIR_NAME}/perf.0.log | awk '{print $3}')

    SERVERS_NUM=1
    PERFS_NUM=$(find ${DIR_NAME} -name "perf.*.log" -printf '.' | wc -m)
    CORES_PER_SERVER=$(count_bits_hex $TGT_MASK)
    CORES_PER_PERF=$(count_bits_hex $PERF_MASK)
    CORES_ON_SERVERS=$(( $SERVERS_NUM*$CORES_PER_SERVER ))
    CORES_ON_PERFS=$(( $PERFS_NUM*$CORES_PER_PERF ))

    tmpfile=$(mktemp /tmp/parse_mem_results_tmp.XXXXXX)
    for fname in $DIR_NAME/perf.*.log
    do
	grep -m1 -E "Total[[:space:]]*:" $fname >> $tmpfile       
    done
    DATA=($(awk '{IOPS+=$3;MIBS+=$4; LAT+=$5; LAT_SQ+=$5*$5}END{print int(IOPS), MIBS, LAT/NR, sqrt(LAT_SQ/NR - (LAT/NR)**2)}' $tmpfile))
    IOPS=${DATA[0]}
    MIBS=${DATA[1]}
    LAT=${DATA[2]}
    LAT_DEV=${DATA[3]}
    rm -f $tmpfile
    printf "%d %d %d %f %f %f\n" ${CORES_ON_SERVERS} ${CORES_ON_PERFS} ${IOPS} ${MIBS} ${LAT} ${LAT_DEV}
done


