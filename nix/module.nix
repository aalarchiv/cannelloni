{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.cannelloni;
in
{
  options.services.cannelloni = with lib; {
    enable = mkEnableOption "enable cannelloni service";
    canInterface = mkOption {
      type = types.str;
      default = "can0";
      description = "cannelloni will run on this CAN interface";
    };
    remoteAddress = mkOption {
      type = types.nullOr types.str;
      default = null;
      description = "IP Address of the remote cannelloni instance. Optional for UDP";
    };
    peers = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "node_b" "node_c:20000" ];
      description = ''
        UDP hub peers as HOST[:PORT] (PORT defaults to remotePort). When
        non-empty, cannelloni runs as a multi-peer hub: each entry is emitted as
        a repeatable --peer argument and -R/-p is omitted.
      '';
    };
    discover = mkOption {
      type = types.bool;
      default = false;
      description = ''
        Enable dynamic UDP peer discovery (hub only). When set, cannelloni learns
        peers at runtime from valid incoming traffic and may run with an empty
        static peer list, so devices can dial in without static config. Evicts a
        discovered peer after peerTimeout seconds of silence. UDP only; off by
        default. Trusted networks only -- discovery is unauthenticated by design.
      '';
    };
    mdns = mkOption {
      type = types.bool;
      default = false;
      description = ''
        Enable mDNS/Avahi zeroconf peer discovery (hub only). When set,
        cannelloni advertises itself and browses the LAN for other instances,
        learning peers (address + port) before any data flows, and may run with
        an empty static peer list. Feeds the same dynamic-peer machinery as
        discover (peers age out / re-learn by traffic), so maxPeers and
        peerTimeout apply. Requires a running avahi-daemon (services.avahi). UDP
        only; off by default. Trusted networks only -- unauthenticated by design.
      '';
    };
    maxPeers = mkOption {
      type = types.int;
      default = 16;
      description = ''
        Cap on dynamically discovered UDP peers (with discover = true or
        mdns = true) or simultaneously accepted TCP server clients. Matches the
        cannelloni --max-peers default of 16 when left unset.
      '';
    };
    peerTimeout = mkOption {
      type = types.int;
      default = 30;
      description = ''
        Evict a discovered peer after this many seconds of silence (0 = never).
        Only used with discover = true.
      '';
    };
    bufferFrames = mkOption {
      type = types.nullOr types.int;
      default = null;
      description = ''
        Per-participant egress FrameBuffer pool: number of CAN frames
        preallocated up front (cannelloni --buffer-frames). null leaves the
        cannelloni default (1000). Lower it on a many-peer hub to cut the
        baseline RAM, which scales as (peers + 1) x this.
      '';
    };
    bufferMax = mkOption {
      type = types.nullOr types.int;
      default = null;
      description = ''
        Per-participant egress FrameBuffer pool: hard cap on frames the pool may
        grow to (cannelloni --buffer-max, 0 = unlimited). null leaves the
        cannelloni default (16000). Bounds the worst-case RAM of a many-peer hub.
      '';
    };
    remotePort = mkOption {
      type = types.int;
      default = 10000;
      description = "Port of the remote cannelloni instance";
    };
    localAddress = mkOption {
      type = types.str;
      default = "0.0.0.0";
      description = "IP address of the local cannelloni instance";
    };
    localPort = mkOption {
      type = types.int;
      default = 10000;
      description = "Port of the local cannelloni instance";
    };
    transport = mkOption {
      type = types.enum [ "udp" "tcp" "sctp" ];
      default = "tcp";
      description = "which transport to use";
    };
    ipProtocol = mkOption {
      type = types.enum [ "ipv4" "ipv6" ];
      default = "ipv4";
      description = "which IP protocol to use";
    };
    mode = mkOption {
      type = types.enum [ "server" "client" ];
      default = "server";
      description = "which mode to run in (server or client)";
    };
    extraArgs = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "-s" "-t" "1000000" ];
      description = "extra command line arguments appended verbatim to cannelloni (e.g. -s to sort frames)";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.cannelloni =
    let
      mode = if cfg.mode == "server" then "s" else "c";
      transportAndMode = if cfg.transport != "udp" then (if cfg.transport == "tcp" then "-C ${mode}" else "-S ${mode}") else "";
      # A non-empty peer list makes this a UDP hub: emit repeatable --peer and
      # drop -R/-p (peers carry their own addresses; sources are matched by them).
      peerArgs = lib.concatStringsSep " " (map (p: "--peer ${p}") cfg.peers);
      # A discovery hub (traffic- or mDNS-learnt) may run with no static remote
      # at all (it learns peers).
      remoteAddress =
        if cfg.peers != [ ] then ""
        else if cfg.remoteAddress != null then "-R ${cfg.remoteAddress}"
        else if (cfg.discover || cfg.mdns) then ""
        else "-p";
      # --discover / --mdns are sibling discovery backends; both feed the same
      # dynamic-peer machinery, so the cap + liveness knobs apply to either.
      discoverArgs = lib.concatStringsSep " " (
        lib.optional cfg.discover "--discover"
        ++ lib.optional cfg.mdns "--mdns"
        ++ lib.optional (cfg.discover || cfg.mdns)
          "--max-peers ${builtins.toString cfg.maxPeers} --peer-timeout ${builtins.toString cfg.peerTimeout}"
      );
      # Egress pool sizing: emit only when overridden so the default command line
      # (and every existing test) is left byte-for-byte unchanged.
      bufferArgs = lib.concatStringsSep " " (
        lib.optional (cfg.bufferFrames != null) "--buffer-frames ${builtins.toString cfg.bufferFrames}"
        ++ lib.optional (cfg.bufferMax != null) "--buffer-max ${builtins.toString cfg.bufferMax}"
      );
      extraArgs = lib.concatStringsSep " " cfg.extraArgs;
    in {
      description = "cannelloni";
      after = [ "network.target" ];
      wantedBy = [ "multi-user.target" ];

      serviceConfig = {
        ExecStart = "${pkgs.cannelloni}/bin/cannelloni ${transportAndMode} -I ${cfg.canInterface} -l ${builtins.toString cfg.localPort} -L ${cfg.localAddress} -r ${builtins.toString cfg.remotePort} ${remoteAddress} ${peerArgs} ${discoverArgs} ${bufferArgs} ${extraArgs}";
        User="cannelloni";
        DynamicUser=true;
       };
    };
  };
}