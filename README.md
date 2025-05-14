
<img align="right" width="26%" src="./misc/logo.png">

warpout
===

simple udev input device forwarder to remote machine



this is a simple UDP-based input device forwarder that can be used to
forward input events from one machine to another.  It is heavily basd on
[netstick](https://github.com/funkenstein/netstick), but uses morern C++
and used CLI11 for argument parsing.


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
