#!/bin/bash -eE
#
# SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
#
set -x

# DOCA's internal private repository
export NGC_CLI_ACE=no-ace
export NGC_CLI_FORMAT_TYPE=ascii
export NGC_CLI_ORG=nvstaging
export NGC_CLI_TEAM=doca
export NGC_CLI_API_KEY=${NGC_CLI_API_KEY:-'no-apikey'}
export NGC_CLI_RESOURCE_NAME=doca_nvmf_target_offload
 
_ver=$(cat VERSION)
NVMF_TARGET_OFFLOAD_VERSION=${_ver%%-*}

# Download the (latest) YAML files
ngc registry resource download-version "${NGC_CLI_ORG}/${NGC_CLI_TEAM}/${NGC_CLI_RESOURCE_NAME}" --format_type json | tee ngc_cli_output.txt
DOWNLOADED_FOLDER=$(cat ngc_cli_output.txt | jq -r '.local_path')
CURRENT_VERSION=$(echo $DOWNLOADED_FOLDER | egrep -o '[0-9]+\.[0-9]+\.[0-9]+')
rm -f ngc_cli_output.txt

# Calculate the new version
IFS='.'
read -a strarr <<< "$CURRENT_VERSION"

NGC_VERSION_MAJOR="${strarr[0]}"
NGC_VERSION_MINOR="${strarr[1]}"
NGC_VERSION_PATCH="${strarr[2]}"

unset IFS

NEW_NGC_VERSION=${NGC_VERSION_MAJOR}.${NGC_VERSION_MINOR}."$(($NGC_VERSION_PATCH + 1))"

# Copy the directory so to have a backup to diff against
cp -r ${DOWNLOADED_FOLDER} candidate_ngc_resource

# Update all the YAML files that we are aware of:
# Base Image
# Create NVMF TARGET OFFLOAD version folder if doesn't exist
mkdir -p candidate_ngc_resource/configs/${NVMF_TARGET_OFFLOAD_VERSION}
 
# Update base image YAML:
cp container/doca_nvmf_target_offload.yaml candidate_ngc_resource/configs/${NVMF_TARGET_OFFLOAD_VERSION}/

# Diff and check if there is a need for update
set +e
diff -r $DOWNLOADED_FOLDER candidate_ngc_resource
diff_rc=$?
set -e

if [[ "$diff_rc" != "0" ]]; then
    ngc registry resource upload-version "${NGC_CLI_ORG}/${NGC_CLI_TEAM}/${NGC_CLI_RESOURCE_NAME}:${NEW_NGC_VERSION}" --source candidate_ngc_resource --format_type json
    echo "NGC=${NGC_CLI_ORG}/${NGC_CLI_TEAM}/${NGC_CLI_RESOURCE_NAME}:${NEW_NGC_VERSION}" >> artifact.properties
fi

rm -rf ${DOWNLOADED_FOLDER}
rm -rf candidate_ngc_resource
