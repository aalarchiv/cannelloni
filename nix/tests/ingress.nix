{ testers, pkgs }:
# CAN ingress-overrun stress test (cannelloni-84a.5, epic caveat 8).
#
# A hub fans every CAN frame read from its local bus out to every peer inline
# (allocate + copy + enqueue) before the next recv(), so the CAN RX critical
# path grows with the peer count. Under a high-rate burst the kernel CAN socket
# receive buffer can overflow and drop frames *at ingress* -- before cannellonis
# sees them -- which is distinct from (and invisible to) the bounded drop-oldest
# at egress. The hub now (a) requests a large SO_RCVBUF so the kernel queue
# absorbs the inline fan-out and (b) enables SO_RXQ_OVFL so any overrun is
# reported ("CAN ingress overrun" + the "ingress drops" shutdown summary) rather
# than lost silently.
#
# Topology: one UDP hub with three real leaves plus several unreachable static
# peers. The dummy peers cost the hub a full fan-out copy+enqueue per frame (the
# expensive part of the ingress path) without booting more VMs, so the fan-out
# is ~9 wide while only four nodes boot.
#
# Acceptance: under a -g0 cangen flood on the hub's own bus, with the receive
# buffer raised (net.core.rmem_max) so its capacity exceeds the burst, the hub
# (1) survives and keeps serving a fresh frame to a leaf, and (2) its CAN
# shutdown summary shows many frames received and *zero* ingress drops -- i.e.
# the mitigation kept the kernel from silently dropping at ingress, and the
# instrumentation is present to prove it.
testers.nixosTest {
  name = "ingress";

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
          services.setup_can.mtu = 72;
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
          services.setup_can.mtu = 72;
          # Raise the socket receive-buffer ceiling so the hub's 4 MiB SO_RCVBUF
          # request is honoured and the kernel queue can hold the whole burst,
          # leaving overflow attributable to a real ingress-path stall.
          boot.kernel.sysctl."net.core.rmem_max" = 33554432; # 32 MiB
          services.cannellonis = {
            enable = true;
            transport = "udp";
            ipProtocol = "ipv4";
            localPort = 10000;
            remotePort = 10000;
            canInterface = "vcan0";
            # Three real leaves + six unreachable TEST-NET-1 (RFC 5737) peers:
            # the dummy peers still incur a per-frame fan-out copy/enqueue on the
            # hub (drop-oldest at their egress), widening the inline ingress path
            # to ~9 without extra VMs.
            peers = [
              "node_a" "node_b" "node_c"
              "192.0.2.11" "192.0.2.12" "192.0.2.13"
              "192.0.2.14" "192.0.2.15" "192.0.2.16"
            ];
          };
        };
      node_a = leaf;
      node_b = leaf;
      node_c = leaf;
    };

  testScript = ''
    leaves = {"node_a": node_a, "node_b": node_b, "node_c": node_c}

    start_all()
    node_hub.wait_for_unit("cannellonis")
    for n in leaves.values():
        n.wait_for_unit("cannellonis")
    node_hub.wait_until_succeeds("journalctl -u cannellonis | grep 'UDPThread up and running'")
    node_hub.wait_until_succeeds("journalctl -u cannellonis | grep 'CANThread up and running'")
    # The mitigation must actually be in effect: enabling SO_RXQ_OVFL must not
    # have failed (otherwise a 0-drop result would be meaningless).
    node_hub.fail("journalctl -u cannellonis | grep -q 'Could not enable SO_RXQ_OVFL'")

    # --- Flood the hub's own CAN bus; the hub fans every frame out ~9 ways ----
    # -g0 sends as fast as possible; -n bounds the burst below the (raised)
    # socket capacity so a healthy ingress path never overflows.
    node_hub.succeed("cangen vcan0 -g 0 -I i -D r -L 8 -n 5000")

    # Everyone survives the flood.
    node_hub.succeed("systemctl is-active cannellonis")
    for n in leaves.values():
        n.succeed("systemctl is-active cannellonis")

    # Recovery: once the backlog drains a fresh frame still reaches a leaf.
    node_a.execute("pkill -x candump 2>/dev/null; true")
    node_a.execute("rm -f /tmp/cap.log")
    node_a.execute("setsid stdbuf -oL candump -L vcan0 > /tmp/cap.log 2>&1 < /dev/null &")
    node_a.sleep(1)
    node_hub.succeed("cansend vcan0 7AA#CAFEBABE")
    node_a.wait_until_succeeds("grep -q '7AA#CAFEBABE' /tmp/cap.log")
    node_a.execute("pkill -x candump")

    # --- No silent ingress loss --------------------------------------------
    # Stop the hub so it prints its CAN shutdown summary, then assert it read a
    # large number of frames AND dropped none at ingress.
    node_hub.succeed("systemctl stop cannellonis")
    node_hub.wait_until_succeeds("journalctl -u cannellonis | grep 'CAN Transmission Summary'")
    summary = node_hub.succeed(
        "journalctl -u cannellonis | grep 'CAN Transmission Summary' | tail -1")
    print("[ingress] hub summary: %s" % summary.strip())

    import re
    m = re.search(r"RX:\s*(\d+).*ingress drops:\s*(\d+)", summary)
    assert m, "could not parse CAN summary: %r" % summary
    rx, drops = int(m.group(1)), int(m.group(2))
    # Non-vacuous: the hub must actually have ingested most of the burst.
    assert rx >= 4000, "hub read only %d frames; flood did not exercise ingress" % rx
    # The mitigation must have kept the kernel from dropping at ingress.
    assert drops == 0, "hub saw %d ingress drops (RX=%d): receive buffer overflowed" % (drops, rx)
    # And no overrun warning should have been logged during the run.
    node_hub.fail("journalctl -u cannellonis | grep -q 'CAN ingress overrun'")
    print("[ingress] OK: RX=%d ingress drops=%d" % (rx, drops))
  '';
}
