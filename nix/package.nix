{
  stdenv,
  cmake,
  lib,
  lksctp-tools,
  pkg-config,
  avahi,
}:
stdenv.mkDerivation {
  name = "cannellonis";
  version = "2.0.1";

  src = builtins.filterSource (
    path: type: !(lib.strings.hasSuffix "nix" path || lib.strings.hasSuffix "flake.lock" path)
  ) (lib.cleanSource ../.);

  # pkg-config (native) lets CMake locate avahi-client; avahi provides the
  # client library + headers for the optional mDNS peer discovery (84a.7).
  nativeBuildInputs = [
    pkg-config
  ];

  propagatedBuildInputs = [
    cmake
    lksctp-tools
    avahi
  ];
  meta = with lib; {
    description = "A SocketCAN over Ethernet Tunnel";
    homepage = "https://github.com/mguentner/cannelloni";
    platforms = platforms.linux;
  };
}
