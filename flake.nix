# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "dualthrust — CRT dual-engine lander (SDL2), Linux desktop integration";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });
    in
    {
      packages = forAllSystems ({ pkgs }: {
        default = pkgs.stdenv.mkDerivation {
          pname = "dualthrust";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [ pkgs.SDL2 ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
            "-GNinja"
          ];

          meta = with pkgs.lib; {
            description = "CRT dual-engine lander controlled by gamepad triggers";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "dualthrust";
          };
        };
      });

      apps = forAllSystems ({ pkgs }: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.default}/bin/dualthrust";
        };
      });

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell.override { stdenv = pkgs.ccacheStdenv; } {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.gdb
            pkgs.ccache
            pkgs.pkg-config
            (pkgs.writeShellScriptBin "dualthrust-configure" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "$0: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/dualthrust-build}"
              mkdir -p "$PROJECT_BUILD_DIR"
              cd "$PROJECT_BUILD_DIR"
              cmake -GNinja \
                -DCMAKE_BUILD_TYPE="''${CMAKE_BUILD_TYPE:-Debug}" \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                "$PROJECT_SOURCE"
            '')
            (pkgs.writeShellScriptBin "dualthrust-build" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "$0: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/dualthrust-build}"
              if [ ! -f "$PROJECT_BUILD_DIR/CMakeCache.txt" ]; then
                dualthrust-configure
              else
                # Reconfigure if source path changed
                cached=$(grep -E '^CMAKE_HOME_DIRECTORY:' "$PROJECT_BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)
                if [ "$cached" != "$PROJECT_SOURCE" ]; then
                  echo "Source path changed ($cached -> $PROJECT_SOURCE), reconfiguring..."
                  dualthrust-configure
                fi
              fi
              cmake --build "$PROJECT_BUILD_DIR" -j"$(nproc)"
            '')
            (pkgs.writeShellScriptBin "dualthrust-run" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "$0: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              dualthrust-build
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/dualthrust-build}"
              exec "$PROJECT_BUILD_DIR/dualthrust" "$@"
            '')
            (pkgs.writeShellScriptBin "dualthrust-run-gdb" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "$0: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              dualthrust-build
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/dualthrust-build}"
              exec gdb -q \
                -ex "set pagination off" \
                -ex "set confirm off" \
                -ex "set debuginfod enabled off" \
                -ex run \
                -ex 'python
try:
  ec = gdb.parse_and_eval("$_exitcode")
  if int(ec) == 0:
    gdb.execute("quit")
except Exception:
  pass
' \
                --args "$PROJECT_BUILD_DIR/dualthrust" "$@"
            '')
          ];
          CMAKE_BUILD_TYPE = "Debug";
          shellHook = ''
            export PROJECT_SOURCE="$PWD"
            export PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/dualthrust-build}"
            echo "dualthrust develop shell"
            echo "  dualthrust-configure | dualthrust-build | dualthrust-run | dualthrust-run-gdb"
          '';
        };
      });
    };
}
