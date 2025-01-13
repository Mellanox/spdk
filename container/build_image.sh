#!/bin/bash -eEx

# SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

set -o pipefail

SCRIPT=$(readlink -f "$0")
SCRIPTPATH=$(dirname "$SCRIPT")
WD=${WORKSPACE:-$(dirname $SCRIPTPATH)}
GIT_COMMIT=$(git rev-parse --short HEAD)

# Default variables
NVMF_TARGET_OFFLOAD_BUILD_ARGS=()
NVMF_TARGET_OFFLOAD_BUILD_TYPE="debug"
NVMF_TARGET_OFFLOAD_VERSION="$(cat VERSION)"
ARTIFACT_PROP_NAME="DOCKER_IMAGE"
DOCKER_CONFIG="~/.docker"
DOCKER_ARCH=$(uname -m)
DOCKER_FILE="container/Dockerfile.nvmf_target_offload"
DOCKER_BUILD_ARGS=()
DOCKER_IMAGE_NAME="doca_nvmf_target_offload"
DOCKER_REGISTRY_HOST="nbu-harbor.gtm.nvidia.com"
DOCKER_REGISTRY_PATH="swx-storage/doca_nvmf_target_offload"
PUSH_IMAGE=false
SIGN=false

function usage() 
{
cat <<EOF

Usage: $SCRIPT <[options=value]>

Arguments:
   --build-type                    Build type ['release', 'debug'] (default: ${NVMF_TARGET_OFFLOAD_BUILD_TYPE})
   --docker-image-name             DOCKER image name (default: ${DOCKER_IMAGE_NAME})
   --docker-config                 DOCKER config path with config.json (default: ${DOCKER_CONFIG})
   --docker-file                   Dockerfile location (default: ${DOCKER_FILE})
   --docker-registry-host          DOCKER registry host (default: ${DOCKER_REGISTRY_HOST})
   --docker-registry-path          DOCKER registry path (default: ${DOCKER_REGISTRY_PATH})
   --doca-builder-tag              DOCA docker tag for devel image (example: 2.9.0056-devel-ubuntu22.04-arm64)
   --doca-runtime-tag              DOCA docker tag for runtime image (example: 2.9.0056-full-rt-ubuntu22.04-arm64)
   --doca-image                    DOCA image URL for build & runtime images (example: nvcr.io/nvstaging/doca/doca)
   --push                          Wheather to push image into a registry (default: ${PUSH_IMAGE})
   --sign                          Sign DPA binary (default: ${SIGN}). Requires DPA_SIGN_USER and DPA_SIGN_PASS env variables to be set
   --artifact-prop-name            Key name in artifact.properties (default: ${ARTIFACT_PROP_NAME})
   --doca-sta-url                  URL for DOCA-STA file: https://urm.nvidia.com/artifactory/sw-nbu-doca-local/doca-sdk/2.10.0/DOCA_2-10-0065-1/doca-sdk-sta-2.10.0065.tar.gz (default: empty)
EOF
}

[ $# -eq 0 ] && usage && exit 1
while getopts ":h-:" optchar; do
    case "${optchar}" in
        -)
            case "${OPTARG}" in
                build-type=*)
                    NVMF_TARGET_OFFLOAD_BUILD_TYPE=${OPTARG#*=}
                    if [[ "$NVMF_TARGET_OFFLOAD_BUILD_TYPE" != "release" && "$NVMF_TARGET_OFFLOAD_BUILD_TYPE" != "debug" ]]; then
                        echo "[ERROR]: Invalid --build-type. Accepted values are 'debug' or 'release'"
                    fi
                    ;;
                docker-image-name=*)
                    DOCKER_IMAGE_NAME=${OPTARG#*=}
                    ;;
                docker-config=*)
                    DOCKER_CONFIG=${OPTARG#*=}
                    ;;
                docker-file=*)
                    DOCKER_FILE=${OPTARG#*=}
                    ;;
                docker-registry-host=*)
                    DOCKER_REGISTRY_HOST=${OPTARG#*=}
                    ;;
                docker-registry-path=*)
                    DOCKER_REGISTRY_PATH=${OPTARG#*=}
                    ;;
                doca-builder-tag=*)
                    DOCA_BUILDER_TAG=${OPTARG#*=}
                    ;;
                doca-runtime-tag=*)
                    DOCA_RUNTIME_TAG=${OPTARG#*=}
                    ;;
                doca-image=*)
                    DOCA_IMAGE=${OPTARG#*=}
                    ;;
                push)
                    PUSH_IMAGE="true"
                    ;;
                sign)
                    SIGN="true"
                    if [[ "$SIGN" == "true" ]]; then
                        [ -z "${DPA_SIGN_USER}" ] && { echo "[ERROR]: DPA_SIGN_USER env var is not set."; exit 1; }
                        [ -z "${DPA_SIGN_PASS}" ] && { echo "[ERROR]: DPA_SIGN_PASS env var is not set."; exit 1; }
                    fi
                    ;;
                artifact-prop-name=*)
                    ARTIFACT_PROP_NAME=${OPTARG#*=}
                    ;;
                doca-sta-url=*)
                    DOCA_STA_URL=${OPTARG#*=}
                    ;;
                *)
                    if [ "$OPTERR" = 1 ] && [ "${optspec:0:1}" != ":" ]; then
                        echo "Unknown option --${OPTARG}" >&2
                        usage
                        exit 1
                    fi
                    ;;
            esac;;
        h | *)  
            usage
            exit 0 
            ;;
    esac
done

# Check mandatory parameters
: ${DOCA_IMAGE:?[ERROR]: Missing mandatory parameter --doca-image!}
: ${DOCA_BUILDER_TAG:?[ERROR]: Missing mandatory parameter --doca-builder-tag}
: ${DOCA_RUNTIME_TAG:?[ERROR]: Missing mandatory parameter --doca-runtime-tag}

DOCKER_BUILD_ARGS+=("--build-arg DOCA_IMAGE=$DOCA_IMAGE")
DOCKER_BUILD_ARGS+=("--build-arg DOCA_BUILDER_TAG=$DOCA_BUILDER_TAG")
DOCKER_BUILD_ARGS+=("--build-arg DOCA_RUNTIME_TAG=$DOCA_RUNTIME_TAG")

if [ "${NVMF_TARGET_OFFLOAD_BUILD_TYPE}" == "debug" ]; then
    NVMF_TARGET_OFFLOAD_BUILD_ARGS+=("--enable-debug")
fi

DOCKER_BUILD_ARGS+=("--build-arg NVMF_TARGET_OFFLOAD_BUILD_ARGS='${NVMF_TARGET_OFFLOAD_BUILD_ARGS[@]}'")

DOCA_VERSION=$(echo $DOCA_RUNTIME_TAG | grep -o "[0-9]\+\.[0-9]\+\.[0-9]")
NVMF_TARGET_OFFLOAD_VER_MID=$(echo $NVMF_TARGET_OFFLOAD_VERSION | grep -o "[0-9]\.[0-9]\.[0-9]")

case "$DOCKER_REGISTRY_HOST"  in
    "nbu-harbor.gtm.nvidia.com")
        if test -n "$ghprbPullId"; then
            NVMF_TARGET_OFFLOAD_VERSION="${NVMF_TARGET_OFFLOAD_VER_MID}-pr${ghprbPullId}"
        else
            NVMF_TARGET_OFFLOAD_VERSION="${NVMF_TARGET_OFFLOAD_VERSION}.${GIT_COMMIT}"
        fi

        DOCKER_IMG="${DOCKER_REGISTRY_HOST}/${DOCKER_REGISTRY_PATH}/${DOCKER_ARCH}/${DOCKER_IMAGE_NAME}:${NVMF_TARGET_OFFLOAD_VERSION}-doca${DOCA_VERSION}"
        ;;
    "nvcr.io")
        DOCKER_IMG="${DOCKER_REGISTRY_HOST}/${DOCKER_REGISTRY_PATH}/${DOCKER_IMAGE_NAME}:${NVMF_TARGET_OFFLOAD_VERSION}-doca${DOCA_VERSION}"
    ;;
    *)
        echo "[ERROR]: Wrong docker_registry_host: $DOCKER_REGISTRY_HOST!"
        exit 1
esac

DOCKER_BUILD_ARGS+=("--build-arg VERSION=${NVMF_TARGET_OFFLOAD_VERSION}")
DOCKER_BUILD_ARGS+=("--build-arg GIT_COMMIT=${GIT_COMMIT}")

doca_version=$(echo $DOCA_BUILDER_TAG | grep -o "[0-9]\+\.[0-9]\+\.[0-9]*")
DOCKER_BUILD_ARGS+=("--build-arg DOCA_VERSION=${doca_version}")


if [ "$SIGN" = true ]; then
    test -d sign-tool || { echo "[ERROR]: sign-tool is not found!"; exit 1; }
    DOCKER_BUILD_ARGS+=("--build-arg SIGN_DPA=true")
    DOCKER_BUILD_ARGS+=("--build-arg DPA_SIGN_USER=${DPA_SIGN_USER}")
    DOCKER_BUILD_ARGS+=("--build-arg DPA_SIGN_PASS=${DPA_SIGN_PASS}")
fi

if [ ! -z "$DOCA_STA_URL" ]; then
    doca_sta_version=$(echo $DOCA_STA_URL | grep -o "[0-9]\+\.[0-9]\+\.[0-9]\{4\}")
    DOCKER_BUILD_ARGS+=("--build-arg DOCA_STA_URL=${DOCA_STA_URL}")
    DOCKER_BUILD_ARGS+=("--build-arg DOCA_STA_VERSION=${doca_sta_version}")
fi

cmd="podman build --format docker -f ${DOCKER_FILE} -t ${DOCKER_IMG} ${DOCKER_BUILD_ARGS[@]} ."

echo "[INFO]: Running command: [ $cmd ]"
eval $cmd

if [ "$PUSH_IMAGE" = true ]; then
    echo "[INFO]: Pushing ${DOCKER_IMG} into ${DOCKER_REGISTRY_HOST} registry"
    podman push $DOCKER_IMG
    echo "${ARTIFACT_PROP_NAME}=${DOCKER_IMG}" >> "$WD/artifact.properties"

    if test ! -n "$ghprbPullId"; then
        MID_TAG_IMAGE="${DOCKER_IMG%%:*}:${NVMF_TARGET_OFFLOAD_VER_MID}-doca${DOCA_VERSION}"
        echo "[INFO]: Pushing ${MID_TAG_IMAGE} into ${DOCKER_REGISTRY_HOST} registry"
        podman tag ${DOCKER_IMG} ${MID_TAG_IMAGE}
        podman push ${MID_TAG_IMAGE}

        LATEST_TAG_IMAGE="${DOCKER_IMG%%:*}:latest"
        echo "[INFO]: Pushing ${LATEST_TAG_IMAGE} into ${DOCKER_REGISTRY_HOST} registry"
        podman tag ${DOCKER_IMG} ${LATEST_TAG_IMAGE}
        podman push ${LATEST_TAG_IMAGE}
    fi
fi

