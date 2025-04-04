#!/bin/bash -eE

set -x

export NUM_JOBS=${NUM_JOBS:=$(nproc)}

build_tar() {
    VER=${_ver%%-*}
    REV=${_ver#*-}

    git archive \
        --format=tar --prefix=$name-$VER/ -o $name-$VER.tar HEAD

    git submodule init
    git submodule update

    for MOD in $(git submodule | awk '{print $2}'); do
        (
        pushd $MOD
        git archive \
            --format=tar --prefix=$name-$VER/$MOD/ -o ../$name-$MOD-$VER.tar HEAD
        popd
        )
    done


    for MOD in $(git submodule |awk '{print $2}')
    do
        tar --concatenate --file=$name-$VER.tar $name-$MOD-$VER.tar
    done
    tar -f $name-$VER.tar --delete $name-$VER/.gitignore \
    $name-$VER/.gitmodules $name-$VER/.ci $name-$VER/.github $name-$VER/.gitlab-ci.yml \
    $name-$VER/container $name-$VER/debian $name-$VER/contrib $name-$VER/scripts/spdk.spec \
    $name-$VER/.githooks
    tar --append -f $name-$VER.tar $name-$VER/.ci/dpdk_patch.sh
    gzip -c $name-$VER.tar > $name-$VER-$REV.tar.gz

}

usage() {
    echo "Usage: $0 [-h Help] [-f <path to the BUILD_VERSION file>] [-i Install package]" 1>&2
    exit 1
}

[ $# -eq 0 ] && usage
while getopts "hf:i" opt; do
    case "$opt" in
    f)
        filepath=$OPTARG
        ;;
    h | *)
        usage
        exit 0
        ;;
    esac
done

if [ -e $filepath ]; then
    _ver=$(cat $filepath)
else
    echo "${filepath} not found!"
    exit 1
fi
name="doca-nvmf-target-offload"


git config --global user.name "Jenkins"
git config --global user.email "swx-jenkins@nvidia.com"

# Detect distribution and perform appropriate actions
if [[ -f "/etc/os-release" ]]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian)
            build_tar
            ;;
        *)
            echo "Not supported Linux distribution!"
            exit 1
            ;;
    esac
else
    echo "Not a Linux!"
    exit 1
fi
