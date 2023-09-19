#!/usr/bin/env python2
from __future__ import print_function
import sys
import os
import subprocess
from argparse import ArgumentParser
import re
import time
# from requests.structures import CaseInsensitiveDict

DEFAULT_NVMF_TGT_CONF = '/etc/spdk/nvmf_tgt.conf'

pci_ids = dict()
iotdma_ids = []
username = None
has_iommu = os.path.exists('/sys/kernel/iommu_groups/0')
stub_driver = 'vfio-pci' if has_iommu and os.path.exists('/sys/bus/pci/drivers/vfio-pci') else 'uio_pci_generic'


class SpdkConfigParser(object):

    def __init__(self):
        self._sections = {}
        self._accum = []
        self._hdr_ary = []
        self._hdr = ''

    def _flush_accum(self):
        if self._accum:
            self._sections[self._hdr] = self._accum
            self._accum = []
            self._hdr_ary.append(self._hdr)
            self._hdr = ''

    def readfp(self, fp):
        """
        Read raw config stream from the file object `fp`
        Populate internal structures with data
        """
        raw_body = fp.read()
        # 1st, need to handle Unicode BOM properly
        if buffer(raw_body, 0, 3) == b"\xEF\xBB\xBF":
            raw_body = buffer(raw_body, 3, len(raw_body)-3).decode('utf-8')
        else:
            # assume we have plain ascii file
            pass
        re_comment = re.compile(r'^\s*#|^\s*$')
        re_sect = re.compile(r'^\[(\w+)\]')
        for line in raw_body.splitlines():
            if re_comment.search(line):
                continue
            m = re_sect.search(line)
            if m:
                self._flush_accum()
                self._hdr = m.group(1)
                continue
            ary = line.strip().split(' ', 1)
            if ary:
                self._accum.append(ary)
        self._flush_accum()
        # print(repr(self._sections))

    def sections(self):
        # return self._sections.keys()
        return self._hdr_ary

    def dump_cfg(self, fp):
        for hdr in self.sections():
            fp.write("[{0}]\n".format(hdr))
            for ary in self._sections[hdr]:
                fp.write("  {0}\n".format(' '.join(ary)))

    def __getitem__(self, key):
        """
        Return section by key
        """
        k_l = key.lower()
        ary = filter(lambda a: a.lower() == k_l, self._hdr_ary)
        if not ary:
            raise KeyError(key)
        return self._sections[ary[0]]


def do_test(config):
    # for sec in config.sections():
    #    print(sec)
    config.dump_cfg(sys.stdout)
    return os.EX_OK


def _rescan_pci_bus():
    open('/sys/bus/pci/rescan', 'w').write("1")


def linux_bind_driver(bdf, driver_name):
    old_driver_name = "no driver"
    cls, ven_id, dev_id = pci_ids[bdf]
    sysfs = '/sys/bus/pci/devices'
    driver_fn = os.path.join(sysfs, bdf, 'driver')
    if os.path.exists(driver_fn):
        old_driver_name = os.path.basename(os.readlink(driver_fn))
        if old_driver_name == driver_name:
            return 0
        try:
            with open(os.path.join(driver_fn, 'remove_id'), 'w') as fo:
                fo.write("{0} {1}".format(ven_id, dev_id))
        except:
            pass
        with open(os.path.join(driver_fn, 'unbind'), 'w') as fo:
              fo.write(bdf)
    print("{0} ({1} {2}): {3} -> {4}".format(bdf, ven_id, dev_id, old_driver_name, driver_name))

    try:
        fn = "/sys/bus/pci/drivers/{0}/new_id".format(driver_name)
        # print("new_id {0}:{1} to {2}".format(ven_id, dev_id, driver_name))
        with open(fn, 'w') as fo:
            fo.write("{0} {1}".format(ven_id, dev_id))
    except IOError, ex:
        pass  # [Errno 17] File exists
    except Exception, ex:
        print("ERR: {0}: {1}".format(fn, ex), file=sys.stderr)
    try:
        # print("bind {0} to {1}".format(bdf, driver_name))
        fn = "/sys/bus/pci/drivers/{0}/bind".format(driver_name)
        with open(fn, 'w') as fo:
            fo.write(bdf)
    except IOError, ex:
        pass  # [Errno 17] File exists
    except Exception, ex:
        print("ERR: {0}: {1}".format(fn, ex), file=sys.stderr)
    fn = os.path.join('/sys/bus/pci/devices', bdf, 'iommu_group')
    if not os.path.exists(fn):
        return 0
    iommu_group = os.path.basename(os.readlink(fn))
    fn = os.path.join('/dev/vfio', iommu_group)
    if os.path.exists(fn) and username:
        subprocess.call(['chown', username, fn])


def do_reset(args):
    print("Rescan PCI devices (load kernel modules)")
    nvme_bdf_list = filter(lambda a: pci_ids[a][0] == '0108', pci_ids.keys())
    for bdf in nvme_bdf_list:
        linux_bind_driver(bdf, 'nvme')
    for bdf in iotdma_ids:
        linux_bind_driver(bdf, 'ioatdma')
    _rescan_pci_bus()
    return os.EX_OK


def unbind_nvme_dev(pciaddr=None):
    sysfs = '/sys/class/nvme'
    if not os.path.exists(sysfs):
        return 0
    nvme_devs = filter(lambda e: os.path.islink(os.path.join(sysfs, e)),
                       os.listdir(sysfs))
    if not nvme_devs:
        sys.stderr.write("unbind_nvme_dev: no NVMe PCI devices found\n")
        return

    for dev in nvme_devs:
        pa = os.path.basename(os.readlink(os.path.join(sysfs, dev, 'device')))
        if pciaddr is not None:
            assert type(pciaddr) == str
            if pciaddr != pa:
                continue
#        remove_fn = os.path.join(sysfs, dev, 'device/remove')
#        print("Remove [%s] %s" % (pa, remove_fn))
#        for it in range(3):
#            try:
#                with open(remove_fn, 'w') as fo:
#                    fo.write("1")
#                break
#            except:
#                time.sleep(1)
        print("Load %s for %s" % (stub_driver, pa))
        linux_bind_driver(pa, stub_driver)


def do_config(args):
    try:
        nvme_sec = config['Nvme']
        nvme_list = map(lambda a: a[1],
                        filter(lambda a: a[0] == 'TransportId', nvme_sec))
    except KeyError:
        sys.stderr.write("No [Nvme] section found in the configuration file\n")
        # return os.EX_DATAERR
        # It should be ok -> nothing to do
        # On other hand I need to check presence of Nvmf.Hotplug
        return os.EX_OK
    if not nvme_list:
        # no explicitly defined PCIe devices?
        hp = map(lambda a: a[1].lower(),
                 filter(lambda a: a[0] == 'HotplugEnable', nvme_sec))
        if hp and hp[0] == 'yes':
            unbind_nvme_dev()  # unbind ALL
        else:
            print("No PCIe NVME(s) used, hotplug disabled. Exit")
        return os.EX_OK
    # trtype:PCIe traddr:0000:81:00.0
    rx = re.compile(r'trtype:PCIe\s+traddr:([0-9a-f:.]+)')
    for line in nvme_list:
        m = rx.search(line)
        if m:
            pciaddr = m.group(1)
            unbind_nvme_dev(pciaddr)
            linux_bind_driver(pciaddr, stub_driver)
    for bdf in iotdma_ids:
        linux_bind_driver(bdf, stub_driver)
    time.sleep(1)
    return os.EX_OK


def find_iotdma():
    # lspci -D -v |grep 'System peripheral: Intel.*DMA'
    global iotdma_ids
    stdout = subprocess.check_output(['lspci', '-D', '-v'])
    rx = re.compile('^(\S+)\sSystem peripheral: Intel.*DMA')
    for line in stdout.splitlines():
        m = rx.search(line)
        if not m:
           continue
        iotdma_ids.append(m.group(1))


def load_stub_driver():
    global stub_driver
    subprocess.call(['/sbin/modprobe', stub_driver])
    if has_iommu and os.path.exists('/sys/bus/pci/drivers/vfio-pci'):
        stub_driver = 'vfio-pci'
    else:
        stub_driver = 'uio_pci_generic'
    # print("stub_driver: %s" % stub_driver)


def load_pci_ids():
    global pci_ids
    #ff:1f.2 0880: 8086:2f8a (rev 02)
    stdout = subprocess.check_output(['lspci', '-D', '-n'])
    rx = re.compile('^(\S+)\s(\w+):\s(\w+):(\w+)\s?')
    for line in stdout.splitlines():
       m = rx.search(line)
       if not m:
           continue
       bdf, cls, ven_id, dev_id = map(lambda i: m.group(i), (1,2,3,4))
       pci_ids[bdf] = (cls, ven_id, dev_id)
    # print(repr(pci_ids))


if __name__ == '__main__':
    parser = ArgumentParser(prog='setup_nvmf_tgt')
    parser.add_argument('-f', '--cfg-file', dest='config_fn',
                        help='nvmf_tgt configuration file',
                        default=DEFAULT_NVMF_TGT_CONF)
    meg = parser.add_mutually_exclusive_group(required=True)
    meg.add_argument('-t', '--test', dest='do_test',
                     action='store_true', default=False,
                     help='Test configuration file')
    meg.add_argument('-c', '--config', dest='do_config',
                     action='store_true', default=False,
                     help='Configure (unbinding of NVME)')
    meg.add_argument('-r', '--reset', dest='do_reset',
                     action='store_true', default=False,
                     help='Do reset (restore bindings of NVME)')
    args = parser.parse_args()
    config = SpdkConfigParser()
    try:
        config.readfp(open(args.config_fn))
        load_pci_ids()
        find_iotdma()
        load_stub_driver()
        if args.do_test:
            rc = do_test(config)
        elif args.do_config:
            rc = do_config(config)
        elif args.do_reset:
            rc = do_reset(config)
    except IOError, ex:
        sys.stderr.write("%s: %s" % (args.config_fn, str(ex)))
        rc = os.EX_IOERR
    sys.exit(rc)
