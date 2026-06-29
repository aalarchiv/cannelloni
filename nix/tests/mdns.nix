{ testers, pkgs }:
# Acceptance test for mDNS/Avahi zeroconf peer discovery (cannelloni-84a.7).
#
# Every node runs cannelloni with --mdns and NO static peer config, plus a local
# avahi-daemon. Each instance advertises "_cannelloni._udp" on its transport port
# (with TXT records: protocol version + CAN-FD capability + interface) and
# browses the LAN for the same service, so peers learn each other before any CAN
# data flows.
#
#   node_a (CAN-FD) <--- mDNS ---> node_b (CAN-FD)      compatible: form a hub
#   node_c (classic CAN)                                incompatible FD cap: skipped
#
# Acceptance:
#   1. node_a and node_b discover each other via mDNS (no peer config) and form a
#      working hub -- fan-out + reflection hold;
#   2. an instance never adds ITSELF as a peer (no self-echo on the source bus);
#   3. the TXT-advertised CAN-FD capability is used to skip the incompatible
#      classic-CAN node_c (and node_c skips the FD nodes), so node_c stays
#      isolated from the FD hub;
#   4. the hub never crashes through any of this.
testers.nixosTest {
  name = "mdns";

  nodes =
    let
      base = mtu: {
        imports = [
          ../module.nix
          ./common.nix
        ];
        networking.firewall.enable = false;
        # avahi-daemon must be running for cannelloni's Avahi client to publish
        # and browse. publish.enable is required or the daemon refuses ALL
        # publishing (disable-publishing=yes), including our client entry group;
        # publish.addresses makes it announce our host address so peers can
        # actually resolve us. ipv4 mDNS is enough for this test.
        services.avahi = {
          enable = true;
          ipv4 = true;
          publish = {
            enable = true;
            addresses = true;
            userServices = true;
          };
        };
        services.setup_can.mtu = mtu;
        services.cannelloni = {
          enable = true;
          transport = "udp";
          ipProtocol = "ipv4";
          localPort = 10000;
          remotePort = 10000;
          canInterface = "vcan0";
          # No static peers: every participant is learnt over mDNS. Liveness is
          # only refreshed when a peer *sends*, and the driver burns a lot of
          # guest time per step, so keep the eviction window well above that gap
          # (mDNS adds peers proactively; we are not exercising eviction here).
          mdns = true;
          peerTimeout = 120;
        };
      };
    in
    {
      node_a = { ... }: base 72; # CANFD_MTU -> CAN-FD negotiated
      node_b = { ... }: base 72; # CANFD_MTU -> CAN-FD negotiated
      node_c = { ... }: base 16; # classic CAN MTU -> CAN-FD capability differs
    };

  testScript = ''
    fd_nodes = {"node_a": node_a, "node_b": node_b}
    nodes = dict(fd_nodes, node_c=node_c)

    def cap_start(node):
        node.execute("pkill -x candump 2>/dev/null; true")
        node.execute("rm -f /tmp/cap.log")
        node.execute("setsid stdbuf -oL candump -L vcan0 > /tmp/cap.log 2>&1 < /dev/null &")
        node.sleep(1)

    def cap_count(node, needle):
        rc, out = node.execute("grep -c '%s' /tmp/cap.log" % needle)
        try:
            return int(out.strip())
        except ValueError:
            return 0

    def log_count(node, needle):
        rc, out = node.execute("journalctl -u cannelloni | grep -c '%s'" % needle)
        try:
            return int(out.strip())
        except ValueError:
            return 0

    # A frame sent from `src` must reach every node in `expect` and appear
    # exactly once on src's own bus (origin-exclusion -- a self-peer bug, i.e. a
    # failed self-filter, would echo it back).
    def fanout(src_name, frame, expect):
        for n in [src_name] + expect:
            cap_start(nodes[n])
        nodes[src_name].succeed("cansend vcan0 %s" % frame)
        nodes[src_name].sleep(3)
        for n in [src_name] + expect:
            nodes[n].execute("pkill -x candump")
        for name in expect:
            c = cap_count(nodes[name], frame)
            assert c >= 1, "%s did not receive %s (count=%d)" % (name, frame, c)
        c = cap_count(nodes[src_name], frame)
        assert c == 1, "%s should show its own frame once, got %d (self-peer echo?)" % (src_name, c)
        print("[fanout] %s -> %s OK" % (src_name, expect))

    start_all()
    for n in nodes.values():
        n.wait_for_unit("avahi-daemon")
        n.wait_for_unit("cannelloni")
    for n in nodes.values():
        n.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
        n.wait_until_succeeds("journalctl | grep 'CANThread up and running'")

    # --- 1. mDNS discovery between the two compatible CAN-FD nodes ---------
    # No peer config: each must resolve and learn the other purely over mDNS.
    for n in fd_nodes.values():
        n.wait_until_succeeds("journalctl -u cannelloni | grep 'mDNS: resolved peer'")
        n.wait_until_succeeds("journalctl -u cannelloni | grep 'Discovered UDP peer'")
    print("[discovery] node_a and node_b resolved each other over mDNS")

    # --- 2 + 3. Working hub + self-filter (no echo) -----------------------
    fanout("node_a", "1A1#AA01", ["node_b"])
    fanout("node_b", "2B2#BB02", ["node_a"])

    # --- 4. Capability pre-filter: the classic node_c is skipped ----------
    # node_a/node_b advertise canfd=1; node_c advertises canfd=0, so each side
    # refuses to peer across the mismatch (epic caveat 4 / acceptance #4).
    node_a.wait_until_succeeds("journalctl -u cannelloni | grep 'mDNS: skipping incompatible'")
    node_c.wait_until_succeeds("journalctl -u cannelloni | grep 'mDNS: skipping incompatible'")
    print("[capability] FD/classic mismatch skipped on both sides")

    # node_c never peers with the FD hub, so a frame on its bus must not reach
    # node_a (they never exchange datagrams).
    cap_start(node_a)
    node_c.succeed("cansend vcan0 3C3#CC03")
    node_c.sleep(3)
    node_a.execute("pkill -x candump")
    assert cap_count(node_a, "3C3#CC03") == 0, "incompatible node_c reached the FD hub"
    print("[isolation] classic node_c stayed isolated from the FD hub")

    # --- 5. The hub survived all of the above -----------------------------
    for n in nodes.values():
        n.succeed("systemctl is-active cannelloni")
    print("[alive] all instances still running")
  '';
}
