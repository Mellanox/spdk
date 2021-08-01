#!/usr/bin/env bash

SERVER_IFACE=${SER3ER_IFACE:-ib0}
LEAD_IFACE=eno1
NETWORK_PREFIX=65.65.65


IP_ADDRESS=$(ip -4 addr show $SERVER_IFACE 2> /dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}')
LEAD_IP_ADDRESS=$(ip -4 addr show $LEAD_IFACE 2> /dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}')


function kill_if() {
    PID=$(pidof $1)
    [[ -z "$PID" ]] || { echo "Killing $1"; sudo kill -9 $PID; } 
}

function check_servers_number() {
    SERVERS_NOW=0
    if [ ! -f $OUTPUT_DIR/*.server ]; then
	return
    fi
    SERVERS_NOW=$(ls $OUTPUT_DIR/*.server | wc -l)
}

function wait_for_n_servers() {
    local number=$1
    while check_servers_number
    do
        if [ $number = $SERVERS_NOW ]
        then
            break
        else
            echo "Waiting for $number servers started... now there are $SERVERS_NOW servers"
            sleep 1
        fi 
    done
    echo $server_files_now
}

function get_servers_list() {
    SERVERS=($(ls ${OUTPUT_DIR}/*.server | sed -e 's/\.server//' | sed -e 's/.*job.*\///'))
}


function stddev() {
    num_list=$1
    echo "$num_list" |
        awk '{sum+=$1; sumsq+=$1*$1}END{print sqrt(sumsq/NR - (sum/NR)**2)}'
}

function reset_ip() {
    echo "Resetting IP"
    if [ -n "${IP_ADDRESS}" ]; then
	echo "Non empty ip address on iface ${IP_ADDRESS}. Resetting..."
	FULL_IP_ADDRESS=$(ip -4 addr show $SERVER_IFACE 2> /dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}\/\d+')
        IP_ADDR=$(echo $FULL_IP_ADDRESS | sed -r 's/([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\/([0-9]+)/\1/')
        IP_MASK=$(echo $FULL_IP_ADDRESS | sed -r 's/([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\/([0-9]+)/\2/')
	echo "ip addr del ${FULL_IP_ADDRESS} dev ${SERVER_IFACE}"
	sudo ip addr del ${FULL_IP_ADDRESS} dev ${SERVER_IFACE}
    fi
    if [ -z "${IP_MASK}" ]; then
	IP_MASK=24
    fi
    LEAD_SUFFIX=$(echo $LEAD_IP_ADDRESS | sed -r 's/[0-9]+\.[0-9]+\.[0-9]+\.([0-9]+)/\1/')
    IP_ADDRESS=${NETWORK_PREFIX}.${LEAD_SUFFIX}
    echo "ip add add ${IP_ADDRESS}/${IP_MASK} dev ${SERVER_IFACE}"
    sudo ip add add ${IP_ADDRESS}/${IP_MASK} dev ${SERVER_IFACE}
}

export stddev
export check_servers_number
export wait_for_n_servers
export get_servers_list
export kill_if
export reset_ip
