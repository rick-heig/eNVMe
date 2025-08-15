#!/bin/bash

echo Installing dependencies...
sudo apt install -y libusb-1.0-0-dev build-essential pkgconf liblz4-dev libfuse-dev
clear

mkdir PCILeech && cd PCILeech
# Clone Leechcore
git clone https://github.com/ufrisk/LeechCore.git
# Clone Leechcore-plugins
git clone https://github.com/rwk-git/LeechCore-plugins.git --branch generic --single-branch
# Clone MemProcFS
git clone https://github.com/ufrisk/MemProcFS.git
# Clone PCILeech
git clone https://github.com/ufrisk/pcileech.git

# Build the LeechCore library
pushd LeechCore/leechcore
make
popd
# Build the MemProcFS library
pushd MemProcFS/vmm
make
cd ../memprocfs
make
popd
# Build the LeechCore plugin
pushd LeechCore-plugins/leechcore_device_generic
make
popd
# Build PCI Leech
pushd pcileech/pcileech
make
cd ../files
cp ../../LeechCore-plugins/files/leechcore_device_generic.so .
popd
