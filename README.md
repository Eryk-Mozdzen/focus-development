# FOCUS development platform & thrust bench

## Build

```bash
cd firmware
mkdir build
cd build

cmake ..
make

../flash.sh
```

## PPP session

```bash
sudo pppd /dev/ttyACM0 921600 192.168.7.1:192.168.7.2 local noauth debug nodetach
```

## iPerf

```bash
iperf -c 192.168.7.2 -e -i 1 -M 5000 -l 8192
```
