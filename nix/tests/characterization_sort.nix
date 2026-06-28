{ testers, pkgs }:
# Characterization of the UDP frame-sorting path (cannelloni -s), part of
# issue cannelloni-84a.8 (Phase 0). This lives in its own test because -s is a
# start-up flag: the sender must sort, which is incompatible with the FIFO
# ordering checked in characterization.nix on the same instance.
#
# node_a sends a shuffled batch of SFF frames with -s enabled; with a 1s buffer
# timeout the whole batch is sorted as one packet, so node_b must observe the
# frames in ascending CAN-id order. node_a's own bus still shows the shuffled
# send order (the negative control: it is NOT sorted).
let
  frameCompare = ../../tests/frame_compare.py;
in
testers.nixosTest {
  name = "characterization_sort";

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
          services.cannelloni = {
            enable = true;
            transport = "udp";
            ipProtocol = "ipv4";
            localPort = 10000;
            canInterface = "vcan0";
            extraArgs = [ "-s" "-t" "1000000" ]; # sort + batch as one packet
          };
        };
    in
    {
      node_a =
        { ... }:
        {
          imports = [ common ];
          services.cannelloni.remoteAddress = "node_b";
        };
      node_b =
        { ... }:
        {
          imports = [ common ];
          services.cannelloni.remoteAddress = "node_a";
        };
    };

  testScript = ''
    import sys
    import subprocess
    import tempfile

    frame_compare = "${frameCompare}"

    def cap_start(node):
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
        return res.returncode

    start_all()
    node_a.wait_for_unit("cannelloni")
    node_b.wait_for_unit("cannelloni")
    node_a.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")
    node_b.wait_until_succeeds("journalctl | grep 'UDPThread up and running'")

    cap_start(node_a)
    cap_start(node_b)
    shuffled = ["500", "100", "300", "0AA", "200", "7FF", "050", "400"]
    node_a.succeed("; ".join("cansend vcan0 %s#DEAD" % i for i in shuffled))
    node_a.sleep(3)
    a = cap_stop_read(node_a)
    b = cap_stop_read(node_b)

    # node_b must observe the same frames, sorted ascending by CAN id.
    rc = compare("sorted", a, b, "node_b observes -s sorted output")
    assert rc == 0, "receiver bus is not sorted by CAN id"

    # Negative control: the sender's own bus is NOT in sorted order.
    rc = compare("sorted", b, a, "sender bus is unsorted (negative control)")
    assert rc != 0, "sender bus unexpectedly already sorted - test is vacuous"
  '';
}
