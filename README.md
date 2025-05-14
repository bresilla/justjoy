
<img align="right" width="26%" src="./misc/logo.png">

warpout
===

simple udev input device forwarder to remote machine


## Building

```
mkdir build
cd build
cmake ..
make
```


## Running

### Server

```bash
warpout server 12398
```

### Client

```bash
warpout client -d /dev/input/event-xbox -a 172.30.0.175 -p 12398
```
