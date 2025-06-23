# Platform setup

Here are the instructions to setup the eNVMe platform from scratch, a prebuilt SD card image will be provided **TODO** as setting everything up from scratch takes quite some time.

The instructions below were done on an Ubuntu 24.04.1 LTS host system, they should work for any Linux distribution, some changes may be required, e.g., install programs via another package manager etc.

## Setup

From the `work` directory :

### 1. Clone linux, buildroot and build

```shell
git clone -b rk3588_ep_v17 --single-branch https://github.com/rick-heig/linux.git
ENVME_LINUX_PATH=$(realpath linux)
git clone -b rk3588_on_rock5b_ep_v15 --single-branch https://github.com/rick-heig/buildroot.git
cd buildroot
echo "LINUX_OVERRIDE_SRCDIR = ${ENVME_LINUX_PATH}" > local.mk
# This is the configuration for the CM3588 board (see note below)
make cm3588_nas_ep_defconfig
make
```

**Note:** because the CM3588 and T6 boards have very similar schematics and the same CPU the image for the CM3588 will boot on the T6, changing the Linux device tree and device tree overlay will be enough. Instructions to do so are given below.

Board information and schematics:
- https://wiki.friendlyelec.com/wiki/index.php/CM3588
- https://wiki.friendlyelec.com/wiki/index.php/NanoPC-T6

### 2. Build OoT eNVMe Module

See [firmware/README.md](../firmware/README.md).

### 3. Prepare SD card

```shell
# Copy data to the SD card
sudo dd if=output/images/sdcard.img of=/dev/<your SD card device>
# Sync so that the SD can be safely ejected
sudo sync
```

### 4. Resize the partition

```shell
# Make sure the partitions are unmounted
sudo umount /dev/<the SD card device>*
# Open the SD card device with fdisk
sudo fdisk /dev/<the SD card device> # e.g., /dev/sdg
# Print the current partition table with 'p' and note the start sector of the RootFS (last) partition
# Delete the current RootFS partition (it should be partition 1, the last one) with 'd'
# Create a new partition (number 1) with 'n', set the first sector to be exactly the same as it was above and use the default parameters for the size, keep the signature !
# write the changes with 'w'
# Also redo the same in the hybrid MBR, by pressing 'M' to enter, then delete and redo the partition as above
# Check the filesystem with e2fsck (fix if errors are found)
sudo e2fsck -f /dev/<the SD card device RootFS partition> # e.g., /dev/sdg1
# Resize the RootFS
sudo resize2fs /dev/<the SD card device RootFS partition> # e.g., /dev/sdg1
# Check the filesystem with e2fsck (fix if errors are found)
sudo e2fsck -f /dev/<the SD card device RootFS partition> # e.g., /dev/sdg1
# Make sure changes are written to SD card with sync
sudo sync
# Eject and remount
# check size with
df -h
```

### 5. Prepare the Ubuntu 24.04.1 RootFS

**Note:** The `.img` we mount here has a fixed size, so we can't just copy files to it indefinitely, we copy some small files here, but the rest is copied onto the SD card rather than in the mounted `.img`.

```shell
# Go to the work directory
cd work
# Download a pre-installed image
wget https://cdimage.ubuntu.com/ubuntu-server/noble/daily-preinstalled/current/noble-preinstalled-server-arm64.img.xz
# Unxzip
unxz noble-preinstalled-server-arm64.img.xz
# Check disk partitions with fdisk # Take note of the "start" offset of the partition of interest (the first one, the biggest one)
fdisk -lu $(pwd)/noble-*
# Create mount point
mkdir mount_rootfs
# Mount the partition, here 215040 is the start offset, multiplied by the block size (512)
sudo mount -o loop,offset=$((215040*512)) $(pwd)/noble-preinstalled-server-arm64.img $(pwd)/mount_rootfs
# Check the rootfs
ls mount_rootfs
# Go in the Buildroot Linux build directory
cd buildroot/output/build/linux-custom
# Install the drivers (modules) in the RootFS
sudo ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=../../../../mount_rootfs make modules_install
# Remove generated symlink
sudo rm ../../../../mount_rootfs/lib/modules/6.12.0-rc3/build
# Create a real directory
sudo mkdir ../../../../mount_rootfs/lib/modules/6.12.0-rc3/build
# Go back to "work" directory
cd ../../../..
# Copy QEMU ARM64 emulator (static binary) into the rootfs (install if you don't have it)
sudo apt install qemu-user-static
# The binary can later be removed, or simply leave it if you want to chroot from host again later
sudo cp $(which qemu-aarch64-static) mount_rootfs/usr/bin/
# Chroot into the rootfs and run the QEMU emulator to run bash
sudo chroot mount_rootfs qemu-aarch64-static /bin/bash
```

### 6. Configure the RootFS

This is run in the chroot above.

```shell
# Set a hostname for the device
echo "ENVME" > /etc/hostname
echo "127.0.0.1 ENVME" >> /etc/hosts
# Set fstab
echo "/dev/mmcblk1p1	/	ext4	errors=remount-ro	0	1" > /etc/fstab
# Set a new root password
passwd
# Setup networking
systemctl enable systemd-networkd
# Write the networking config below (with vi or other editor)
vi /etc/systemd/network/ethernet.network
# Generate and chose locales for the system (there will be some warnings and takes a while)
# Choose en_US
dpkg-reconfigure locales
# Add a non root user
adduser ubuntu
# Add to sudoers
usermod -aG sudo ubuntu
# Finally exit the chroot env with "ctrl-D"
```

Config for /etc/systemd/network/ethernet.network (of the embedded RootFS, not the host computer !) :

```
[Match]
Name=enP4p65s0

[Network]
DHCP=yes
```

Note this is for the leftmost ethernet port on the T6, you can create a similar config for the other port by using the name `enP2p33s0` (You can create 2 files, one for each interface).

Note that this config can also be written later in the rootfs from outside the `chroot` environment.

### 7. Replace the old RootFS (on the SD) with the new one

From the `work` directory

```shell
# Mount the old RootFS (if not already mounted)
sudo mount /dev/<sdX1> /media/<user>/RootFS/
# Copy boot files locally
sudo cp -rp /media/<user>/RootFS/boot/ .
# Remove all old files
sudo rm -rf /media/<user>/RootFS/*
# Copy all new RootFS files (the 'p' option is to keep permissions)
sudo cp -rp mount_rootfs/* /media/<user>/RootFS/
# Put the boot files back
sudo cp -rp boot /media/<user>/RootFS/

# Before you sync and unmount
# Copy the other files now (see below)

# Sync to make sure all data is written to SD card
sudo sync
# Unmount
sudo umount /dev/<sdX*>
```

### 8. Copy the eNVMe launch script

```shell
sudo cp -p buildroot/board/friendlyelec/cm3588-nas/overlay/root/pci-ep/nvme-epf /media/<user>/RootFS/usr/bin/
```

### (Optional/for T6) Copy other device tree blobs and overlays

This is necessary if you build for the FriendlyElec NanoPC T6 board.

in the `work/buildroot` directory (you must already have built it once and setup everything), for example :

```shell
sudo cp output/build/linux-custom/arch/arm64/boot/dts/rockchip/rk3588-nanopc-t6.dtb /media/<user>/RootFS/boot/
sudo cp output/build/linux-custom/arch/arm64/boot/dts/rockchip/rk3588-nanopc-t6-pcie-ep.dtbo /media/<user>/RootFS/boot/
```

You can edit `/media/<user>/RootFS/boot/extlinux/extlinux.conf`

For example :

```
label FriendlyElec NanoPC T6 endpoint mode
  kernel /boot/Image
  devicetree /boot/rk3588-nanopc-t6.dtb
  devicetree-overlay /boot/rk3588-nanopc-t6-pcie-ep.dtbo
  append root=/dev/mmcblk1p1 rw earlycon rootwait
```

Note this will only apply to Linux, so if you change board completely you will need to generate an adequate uboot for it.
Here both the FriendlyElec NanoPC-T6 and CM3588 rely on the exact same SoC and their schematics are highly similar so it will be possible to boot the same uboot on both boards. This will not apply to other boards.

### (Optional) Copy Linux kernel sources for OoT build on embedded board

In order to build the eNVMe firmware (Linux PCI endpoint function) `pci-epf-nvme.c` on the board directly, out-of-tree (OoT), we will need the kernel sources on the board.

```shell
# From the work directory

# Copy the Linux sources
sudo cp -r linux/. /media/<user>/RootFS/lib/modules/6.12.0-rc3/build/
# Remove the .git directory (Do this otherwise Oot built .ko will take git commit hash in version and will refuse to be inserted)
sudo rm -rf /media/<user>/RootFS/lib/modules/6.12.0-rc3/build/.git
# Copy the .config from Buildroot
sudo cp buildroot/output/build/linux-custom/.config /media/rick/rootfs1/lib/modules/6.12.0-rc3/build/
# Copy Module.symvers
sudo cp buildroot/output/build/linux-custom/Module.symvers /media/rick/rootfs1/lib/modules/6.12.0-rc3/build/

# Copy the firmware (allows to build on embedded board)
cp -r ../firmware /media/<user>/RootFS/home/ubuntu/
```

### 10. Launch and test on board

- Test boot
- Update apt sources (do not upgrade, it might replace our custom kernel by the standard Ubuntu kernel)
- Install whatever is necessary

```shell
sudo apt update
sudo apt install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf llvm build-essential
```

### OoT Build

See [firmware/README.md](../firmware/README.md).

#### OoT build on embedded board

If you followed the instructions to prepare the platform from scratch you will need to first run:

```shell
cd /lib/modules/$(uname -r)/build
sudo make modules_prepare
# If the kernel config asks just accepts default configurations
```

otherwise you may encounter the following message when running `make`.

```
make
make -C /lib/modules/6.12.0-rc3/build M=$PWD
make[1]: Entering directory '/usr/lib/modules/6.12.0-rc3/build'

  ERROR: Kernel configuration is invalid.
         include/generated/autoconf.h or include/config/auto.conf are missing.
         Run 'make oldconfig && make prepare' on kernel src to fix it.

/usr/lib/modules/6.12.0-rc3/build/Makefile:723: include/config/auto.conf: No such file or directory
make[2]: *** [/usr/lib/modules/6.12.0-rc3/build/Makefile:788: include/config/auto.conf] Error 1
make[1]: *** [Makefile:224: __sub-make] Error 2
make[1]: Leaving directory '/usr/lib/modules/6.12.0-rc3/build'
make: *** [Makefile:24: default] Error 2
```

## Launching the eNVMe

```shell
sudo ./nvme-epf --model "Trust me 980 pro" --loop /dev/... start
```

## Recompile and update the kernel

in `work/buildroot` (you must already have built it once and setup everything) :

```shell
make linux-rebuild all
```

The kernel itself will be `output/images/Image`.

Update it on the SD card :

```shell
# Update the kernel
sudo cp output/images/Image /media/<user>/RootFS/boot/Image
# Update kernel modules
# Go in the Buildroot Linux build directory
cd buildroot/output/build/linux-custom
# Install the drivers (modules) in the RootFS
sudo ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=/media/<user>/ make modules_install
```
