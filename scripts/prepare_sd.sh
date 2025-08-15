#!/bin/bash

if ! command -v realpath &> /dev/null
then
    realpath() {
        [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
    }
fi

# Get the path of this script
SCRIPTPATH=$(realpath  $(dirname "$0"))
# Go to the script path
cd "${SCRIPTPATH}/../work"

SD_MOUNT="sd_mount"
LINUX_VERSION="6.15.0"
SD_DEV=""

POSITIONAL=()
while [[ $# -gt 0 ]]
do
key="$1"

case $key in
    --sd-dev)
    SD_DEV="$2"
    shift # past argument
    shift # past value
    ;;
    *)    # unknown option
    POSITIONAL+=("$1") # save it in an array for later
    shift # past argument
    ;;
esac
done
set -- "${POSITIONAL[@]}" # restore positional parameters

if [ -z "${SD_DEV}" ]
then
    echo "Specify the SD card device with --sd-dev </dev/...>"
    exit 1
fi

while true; do
    lsblk "${SD_DEV}"
    read -p "Do you want to prepare the SD ${SD_DEV} ? [y/n]" yn
    case $yn in
        y)
        echo "Preparing !";
        break
        ;;
        n)
        echo "exiting...";
        exit
        ;;
        *)
        echo "unexpected input"
        ;;
    esac
done

sudo umount "${SD_DEV}"*
mkdir -p "${SD_MOUNT}"

set -eo pipefail

sudo mount "${SD_DEV}1" $(realpath "${SD_MOUNT}")

# Copy boot files locally
echo "Extracting boot files..."
sudo cp -rp "${SD_MOUNT}"/boot/ .
# Remove all old files (minimal buildroot rootfs)
echo "Removing buildroot minimal RootFS..."
sudo rm -rf "${SD_MOUNT}"/*
# Copy all new RootFS files (the 'p' option is to keep permissions)
echo "Copying ubuntu RootFS (must be mounted)..."
sudo cp -rp mount_rootfs/* "${SD_MOUNT}"/
# Put the boot files back
echo "Copying boot files..."
sudo cp -rp boot "${SD_MOUNT}"/

# Copy the scripts
echo "Copying scripts..."
sudo cp -p buildroot/board/friendlyelec/cm3588/overlay/root/pci-ep/* "${SD_MOUNT}"/usr/bin/

# Installing modules
REAL_MOUNT_PATH=$(realpath "${SD_MOUNT}")
pushd buildroot/output/build/linux-custom
sudo ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH="${REAL_MOUNT_PATH}" make modules_install
popd

# Remove old build directory (for OoT build)
echo "Preparing environment for OoT build on board..."
sudo rm -rf "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build"
sudo mkdir -p "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build"
sudo cp -r linux/. "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build/"
sudo rm -rf "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build/.git"
sudo cp buildroot/output/build/linux-custom/.config "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build/"
sudo cp buildroot/output/build/linux-custom/Module.symvers "${SD_MOUNT}/lib/modules/${LINUX_VERSION}/build/"

echo "Copying firmware..."
cp -r ../firmware/evil_nvme_oot "${SD_MOUNT}/home/ubuntu/"

echo "Copying PCILeech setup script..."
cp ../scripts/setup_pcileech.sh "${SD_MOUNT}/home/ubuntu/"

echo "Unmounting ${SD_DEV}1 and syncing..."
sudo umount "${SD_DEV}1"
sudo sync

echo done !