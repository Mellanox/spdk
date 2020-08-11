#!/usr/bin/env python

from distutils.core import setup
import os.path
import shutil
long_description = 'Storage Performance Development Kit'

# I would like to rename rpc.py in the git repo. But
if not os.path.exists('spdk_rpc.py'):
    shutil.copy('rpc.py', 'spdk_rpc.py')
setup(
    name='spdk-rpc',
    version='20.07',
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
    scripts=['spdk_rpc.py', 'spdkcli.py', 'iostat.py'],
    data_files=[
        ('share/spdk',
            ['config_converter.py', 'dpdk_mem_info.py',
             'histogram.py', 'iostat.py', 'rpc_http_proxy.py']
        )]
)
