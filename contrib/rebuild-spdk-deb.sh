#!/bin/bash -ex
# :vim: set expandtab tabstop=4 shiftwidth=4 softtabstop=4
# The script to (re)build SPDK DEB (Ubuntu) packags
# from the checked out source tree (spdk subdir)
function get_ver()
{
    by_tag=$(git describe HEAD --tags)
    v_pfx=${by_tag%%-*}
    v_num=${v_pfx#v}
    echo $v_num
}
test -e spdk
cd spdk
VER=$(get_ver)
test -n "$VER"
cd ..
if [ -z "$BUILD_NUMBER" ] ; then
    BUILD_NUMBER=1
fi

function pack_dist()
{
    cd spdk
    export VER
    ./contrib/git-archive-spdk.sh
    cd scripts
    python3 setup.py sdist -d ../../
    cd ../..
}

function generate_changelog()
{
    today=$(date +"%a, %d %b %Y %T %z")
    sed -e "s/@PACKAGE_VERSION@/$VER/" -e "s/@PACKAGE_REVISION@/$BUILD_NUMBER/" \
        -e 's/@PACKAGE_BUGREPORT@/support@mellanox.com/' -e "s/@BUILD_DATE_CHANGELOG@/$today/" \
        debian/changelog.in > debian/changelog
}
function unpack_dist()
{
  rm -rf spdk-$VER
  tar xf spdk-$VER.tar.gz
  cd spdk-$VER
  for MOD in dpdk ocf intel-ipsec-mb isa-l ; do
  	tar xf ../spdk-$MOD-$VER.tar.gz
  done
  generate_changelog
  cd ..
  tar zcf ./spdk_$VER.orig.tar.gz spdk-$VER
  # cd spdk-$VER
  # tar xf ../debian.tar.gz
  # cd ..
}

function build_main()
{
  cd spdk-$VER
  dpkg-buildpackage -uc -us -rfakeroot
  cd ..
}

function unpack_dist_rpc()
{
  rm -rf spdk-rpc-$VER
  tar xf ./spdk-rpc-$VER.tar.gz
  tar zcf ./spdk-rpc_$VER.orig.tar.gz spdk-rpc-$VER
  cd spdk-rpc-$VER
  # tar xf ../scripts-debian.tar.gz
  generate_changelog
  cd ..
}
function build_rpc()
{
  cd spdk-rpc-$VER
  dpkg-buildpackage -uc -us -rfakeroot
  cd ..
}
pack_dist
unpack_dist
build_main
unpack_dist_rpc
build_rpc
