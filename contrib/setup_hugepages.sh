#!/bin/bash -e
# Dependency on package:
#   libhugetlbfs-utils @ CentOS
#   hugepages @ Ubuntu
exec /usr/bin/hugeadm --pool-pages-min DEFAULT:${MIN_HUGEMEM:-2G}
