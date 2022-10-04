#!/bin/bash -e
# Dependency on package:
#   libhugetlbfs-utils @ CentOS
#   hugepages @ Ubuntu

min_hugemem=${MIN_HUGEMEM:-2G}
Hugetlb=$(grep Hugetlb /proc/meminfo | awk '{ print $2 }')

case $(echo ${min_hugemem: -1}) in
    M)
        unit=m
        ;;
    G)
        unit=g
        ;;
    K)
        unit=k
        ;;
    *)
        echo "[ERROR]: Unsupported unit format for hugepages!"
        exit 1
        ;;
esac

if [ $Hugetlb -gt 0 ]; then
    if [ $unit = "k" ]; then
        required_size=${min_hugemem%?}
    elif [ $unit = "m" ]; then
	required_size=$((${min_hugemem%?} * 1024))
    elif [ $unit = "g" ]; then
	required_size=$((${min_hugemem%?} * 1024 * 1024))
    fi

    Hugepagesize=$(grep Hugepagesize /proc/meminfo | awk '{ print $2 }')
    HugePages_Free=$(grep HugePages_Free /proc/meminfo | awk '{ print $2 }')

    huge_free_size=$((Hugepagesize * HugePages_Free))

    if [ $huge_free_size -ge $required_size ]; then
        exit 0
    fi
fi

exec /usr/bin/hugeadm --pool-pages-min DEFAULT:+${min_hugemem}
