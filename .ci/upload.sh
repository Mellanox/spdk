#!/bin/bash -eEx

upload_deb() {

    # Import gpg key
    gpg --import ${GPG_KEY_PATH}

    shopt -s nullglob

    deb_pkgs=(${name}*${VER}-${REV}_{${arch},all}*.deb)
    for deb_pkg in ${deb_pkgs[@]}; do
        test -e $deb_pkg
        echo "INFO: Signing package ${deb_pkg##*/}"
        dpkg-sig -k ${gpg_key_name} -s builder ${deb_pkg}
        upload_url="${REPO_URL}/${repo_name}/"
        echo "INFO: Uploading package ${deb_pkg} to ${upload_url}"
        ${bd}/actions/nexus.py apt -u ${REPO_URL} -U ${REPO_USER} \
            -P ${REPO_PASS} -a upload -n ${repo_name} --file ${deb_pkg}

        if test -n "$release_dir" ; then
            upload_dir="${release_dir}/${repo_name}/${arch}/"
            mkdir -p $upload_dir
            cp -f $deb_pkg $upload_dir
        fi
    done
}

upload_rpm() {

    shopt -s nullglob

    releasever=$(rpm --eval "%{rhel}")

    rpms_location=(${HOME}/rpmbuild/RPMS/${arch}/${name}-*${VER}-${REV}*.rpm)
    for rpm_location in ${rpms_location[@]}; do
        test -f $rpm_location
        rpm_name="${rpm_location##*/}"
        ${bd}/actions/nexus.py yum -u ${REPO_URL} -U $REPO_USER \
            -P $REPO_PASS -a upload -n ${repo_name} --file ${rpm_location} \
            --upload_path ${releasever}/${arch}/

        if test -n "$release_dir" ; then
            upload_dir="${release_dir}/${repo_name}/${releasever}/${arch}/"
            mkdir -p $upload_dir
            cp -f $rpm_location $upload_dir
        fi
    done

    srpms_location=(${HOME}/rpmbuild/SRPMS/${name}-*${VER}-${REV}*.src.rpm)
    for srpm_location in ${srpms_location[@]}; do
        test -f $srpm_location
        srpm_name="${srpm_location##*/}"
        ${bd}/actions/nexus.py yum -u ${REPO_URL} -U $REPO_USER \
            -P $REPO_PASS -a upload -n ${repo_name} --file ${srpm_location} \
            --upload_path ${releasever}/SRPMS/

        if test -n "$release_dir" ; then
            upload_dir="${release_dir}/${repo_name}/${releasever}/SRPMS/"
            mkdir -p $upload_dir
            cp -f $srpm_location $upload_dir
        fi
    done
}

bd=$(dirname $0)
user=${USER:-root}
: ${REPO_URL:?REPO_URL is not found!}
: ${REPO_USER:?REPO_USER is not found!}
: ${REPO_PASS:?REPO_PASS is not found!}

branch=$(git name-rev --name-only HEAD | awk -F/ '{print $NF}')

if [ -z "$VER" ] ; then
    export VER=$(echo $branch | grep -o '[0-9]\+\(\.[0-9]\+\)*')
fi

name="spdk"
repo_name="${name}"

if command -v ofed_info >/dev/null 2>&1; then
    # 4.6-1.0.1.2 => 4.6
    ofed_ver=$(ofed_info -n | cut -d - -f1)
    repo_name="${repo_name}-mlnx-ofed-${ofed_ver}"
fi

if test -n "$ghprbPullId" ; then
    REV="pr${ghprbPullId}"
    repo_name="${repo_name}-pr"
else
    REV=${BUILD_NUMBER:-1}
fi

if [[ -f /etc/debian_version ]]; then

    codename=$(lsb_release -cs)
    repo_name="${repo_name}-${codename}-apt"
    arch=$(dpkg --print-architecture)
    : ${GPG_KEY_PATH:? GPG_KEY_PATH is not found!}
    gpg_key_name=$(echo ${GPG_KEY_PATH##*/} | cut -d . -f 1)

    apt update && apt install python3-requests -y

    # Create APT repository
    ${bd}/actions/nexus.py apt --name ${repo_name} \
        --url ${REPO_URL} --user ${REPO_USER} \
        --password ${REPO_PASS} --action create \
        --distro ${codename} --keypair-file ${GPG_KEY_PATH%.*}.priv \
        --write-policy allow || true

    upload_deb

elif [[ -f /etc/redhat-release ]]; then

    repo_name="${repo_name}-yum"
    arch=$(uname -m)

    yum install python3-requests -y || yum install python36-requests -y

    # Create YUM repository
    ${bd}/actions/nexus.py yum --name ${repo_name} \
        --url ${REPO_URL} --user ${REPO_USER} \
        --password ${REPO_PASS} --repo-data-depth 2 \
        --write-policy allow --action create || true

    upload_rpm

else

    echo "Not supported Linux version!"
    exit 1

fi
