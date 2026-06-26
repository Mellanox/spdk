#!/bin/bash -ex
# SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

################################################################################
# DPA Binary Signing Script for Ubuntu 24
#
# Purpose:
#   Sign the NVMF TARGET OFFLOAD DPA binary using NVIDIA's signing tool container.
#
# Problem:
#   Ubuntu 24's rootless podman has stricter UID/GID namespace mapping that
#   causes failures when pulling container images with files owned by
#   unmapped UIDs (e.g., /etc/shadow owned by UID 0:42).
#
# Solution:
#   1. Use skopeo to download the sign-tool container image (bypasses podman's
#      UID/GID mapping issues during pull)
#   2. Manually extract image layers to a rootfs directory
#   3. Create a podman wrapper script that intercepts the signing command and
#      runs it via chroot instead of containers
#   4. Setup DNS resolution in chroot to allow vault connectivity
#
# Usage:
#   DPA_SIGN_USER=<user> DPA_SIGN_PASS=<pass> ./sign_dpa.sh <path-to-binary> [...]
################################################################################

export VAULT_ROLE_CREDS="${DPA_SIGN_USER}:${DPA_SIGN_PASS}"

if [ $# -lt 1 ]; then
	echo "Usage: $0 <path-to-binary> [<path-to-binary> ...]"
	exit 1
fi
SIGN_TOOL="/usr/bin/bf3_dpa_sign.sh"

# Create temporary directory for all work (cleaned up on exit)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf -- "$TEMP_DIR"' EXIT

# Validate sign tool exists
if [ ! -f "${SIGN_TOOL}" ]; then
	echo "${SIGN_TOOL} does not exist, exiting"
	exit 255
fi

# Validate all binaries to sign exist
for f in "$@"; do
	if [ ! -f "$f" ]; then
		echo "$f does not exist, exiting"
		exit 254
	fi
done

################################################################################
# Step 1: Extract sign-tool image information
################################################################################

SIGN_IMAGE=$(grep 'DOCKER_IMAGE=' "${SIGN_TOOL}" | grep -oE 'nbu-harbor[^"]*' | head -1)
if [ -z "${SIGN_IMAGE}" ]; then
	echo "Could not extract DOCKER_IMAGE from ${SIGN_TOOL}, exiting"
	exit 252
fi

SIGN_VERSION="${SIGN_IMAGE##*:}"
LOCAL_IMAGE="localhost/sign-tool:${SIGN_VERSION}"

echo "Using sign-tool image: ${SIGN_IMAGE}"
echo "Local image will be: ${LOCAL_IMAGE}"

################################################################################
# Step 2: Download and extract sign-tool container image
################################################################################

ROOTFS_DIR="${TEMP_DIR}/rootfs"
mkdir -p "${ROOTFS_DIR}"

echo "Downloading sign-tool image with skopeo..."
skopeo copy --override-os linux --override-arch arm64 "docker://${SIGN_IMAGE}" "dir:${TEMP_DIR}/image"

echo "Extracting image layers..."
LAYER_DIGESTS=$(python3 -c "
import json
with open('${TEMP_DIR}/image/manifest.json') as f:
    manifest = json.load(f)
    for layer in manifest.get('layers', []):
        digest = layer.get('digest', '')
        if digest.startswith('sha256:'):
            print(digest.replace('sha256:', ''))
")

for digest in $LAYER_DIGESTS; do
	layer_file="${TEMP_DIR}/image/${digest}"
	if [ -f "$layer_file" ]; then
		echo "Extracting layer: $digest"
		tar --no-same-owner -C "${ROOTFS_DIR}" -xzf "$layer_file" 2> /dev/null \
			|| tar --no-same-owner -C "${ROOTFS_DIR}" -xf "$layer_file" 2> /dev/null || true
	fi
done

################################################################################
# Step 3: Extract container entrypoint from image config
################################################################################

CONFIG_HASH=$(grep -oE '"digest"[^}]*"sha256:[0-9a-f]{64}"' "${TEMP_DIR}/image/manifest.json" | grep -oE 'sha256:[0-9a-f]{64}' | head -1)
CONFIG_HASH="${CONFIG_HASH#sha256:}"

CONFIG_FILE=""
for cand in "${TEMP_DIR}/image/${CONFIG_HASH}" "${TEMP_DIR}/image/blobs/sha256/${CONFIG_HASH}"; do
	[ -f "$cand" ] && CONFIG_FILE="$cand" && break
done
if [ -z "${CONFIG_FILE}" ]; then
	echo "Could not find image config file (hash=${CONFIG_HASH}), exiting"
	exit 251
fi

ENTRYPOINT=$(python3 -c "import sys,json; cfg=json.load(open('$CONFIG_FILE')); print(' '.join(cfg['config'].get('Entrypoint', [])))")

################################################################################
# Step 4: Create podman wrapper that uses chroot
################################################################################

cat > "${TEMP_DIR}/podman" << 'WRAPPER_EOF'
#!/bin/bash
SANDBOX_PATH=""
CONTAINER_ARGS=()
parse_container_args=0

while [ $# -gt 0 ]; do
	if [ $parse_container_args -eq 1 ]; then
		CONTAINER_ARGS+=("$1")
		shift
		continue
	fi

	case "$1" in
		-v)
			SANDBOX_PATH="${2%%:*}"
			shift 2
			;;
		-e)
			export "$2"
			shift 2
			;;
		--pid=host|--uts=host|--rm|-t|--network=host)
			shift
			;;
		localhost/sign-tool:*)
			parse_container_args=1
			shift
			;;
		*)
			shift
			;;
	esac
done
WRAPPER_EOF

cat >> "${TEMP_DIR}/podman" << EOF

mkdir -p "${ROOTFS_DIR}/sandbox"
if [ -n "\${SANDBOX_PATH}" ]; then
	for item in "\${SANDBOX_PATH}"/*; do
		[ -e "\$item" ] || continue
		[ "\$(basename "\$item")" = "rootfs" ] && continue
		[ "\$(basename "\$item")" = "image" ] && continue
		[ "\$(basename "\$item")" = "podman" ] && continue
		cp -a "\$item" "${ROOTFS_DIR}/sandbox/"
	done
fi

mkdir -p "${ROOTFS_DIR}/etc"
cp -L /etc/resolv.conf "${ROOTFS_DIR}/etc/resolv.conf" 2> /dev/null || true
cp -L /etc/hosts "${ROOTFS_DIR}/etc/hosts" 2> /dev/null || true
cp -L /etc/nsswitch.conf "${ROOTFS_DIR}/etc/nsswitch.conf" 2> /dev/null || true

cd "${ROOTFS_DIR}/sandbox"
chroot "${ROOTFS_DIR}" "${ENTRYPOINT}" "\${CONTAINER_ARGS[@]}"

if [ -n "\${SANDBOX_PATH}" ]; then
	for item in "${ROOTFS_DIR}/sandbox"/*; do
		[ -e "\$item" ] || continue
		cp -a "\$item" "\${SANDBOX_PATH}/"
	done
fi
EOF
chmod +x "${TEMP_DIR}/podman"

################################################################################
# Step 5: Patch sign tool to use our wrapper
################################################################################

SIGN_TOOL_COPY_DIR="${TEMP_DIR}/sign-tool-bin"
mkdir -p "${SIGN_TOOL_COPY_DIR}"
cp "$(dirname "${SIGN_TOOL}")"/*.sh "${SIGN_TOOL_COPY_DIR}/"
SIGN_TOOL_COPY="${SIGN_TOOL_COPY_DIR}/$(basename "${SIGN_TOOL}")"
chmod +x "${SIGN_TOOL_COPY}"

sed -i "s|${SIGN_IMAGE}|${LOCAL_IMAGE}|g" "${SIGN_TOOL_COPY}"
sed -i 's|docker run --rm -t|podman run --pid=host --uts=host --rm -t|' "${SIGN_TOOL_COPY}"

export PATH="${TEMP_DIR}:${PATH}"

################################################################################
# Step 6: Sign all provided binaries
################################################################################

for SIGN_TARGET in "$@"; do
	SIGN_BASENAME=$(basename "${SIGN_TARGET}")

	cp "${SIGN_TARGET}" "${TEMP_DIR}/${SIGN_BASENAME}"
	"${SIGN_TOOL_COPY}" -f "${TEMP_DIR}/${SIGN_BASENAME}" --platform ARM --prod \
		-d "Signing NVMF TARGET OFFLOAD APP" -o "${TEMP_DIR}/${SIGN_BASENAME}.signed"

	md5sum_unsigned="$(md5sum "${SIGN_TARGET}" | awk '{print $1}')"
	md5sum_signed="$(md5sum "${TEMP_DIR}/${SIGN_BASENAME}.signed" | awk '{print $1}')"
	if [ "${md5sum_unsigned}" = "${md5sum_signed}" ]; then
		echo "MD5 are equal for ${SIGN_BASENAME}: unsigned and signed binaries are identical, exiting"
		exit 253
	fi

	mv "${TEMP_DIR}/${SIGN_BASENAME}.signed" "${SIGN_TARGET}" && chmod 755 "${SIGN_TARGET}"
	echo "Signed: ${SIGN_TARGET}"
done
