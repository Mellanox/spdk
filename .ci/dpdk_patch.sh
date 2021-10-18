#!/usr/bin/bash -e

mkdir -p dpdk/config/arm

cat << EOF > dpdk/config/arm/arm64_bluefield_linux_native_gcc
[binaries]
c = 'gcc'
cpp = 'cpp'
ar = 'ar'
strip = 'strip'
pkgconfig = 'pkg-config'
pcap-config = ''

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
implementor_id = '0x41'
implementor_pn = '0xd08'
platform = 'bluefield'
EOF
