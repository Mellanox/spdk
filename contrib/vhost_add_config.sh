#!/usr/bin/bash -x
tts=${1-2}
sleep $tts
mkdir -p /dev/hugepages/qemu
chown qemu /dev/hugepages/qemu

function rpc()
{
   /usr/bin/spdk_rpc.py -s /var/tmp/spdk.sock $@
}

rpc start_subsystem_init
rpc wait_subsystem_init

#  TransportID "trtype:RDMA adrfam:IPv4 subnqn:nqn.2016-06.io.spdk.swx-bw-03:nvme0 traddr:11.212.79.34 trsvcid:1023" Nvme1

# rpc construct_nvme_bdev -b Nvme0 -t RDMA -a 11.210.99.24 -f IPv4 -s 1023 -n nqn.2018-01.mini-regression:ram2
rpc get_bdevs
rpc construct_vhost_scsi_controller --cpumask 0x30000 vhost.0
chown qemu /var/run/vhost/vhost.0

rpc add_vhost_scsi_lun vhost.0 1 Nvme0n1
# rpc add_vhost_scsi_lun vhost.0 2 Nvme1n2
# rpc add_vhost_scsi_lun vhost.0 3 Nvme1n3
