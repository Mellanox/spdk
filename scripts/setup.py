#!/usr/bin/env python

from distutils.core import setup
import os.path
import shutil
long_description = 'Storage Performance Development Kit'

# I would like to rename rpc.py in the git repo. But
for fname in ['rpc.py', 'rpc_http_proxy.py']:
    if not os.path.exists('spdk_{}'.format(fname)):
        shutil.copy('{}'.format(fname), 'spdk_{}'.format(fname))
setup(
    name='spdk-rpc',
    version='22.01',
    author='SPDK Mailing List',
    author_email='spdk@lists.01.org',
    description='SPDK RPC modules',
    long_description=long_description,
    url='https://spdk.io/',
    packages=['rpc', 'spdkcli'],
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
    scripts=[
        'spdk_rpc.py',
        'spdkcli.py',
        'iostat.py',
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
