{
  description = "FunBox (SubstationNS fork) — Daisy Seed pedal firmware dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            # Daisy toolchain
            gcc-arm-embedded-13  # newest GCC trips -Werror in CloudSeed
            gnumake

            # Flashing / debugging
            dfu-util   # make program-dfu
            openocd    # make program (ST-Link)
            tio        # USB serial log (tio /dev/ttyACM0)
            usbutils   # lsusb, used by software/Ceres/flash.sh
          ];

          shellHook = ''
            echo "FunBox / Daisy dev environment"
            echo "gcc      $(arm-none-eabi-gcc --version | head -n1)"
            echo "dfu-util $(dfu-util --version | head -n1)"
          '';
        };
      });
}
