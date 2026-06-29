{ testers, pkgs }:
# Phase 3 acceptance test (cannelloni-84a.3): dynamic UDP peer discovery.
#
# The hub starts with NO static peers and --discover, so it learns every peer at
# runtime from valid incoming traffic and evicts a peer that goes silent for
# longer than peerTimeout. Two stock single-peer leaves dial in:
#
#   node_a --\                       (no static peers configured on the hub;
#             +-- UDP --> node_hub    every participant is learnt from its RX)
#   node_b --/
#
# Acceptance:
#   1. an unknown source carrying valid cannellonis traffic is discovered and
#      joins the virtual bus (fan-out + peer<->peer reflection still hold);
#   2. a discovered peer that goes silent past peerTimeout is evicted;
#   3. a peer that dials back in after eviction is re-learnt as a fresh
#      participant; the hub never crashes through any of this.
testers.nixosTest {
  name = "discovery";

  nodes =
    let
      leaf =
        { ... }:
        {
          imports = [
            ../module.nix
            ./common.nix
          ];
          networking.firewall.enable = false;
          services.setup_can.mtu = 72; # CANFD_MTU -> CAN-FD negotiated
          services.cannellonis = {
            enable = true;
            transport = "udp";
            ipProtocol = "ipv4";
            localPort = 10000;
            remotePort = 10000;
            canInterface = "vcan0";
            remoteAddress = "node_hub";
          };
        };
    in
    {
      node_hub =
        { ... }:
        {
          imports = [
            ../module.nix
            ./common.nix
          ];
          networking.firewall.enable = false;
          services.setup_can.mtu = 72; # CANFD_MTU -> CAN-FD negotiated
          services.cannellonis = {
            enable = true;
            transport = "udp";
            ipProtocol = "ipv4";
            localPort = 10000;
            remotePort = 10000;
            canInterface = "vcan0";
            # No static peers: the hub learns them from valid RX and evicts a
            # peer after 30 s of silence. A peer's liveness is only refreshed
            # when it *sends* (a receiver does not), and the test driver burns a
            # lot of guest time per step, so every correctness check below
            # refreshes the peers it depends on immediately beforehand and the
            # timeout is kept well above that gap. Eviction is exercised
            # explicitly at the end with a dedicated silence window.
            discover = true;
            peerTimeout = 30;
          };
        };
      node_a = leaf;
      node_b = leaf;
    };

  testScript = ''
    leaves = {"node_a": node_a, "node_b": node_b}
    nodes = dict(leaves, node_hub=node_hub)

    def cap_start(node):
        node.execute("pkill -x candump 2>/dev/null; true")
        node.execute("rm -f /tmp/cap.log")
        node.execute("setsid stdbuf -oL candump -L vcan0 > /tmp/cap.log 2>&1 < /dev/null &")
        node.sleep(1)

    def cap_all():
        for n in nodes.values():
            cap_start(n)

    def cap_count(node, needle):
        rc, out = node.execute("grep -c '%s' /tmp/cap.log" % needle)
        try:
            return int(out.strip())
        except ValueError:
            return 0

    # A frame sent from `src` both keeps `src` alive (the hub refreshes its
    # lastSeen) and must reach every node in `expect`, and appear exactly once
    # on src's own bus (origin-exclusion -- never echoed back).
    def fanout(src_name, frame, expect, needle=None):
        needle = needle or frame
        cap_all()
        nodes[src_name].succeed("cansend vcan0 %s" % frame)
        nodes[src_name].sleep(3)
        for n in nodes.values():
            n.execute("pkill -x candump")
        for name in expect:
            c = cap_count(nodes[name], needle)
            assert c >= 1, "%s did not receive %s (count=%d)" % (name, frame, c)
        c = cap_count(nodes[src_name], needle)
        assert c == 1, "%s should show its own frame once, got %d (echo?)" % (src_name, c)
        print("[fanout] %s -> %s OK" % (src_name, expect))

    def hub_log_count(needle):
        rc, out = node_hub.execute("journalctl -u cannellonis | grep -c '%s'" % needle)
        try:
            return int(out.strip())
        except ValueError:
            return 0

    start_all()
    for n in nodes.values():
        n.wait_for_unit("cannellonis")
    for n in nodes.values():
        n.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
        n.wait_until_succeeds("journalctl | grep 'CANThread up and running'")

    # --- 1. Discovery: an unknown source is learnt from valid RX ----------
    # node_a dials in; the hub learns it and its frame reaches the hub's bus.
    fanout("node_a", "1A1#AA01", ["node_hub"])
    node_hub.wait_until_succeeds("journalctl -u cannellonis | grep 'Discovered UDP peer'")

    # --- 2. The hub routes its own CAN frames to a learnt peer ------------
    # Refresh node_a right before (liveness is only bumped when a peer sends),
    # then transmit from the hub: the frame must reach the dynamically-learnt
    # node_a -- i.e. a discovered peer is a full participant, not RX-only.
    node_a.succeed("cansend vcan0 0A0#0A")
    cap_start(node_a)
    node_hub.succeed("cansend vcan0 0FF#FEEDFACE")
    node_hub.sleep(3)
    node_a.execute("pkill -x candump")
    assert cap_count(node_a, "0FF#FEEDFACE") >= 1, "hub frame did not reach the learnt peer"
    print("[hub->peer] OK")

    # --- 3. A second peer is learnt; the two reflect through the hub ------
    disc_before = hub_log_count("Discovered UDP peer")
    node_b.succeed("cansend vcan0 2B2#BB02")
    node_hub.wait_until_succeeds(
        "test $(journalctl -u cannellonis | grep -c 'Discovered UDP peer') -gt %d" % disc_before)
    # Both peers send in a tight window, so each is fresh when the other's frame
    # is reflected back through the hub (node_a's send also re-refreshes it).
    cap_start(node_a)
    cap_start(node_b)
    node_a.succeed("cansend vcan0 5A5#A5")   # node_a -> hub -> node_b
    node_b.succeed("cansend vcan0 5B5#B5")   # node_b -> hub -> node_a
    node_hub.sleep(3)
    node_a.execute("pkill -x candump")
    node_b.execute("pkill -x candump")
    assert cap_count(node_b, "5A5#A5") >= 1, "peer->peer reflection node_a->node_b failed"
    assert cap_count(node_a, "5B5#B5") >= 1, "peer->peer reflection node_b->node_a failed"
    print("[reflection] OK")

    # --- 4. Eviction: go silent past peerTimeout (30 s) -------------------
    # Neither leaf sends during this window, so both fall silent and age out.
    evicted_before = hub_log_count("Evicting silent UDP peer")
    node_hub.sleep(36)
    assert hub_log_count("Evicting silent UDP peer") > evicted_before, \
        "hub did not evict the silent discovered peers"
    node_hub.succeed("systemctl is-active cannellonis")
    print("[eviction] OK")

    # --- 5. Re-learn after eviction ---------------------------------------
    discovered_before = hub_log_count("Discovered UDP peer")
    fanout("node_a", "3C3#CC03", ["node_hub"])
    assert hub_log_count("Discovered UDP peer") > discovered_before, \
        "hub did not re-learn node_a after eviction"
    node_hub.succeed("systemctl is-active cannellonis")
    print("[re-learn] OK")
  '';
}
