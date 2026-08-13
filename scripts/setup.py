#!/usr/bin/env python
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

from distutils.core import setup
import os.path
import shutil
long_description = 'Storage Performance Development Kit'

# I would like to rename rpc.py in the git repo.
for fname in ['rpc.py',
              'rpc_http_proxy.py',
              'iostat.py']:
    if not os.path.exists('spdk_{}'.format(fname)):
        shutil.copy('{}'.format(fname), 'spdk_{}'.format(fname))
if not os.path.exists('spdk'):
    shutil.copytree('../python/spdk', 'spdk')
setup(
    name='spdk-rpc',
    version='25.05.6',
    author='SPDK Mailing List',
    author_email='spdk@lists.01.org',
    description='SPDK RPC modules',
    long_description=long_description,
    url='https://spdk.io/',
    packages=['spdk.rpc', 'spdk.spdkcli', 'spdk.cli'],
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
    scripts=[
        'spdk_rpc.py',
        'spdkcli.py',
        'spdk_iostat.py',
        'spdk_rpc_http_proxy.py'
    ],
    data_files=[
        (
            'share/spdk',
            [
                'dpdk_mem_info.py',
                'histogram.py'
            ]
        )
    ]
)
