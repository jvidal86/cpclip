{
  description = "Minimal X11 and Wayland clipboard CLI";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAll (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in {
          default = pkgs.stdenv.mkDerivation {
            pname = "cpclip";
            version = "0.1.0";
            src = ../.;                      # the repo root (this flake lives in packaging/)

            nativeBuildInputs = [ pkgs.pkg-config pkgs.wayland-scanner ];
            buildInputs = [
              pkgs.xorg.libX11
              pkgs.xorg.libXfixes
              pkgs.wayland
            ];

            # PREFIX points straight at $out; the Makefile honors PREFIX/DESTDIR.
            installFlags = [ "PREFIX=${placeholder "out"}" ];

            meta = with nixpkgs.lib; {
              description = "Minimal X11 and Wayland clipboard CLI";
              homepage = "https://github.com/jvidal86/cpclip";
              license = licenses.gpl2Plus;
              platforms = systems;
              mainProgram = "cpclip";
            };
          };
        });
    };
}
