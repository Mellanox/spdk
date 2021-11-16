#!/bin/bash
#
function get_ver()
{
    by_tag=$(git describe HEAD --tags)
    v_pfx=${by_tag%%-*}
    v_num=${v_pfx#v}
    echo $v_num
}
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
args="$@"
wd=$(dirname $0)
wdir=$(dirname $wd)
cd $wdir
mkdir -p $HOME/rpmbuild/{SOURCES,RPMS,SRPMS,SPECS,BUILD,BUILDROOT}
OUTDIR=$HOME/rpmbuild/SOURCES
set -e
if [ -z "$VER" ] ; then
    VER=$(get_ver)
    if [ -z "$VER" ] ; then
        VER=21.10
    fi
    export VER
fi
branch=$(git rev-parse --abbrev-ref HEAD)
sha1=$(git rev-parse HEAD |cut -c -8)
_date=$(date +'%a %b %d %Y')
#---
# VladS asked to have "Revision" hardcoded into the SPEC file
# So that we have to do inline editing
OUT_SPEC=./spdk.spec
sed -e "s#scm_rev %{_rev}#scm_rev ${BUILD_NUMBER:-1}#" scripts/spdk.spec > $OUT_SPEC
sed -i "s#%{_date}#$_date#; s#%{_sha1}#$sha1#; s#%{_branch}#$branch# " $OUT_SPEC

git archive \
    --format=tar --prefix=spdk-$VER/ -o $OUTDIR/spdk-$VER.tar  HEAD
generate_changelog
tar -uf  $OUTDIR/spdk-$VER.tar spdk-$VER/debian/changelog spdk-$VER/scripts/debian/changelog
rm -rf spdk-$VER/
# echo ***********
# ls -l ~/rpmbuild/SOURCES/spdk-$VER.tar.gz
# echo ***********
git submodule init
git submodule update
for MOD in $(git submodule |awk '{print $2}')
do
  (cd $MOD;
   git archive \
    --format=tar --prefix=spdk-$VER/$MOD/ -o $OUTDIR/spdk-$MOD-$VER.tar  HEAD
  )
done
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
