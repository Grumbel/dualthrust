# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "dualthrust — CRT dual-engine lander (SDL2), Linux desktop integration";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # ArkOS (R36S) aarch64 sysroot: headers + libs the handheld port links against
    arkos-sysroot = {
      url = "github:grumnix/arkos-sysroot";
      flake = false;
    };

    # SDL2 sources for the WebAssembly build (nix/wasm.nix)
    sdl2-src = {
      url = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-2.30.3.tar.gz";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, arkos-sysroot, sdl2-src }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];

      # VERSION is the only source of truth; -dev builds get .<revCount>+g<rev> appended.
      versionBase = nixpkgs.lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);
      gitRev = "${self.shortRev or self.dirtyShortRev or "dirty"}";
      isDev = nixpkgs.lib.strings.hasInfix "-dev" versionBase;
      version =
        if isDev then "${versionBase}.${toString (self.revCount or 0)}+g${gitRev}"
        else versionBase;

      mkWasm = pkgs: import ./nix/wasm.nix {
        inherit pkgs version gitRev;
        sdlSrc = sdl2-src;
        box2dSrc = pkgs.box2d.src;
        sdlVersion = "2.30.3";
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });
    in
    {
      packages = forAllSystems ({ pkgs }:
        let
          lib = pkgs.lib;
          wasm = mkWasm pkgs;
          r36s = import ./nix/r36s.nix {
            inherit (pkgs) lib stdenv stdenvNoCC cmake pkg-config writeShellScript zip pkgsCross;
            sysrootSrc = arkos-sysroot;
            box2dSrc = pkgs.box2d.src;
            # `DUALTHRUST_ARKOS_SYSROOT=/nix/store/…-arkos-sysroot-… nix build --impure .#…` uses an
            # already-built sysroot instead of the input (e.g. one a pingus build unpacked).
            sysrootOverride =
              let p = builtins.getEnv "DUALTHRUST_ARKOS_SYSROOT";
              in if p == "" then null else builtins.storePath p;
          };
          dualthrustR36s = r36s.mkDualthrustR36s {
            src = lib.cleanSource ./.;
            inherit version;
          };
          dualthrustR36sPortMaster = r36s.mkDualthrustR36sPortMaster {
            r36sPkg = dualthrustR36s;
            inherit version;
          };
          dualthrustNative = pkgs.stdenv.mkDerivation {
            pname = "dualthrust";
            inherit version;
            src = self;

            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs = [ pkgs.SDL2 pkgs.box2d ];

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
              "-DPROJECT_VERSION_FULL=${version}"
              "-GNinja"
            ];
            # Keep symbols so gdb/backtrace work out of the store path
            dontStrip = true;
            separateDebugInfo = false;

            meta = with pkgs.lib; {
              description = "CRT dual-engine lander controlled by gamepad triggers";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "dualthrust";
            };
          };
        in {
        # WebAssembly (Emscripten): `nix build .#dualthrust-wasm`, `nix run .#dualthrust-wasm`
        sdl2-wasm = wasm.sdl2Wasm;
        dualthrust-wasm = wasm.dualthrustWasm;

        # R36S / ArkOS handheld (aarch64, linked against the ArkOS sysroot)
        arkos-sysroot = r36s.arkosSysroot;
        dualthrust-r36s = dualthrustR36s;
        dualthrust-r36s-portmaster = dualthrustR36sPortMaster;
        dualthrust-r36s-portmaster-zip = r36s.mkDualthrustR36sPortMasterZip {
          portMasterPkg = dualthrustR36sPortMaster;
          inherit version;
        };

        # the native package; `dualthrust` is an alias so `nix build .#dualthrust` reads naturally
        default = dualthrustNative;
        dualthrust = dualthrustNative;
      });

      apps = forAllSystems ({ pkgs }: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/bin/dualthrust";
        };
        # serve the WebAssembly build locally and open a browser
        dualthrust-wasm = (mkWasm pkgs).serveApp;
      });

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell.override { stdenv = pkgs.ccacheStdenv; } {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
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
                -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g" \
                -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG" \
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
