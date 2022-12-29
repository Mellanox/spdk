%define scm_version 22.05
%define unmangled_version %{scm_version}
%define scm_rev %{_rev}
Epoch: 0

%define pkg_prefix /opt/mellanox/spdk
%define _build_id_links alldebug

Name:           spdk
Version:        %{scm_version}
Release:        %{scm_rev}%{?dist}
Summary:        Storage Performance Development Kit
Packager:       andriih@nvidia.com

Group:          System Environment/Daemons
License:        BSD and LGPLv2 and GPLv2
URL:            http://www.spdk.io
Source0:        spdk-%{version}.tar.gz

%define package_version %{epoch}:%{version}-%{release}
%define install_datadir %{buildroot}/%{_datadir}/%{name}
%define install_sbindir %{buildroot}/%{_sbindir}
%define install_bindir %{buildroot}/%{_bindir}
%define install_docdir %{buildroot}/%{_docdir}/%{name}

# It is somewhat hard to get SPDK RPC working with python 2.7
# Distros that don't support python3 will use python2
%if 0%{?rhel} >= 7
# So, let's switch to Python36 from IUS repo - https://github.com/iusrepo/python36
%define use_python python3.6
%define python_ver 3.6
%else
# on Fedora 28+ we have python3 == 3.7
%define use_python python3
%define python_ver 3.7
%ifarch x86_64
BuildRequires:  clang-analyzer
%endif
%endif

ExclusiveArch: x86_64 aarch64
%ifarch aarch64
%global machine armv8a
%global target arm64-%{machine}-linuxapp-gcc
%global _config arm64-%{machine}-linuxapp-gcc
%else
%global machine default
%global target %{_arch}-%{machine}-linuxapp-gcc
%global _config %{_arch}-native-linuxapp-gcc
%endif

# DPDK dependencies
BuildRequires: kernel-headers
BuildRequires: doxygen
BuildRequires: graphviz
BuildRequires: numactl-devel
BuildRequires: libiscsi-devel

# SPDK build dependencies
BuildRequires:	make gcc gcc-c++
BuildRequires:	CUnit-devel, libaio-devel, openssl-devel, libuuid-devel 
BuildRequires:	libiscsi-devel

%if 0%{?rhel} >= 8
# Starting from rhel 8 package name git-core
BuildRequires:  git-core
%else
BuildRequires:  git
%endif

# lcov is only available in rhel 7
%if 0%{?rhel} == 7
BuildRequires:  lcov
%endif

# Additional dependencies for NVMe over Fabrics
BuildRequires:	libibverbs-devel, librdmacm-devel

# SPDK runtime dependencies
Requires:	libibverbs
Requires:	librdmacm 
Requires:	sg3_utils
Requires:   libhugetlbfs-utils
%if "%{use_python}" == "python3.6"
Requires: %{name}%{?_isa} = %{package_version} python36
%else
Requires: %{name}%{?_isa} = %{package_version} python3 python3-pexpect
# Additional dependencies for SPDK CLI 
BuildRequires:	python3-pep8 python3-configshell
%endif

%if 0%{?rhel} > 7 || %{defined openEuler}
Requires: python3-configshell
BuildRequires: python3-configshell
%endif

%description
The Storage Performance Development Kit (SPDK) provides a set of tools and
libraries for writing high performance, scalable, user-mode storage
applications (sha1 %{_sha1}).

%global debug_package %{nil}

%prep
%setup -q

%build
sed -i -e 's!/usr/bin/python$!/usr/bin/python'%{python_ver}'!' dpdk/config/arm/armv8_machine.py
sed -i 's#CONFIG_PREFIX="/usr/local"#CONFIG_PREFIX="'%{pkg_prefix}'"#' CONFIG

%ifarch aarch64
sed -i 's#CONFIG_ARCH=.*#CONFIG_ARCH=armv8-a#' CONFIG
%endif

LDFLAGS="$LDFLAGS -Wl,-rpath,%{pkg_prefix}/lib"
export LDFLAGS
./configure \
        --prefix=%{pkg_prefix} \
%ifarch aarch64
        --target-arch=armv8-a \
%endif
        --disable-coverage \
        --disable-debug \
        --disable-tests \
        --disable-unit-tests \
        --without-isal \
        --without-crypto \
        --without-fio \
        --with-vhost \
        --without-pmdk \
        --without-rbd \
        --with-rdma \
        --without-vtune \
        --with-shared \
        --with-raid5

# SPDK make
make %{?_smp_mflags}

%install
mkdir -p %{install_bindir}
mkdir -p %{install_sbindir}
install -p -m 755 build/bin/spdk_tgt %{install_sbindir}
install -p -m 755 build/bin/vhost %{install_sbindir}
install -p -m 755 build/bin/iscsi_tgt %{install_sbindir}
install -p -m 755 build/bin/iscsi_top %{install_bindir}
install -p -m 755 build/bin/spdk_trace %{install_bindir}
install -p -m 755 build/bin/spdk_lspci %{install_bindir}
install -p -m 755 build/bin/spdk_trace_record %{install_bindir}
install -p -m 755 build/bin/spdk_top %{install_bindir}
install -p -m 755 build/examples/perf %{install_sbindir}/nvme-perf
install -p -m 755 build/examples/identify %{install_sbindir}/nvme-identify
install -p -m 755 build/examples/nvme_manage %{install_sbindir}/
install -p -m 755 build/examples/blobcli %{install_sbindir}/
install -p -m 755 contrib/setup_nvmf_tgt.py %{install_sbindir}
install -p -m 755 contrib/setup_vhost.py %{install_sbindir}
install -p -m 755 contrib/vhost_add_config.sh %{install_sbindir}
install -p -m 755 contrib/setup_hugepages.sh %{install_sbindir}
systemd_dir=${RPM_BUILD_ROOT}%{_prefix}/lib/systemd/system
mkdir -p ${systemd_dir}
mkdir -p %{buildroot}%{_sysconfdir}/default
mkdir -p %{buildroot}%{_sysconfdir}/spdk
mkdir -p %{install_datadir}
mkdir -p %{install_datadir}/scripts
mkdir -p %{install_datadir}/include/spdk
install -p -m 644 include/spdk/pci_ids.h %{install_datadir}/include/spdk
install -p -m 644 scripts/common.sh %{install_datadir}/scripts
install -p -m 755 scripts/setup.sh %{install_datadir}/scripts
install -p -m 755 scripts/spdk-gpt.py %{install_datadir}/scripts
mkdir -p ${RPM_BUILD_ROOT}%{pkg_prefix}
cp -pr dpdk/build/lib ${RPM_BUILD_ROOT}%{pkg_prefix}
cp -pr dpdk/build/include ${RPM_BUILD_ROOT}%{pkg_prefix}
rm -rf ${RPM_BUILD_ROOT}%{pkg_prefix}/share/dpdk/examples
cp -pr include/spdk ${RPM_BUILD_ROOT}%{pkg_prefix}/include/
cp -pr build/lib/*.*    ${RPM_BUILD_ROOT}%{pkg_prefix}/lib/
install -p -m 644 contrib/spdk_tgt.service ${systemd_dir}
install -p -m 644 contrib/vhost.service ${systemd_dir}
install -p -m 644 contrib/default/spdk_tgt %{buildroot}%{_sysconfdir}/default/spdk_tgt
install -p -m 644 contrib/default/vhost %{buildroot}%{_sysconfdir}/default/vhost
install -p -m 644 contrib/vhost.conf.example %{buildroot}%{_sysconfdir}/spdk/

# Install SPDK rpc services
mkdir -p %{buildroot}/%{_libdir}/python%{python_ver}/site-packages
cp -a python/spdk %{buildroot}/%{_libdir}/python%{python_ver}/site-packages
install -p -m 755 scripts/rpc.py %{install_bindir}/spdk_rpc.py
install -p -m 755 scripts/rpc_http_proxy.py %{install_bindir}/spdk_rpc_http_proxy.py
install -p -m 755 scripts/spdkcli.py %{install_bindir}/spdkcli
sed -i -e 's!/usr/bin/env python3$!/usr/bin/python'%{python_ver}'!' %{install_bindir}/{spdk_rpc.py,spdk_rpc_http_proxy.py,spdkcli}
mkdir -p %{buildroot}%{_sysconfdir}/bash_completion.d
install -p -m 644 scripts/bash-completion/spdk %{buildroot}%{_sysconfdir}/bash_completion.d
sed -i -e 's/ rpc.py/ spdk_rpc.py/' %{buildroot}%{_sysconfdir}/bash_completion.d/spdk

%files
%{_sbindir}/*
%{_bindir}/*
%{_prefix}/lib/systemd/system/*.service
%{_libdir}/*
%{_datadir}/*
%config(noreplace) %{_sysconfdir}/default/*
%config(noreplace) %{_sysconfdir}/spdk/*
%{_sysconfdir}/bash_completion.d/*
%doc README.md LICENSE
# %files -n dev
%{pkg_prefix}/*

%post
case "$1" in
1) # install
	systemctl daemon-reload
	;;
2) # upgrade
	systemctl daemon-reload
	;;
esac

%changelog
* %{_date} Andrii Holovchenko <andriih@nvidia.com>
- build from %{_branch} (sha1 %{_sha1})

* Mon Jun 6 2022 Andrii Holovchenko <andriih@nvidia.com>
- Ported to v22.05 release

* Wed Feb 2 2022 Andrii Holovchenko <andriih@nvidia.com>
- Ported to v22.01 release

* Tue Oct 26 2021 Andrii Holovchenko <andriih@nvidia.com>
- Use armv8-a CPU for aarch64

* Mon Oct 11 2021 Andrii Holovchenko <andriih@nvidia.com>
- Add spdk_rpc_http_proxy.py

* Thu Sep 30 2021 Andrii Holovchenko <andriih@nvidia.com>
- Add raid5 support

* Sun Aug 8 2021 Andrii Holovchenko <andriih@nvidia.com>
- Ported to v21.07 release

* Thu Apr 22 2021 Andrii Holovchenko <andriih@nvidia.com>
- Ported to v21.01.1 release

* Mon Dec 14 2020 Andrii Holovchenko <andriih@nvidia.com>
- Install spdk_top

* Wed Nov 18 2020 Andrii Holovchenko <andriih@nvidia.com>
- build requires git-core for rhel 8.x
- explicitly define python version in dpdk/config/arm/armv8_machine.py

* Thu Nov 5 2020 Andrii Holovchenko <andriih@nvidia.com>
- fix setup.sh install path
- remove nvmf_tgt service files

* Wed Nov 4 2020 Andrii Holovchenko <andriih@nvidia.com>
- ported to v20.10 release
- add rhel8 support

* Tue Aug 11 2020 Andrii Holovchenko <andriih@nvidia.com>
- ported to v20.07 release

* Fri Jun 05 2020 Yuriy Shestakov <yuriis@mellanox.com>
- ported to v20.04.1 release

* Tue Jan 28 2020 Yuriy Shestakov <yuriis@mellanox.com>
- ported to v20.01 pre-release

* Wed Aug  7 2019 Yuriy Shestakov <yuriis@mellanox.com>
- ported to v19.07 release

* Mon Jul 15 2019 Yuriy Shestakov <yuriis@mellanox.com>
- ported to v19.04.1 release

* Thu May  2 2019 Yuriy Shestakov <yuriis@mellanox.com>
- ported to v19.04 release

* Thu Apr 11 2019 Yuriy Shestakov <yuriis@mellanox.com>
- added vhost config/service definition
- packaged v19.04-pre

* Fri Nov 16 2018 Yuriy Shestakov <yuriis@mellanox.com>
- build in a Docker image with upstream rdma-core libs
- packaged v18.10

* Wed May 16 2018 Yuriy Shestakov <yuriis@mellanox.com>
- Initial packaging
