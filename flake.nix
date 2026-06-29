{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.nix-github-actions.url = "github:nix-community/nix-github-actions";
  inputs.nix-github-actions.inputs.nixpkgs.follows = "nixpkgs";

  outputs =
    {
      self,
      nixpkgs,
      nix-github-actions,
      ...
    }:
    let
      overlay = import ./nix/overlay.nix;

      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;

      nixpkgsFor = forAllSystems (
        system:
        import nixpkgs {
          inherit system;
          overlays = [ overlay ];
        }
      );
    in
    {
      overlays.default = overlay;

      packages = forAllSystems (
        system:
        let
          cannellonis = (nixpkgsFor.${system}).cannellonis;
        in
        {
          inherit cannellonis;
          default = cannellonis;
        }
      );

      devShells = forAllSystems (system: {
        default = import ./shell.nix { pkgs = (nixpkgsFor.${system}); };
      });

      nixosModules.default = import ./nix/module.nix;

      checks = forAllSystems (system: {
        sctp = nixpkgsFor.${system}.callPackage ./nix/tests/sctp.nix { };
        tcp = nixpkgsFor.${system}.callPackage ./nix/tests/tcp.nix { };
        udp = nixpkgsFor.${system}.callPackage ./nix/tests/udp.nix { };
        multipeer = nixpkgsFor.${system}.callPackage ./nix/tests/multipeer.nix { };
        tcp_multipeer = nixpkgsFor.${system}.callPackage ./nix/tests/tcp_multipeer.nix { };
        discovery = nixpkgsFor.${system}.callPackage ./nix/tests/discovery.nix { };
        mdns = nixpkgsFor.${system}.callPackage ./nix/tests/mdns.nix { };
        ingress = nixpkgsFor.${system}.callPackage ./nix/tests/ingress.nix { };
        characterization = nixpkgsFor.${system}.callPackage ./nix/tests/characterization.nix { };
        characterization_sort = nixpkgsFor.${system}.callPackage ./nix/tests/characterization_sort.nix { };
      });

      githubActions = nix-github-actions.lib.mkGithubMatrix {
        checks = nixpkgs.lib.getAttrs [ "x86_64-linux" ] self.checks;
      };
    };
}
