# Important: all variables must start with CI_ENV_
# otherwise it will not be replaced!

export CI_ENV_MOFED_VER="24.07-0.6.1.0"
export CI_ENV_REGISTRY_AUTH_FILE="/mnt/secret_podman/config_podman.json"
export CI_ENV_GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=no"
export CI_ENV_STORAGE_DRIVER="overlay"

export CI_ENV_NVCR_REGISTRY_HOST="nvcr.io"
export CI_ENV_NVCR_REGISTRY_PATH="nvstaging/doca"
  
export CI_ENV_HARBOR_REGISTRY_HOST="nbu-harbor.gtm.nvidia.com"
export CI_ENV_HARBOR_REGISTRY_PATH="swx-storage/doca_nvmf_target_offload"

export CI_ENV_ARTIFACT_PROPERTIES="/mnt/pvc/doca-sta-artifact.properties"

export CI_ENV_DOCA_BUILDER_IMAGE="nvcr.io/nvstaging/doca/doca"
export CI_ENV_DOCA_RUNTIME_TAG="3.1.0031-full-rt-ubuntu22.04-arm64"
export CI_ENV_DOCA_BUILDER_TAG="3.1.0031-devel-ubuntu22.04-arm64"

# Jenkins JNLP image
# https://nvbugspro.nvidia.com/bug/5115436
# https://gitlab-master.nvidia.com/nbu-sw-cd/docker-images/-/blob/master/c3po-jnlp/Dockerfile
export CI_ENV_JNLP_X86="nbu-harbor.gtm.nvidia.com/toolbox/c3po-jnlp"
export CI_ENV_JNLP_AARCH64="nbu-harbor.gtm.nvidia.com/toolbox/c3po-jnlp"

# Jenkins Docker image
export CI_ENV_DOCKER_X86="urm.nvidia.com/quay-remote/podman/stable:v5.3.2"
export CI_ENV_DOCKER_AARCH64="urm.nvidia.com/quay-remote/podman/stable:v5.3.2"


# Change this revision number always when you make any
# changes that affect components in CI builder images.
# CI builder images use it as docker tag.
# Format=<YYMMDD>-<ID>
export CI_ENV_CI_REV="250605-1"
