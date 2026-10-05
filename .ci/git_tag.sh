#!/bin/bash -ex
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

branch=$(git name-rev --name-only --refs '*nvda*' HEAD | awk -F/ '{print $NF}')

if [ -z "$VER" ]; then
	# Prefer version from the branch name. When HEAD is detached the ref
	# carries no version, so use the spec instead.
	if [[ "$branch" =~ [0-9]+(\.[0-9]+)* ]]; then
		VER=$BASH_REMATCH
	else
		# build_rpm.sh moves the spec to the tree root
		spec=scripts/spdk.spec
		[ -e "$spec" ] || spec=spdk.spec
		VER=$(awk '/^%define scm_version/ {print $3}' "$spec")
	fi
	export VER
fi

REV=${BUILD_NUMBER:-1}

git_tag="v$VER-${REV}"

# SCM checkout is from gitlab-master. Push the same tag to GitHub only when
# the PUBLISH_GITHUB pipeline parameter is checked.
GITLAB_REMOTE="${GITLAB_REMOTE:-ssh://git@gitlab-master.nvidia.com:12051/spdk_team/spdk.git}"
GITHUB_REMOTE="${GITHUB_REMOTE:-git@github.com:Mellanox/spdk.git}"

git tag $git_tag
git push "$GITLAB_REMOTE" $git_tag
if [[ "${PUBLISH_GITHUB:-}" == "true" ]]; then
	git push "$GITHUB_REMOTE" $git_tag
fi
