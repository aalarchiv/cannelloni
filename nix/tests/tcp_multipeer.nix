{ testers, pkgs }:
# Phase 4 acceptance test (cannelloni-84a.4): a TCP multi-peer server hub.
#
# Topology mirrors the UDP multipeer test, but over TCP: one TCP *server* hub
# accepts three TCP *client* leaves. The virtual shared bus has four
# participants: the hub's CAN bus and each leaf's CAN bus.
#
#   node_a --\
#   node_b ---+-- TCP --> node_hub (server) -- TCP --> the other leaves + own CAN
#   node_c --/
#
# Acceptance: every node receives every OTHER node's frames and NEVER its own;
# a stalled peer (SIGSTOP) does not stall frame flow to the others and the hub
# survives (backpressure isolation, Phase 4b); and a disconnecting client does
# not disrupt the rest.
testers.nixosTest {
  name = "tcp_multipeer";

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
            transport = "tcp";
            mode = "client";
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
            transport = "tcp";
            mode = "server";
            ipProtocol = "ipv4";
            localPort = 10000;
            remotePort = 10000;
            canInterface = "vcan0";
            # no remoteAddress => the module emits -p, so the hub accepts every
            # connecting client rather than a single configured remote.
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
    # echo from the hub -- origin-exclusion).
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
        n.wait_for_unit("cannellonis")
    # The hub logs one negotiated line per accepted client; wait for all three.
    node_hub.wait_until_succeeds("journalctl -u cannellonis | grep 'TCPServerThread up and running'")
    node_hub.wait_until_succeeds(
        "test $(journalctl -u cannellonis | grep -c 'negotiated as participant') -ge 3"
    )

    # --- Every participant reaches every other, never itself --------------
    fanout("node_a", "1A1#AA01", ["node_hub", "node_b", "node_c"])
    fanout("node_hub", "0FF#FEEDFACE", ["node_a", "node_b", "node_c"])
    # peer -> peer reflection through the hub (node_b's frame reaches node_c)
    fanout("node_b", "2B2#BB02", ["node_hub", "node_a", "node_c"])
    # CAN-FD + BRS passes through the hub unchanged (match the stable payload)
    fanout("node_c", "3C3##1DEADBEEFCAFEBABE0011223344",
           ["node_hub", "node_a", "node_b"],
           needle="DEADBEEFCAFEBABE0011223344")

    # --- Backpressure isolation: stall node_c, others keep flowing --------
    # Freeze node_c's cannellonis so it stops reading its TCP socket; the hub's
    # send queue to it fills. With blocking writes this would stall the whole
    # hub; with non-blocking per-peer TX (Phase 4b) the others are unaffected.
    node_c.succeed("pkill -STOP -x cannellonis")
    node_a.succeed("cangen vcan0 -g 0 -I i -D r -L 8 -n 5000")
    node_a.sleep(5)  # let the healthy node_a -> hub -> node_b path drain
    # A fresh sentinel from node_a must still reach node_b while node_c is stuck.
    cap_start(node_b)
    node_a.succeed("cansend vcan0 7AA#CAFEBABE")
    node_b.wait_until_succeeds("grep -q '7AA#CAFEBABE' /tmp/cap.log")
    node_b.execute("pkill -x candump")
    node_hub.succeed("systemctl is-active cannellonis")
    print("[isolation] node_b kept flowing while node_c was stalled")

    # Recovery: unfreeze node_c, it must receive new frames again.
    node_c.succeed("pkill -CONT -x cannellonis")
    node_c.sleep(2)
    cap_start(node_c)
    node_a.succeed("cansend vcan0 7BB#0BADF00D")
    node_c.wait_until_succeeds("grep -q '7BB#0BADF00D' /tmp/cap.log")
    node_c.execute("pkill -x candump")
    print("[isolation] node_c recovered after SIGCONT")

    # --- Dead peer: stop one leaf, the hub keeps serving the rest ---------
    node_c.succeed("systemctl stop cannellonis")
    cap_start(node_b)
    node_a.succeed("cansend vcan0 555#DDDDDDDD")
    node_b.wait_until_succeeds("grep -q '555#DDDDDDDD' /tmp/cap.log")
    node_hub.succeed("systemctl is-active cannellonis")
    node_b.execute("pkill -x candump")
  '';
}
