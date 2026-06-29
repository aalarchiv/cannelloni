{ config, pkgs, lib, ... }:
{
  options.services.dump_can = with lib; {
    enable = mkEnableOption "enable the can dump service";
  };

  # MTU of the vcan0 interface. The default of 16 is the classic CAN MTU;
  # set to 72 (CANFD_MTU) so cannelloni negotiates CAN-FD and FD frames can
  # be exercised (see the characterization test).
  options.services.setup_can.mtu = with lib; mkOption {
    type = types.int;
    default = 16;
    description = "MTU for the vcan0 test interface (16 = classic CAN, 72 = CAN-FD)";
  };

  config = {
    # can-utils (cangen/cansend/candump) and procps (pkill) on PATH for tests.
    environment.systemPackages = [ pkgs.can-utils ];

    systemd.services.setup_can = {
      wantedBy = [ "multi-user.target" ];
      before = [ "cannelloni.service" ];
      script = ''
        ${pkgs.kmod}/bin/modprobe vcan
        ${pkgs.iproute2}/bin/ip link add name vcan0 type vcan
        ${pkgs.iproute2}/bin/ip link set dev vcan0 up mtu ${toString config.services.setup_can.mtu}
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };
    systemd.services.dump_can = lib.mkIf config.services.dump_can.enable {
      wantedBy = [ "multi-user.target" ];
      before = [ "cannelloni.service" ];
      after = [ "setup_can.service" ];
      wants = [ "setup_can.service" ];
      script = ''
        ${pkgs.can-utils}/bin/candump vcan0 > /tmp/vcan0.dump
      '';
    };
  };
}
