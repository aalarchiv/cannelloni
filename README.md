# cannellonis
*a SocketCAN over Ethernet tunnel*

> **cannellonis** is a multi-peer fork of [cannelloni](https://github.com/mguentner/cannelloni):
> a single instance can bridge a CAN bus to **many** peers at once, forming a
> virtual shared bus. The binary was renamed `cannelloni` → `cannellonis` (plural)
> to denote this hub capability. The on-wire protocol and the `cannelloni-common`
> library are unchanged and remain interoperable with upstream cannelloni.

[![Chat on Matrix](https://matrix.to/img/matrix-badge.svg)](https://matrix.to/#/#cannelloni:matrix.org)

cannellonis is written in C++11 and uses UDP, TCP or SCTP to transfer CAN frames
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
cannellonis is **not suited** for production deployments. Use it only in environments where packet loss is tolerable.
There is **no guarantee** that CAN frames will reach their destination at all **and/or** in the right order.

# Trust model

cannellonis is designed for **internal, trusted networks**. Its threat model
deliberately **excludes external attackers**: there is **no authentication and
no encryption** between peers, consistent with upstream cannelloni. Any host
that can reach the transport port can both read every CAN frame on the bus and
inject frames of its own.

This is a deliberate scoping decision, not an oversight, so it is the
operator's responsibility to keep the transport secure:

- Bind cannellonis to a trusted interface and keep the transport port off any
  untrusted network — firewall it, put it on a VPN/overlay, or use a physically
  isolated segment.
- Treat the multi-peer hub, dynamic discovery (`--discover`) and mDNS discovery
  (`--mdns`) as trusted-segment features. They learn peers from the network
  *without authenticating them*. Where a peer allowlist is offered it is
  **misconfiguration hygiene** (catching the wrong host dialing in), **not** a
  defence against a hostile one.

If you must cross an untrusted network, run cannellonis **inside** a transport
that does provide authentication and confidentiality (e.g. WireGuard, IPsec or
an SSH tunnel).

# Ecosystem

- https://github.com/PhilippFux/ESP32_CAN_Interface
- https://github.com/tuvok/qtCannelloniCanBus
- https://github.com/mguentner/cannelloni_ports (currently only a lwIP implementation)
- https://github.com/epozzobon/lasagne (another esp32)
- https://github.com/GENIVI/CANdevStudio

## Compilation

cannellonis uses cmake to generate a Makefile.
You can build cannellonis using the following command.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If you do not want or need SCTP support, you can disable it
by setting `-DSCTP_SUPPORT=OFF`.
SCTP support is also disabled if you don't have `lksctp-tools`
installed.

Optional mDNS/zeroconf peer discovery (see [mDNS / zeroconf
discovery](#mdns--zeroconf-discovery)) is built when `libavahi-client` is
present and can be turned off with `-DAVAHI_SUPPORT=OFF`. It is also disabled
automatically if Avahi is not installed.

## Installation

Just install it using

```
  cmake --install build
```

### Debian package

To build a `.deb`, run CPack against a configured build tree:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cpack -G DEB --config build/CPackConfig.cmake
```

This produces `build/cannellonis_<version>_<arch>.deb` containing just the
`cannellonis` binary and docs (the `cannelloni-common` shared library is
intentionally left out — the binary static-links it). Runtime dependencies
(`libsctp1`, `libavahi-client3`, …) are detected automatically, so build on a
host that has those optional libraries installed if you want SCTP and mDNS
support in the package.

The package declares `Breaks`/`Replaces: cannelloni`, so installing it cleanly
supersedes an upstream `cannelloni` package rather than running two CAN tunnels
side by side.

## Usage

### Example

Two machines 1 and 2 need to be connected:

![](doc/firstexp.png)

Machine 2 needs to be connected to the physical CAN Bus that is attached
to Machine 1.

Start cannellonis on Machine 1:

```
cannellonis -I slcan0 -R 192.168.0.3 -r 20000 -l 20000
```
cannellonis will now listen on port 20000 and has Machine 2 configured as
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

Now start cannellonis on Machine 2:
```
cannellonis -I vcan0 -R 192.168.0.2 -r 20000 -l 20000
```

The tunnel is now complete and can be used on both machines.
Simply try it by using `candump` and/or `cangen`.

If something does not work, try the debug switch `-d cut` to find out
what is wrong.

### Timeouts

*UDP + SCTP only!*


cannellonis either sends a full UDP frame or all CAN frames that
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

You can load this file into each cannellonis instance with the `-T
file.csv` option.
Please note that the whole buffer will be flushed and not only the two
frames.

If you enable timer debugging using `-d t` you should see that the table
has been loaded successfully into cannellonis:

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

cannellonis supports UDP for stable connections where no packet loss
is expected. Here two instances communicate using defined ports.

Usage example

IP: 192.168.0.2
```
cannellonis -I vcan0 -R 192.168.0.3 -r 12000 -l 13000
```

IP: 192.168.0.3
```
cannellonis -I vcan0 -R 192.168.0.2 -r 13000 -l 12000
```

Set the *MTU* using `-m` depending on your connection. Default is
1500 bytes.

### UDP multi-peer hub

A single UDP instance can bridge a CAN bus to **several** UDP peers at once,
turning cannellonis into a hub: the local CAN bus and every peer form one
virtual shared CAN bus where each participant sees every other participant's
frames — like real devices on the same wire. A frame ingested from one
participant is delivered to every participant *except* its origin
(origin-exclusion), which gives bus behaviour plus one-hop loop avoidance.

List the peers with the repeatable `--peer HOST[:PORT]` option (the port
defaults to `-r`), or put one `HOST[:PORT]` per line in a file passed to
`--peers-file` (blank lines and `#` comments are ignored):

```
# hub: bridge vcan0 to three peers
cannellonis -I vcan0 -l 20000 --peer 192.168.0.3 --peer 192.168.0.4 --peer 192.168.0.5
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
cannellonis -I vcan0 -l 20000 --discover
```

With `--discover` a datagram from an unknown source that is a valid cannellonis
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
must be confined to a trusted network (see the [Trust model](#trust-model)); the
cap is misconfiguration hygiene, not a security control.

#### mDNS / zeroconf discovery

If cannellonis was built with Avahi support (`AVAHI_SUPPORT`, on by default when
`libavahi-client` is present), the hub can discover peers over **mDNS/zeroconf**
rather than being told about them. Enable it with `--mdns`:

```
# mDNS hub: advertise + browse the LAN, no peer config at all
cannellonis -I vcan0 -l 20000 --mdns
```

Each instance advertises a `_cannelloni._udp` service on its transport port and
browses the LAN for the same service, so two or more instances on a segment find
each other and form a hub with **no static configuration** — each learns the
others' address and port *before* any CAN data flows. A running `avahi-daemon`
is required.

mDNS is a sibling of (not a replacement for) `--discover`: a resolved peer joins
the same dynamic-peer set, so `--max-peers` and `--peer-timeout` apply
identically (a peer that later falls silent ages out and is re-learnt from its
own traffic). An instance **never peers with itself** — it skips its own
advertisement.

The advertised TXT records carry the protocol version, the CAN interface name
and the **CAN-FD capability**, and a peer whose protocol version or CAN-FD
capability does not match ours is **skipped**. So a classic-CAN node and a
CAN-FD node on the same LAN will not auto-peer into the undefined
mixed-FD/classic case (caveat below); use an explicit `--peer` if you really
intend to bridge them. Like all discovery here, mDNS is **unauthenticated** and
link-local — keep it on a trusted segment (see the [Trust
model](#trust-model)). Build it out entirely with `-DAVAHI_SUPPORT=OFF`.

Notes and limits:

- A single `-R`/`-r` invocation is unchanged and byte-for-byte identical to
  before — `--peer` is purely additive.
- The hub preserves **per-link** order, not a single global bus order; use `-s`
  to sort each flushed batch by CAN id. Buffering adds latency and jitter versus
  a real wire — see [Tuning](#tuning) for a low-latency configuration.
- The aggregate CAN bitrate is the hard ceiling for peers→CAN traffic; under
  sustained overload frames are dropped oldest-first at egress (the path
  recovers once the backlog drains) and no peer can stall the others.
- Each peer owns an egress pool, so a many-peer hub's RAM scales with the peer
  count; size it with `--buffer-frames`/`--buffer-max` (see
  [Buffer memory](#buffer-memory)).
- Hub-to-hub topologies (a peer that is itself a hub) can form multi-hop loops
  and are out of scope; keep the topology a star of single-peer leaves around
  one hub.
- The port must be kept on a trusted network: there is no peer authentication
  (see the [Trust model](#trust-model)), so any reachable host can inject CAN
  frames.

## SCTP

With SCTP it is possible to use cannellonis over lossy connections
where packet loss and re-ordering is very likely.
The SCTP implementation uses the server-client model (for now).
One instance binds on a fixed port and the other instance (client)
connects to it.

Usage example:

IP: 192.168.0.2 (Server)
```
cannellonis -I vcan0 -S s
```

IP: 192.168.0.3 (Client)
```
cannellonis -I vcan0 -S c -R 192.168.0.2
```

If there is no remote IP supplied to the server instance, every client
(any IP) will be accepted. Only one client can be connected at a time.
After the client disconnects, the server waits for a new client.

## TCP

Usage example:

IP: 192.168.0.2 (Server)
```
cannellonis -I vcan0 -C s
```

IP: 192.168.0.3 (Client)
```
cannellonis -I vcan0 -C c -R 192.168.0.2
```

With TCP, no frame buffer is used an frames are immediately transmitted,
frame sorting and timeouts do not apply here.

### TCP multi-peer hub

A TCP **server** is a hub: it accepts and tracks **many** clients at once, so
the server's CAN bus and every connected client's CAN bus form one virtual
shared bus. A frame ingested from any participant is delivered to every other
participant (origin-exclusion), exactly like the UDP hub above. A single
client behaves identically to the previous one-to-one server, so existing
setups are unchanged.

```
# hub: one server, any number of TCP clients dial in
cannellonis -I vcan0 -C s -l 20000 -p

# each leaf is an ordinary TCP client pointing at the hub
cannellonis -I vcan0 -C c -R <hub> -r 20000
```

The clients are learnt on connect and dropped on disconnect; a new client can
connect or an existing one can drop without disrupting the others. Use `-p`
(no peer check) so the server accepts every client rather than a single
configured `-R` remote, and `--max-peers N` (default 16) to cap the number of
simultaneous clients.

Each client gets its own per-peer egress with **non-blocking** writes, so a
slow or stalled client can never stall the hub or the other clients: its
backlog is bounded and the oldest frames are dropped under overload (a
hopelessly stuck client is disconnected and may reconnect). Like the UDP hub,
ordering is preserved **per-link**, not globally; the CAN bitrate is the hard
ceiling for hub→CAN traffic; and the port must be kept on a trusted network
(there is no peer authentication — see the [Trust model](#trust-model)).
Hub-to-hub topologies are out of scope — keep a star of single-peer leaves
around one hub.

Each accepted client owns an egress pool sized by `--buffer-frames`/
`--buffer-max`, so the hub's RAM scales with the number of connected clients
(see [Buffer memory](#buffer-memory)). TCP writes every frame immediately, so
the buffer timeout and frame sorting do not apply.

# Tuning

## Latency and jitter

A cannellonis link is **not** a CAN wire and does not behave like one with
respect to timing. On UDP and SCTP frames are buffered and flushed either when
the buffer timeout (`-t`, default 100000 us = 100 ms) is reached or when a
datagram fills, so a frame can wait up to the timeout before it leaves. The
worst-case one-way latency is roughly

```
Lw ~= buffer timeout (-t) + network latency + delay on the receiver
```

Because each peer is served by its own timer and buffer, a hub preserves
**per-link** order, not a single global bus order, and different peers can
observe the same frame with different delays — i.e. there is jitter that a real
shared bus does not have. TCP is different: it has no buffer timeout and writes
every frame immediately (frame sorting and `-t` do not apply), trading datagram
aggregation for lower per-frame latency.

For a **low-latency configuration**:

- Lower the buffer timeout, e.g. `-t 1000` (1 ms), so frames flush almost
  immediately. This raises the packet rate and CPU cost and reduces
  aggregation, so balance it against your bandwidth.
- Or give only the latency-sensitive CAN ids a short timeout with a per-id
  timeout table (`-T table.csv`, see [Timeouts](#timeouts)) and leave the rest
  batched.
- Or use TCP, which sends each frame as soon as it is read (at the cost of one
  TCP segment per frame under load).

These are *latency* tunables; they do not change the fact that delivery is
best-effort (see the usage notice) and that the aggregate CAN bitrate is the
hard ceiling for traffic written to a CAN bus.

## Buffer memory

Every participant on the virtual bus — the local CAN bus and **each** network
peer — owns its own egress frame pool, so a hub's worst-case frame memory grows
with the peer count:

```
worst-case frame memory ~= (peers + 1) x (--buffer-max) x sizeof(canfd_frame)
```

Two options size every pool (CAN, static peers, discovered UDP peers and
accepted TCP clients alike):

- `--buffer-frames N` (default 1000): frames preallocated up front per pool.
  This is the baseline committed memory; lower it on a many-peer hub.
- `--buffer-max N` (default 16000, `0` = unlimited): the hard cap a pool may
  grow to. This is also the depth of the drop-oldest ring under overload — a
  smaller cap drops frames sooner but bounds RAM; a larger cap absorbs longer
  bursts at higher peak RAM. `--buffer-frames` must not exceed `--buffer-max`.

For example, a 16-peer hub that only needs to absorb short bursts might run
`--buffer-frames 100 --buffer-max 2000` to keep both baseline and worst-case
memory an order of magnitude below the defaults.

## Ingress overrun

The per-pool drop-oldest above bounds loss at *egress* — frames waiting to be
written to a slow participant. A hub has a second, distinct loss point at
*ingress*: each CAN frame read from the local bus is fanned out to every peer
(allocate + copy + enqueue) inline before the next `recv()`, so at high bus
load times many peers the kernel's CAN socket receive buffer can fill and the
kernel drops frames *before* cannellonis ever sees them. This is invisible to
the egress drop-oldest and to the receivers.

cannellonis mitigates this by requesting a large CAN socket receive buffer
(clamped by `net.core.rmem_max` — raise that sysctl if you run a busy
many-peer hub) and reports any overrun rather than dropping silently: each
event is logged as a `CAN ingress overrun` warning and the running total
appears as `ingress drops` in the CAN shutdown summary. If you see those,
reduce the bus load, lower the peer count, or raise `net.core.rmem_max`.

# Frame sorting

CAN frames can be sorted by their ID in each ethernet frame to write
high priority frames first on the receiving CAN bus.

This can be achieved by supplying the `-s` option.

# Filtering

cannellonis does not support filtering, if however you want to only bridge a
certain set of CAN IDs, you can first forward the frames of interest to virtual CAN
interface. From there you will send using cannellonis.

This can be achieved with `cangw` which is part of [can-utils](https://github.com/linux-can/can-utils/) and its respective
kernel module is also present in upstream Linux.

Let's look at an example where currently `can0` is sent to a remote machine:

```
cannellonis -I can0 -R 192.168.0.3 -r 12000
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
Now change the cannellonis command to

```
cannellonis -I vcan0 -R 192.168.0.3 -r 12000
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

cannellonis is licensed under the GPL, version 2. See gpl-2.0.txt for
more information.
