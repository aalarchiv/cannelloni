# cannelloni
*a SocketCAN over Ethernet tunnel*

[![Chat on Matrix](https://matrix.to/img/matrix-badge.svg)](https://matrix.to/#/#cannelloni:matrix.org)

cannelloni is written in C++11 and uses UDP, TCP or SCTP to transfer CAN frames
between two machines.

Features:

- frame aggregation in Ethernet frames (multiple CAN frames in one
  Ethernet frame)
- IPv4/IPv6
- efficient protocol
- very high data rates possible (10 Mbit/s +)
- custom timeouts for certain IDs (see below)
- easy debugging
- CAN FD support on interfaces that support it
- UDP support (fast, unreliable transport)
- TCP support (reliable transport)
- SCTP support (optional, reliable transport)

# Important Usage Notice
cannelloni is **not suited** for production deployments. Use it only in environments where packet loss is tolerable.
There is **no guarantee** that CAN frames will reach their destination at all **and/or** in the right order.

# Ecosystem

- https://github.com/PhilippFux/ESP32_CAN_Interface
- https://github.com/tuvok/qtCannelloniCanBus
- https://github.com/mguentner/cannelloni_ports (currently only a lwIP implementation)
- https://github.com/epozzobon/lasagne (another esp32)
- https://github.com/GENIVI/CANdevStudio

## Compilation

cannelloni uses cmake to generate a Makefile.
You can build cannelloni using the following command.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If you do not want or need SCTP support, you can disable it
by setting `-DSCTP_SUPPORT=OFF`.
SCTP support is also disabled if you don't have `lksctp-tools`
installed.

## Installation

Just install it using

```
  cmake --install build
```

## Usage

### Example

Two machines 1 and 2 need to be connected:

![](doc/firstexp.png)

Machine 2 needs to be connected to the physical CAN Bus that is attached
to Machine 1.

Start cannelloni on Machine 1:

```
cannelloni -I slcan0 -R 192.168.0.3 -r 20000 -l 20000
```
cannelloni will now listen on port 20000 and has Machine 2 configured as
its remote.

Prepare vcan on Machine 2:

```
sudo modprobe vcan
sudo ip link add name vcan0 type vcan
sudo ip link set dev vcan0 up
```

When operating with `vcan` interfaces always keep in mind that they
easily surpass the possible data rate of any physical CAN interface.
An application that just sends whenever the bus is ready would simply
send with many Mbit/s.
The receiving end, a physical CAN interface with a net. data rate of
<= 1 Mbit/s would not be able to keep up.
It is therefore a good idea to rate limit a `vcan` interface to
prevent packet loss.

```
sudo tc qdisc add dev vcan0 root tbf rate 300kbit latency 100ms burst 1000
```
This command will rate limit `vcan0` to 300 kbit/s.
Try to match the rate limit with your physical interface on the remote.
Keep also in mind that this also increases the overall latency!

Now start cannelloni on Machine 2:
```
cannelloni -I vcan0 -R 192.168.0.2 -r 20000 -l 20000
```

The tunnel is now complete and can be used on both machines.
Simply try it by using `candump` and/or `cangen`.

If something does not work, try the debug switch `-d cut` to find out
what is wrong.

### Timeouts

*UDP + SCTP only!*


cannelloni either sends a full UDP frame or all CAN frames that
are queued when the timeout that has been specified by the `-t` option
has been reached.
The default value is 100000 us, so the worst case latency for any can
frame is

```
Lw ~= 100ms + Ethernet latency + Delay on Receiver
```

If you have high priority frames but you also want a small ethernet
overhead, you can create a csv in the format
```
CAN_ID,Timeout in us
```
to specify these frames. You can use the `#` character to comment your
frames.

For example, if the frames with the IDs 5 and 15 should be send after
a shorter timeout you can create a file with the following content.

```
# 15ms
5,15000
# 50ms
15,50000
```

You can load this file into each cannelloni instance with the `-T
file.csv` option.
Please note that the whole buffer will be flushed and not only the two
frames.

If you enable timer debugging using `-d t` you should see that the table
has been loaded successfully into cannelloni:

```
[...]
INFO:cannelloni.cpp[148]:main:Custom timeout table loaded:
INFO:cannelloni.cpp[149]:main:*---------------------*
INFO:cannelloni.cpp[150]:main:|  ID  | Timeout (us) |
INFO:cannelloni.cpp[153]:main:|     5|         15000|
INFO:cannelloni.cpp[153]:main:|    15|         50000|
INFO:cannelloni.cpp[154]:main:*---------------------*
INFO:cannelloni.cpp[155]:main:Other Frames:100000 us.
[...]
```

# Transports

## UDP

cannelloni supports UDP for stable connections where no packet loss
is expected. Here two instances communicate using defined ports.

Usage example

IP: 192.168.0.2
```
cannelloni -I vcan0 -R 192.168.0.3 -r 12000 -l 13000
```

IP: 192.168.0.3
```
cannelloni -I vcan0 -R 192.168.0.2 -r 13000 -l 12000
```

Set the *MTU* using `-m` depending on your connection. Default is
1500 bytes.

### UDP multi-peer hub

A single UDP instance can bridge a CAN bus to **several** UDP peers at once,
turning cannelloni into a hub: the local CAN bus and every peer form one
virtual shared CAN bus where each participant sees every other participant's
frames — like real devices on the same wire. A frame ingested from one
participant is delivered to every participant *except* its origin
(origin-exclusion), which gives bus behaviour plus one-hop loop avoidance.

List the peers with the repeatable `--peer HOST[:PORT]` option (the port
defaults to `-r`), or put one `HOST[:PORT]` per line in a file passed to
`--peers-file` (blank lines and `#` comments are ignored):

```
# hub: bridge vcan0 to three peers
cannelloni -I vcan0 -l 20000 --peer 192.168.0.3 --peer 192.168.0.4 --peer 192.168.0.5
```

Each peer points back at the hub as an ordinary single-peer instance
(`-R <hub> -r 20000`). Incoming datagrams are matched to a peer by source
address and port; datagrams from an unknown source are dropped (a single-peer
instance started with `-p` still accepts any source, as before).

#### Dynamic peer discovery

Instead of (or in addition to) a static `--peer` list, the hub can **learn**
peers at runtime so devices dial in without being configured ahead of time.
Enable it with `--discover`:

```
# discovery hub: no static peers, learn everyone who shows up
cannelloni -I vcan0 -l 20000 --discover
```

With `--discover` a datagram from an unknown source that is a valid cannelloni
packet adds that source as a new peer; from then on it is a full participant on
the virtual bus. Two knobs bound it:

- `--max-peers N` (default 16): the most peers that may be discovered. Once the
  cap is reached, datagrams from further new sources are dropped.
- `--peer-timeout SEC` (default 30, `0` disables): a *discovered* peer that
  sends nothing for this long is evicted and its buffer freed. Liveness is
  refreshed by any valid datagram from the peer.

Only discovered peers are evicted — **statically configured `--peer`/`-R` peers
are never aged out**, so a static peer that is briefly down resumes
automatically when it returns (its frames meanwhile hit the drop-oldest egress
ring rather than growing without bound). Discovery is **unauthenticated** and
must be confined to a trusted network (see the trust model); the cap is
misconfiguration hygiene, not a security control.

Notes and limits:

- A single `-R`/`-r` invocation is unchanged and byte-for-byte identical to
  before — `--peer` is purely additive.
- The hub preserves **per-link** order, not a single global bus order; use `-s`
  to sort each flushed batch by CAN id.
- The aggregate CAN bitrate is the hard ceiling for peers→CAN traffic; under
  sustained overload frames are dropped oldest-first at egress (the path
  recovers once the backlog drains) and no peer can stall the others.
- Hub-to-hub topologies (a peer that is itself a hub) can form multi-hop loops
  and are out of scope; keep the topology a star of single-peer leaves around
  one hub.
- The port must be kept on a trusted network: there is no peer authentication
  (see the trust model), so any reachable host can inject CAN frames.

## SCTP

With SCTP it is possible to use cannelloni over lossy connections
where packet loss and re-ordering is very likely.
The SCTP implementation uses the server-client model (for now).
One instance binds on a fixed port and the other instance (client)
connects to it.

Usage example:

IP: 192.168.0.2 (Server)
```
cannelloni -I vcan0 -S s
```

IP: 192.168.0.3 (Client)
```
cannelloni -I vcan0 -S c -R 192.168.0.2
```

If there is no remote IP supplied to the server instance, every client
(any IP) will be accepted. Only one client can be connected at a time.
After the client disconnects, the server waits for a new client.

## TCP

Usage example:

IP: 192.168.0.2 (Server)
```
cannelloni -I vcan0 -C s
```

IP: 192.168.0.3 (Client)
```
cannelloni -I vcan0 -C c -R 192.168.0.2
```

With TCP, no frame buffer is used an frames are immediately transmitted,
frame sorting and timeouts do not apply here.

# Frame sorting

CAN frames can be sorted by their ID in each ethernet frame to write
high priority frames first on the receiving CAN bus.

This can be achieved by supplying the `-s` option.

# Filtering

cannelloni does not support filtering, if however you want to only bridge a
certain set of CAN IDs, you can first forward the frames of interest to virtual CAN
interface. From there you will send using cannelloni.

This can be achieved with `cangw` which is part of [can-utils](https://github.com/linux-can/can-utils/) and its respective
kernel module is also present in upstream Linux.

Let's look at an example where currently `can0` is sent to a remote machine:

```
cannelloni -I can0 -R 192.168.0.3 -r 12000
```

Let's say you want to only receive CAN frames with the ID `0x042` at the remote machine.
First, load two kernel modules, `can_gw` and `vcan`:

```
modprobe vcan
modprobe can_gw
```

Now, start a virtual CAN bus:

```
ip link add name vcan0 type vcan
ip link set dev vcan0 up mtu 16
```

Add a rule to the gateway that will forward frames with IDs that match `0x042 & 0xC00007F0`. Note that
the bitmask accounts for the type `can_id` which is 4 bytes long.

```
cangw -A -s can0 -d vcan0 -f 042:C00007FF -e
```

If you start candump now on `vcan0`, you should only see frames with ID `0x042`.
Now change the cannelloni command to

```
cannelloni -I vcan0 -R 192.168.0.3 -r 12000
```

Now you should see those `0x042` frames also at the remote machine.

If you want to also send frames from the remote to your original `can0`, you need
to add a rule for that as well!

# Paper

cannelloni was discussed in the paper *Mapping CAN-to-Ethernet communication channels within virtualized embedded environments* on
the Conference *Industrial Embedded Systems (SIES), 2015 10th IEEE International Symposium*.

DOI: [10.1109/SIES.2015.7185064](http://dx.doi.org/10.1109/SIES.2015.7185064)

The papers documentes a PoC how to virtualize CAN controllers similiar to the approach
Xen uses (netback/-front).

# Contributing

Please fork the repository, create a *separate* branch and create a PR
for your work.

# License

Copyright 2014-2023 Maximilian Güntner <code@mguentner.de>

cannelloni is licensed under the GPL, version 2. See gpl-2.0.txt for
more information.
