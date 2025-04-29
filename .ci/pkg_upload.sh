#!/bin/bash -eEx

usage() {
cat <<EOF
"Usage: $0 
Options:
-h                  Help
-f <path>           Path to the versioning file
-n <name>           Package name. Default: "$name"
-r <repo_name>      Repo name. Default: "$repo_name"
-s <stage>          Stage name. Default: "$STAGE"
EOF
exit 1
}


upload_tar_urm() {
    tar_pkg_url="${name}-${VER}.tar.gz"
    MD5=$(md5sum $tar_pkg_url | awk '{print $1}')
    SHA1=$(shasum -a 1 $tar_pkg_url | awk '{ print $1 }')
    SHA256=$(shasum -a 256 $tar_pkg_url | awk '{ print $1 }')
    upload_url_urm="${REPO_URL}/${repo_name}/${STAGE}/${tar_pkg_url}"
    echo "INFO: Uploading package ${tar_pkg_url} to ${upload_url_urm}"
    curl --fail -u "${REPO_USER}:${REPO_PASS}" -X PUT \
        -H "X-Checksum-MD5:${MD5}" \
        -H "X-Checksum-Sha1:${SHA1}" \
        -H "X-Checksum-Sha256:${SHA256}" \
        -T "${tar_pkg_url}" "${upload_url_urm}"

    echo "INFO: Adding TAR package ${tar_pkg_url} to artifact.properties"
    pkg_name=${tar_pkg_url%-*-*}
    pkg_name_upper=$(echo "$pkg_name" | tr '[:lower:]' '[:upper:]')
    echo "PKG_${pkg_name_upper}=${REPO_URL}/${repo_name}/${STAGE}/${tar_pkg_url}" >> artifact.properties 
}

name="doca-nvmf-target-offload"
repo_name="doca-nvmf-target-offload"
STAGE="nightly"

[ $# -eq 0 ] && usage
while getopts "hf:n:p:r:s:" opt; do
    case "$opt" in
    f)
        filepath=$OPTARG
        ;;
    n)
        name=$OPTARG
        ;;
    r)
        repo_name=$OPTARG
        ;;
    s)
        STAGE=$OPTARG
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

VER="$_ver"
: ${REPO_USER:?REPO_USER is not found!}
: ${REPO_PASS:?REPO_PASS is not found!}

# Detect distribution and perform appropriate actions
if [[ -f "/etc/os-release" ]]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian)
            REPO_URL="https://urm.nvidia.com/artifactory/sw-nbu-swx-ci-generic-local/packages/tar"
            upload_tar_urm
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
