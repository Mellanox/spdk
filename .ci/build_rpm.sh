#!/bin/bash -x

function generate_changelog()
{
    today=$(date +"%a, %d %b %Y %T %z")
    mkdir -p spdk-$VER/debian/
    mkdir -p spdk-$VER/scripts/debian/

    for FN in debian/changelog scripts/debian/changelog ; do
      sed -e "s/@PACKAGE_VERSION@/$VER/" -e "s/@PACKAGE_REVISION@/${BUILD_NUMBER-1}/" \
          -e 's/@PACKAGE_BUGREPORT@/support@mellanox.com/' -e "s/@BUILD_DATE_CHANGELOG@/$today/" \
          $FN.in > spdk-$VER/$FN
    done
}

function apply_dpdk_patch()
{
    $BDIR_PATH/dpdk_patch.sh
}

BASEDIR=$(dirname "$0")
BDIR_PATH=$(readlink -f $BASEDIR)
args="$@"
mkdir -p $HOME/rpmbuild/{SOURCES,RPMS,SRPMS,SPECS,BUILD,BUILDROOT}
OUTDIR=$HOME/rpmbuild/SOURCES
set -e

branch=$(git name-rev --name-only --refs *nvda* HEAD | awk -F/ '{print $NF}')
sha1=$(git rev-parse HEAD |cut -c -8)
_date=$(date +'%a %b %d %Y')

if [ -z "$VER" ] ; then
    export VER=$(echo $branch | grep -o '[0-9]\+\(\.[0-9]\+\)*')
fi

#---
# VladS asked to have "Revision" hardcoded into the SPEC file
# So that we have to do inline editing
OUT_SPEC=./spdk.spec
sed -e "s#scm_rev %{_rev}#scm_rev ${BUILD_NUMBER:-1}#" scripts/spdk.spec > $OUT_SPEC
sed -i "s#%{_date}#$_date#; s#%{_sha1}#$sha1#; s#%{_branch}#$branch# " $OUT_SPEC

git archive \
    --format=tar --prefix=spdk-$VER/ -o $OUTDIR/spdk-$VER.tar HEAD
generate_changelog

pushd spdk-$VER
apply_dpdk_patch
popd

tar -uf  $OUTDIR/spdk-$VER.tar \
	spdk-$VER/debian/changelog \
	spdk-$VER/scripts/debian/changelog \
	spdk-$VER/dpdk/config/arm/arm64_bluefield_linux_native_gcc

git submodule init
git submodule update

for MOD in $(git submodule |awk '{print $2}')
do
  (cd $MOD;
   git archive \
    --format=tar --prefix=spdk-$VER/$MOD/ -o $OUTDIR/spdk-$MOD-$VER.tar  HEAD
  )
done

apply_dpdk_patch

for MOD in $(git submodule |awk '{print $2}')
do
    tar --concatenate --file=$OUTDIR/spdk-$VER.tar $OUTDIR/spdk-$MOD-$VER.tar
done

gzip -c $OUTDIR/spdk-$VER.tar >$OUTDIR/spdk-$VER.tar.gz

# BUILD_NUMBER is an env var passed by Jenkins
# https://stackoverflow.com/questions/16155792/using-jenkins-build-number-in-rpm-spec-file
fakeroot  \
  rpmbuild -bs --define "dist %{nil}" $args $OUT_SPEC

rpmbuild -bb $args $OUT_SPEC
