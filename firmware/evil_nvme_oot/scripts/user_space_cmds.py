#!/usr/bin/env python3

import struct
import os
import signal
import sys

DEVICE_PATH = "/dev/envme-io-cmd"

# Define struct formats (little-endian)
nvme_rw_command_format = "<BBHIIIQQQQHHIIHH"
nvme_completion_format = "<QHHHH"

NVME_SC_SUCCESS = 0x0000
NVME_SC_INTERNAL = 0x0007

# Globals to track state
last_command_id = None
dev = None  # file handle
auto_complete_all = False

def parse_nvme_rw_command(data):
    fields = struct.unpack(nvme_rw_command_format, data)
    return {
        "opcode": fields[0],
        "flags": fields[1],
        "command_id": fields[2],
        "nsid": fields[3],
        "cdw2": fields[4],
        "cdw3": fields[5],
        "metadata": fields[6],
        "prp1_or_sgl1": fields[7],
        "prp2_or_sgl2": fields[8],
        "slba": fields[9],
        "length": fields[10],
        "control": fields[11],
        "dsmgmt": fields[12],
        "reftag": fields[13],
        "apptag": fields[14],
        "appmask": fields[15],
    }

def build_nvme_completion(command_id, status=NVME_SC_SUCCESS):
    result_u64 = 0
    sq_head = 0
    sq_id = 0
    return struct.pack(
        nvme_completion_format,
        result_u64,
        sq_head,
        sq_id,
        command_id,
        status
    )

def handle_sigint(sig, frame):
    global dev, last_command_id
    print("\nInterrupted!")

    if dev and last_command_id is not None:
        print(f"Sending completion for pending command_id {last_command_id} before exit...")
        try:
            cqe = build_nvme_completion(last_command_id, NVME_SC_SUCCESS)
            dev.write(cqe)
            print("CQE sent")
        except Exception as e:
            print(f"Failed to send CQE: {e}")

    with open("/sys/kernel/config/pci_ep/functions/nvmet_pci_epf/nvmepf.0/nvme/user_path_enable", "w") as f:
        f.write("0")
    print("eNVMe User-path mode disabled")

    if dev:
        dev.close()
    sys.exit(0)

def main():
    global dev, last_command_id, auto_complete_all

    try:
        with open("/sys/kernel/config/pci_ep/functions/nvmet_pci_epf/nvmepf.0/nvme/user_path_enable", "w") as f:
            f.write("1")
        print("eNVMe User-path mode enabled")
    except PermissionError:
        print("Permission denied; run this script as root")
    except FileNotFoundError:
        print("Sysfs path not found. Is the eNVMe module loaded ?")

    signal.signal(signal.SIGINT, handle_sigint)

    if not os.path.exists(DEVICE_PATH):
        print(f"Device {DEVICE_PATH} not found")
        return

    with open(DEVICE_PATH, "r+b", buffering=0) as dev_file:
        dev = dev_file
        print(f"Waiting for commands on {DEVICE_PATH}...")

        while True:
            cmd_data = dev.read(struct.calcsize(nvme_rw_command_format))
            if not cmd_data:
                continue

            cmd = parse_nvme_rw_command(cmd_data)
            last_command_id = cmd["command_id"]

            print("Received NVMe Command:")
            for k, v in cmd.items():
                print(f"  {k}: {hex(v) if isinstance(v, int) else v}")

            # Ask before sending completion
            if not auto_complete_all:
                while True:
                    choice = input(f"Send completion for command_id {cmd['command_id']}? [y/n/a]: ").strip().lower()
                    if choice in ("y", "n", "a"):
                        break
                    print("Please enter 'y' (yes), 'n' (no), or 'a' (always).")
                if choice == "a":
                    auto_complete_all = True

            # Build and send success CQE
            status = NVME_SC_SUCCESS if choice in ("y", "a") else NVME_SC_INTERNAL
            cqe = build_nvme_completion(cmd["command_id"], status)
            dev.write(cqe)
            # Clear last_command_id after completion is sent
            last_command_id = None
            print(f"Sent CQE with status: {status} for command_id {cmd['command_id']}\n")

if __name__ == "__main__":
    main()
