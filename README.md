## build
```
mkdir build && cd build
camke .. && make -j8
```

## config can for linux start

create a file /etc/systemd/network/80-can.network with the following content:

```
[Match]
Name=can*

[CAN]
BitRate=500K
```
Instead of "can*" you can also specify a specific interface (e.g. "can0").

Then (re-)start the systemd-networkd service:
```
sudo systemctl restart systemd-networkd
```
To have it start automatically when booting:
```
sudo systemctl enable systemd-networkd
```
