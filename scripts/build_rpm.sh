#!/bin/bash
args="$@"
wd=$(dirname $0)
wdir=$(dirname $wd)
cd $wdir
mkdir -p $HOME/rpmbuild/{SOURCES,RPMS,SRPMS,SPECS,BUILD,BUILDROOT}
set -e
if [ -z "$VER" ] ; then
    export VER=20.04.1
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
    --format=tar.gz --prefix=spdk-$VER/ -o ~/rpmbuild/SOURCES/spdk-$VER.tar.gz  HEAD
# echo ***********
# ls -l ~/rpmbuild/SOURCES/spdk-$VER.tar.gz
# echo ***********
git submodule init
git submodule update
for MOD in $(git submodule |awk '{print $2}')
do
  (cd $MOD;
   git archive \
    --format=tar.gz --prefix=$MOD/ -o ~/rpmbuild/SOURCES/spdk-$MOD-$VER.tar.gz  HEAD
  )
done

# BUILD_NUMBER is an env var passed by Jenkins
# https://stackoverflow.com/questions/16155792/using-jenkins-build-number-in-rpm-spec-file
fakeroot  \
  rpmbuild -bs --define "dist %{nil}" $args $OUT_SPEC

# Let's assume all dependencies are already installed
# into *spdk_dev docker image
#--
# if [ "$UID" -eq 0 ] ; then
#    chown 0.0  ~/rpmbuild/SOURCES/spdk-*-$VER.tar.gz
#    # ls -l ~/rpmbuild/SOURCES/*
#    # just a workaround for missed *-source repos:
#    fgrep -l vault.centos /etc/yum.repos.d/*.repo |while read fn ; do mv $fn /tmp/ ; done
#    yum-builddep -y $OUT_SPEC
# fi
rpmbuild -bb $args  $OUT_SPEC
