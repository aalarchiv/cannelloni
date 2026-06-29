{ testers, pkgs }:
# Characterization test for the CAN <-> single-peer data path (issue
# cannelloni-84a.8, Phase 0). Locks in the current, observable behaviour with a
# real byte-level comparison BEFORE the Router refactor (Phase 1, 84a.1) so a
# regression can actually be detected. The legacy one-frame substring grep in
# udp.nix/tcp.nix/sctp.nix cannot catch ordering, batching, FD/RTR/EFF or
# pool-exhaustion regressions; this test can.
#
# Two UDP peers bridge vcan0<->vcan0. Both buses are captured with
# `candump -L` and compared frame-token-for-frame-token with frame_compare.py.
# vcan0 MTU is 72 so CAN-FD is negotiated. A generous buffer timeout (-t) makes
# a rapid burst flush as a single multi-frame packet, so batching/ordering is
# deterministic.
let
  frameCompare = ../../tests/frame_compare.py;
in
testers.nixosTest {
  name = "characterization";

  nodes =
    let
      common =
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
            canInterface = "vcan0";
            # Buffer for 1s so a burst becomes one batched UDP packet
            # (deterministic batching + FIFO ordering for the comparison).
            extraArgs = [ "-t" "1000000" ];
          };
        };
    in
    {
      node_a =
        { ... }:
        {
          imports = [ common ];
          services.cannellonis.remoteAddress = "node_b";
        };
      node_b =
        { ... }:
        {
          imports = [ common ];
          services.cannellonis.remoteAddress = "node_a";
        };
    };

  testScript = ''
    import sys
    import subprocess
    import tempfile

    frame_compare = "${frameCompare}"

    def cap_start(node):
        # Fresh capture; stdbuf -oL forces line buffering so every captured
        # frame hits the file immediately (robust against block buffering).
        node.execute("pkill -x candump 2>/dev/null; true")
        node.execute("rm -f /tmp/cap.log")
        node.execute("setsid stdbuf -oL candump -L vcan0 > /tmp/cap.log 2>&1 < /dev/null &")
        node.sleep(1)

    def cap_stop_read(node):
        node.execute("pkill -x candump")
        node.sleep(1)
        return node.succeed("cat /tmp/cap.log")

    def write_tmp(text):
        f = tempfile.NamedTemporaryFile("w", suffix=".log", delete=False)
        f.write(text)
        f.close()
        return f.name

    def compare(mode, left_text, right_text, msg):
        left = write_tmp(left_text)
        right = write_tmp(right_text)
        res = subprocess.run(
            [sys.executable, frame_compare, mode, left, right],
            capture_output=True, text=True)
        print("[%s] %s\n%s%s" % (mode, msg, res.stdout, res.stderr))
        assert res.returncode == 0, "frame_compare %s failed (%s)" % (mode, msg)

    def send(node, frames):
        node.succeed("; ".join("cansend vcan0 %s" % f for f in frames))

    start_all()
    node_a.wait_for_unit("cannellonis")
    node_b.wait_for_unit("cannellonis")
    node_a.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
    node_b.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
    node_a.wait_until_succeeds("journalctl | grep 'CANThread up and running'")
    node_b.wait_until_succeeds("journalctl | grep 'CANThread up and running'")

    # --- Subtest 1: multi-frame batch, FIFO order preserved (no -s) ---------
    cap_start(node_a)
    cap_start(node_b)
    batch_ids = ["3FF", "010", "280", "0C0", "7A1", "155", "020", "6FE",
                 "400", "088", "1FF", "333", "0AA", "5C5", "240", "011"]
    batch = []
    for i, cid in enumerate(batch_ids):
        batch.append("%s#%02X%02X" % (cid, i, (i * 7) % 256))
    send(node_a, batch)
    node_a.sleep(3)
    a = cap_stop_read(node_a)
    b = cap_stop_read(node_b)
    # node_a's bus shows the send order; node_b's bus must match it exactly.
    compare("identical", a, b, "multi-frame batch is delivered FIFO")

    # --- Subtest 2: CAN-FD + RTR + EFF pass-through (byte identical) ---------
    cap_start(node_a)
    cap_start(node_b)
    typed = [
        "123#1122334455667788",                          # classic SFF, 8 bytes
        "18FFAA01#DEADBEEF",                             # 29-bit EFF
        "200#R",                                         # RTR, no dlc
        "201#R8",                                        # RTR, dlc 8
        "300##0112233445566778899AABBCCDDEEFF00",        # CAN-FD, BRS off
        "301##1112233445566778899AABBCCDDEEFF00",        # CAN-FD, BRS on
    ]
    send(node_a, typed)
    node_a.sleep(3)
    a = cap_stop_read(node_a)
    b = cap_stop_read(node_b)
    compare("multiset", a, b, "FD/RTR/EFF frames pass through unchanged")

    # --- Subtest 3: bidirectional flow (A<->B) ------------------------------
    cap_start(node_a)
    cap_start(node_b)
    from_a = ["101#A1", "102#A2", "103#A3"]
    from_b = ["601#B1", "602#B2", "603#B3"]
    send(node_a, from_a)
    send(node_b, from_b)
    node_a.sleep(3)
    a = cap_stop_read(node_a)
    b = cap_stop_read(node_b)
    exp_a = "".join("(0) x %s\n" % f for f in from_a)
    exp_b = "".join("(0) x %s\n" % f for f in from_b)
    compare("subset", exp_a, b, "node_b receives everything node_a sent")
    compare("subset", exp_b, a, "node_a receives everything node_b sent")
    # Shared-bus invariant: every participant sees the same set of frames.
    compare("multiset", a, b, "both buses observe an identical frame set")

    # --- Subtest 4: pool exhaustion / overflow (must run last) --------------
    # Flooding leaves a draining backlog, so this is the final subtest.
    node_a.succeed("cangen vcan0 -g 0 -I i -D r -L 8 -n 5000")
    # No crash / deadlock under overload.
    node_a.succeed("systemctl is-active cannellonis")
    node_b.succeed("systemctl is-active cannellonis")
    # Recovery: once the backlog drains a fresh frame still gets through.
    node_b.execute("pkill -x candump 2>/dev/null; true")
    node_b.execute("rm -f /tmp/cap.log")
    node_b.execute("setsid stdbuf -oL candump -L vcan0 > /tmp/cap.log 2>&1 < /dev/null &")
    node_b.sleep(1)
    node_a.succeed("cansend vcan0 7AA#CAFEBABE")
    node_b.wait_until_succeeds("grep -q '7AA#CAFEBABE' /tmp/cap.log")
    node_b.execute("pkill -x candump")
  '';
}
