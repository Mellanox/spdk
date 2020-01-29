#!/bin/bash
set -e
WD=$(dirname $(dirname $(readlink -f $0)))
cd $WD
if [ -e /etc/redhat-release  ] ; then
    ODIR=~/rpmbuild/SOURCES
else
	ODIR=$(readlink -f ..)
fi
# VER=18.10
# VER=19.03
# VER=20.01
function get_ver()
{
  by_tag=$(git describe HEAD --tags)
  v_pfx=${by_tag%%-*}
  v_num=${v_pfx#v}
  echo $v_num
}
git submodule init
git submodule update
VER=$(get_ver)
test -n "$VER"
set -x
for mod in dpdk ocf intel-ipsec-mb isa-l ; do
    (cd $mod;
      git archive \
            --format=tar.gz --prefix=$mod/ -o $ODIR/spdk-$mod-$VER.tar.gz  HEAD
    )
done

git archive --format=tar.gz --prefix=spdk-$VER/ -o $ODIR/spdk-$VER.tar.gz  HEAD
