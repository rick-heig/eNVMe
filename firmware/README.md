# *evil* NVMe firmware

The eNVMe firmware is a PCI endpoint function driver for the Linux PCI endpoint framework (https://www.kernel.org/doc/html/latest/PCI/endpoint/index.html). It is based on the following mainline NVMe PCI endpoint : https://elixir.bootlin.com/linux/v6.15/source/drivers/nvme/target/pci-epf.c

## Building

The eNVMe firmware can be built out-of-tree (OoT) on the host computer or in the embedded platform, to do so make sure you followed the [doc/platform.md](../doc/platform.md) instructions to setup.

The firmware can be built from the host or embedded platform with `make`. If building on the embedded platform it can be installed with `sudo make install`, if built from the host the `nvmet-pci-epf.ko` file can be copied on the SD card on `/lib/modules/6.15.0/kernel/drivers/nvme/target/`.

## Launching

On the embedded platform it can be launched with the `nvme-epf` command.

```
nvme-epf
Usage:
    nvme-epf [-h | --help]
    nvme-epf [options] start
    nvme-epf stop
Start command options:
  --debug-pci        : Turn on pci controller debug messages
  --debug-nvme       : Turn on nvme target core debug messages
  --debug-epf        : Turn on nvme PCI endpoint target debug messages
  --debug            : Turn on epf, nvme and pci kernel debug messages
  --nomsix           : Do not enable MSI-X (Default: enabled)
  --nomsi            : Do not enable MSI (Default: enabled)
  --mdts <size (KB)> : Set maximum command transfer size
                       (default 128 KB)
  --mqes <num>       : Set maximum queue size
  --buffered-io      : Used buffered IOs on the target
                       (default: no).
  --nrioq <num>      : Set maximum number of I/O queues
                       (default: number of CPUs).
  --nsdev <path>     : Use file or block device for the namespace
                       (default: use null_blk and /dev/nullb0).
  --sched <sched>    : Use the <sched> I/O scheduler for the NS device
  --bs < LBA size>   : Specify null_blk LBA size (Default: 4096)
  --rot              : Create a null_blk device with rotational=1
```

For example:

With a USB key (`/dev/sda`) as storage backend

```
sudo nvme-epf --loop /dev/sda --model "evil NVMe device" start
```

If there is no need to use backend storage the `--loop` option can be omitted. This will result in a "null" backend, no data will be stored, zeroes will be read. You can also use ram block devices e.g., `/dev/ram0` or any other block device.

Note: The `--loop` option is the same as (an alias for) `--nsdev`.

## Functionalities

### Reading / Writing PCI space

The eNVMe endpoint function driver exposes the entire host PCI space as a character device `/dev/pci-io`. Reading and writing to this character device will in fact read and write the host PCI space.

The 64-bit offset given to `/dev/pci-io` will be the address at which the PCI space is read or written (use `pread()` and `pwrite()` [syscalls](https://man7.org/linux/man-pages/man2/pwrite.2.html)).

This also allows to use the eNVMe device with PCILeech in order to generate PCI direct memory access (DMA) attacks see [doc/pcileech.md](../doc/pcileech.md).

### Remote activation

Remote activation is done through an activation key, a default key, `activation_key`, of 256 bytes is given as an example.

The firmware has a [work queue](https://www.kernel.org/doc/html/latest/core-api/workqueue.html) `evil_work` that allows to schedule asynchronous tasks. The reason for this work queue is so that "evil" related tasks can be executed without interfering with the normal NVMe functionality.

In the firmware for all NVMe "write" commands that have been processed (completion sent to host), the function `pci_epf_nvme_evil_work()` is scheduled. This function will compare the data written (if smaller than 128 KB for performance reasons) and if the data contains the activation key, the eNVMe is "remote activated".

For now this only sets the `evil_activated` variable to true. The idea is to show a way to notify the eNVMe remotely that it should act. Remote activation could be done with any data written to the eNVMe disk, this could be a web cookie, an e-mail, a log file etc.

### Call user space programs

It is possible to launch user space programs from this driver. There is an example in the `pci_epf_nvme_probe()` function as a proof of concept. The example calls `/bin/sh` to execute a command that calls `echo Hello from kernel space!` and redirects the output to `/tmp/kernel_output.txt`.

This provides a way to coordinate attacks based on user space programs through a call within the eNVMe PCI endpoint function driver.

### NVMe

As the handling of NVMe submissions is entirely handled, *hacks* can be added at any stage. Examples include corrupting the data, data manipulation, submission and completion modifications, fuzzing, generation of spurious completions and IRQs, whatever comes to mind.

One example is replacing data for specific read submissions on sensitive data. For example patching the kernel binaries, argument strings, or /init executable while being read.

We implemented an attack against Linux hosts that would replace `/sbin/init` to run a payload, once the payload executed it reread `/sbin/init` from disk and replaces itself with the regular `/sbin/init`.

### Detect host shutdown

When the host machine will shutdown it should gracefully disabled and shutdown the NVMe drive. The host will wait for the controller to set the "shutdown status complete" bit, before the host will finally turn off. The code for this is in the `pci_epf_nvme_disable_ctrl()` function. This leaves a small window of opportunity where we know the host has unmounted all file systems on the disk and is not actively using it. This is a good place to implement attacks that modify the file system. In our experimental setup of course the NVMe device can be left on while the host is turned off to perform in-depths file system modifications, however in a real case scenario the NVMe device will be powered off right after it signals the "shutdown status complete".

### Sending IRQs

It is possible to send PCI legacy / MSI / MSI-X IRQs as well (with `pci_epf_raise_irq()`), for the moment we have no attack examples or ideas based on this but it might be interesting to explore.

### Mount file system locally

The backing storage for the eNVMe can be mounted locally, mounting in read-only can be done any time, this allows to perform file-system aware attacks for example (file extraction, spying, etc.).

Mounting file systems in read-write should be done if unmounted in the host. Here with our experimental setup we can play around with the NVMe device powered on while the host is powered off. However, in real scenarios such attacks would be done in a small timeframe before shutdown, see "Detect host shutdown" above.

#### Linux GRUB disable IOMMU attack

One attack would be to disable the IOMMU if turned on by changing the option in the file system. For example for a Linux host the option could be added to the kernel command line in the grub.cfg file.

```shell
sed -i '/^ *linux / s/quiet \([^ ]*\)/quiet amd_iommu=off intel_iommu=off \1/' /mnt/local/boot/grub/grub.cfg
```