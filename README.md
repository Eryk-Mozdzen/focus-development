# FOCUS development platform & thrust bench

## Bench

### Build

```bash
cd bench
mkdir build
cd build

cmake ..
make
make flash
```

### PPP session

```bash
sudo pppd /dev/ttyACM0 921600 192.168.7.1:192.168.7.2 local noauth debug nodetach
```
