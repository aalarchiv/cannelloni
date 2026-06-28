{ testers, pkgs }:
# Phase 2 acceptance test (cannelloni-84a.2): a UDP multi-peer hub.
#
# Topology is a star (hub-to-hub multi-hop loops are out of v1 scope): one hub
# node with three UDP peer leaves. The leaves are stock single-peer cannelloni
# instances pointing back at the hub, so the virtual shared bus has four
# participants: the hub's CAN bus and each leaf's CAN bus.
#
#   node_a --\
#   node_b ---+-- UDP --> node_hub -- UDP --> the other leaves + its own CAN
#   node_c --/
#
# Acceptance: every node receives every OTHER node's frames and NEVER its own;
# the hub survives an overload from one peer and keeps serving the others.
testers.nixosTest {
  name = "multipeer";

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
          services.cannelloni = {
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
          services.cannelloni = {
            enable = true;
            transport = "udp";
            ipProtocol = "ipv4";
            localPort = 10000;
            remotePort = 10000;
            canInterface = "vcan0";
            peers = [ "node_a" "node_b" "node_c" ];
          };
        };
      node_a = leaf;
      node_b = leaf;
      node_c = leaf;
    };

  testScript = ''
    nodes = {"node_hub": node_hub, "node_a": node_a, "node_b": node_b, "node_c": node_c}

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

    # Send `frame` from `src`; assert it reaches every node in `expect` and
    # appears exactly once on `src`'s own bus (the original cansend, never an
    # echo from the hub -- origin-exclusion). In this star every node is either
    # the source or an expected receiver, so there is no separate "absent" set.
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
        print("[fanout] %s -> %s OK (not echoed back to %s)" % (src_name, expect, src_name))

    start_all()
    for n in nodes.values():
        n.wait_for_unit("cannelloni")
    for n in nodes.values():
        n.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
        n.wait_until_succeeds("journalctl | grep 'CANThread up and running'")

    # --- Every participant reaches every other, never itself --------------
    fanout("node_a", "1A1#AA01", ["node_hub", "node_b", "node_c"])
    fanout("node_hub", "0FF#FEEDFACE", ["node_a", "node_b", "node_c"])
    # peer -> peer reflection through the hub (node_b's frame reaches node_c)
    fanout("node_b", "2B2#BB02", ["node_hub", "node_a", "node_c"])
    # CAN-FD + BRS passes through the hub unchanged (match the stable payload;
    # candump renders the FD flags nibble, which is irrelevant to the data)
    fanout("node_c", "3C3##1DEADBEEFCAFEBABE0011223344",
           ["node_hub", "node_a", "node_b"],
           needle="DEADBEEFCAFEBABE0011223344")

    # --- Overload: one peer floods; hub + all peers must survive ----------
    node_a.succeed("cangen vcan0 -g 0 -I i -D r -L 8 -n 5000")
    for n in nodes.values():
        n.succeed("systemctl is-active cannelloni")
    # Recovery: once the backlog drains a fresh frame still reaches the peers.
    cap_start(node_c)
    node_a.succeed("cansend vcan0 7AA#CAFEBABE")
    node_c.wait_until_succeeds("grep -q '7AA#CAFEBABE' /tmp/cap.log")
    node_c.execute("pkill -x candump")

    # --- Dead peer: kill one leaf, the hub keeps serving the rest ---------
    node_c.succeed("systemctl stop cannelloni")
    cap_start(node_b)
    node_a.succeed("cansend vcan0 555#DDDDDDDD")
    node_b.wait_until_succeeds("grep -q '555#DDDDDDDD' /tmp/cap.log")
    node_hub.succeed("systemctl is-active cannelloni")
    node_b.execute("pkill -x candump")
  '';
}
