# R36S / ArkOS (RK3326) cross build against a published ArkOS sysroot.
#
# The sysroot supplies aarch64 headers + shared libs (glibc ~2.30, SDL2 2.0.x, ALSA, …) so the
# binary runs on stock ArkOS — plain pkgsCross would link modern glibc/libstdc++ instead.
# Sysroot: github:grumnix/arkos-sysroot (flake input, pinned in flake.lock).
# Same sysroot and compiler-wrapper approach as pingus (nix/r36s.nix there), trimmed to what
# dualthrust needs: SDL2 only (no SDL2_image, OpenAL, zlib, GLES link — SDL loads GLES itself).
#
#   nix build .#arkos-sysroot
#   nix build .#dualthrust-r36s
#   nix build .#dualthrust-r36s-portmaster       # tree for /roms/ports
#   nix build .#dualthrust-r36s-portmaster-zip   # PortMaster autoinstall zip
#
# SDL on the device may be as old as 2.0.10: do not use newer SDL API (no SDL_RenderGeometry).
{ lib
, stdenv
, stdenvNoCC
, cmake
, pkg-config
, pkgsCross
, writeShellScript
, zip
  # Unpacked sysroot tree (usr/, lib/): flake input github:grumnix/arkos-sysroot
, sysrootSrc
  # Optional already-built sysroot store path to use instead (dev shortcut, needs --impure).
, sysrootOverride ? null
}:

let
  khrplatformH = ../mk/r36s/include/KHR/khrplatform.h;

  arkosSysroot = if sysrootOverride != null then sysrootOverride else stdenvNoCC.mkDerivation {
    pname = "arkos-sysroot";
    version = "0.1-openal";
    src = sysrootSrc;

    # Unpack-only: aarch64 ELF + linker scripts must not be touched by host fixup.
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    dontPatchELF = true;
    dontStrip = true;
    dontPatchShebangs = true;
    dontCheckForBrokenSymlinks = true;

    installPhase = ''
      runHook preInstall
      mkdir -p "$out"

      if [ -d usr ]; then
        cp -a . "$out/"
      elif [ -d sysroot/usr ]; then
        cp -a sysroot/. "$out/"
      else
        top=
        for d in *; do
          if [ -d "$d/usr" ]; then top="$d"; break; fi
        done
        if [ -z "$top" ]; then
          echo "arkos-sysroot: unrecognized tarball layout (no usr/)" >&2
          exit 1
        fi
        cp -a "$top"/. "$out/"
      fi
      test -d "$out/usr" || { echo "arkos-sysroot: missing $out/usr" >&2; exit 1; }

      for base in "$out/usr/include" "$out/usr/lib" "$out/lib"; do
        if [ -d "$base/aarch64-linux-gnu" ] && [ ! -e "$base/aarch64-unknown-linux-gnu" ]; then
          ln -sfn aarch64-linux-gnu "$base/aarch64-unknown-linux-gnu"
        fi
      done

      mkdir -p "$out/usr/include/KHR"
      cp -f ${khrplatformH} "$out/usr/include/KHR/khrplatform.h"

      # Debian libc.so & co. are linker scripts with absolute /lib paths: rewrite only the
      # multiarch prefixes, on token boundaries.
      find "$out" -type f \( -name 'libc.so' -o -name 'libpthread.so' -o -name 'libm.so' -o -name 'libdl.so' -o -name 'librt.so' -o -name 'libutil.so' -o -name 'libresolv.so' -o -name 'libanl.so' -o -name 'libBrokenLocale.so' -o -name 'libthread_db.so' \) 2>/dev/null | while read -r f; do
        if grep -qE 'GROUP|INPUT' "$f" 2>/dev/null; then
          sed -i -E \
            -e "s#(^|[[:space:](=])/usr/lib/aarch64-linux-gnu/#\1$out/usr/lib/aarch64-linux-gnu/#g" \
            -e "s#(^|[[:space:](=])/lib/aarch64-linux-gnu/#\1$out/lib/aarch64-linux-gnu/#g" \
            "$f" || true
        fi
      done

      ln -sfn . "$out/sysroot"
      echo "arkos-sysroot ready" > "$out/SYSROOT.txt"
      runHook postInstall
    '';

    meta = with lib; {
      description = "ArkOS / R36S aarch64 sysroot (glibc + SDL2 + GLES)";
      license = licenses.free;
      platforms = platforms.linux;
      hydraPlatforms = [];
    };
  };

  crossPkgs = pkgsCross.aarch64-multiplatform;
  crossCc = crossPkgs.stdenv.cc;
  targetPrefix = crossCc.targetPrefix;

  # Compiler wrappers. Compile: modern libstdc++ headers (old string ABI) + ArkOS glibc headers
  # only. Link: ArkOS libstdc++/SDL2 by absolute path (GCC 15's libstdc++ needs GLIBCXX_3.4.32
  # and glibc 2.38), static libgcc, ArkOS dynamic linker.
  mkWrappers = sysroot: let
    gcc = crossCc.cc;
    tp = lib.removeSuffix "-" targetPrefix;
    libdir = "${sysroot}/usr/lib/aarch64-linux-gnu";
    cxxInc = "${gcc}/include/c++/${gcc.version}";
    fixedInc = "${gcc}/lib/gcc/${tp}/${gcc.version}/include";
    fixedInc2 = "${gcc}/lib/gcc/${tp}/${gcc.version}/include-fixed";
    libgccDir = "${gcc}/lib/gcc/${tp}/${gcc.version}";
    gccLibOut = lib.getLib gcc;
    commonCompile = ''
      -nostdinc \
      --sysroot=${sysroot} \
      -isystem ${fixedInc} \
      -isystem ${fixedInc2} \
      -isystem ${sysroot}/usr/include/aarch64-linux-gnu \
      -isystem ${sysroot}/usr/include \
      -pthread \
      -fexceptions \
      -march=armv8-a \
      -mtune=cortex-a35 \
    '';
    commonCompileCxx = ''
      -nostdinc \
      -D_GLIBCXX_USE_CXX11_ABI=0 \
      --sysroot=${sysroot} \
      -isystem ${cxxInc} \
      -isystem ${cxxInc}/${tp} \
      -isystem ${cxxInc}/backward \
      -isystem ${fixedInc} \
      -isystem ${fixedInc2} \
      -isystem ${sysroot}/usr/include/aarch64-linux-gnu \
      -isystem ${sysroot}/usr/include \
      -pthread \
      -fexceptions \
      -march=armv8-a \
      -mtune=cortex-a35 \
    '';
    commonLink = ''
      --sysroot=${sysroot} \
      -Wl,--sysroot=${sysroot} \
      -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
      -B${libdir} \
      -B${libgccDir} \
      -L${libdir} \
      -L${sysroot}/usr/lib \
      -L${sysroot}/lib \
      -L${sysroot}/lib/aarch64-linux-gnu \
      -L${libgccDir} \
      -L${gccLibOut}/lib \
      -L${gccLibOut}/${tp}/lib \
      -static-libgcc \
      -Wl,-Bdynamic \
      -l:libpthread.so.0 \
      -lm \
      -Wl,-rpath-link,${libdir} \
      -Wl,-rpath-link,${sysroot}/lib/aarch64-linux-gnu \
      -Wl,-rpath-link,${libdir}/pulseaudio \
      -Wl,--allow-shlib-undefined \
      -Wl,--as-needed \
      -march=armv8-a \
      -mtune=cortex-a35 \
    '';
  in {
    cc = writeShellScript "aarch64-arkos-gcc" ''
      export PATH="${crossCc.bintools}/bin:$PATH"
      is_compile=
      for a in "$@"; do
        case "$a" in -c|-S|-E|-M|-MM|-MD|-MMD) is_compile=1 ;; esac
      done
      if [ -n "$is_compile" ]; then
        exec ${gcc}/bin/${targetPrefix}gcc -B${crossCc.bintools}/bin ${commonCompile} "$@"
      else
        exec ${gcc}/bin/${targetPrefix}gcc -B${crossCc.bintools}/bin ${commonCompile} ${commonLink} "$@"
      fi
    '';
    cxx = writeShellScript "aarch64-arkos-g++" ''
      export PATH="${crossCc.bintools}/bin:$PATH"
      is_compile=
      for a in "$@"; do
        case "$a" in -c|-S|-E|-M|-MM|-MD|-MMD) is_compile=1 ;; esac
      done
      if [ -n "$is_compile" ]; then
        exec ${gcc}/bin/${targetPrefix}g++ -B${crossCc.bintools}/bin ${commonCompileCxx} "$@"
      fi
      first_existing() { for c in "$@"; do if [ -e "$c" ]; then echo "$c"; return 0; fi; done; return 1; }
      stdcpp=$(first_existing "${libdir}/libstdc++.so" "${libdir}/libstdc++.so.6" \
        "${sysroot}/usr/lib/libstdc++.so.6" "${sysroot}/lib/aarch64-linux-gnu/libstdc++.so.6") || {
        echo "aarch64-arkos-g++: no libstdc++ in sysroot" >&2; exit 1; }
      sdl2=$(first_existing "${libdir}/libSDL2-2.0.so" "${libdir}/libSDL2-2.0.so.0" "${libdir}/libSDL2.so") || {
        echo "aarch64-arkos-g++: no libSDL2 in sysroot" >&2; exit 1; }
      exec ${gcc}/bin/${targetPrefix}g++ \
        -B${crossCc.bintools}/bin \
        ${commonCompileCxx} \
        -nostdlib++ \
        ${commonLink} \
        "$@" \
        -Wl,--no-as-needed "$stdcpp" "$sdl2" \
        -Wl,-Bdynamic -l:libpthread.so.0 -lm \
        -Wl,--as-needed
    '';
  };

  mkDualthrustR36s = {
    src
  , version
  , pname ? "dualthrust-r36s"
  }:
    let
      wrappers = mkWrappers arkosSysroot;
    in
    stdenv.mkDerivation {
      inherit pname version src;

      nativeBuildInputs = [ cmake pkg-config crossCc.bintools ];
      strictDeps = true;

      cmakeFlags = [
        "-DCMAKE_SYSTEM_NAME=Linux"
        "-DCMAKE_SYSTEM_PROCESSOR=aarch64"
        "-DCMAKE_SYSROOT=${arkosSysroot}"
        "-DCMAKE_FIND_ROOT_PATH=${arkosSysroot}"
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY"
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY"
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY"
        "-DCMAKE_C_COMPILER=${wrappers.cc}"
        "-DCMAKE_CXX_COMPILER=${wrappers.cxx}"
        "-DCMAKE_C_COMPILER_WORKS=1"
        "-DCMAKE_CXX_COMPILER_WORKS=1"
        "-DCMAKE_C_COMPILER_FORCED=TRUE"
        "-DCMAKE_CXX_COMPILER_FORCED=TRUE"
        # The binary must use ArkOS libs at runtime, never nix store paths.
        "-DCMAKE_SKIP_RPATH=ON"
        "-DCMAKE_SKIP_INSTALL_RPATH=ON"
        "-DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF"
        "-DCMAKE_INSTALL_RPATH="
        "-DCMAKE_BUILD_TYPE=Release"
        # GCC 15 headers vs ArkOS libstdc++: shim missing ABI symbols.
        "-DDUALTHRUST_CXXABI_SHIM=${../mk/r36s/cxxabi_shim.cpp}"
      ];

      dontPatchELF = true;
      dontStrip = true;
      dontPatchShebangs = true;

      preConfigure = ''
        export NIX_DONT_SET_RPATH=1
        export NIX_NO_SELF_RPATH=1
        export PKG_CONFIG="pkg-config"
        export PKG_CONFIG_SYSROOT_DIR="${arkosSysroot}"
        export PKG_CONFIG_PATH=""
        export PKG_CONFIG_LIBDIR="${arkosSysroot}/usr/lib/aarch64-linux-gnu/pkgconfig:${arkosSysroot}/usr/lib/pkgconfig:${arkosSysroot}/usr/share/pkgconfig"
        pkg-config --modversion sdl2 || { echo "arkos-sysroot: sdl2.pc not found" >&2; exit 1; }
      '';

      meta = with lib; {
        description = "dualthrust for R36S/ArkOS (sysroot-linked aarch64)";
        license = licenses.gpl3Plus;
        platforms = platforms.linux;
        hydraPlatforms = [];
      };
    };

  # PortMaster tree: Dualthrust.sh + dualthrust/ + metadata (copy to /roms/ports/).
  mkDualthrustR36sPortMaster = {
    r36sPkg
  , version
  , pname ? "dualthrust-r36s-portmaster"
  , title ? "Dualthrust"
  , scriptName ? "Dualthrust.sh"
  , portDirName ? "dualthrust"
  , iconSrc ? ../data/icons/hicolor/256x256/apps/dualthrust.png
  }:
    stdenvNoCC.mkDerivation {
      inherit pname version;
      dontUnpack = true;
      dontConfigure = true;
      dontBuild = true;
      dontFixup = true;
      dontPatchShebangs = true;  # device runs ArkOS /bin/bash; no nix store interpreter

      installPhase = ''
        set -euo pipefail
        root="$out"
        gamedir="$root/${portDirName}"
        mkdir -p "$gamedir/conf" "$gamedir/licenses"

        install -m755 "${r36sPkg}/bin/dualthrust" "$gamedir/dualthrust"
        cp -f ${iconSrc} "$root/screenshot.png"
        cp -f ${iconSrc} "$root/cover.png"
        cp -f ${iconSrc} "$gamedir/cover.png"
        cp -f ${../LICENSES/GPL-3.0-or-later.txt} "$gamedir/licenses/GPL-3.0-or-later.txt"

        cat > "$root/${scriptName}" << 'EOF_LAUNCH'
#!/bin/bash
# Dualthrust — PortMaster launcher for R36S / ArkOS

XDG_DATA_HOME=''${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "''${controlfolder}/mod_''${CFW_NAME}.txt" ] && source "''${controlfolder}/mod_''${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/dualthrust"
CONFDIR="$GAMEDIR/conf"

mkdir -p "$CONFDIR"
chmod -R u+rwX "$GAMEDIR" 2>/dev/null || true
cd "$GAMEDIR" || exit 1

if [ -w "$GAMEDIR" ]; then
  > "$GAMEDIR/log.txt" 2>/dev/null && exec > >(tee -a "$GAMEDIR/log.txt") 2>&1 || true
fi

# Config (ship, sound, …) lives next to the port: $CONFDIR/dualthrust/config
export XDG_CONFIG_HOME="$CONFDIR"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

pm_platform_helper "$GAMEDIR/dualthrust" 2>/dev/null || true

# Fullscreen = the 640x480 panel. Engines: triggers / L1 R1 / sticks up.
./dualthrust --fullscreen "$@"
pm_finish 2>/dev/null || true
EOF_LAUNCH
        chmod +x "$root/${scriptName}"

        cat > "$root/port.json" << EOF_JSON
{
  "version": 2,
  "name": "dualthrust.zip",
  "items": ["${scriptName}", "${portDirName}"],
  "items_opt": null,
  "attr": {
    "title": "${title}",
    "desc": "CRT dual-engine cave lander. Left/right engines rotate and thrust; land on pads in a wrapping cave.",
    "inst": "Ready to run. Copy Dualthrust.sh and the dualthrust/ folder into /roms/ports/.",
    "genres": ["action", "arcade"],
    "porter": ["Ingo Ruhnke"],
    "image": {},
    "rtr": true,
    "runtime": null,
    "reqs": [],
    "arch": ["aarch64"]
  }
}
EOF_JSON

        cat > "$root/gameinfo.xml" << EOF_XML
<?xml version="1.0" encoding="utf-8"?>
<gameList>
  <game>
    <path>./${scriptName}</path>
    <name>${title}</name>
    <desc>CRT dual-engine cave lander. Left and right engines rotate and thrust; land on the pads.</desc>
    <developer>Ingo Ruhnke</developer>
    <genre>Arcade</genre>
    <image>./${portDirName}/cover.png</image>
  </game>
</gameList>
EOF_XML

        cat > "$root/README.md" << 'EOF_README'
## Dualthrust (R36S / ArkOS)

Native aarch64 build linked against the ArkOS sysroot (SDL2, GLES2 via SDL's renderer).

### Install
Copy `Dualthrust.sh` and the `dualthrust/` directory to `/roms/ports/`, or put the zip in
`ports/PortMaster/autoinstall/` and open PortMaster once.

### Controls
- **L2 / R2, L1 / R1, or sticks up** — left / right engine
- **Start** — menu (Resume, Fullscreen, New Cave, Ship, Swap Engines, Sound, Quit)
- **A / B** — reset after crash / relight after landing
- **Y** — new cave, **Select** — next ship
EOF_README
      '';

      meta = with lib; {
        description = "PortMaster package of dualthrust for R36S/ArkOS";
        license = licenses.gpl3Plus;
        platforms = platforms.linux;
        hydraPlatforms = [];
      };
    };

  mkDualthrustR36sPortMasterZip = {
    portMasterPkg
  , version
  , pname ? "dualthrust-r36s-portmaster-zip"
  , zipName ? "dualthrust.zip"
  }:
    stdenvNoCC.mkDerivation {
      inherit pname version;
      dontUnpack = true;
      dontConfigure = true;
      dontBuild = true;
      dontFixup = true;
      dontPatchShebangs = true;
      nativeBuildInputs = [ zip ];
      installPhase = ''
        set -euo pipefail
        mkdir -p "$out"
        ( cd "${portMasterPkg}" && zip -r -9 "$out/${zipName}" . )
      '';
      meta = with lib; {
        description = "PortMaster autoinstall zip of dualthrust for R36S/ArkOS";
        license = licenses.gpl3Plus;
        platforms = platforms.linux;
        hydraPlatforms = [];
      };
    };
in
{
  inherit arkosSysroot mkDualthrustR36s mkDualthrustR36sPortMaster mkDualthrustR36sPortMasterZip;
}
