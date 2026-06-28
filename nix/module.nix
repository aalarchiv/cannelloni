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
      remoteAddress =
        if cfg.peers != [ ] then ""
        else if cfg.remoteAddress != null then "-R ${cfg.remoteAddress}"
        else "-p";
      extraArgs = lib.concatStringsSep " " cfg.extraArgs;
    in {
      description = "cannelloni";
      after = [ "network.target" ];
      wantedBy = [ "multi-user.target" ];

      serviceConfig = {
        ExecStart = "${pkgs.cannelloni}/bin/cannelloni ${transportAndMode} -I ${cfg.canInterface} -l ${builtins.toString cfg.localPort} -L ${cfg.localAddress} -r ${builtins.toString cfg.remotePort} ${remoteAddress} ${peerArgs} ${extraArgs}";
        User="cannelloni";
        DynamicUser=true;
       };
    };
  };
}