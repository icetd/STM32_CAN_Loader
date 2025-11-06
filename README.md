BootLoader Uploader
C++ based CAN bus firmware upload tool for embedded devices.

## Quick Start
```bash

Setup CAN interface
sudo ip link set can0 type can bitrate 500000
sudo ip link set up can0

Run uploader
sudo ./bootloader_uploader
```

## Basic Commands
`setid`	Set CAN node ID (0x00-0x1F) <br>
`erase`	Erase application flash <br>
`write`	Upload firmware file <br>
`crc`	Check application CRC <br>
`info`	Show device information <br>
`exit`	Quit application <br>

## Usage Example
```bash
bootloader> setid
Enter node ID: 0x01

bootloader> write
Enter firmware file: firmware.bin
Proceed? (y/n): y

[PROGRESS] 1024/2048 bytes (50%)
[NOTICE] Firmware upload completed!
```

## Protocol
CAN 2.0B standard frame
```
Node ID: Bits 10-5
Command: Bits 4-0
```

## Commands
`0x01`: Erase flash <br>
`0x02`: Start write <br>
`0x03`: Write data <br>
`0x04`: End write <br>
`0x05`: Request CRC <br>

## Requirements
Linux with SocketCAN
CAN interface (can0)
libreadline-dev

```bash
sudo apt-get install libreadline-dev can-utils
```
