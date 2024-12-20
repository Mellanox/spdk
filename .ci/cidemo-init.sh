#!/bin/bash -eE

SCRIPT=$(readlink -f "$0")
SCRIPTPATH=$(dirname "$SCRIPT")

info() {
  echo "[$0] [INFO]: $1"
}

error() {
  echo "[$0] [ERROR]: $1"
}

# conf_file checks
if [ -z "${conf_file}" ]; then
    error "ENV variable 'conf_file' is not defined!"
    exit 1
fi

if [ ! -e "${conf_file}.in" ]; then
    error "Template ${conf_file}.in isn't found!"
    exit 1
fi

# CI env path checks
if [ -z "${CI_ENV_PATH}" ]; then
    info "CI_ENV_PATH isn't defined. Using default: ${SCRIPTPATH}/ci_env.sh"
    CI_ENV_PATH="$SCRIPTPATH/ci_env.sh"
fi

if [ ! -e "$CI_ENV_PATH" ]; then
    error "CI env file ${CI_ENV_PATH} doesn't exist!"
    exit 1
fi

info "CI env file: $CI_ENV_PATH"
cat $CI_ENV_PATH

CI_ENV_VARS=$(grep '^export CI_ENV_' $CI_ENV_PATH | sed 's/^export \(CI_ENV_[^=]*\)=.*$/\$\1/' | tr '\n' ' ')

if [ -z "$CI_ENV_VARS" ]; then
    error "CI Variables not found in the $CI_ENV_PATH!"
    exit 1
fi

source $CI_ENV_PATH

if ! type -p envsubst; then 
    info "envsubst isn't found. Trying to install it..."
    apt update
    apt install -y gettext-base
fi

envsubst "$CI_ENV_VARS" < ${conf_file}.in > ${conf_file}

info "Created ${conf_file}:"
cat "${conf_file}"
