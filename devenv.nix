{ pkgs, inputs, lib, ... }:

let
  zephyr-nix = inputs.zephyr-nix.packages.${pkgs.stdenv.system};

  # Some Zephyr host tools dynamically load libatomic on Linux. Keep the
  # workaround narrow instead of exposing the compiler's entire library tree.
  libatomic = pkgs.runCommand "zmk-libatomic" { } ''
    mkdir -p "$out/lib"
    cp -d ${pkgs.stdenv.cc.cc.lib}/lib/libatomic.so* "$out/lib/"
  '';
in
{
  packages = [
    pkgs.nodejs
    pkgs.python3
    pkgs.shellcheck
    pkgs.gcc
    pkgs.cmake
    pkgs.ninja
  ];

  processes.tuner.exec = "python -m http.server 8787 --directory web";

  tasks = {
    "javascript:check" = {
      description = "Check browser and host JavaScript syntax";
      exec = "npm run check";
    };

    "javascript:test" = {
      description = "Run browser and host JavaScript tests";
      exec = "npm test";
    };
    "host:configure" = {
      description = "Configure native processor tools";
      exec = "cmake -S host -B \"$DEVENV_STATE/host-build\" -G Ninja";
    };

    "host:build" = {
      description = "Build native processor tools";
      exec = "cmake --build \"$DEVENV_STATE/host-build\"";
      after = [ "host:configure" ];
    };

    "host:test" = {
      description = "Run native processor and trace replay tests";
      exec = "ctest --test-dir \"$DEVENV_STATE/host-build\" --output-on-failure";
      after = [ "host:build" ];
    };

    "repository:check" = {
      description = "Run repository checks";
      exec = "git diff --check";
      after = [ "javascript:check" "javascript:test" "host:test" ];
      before = [ "devenv:enterTest" ];
    };
  };

  # Opt-in Zephyr toolchain for the repository-owned west workspace in
  # config/west.yml. Only activated with `devenv shell -P firmware`; the
  # default shell stays host-only and never downloads the SDK.
  profiles."firmware" = {
    module = {
      packages = [
        zephyr-nix.pythonEnv
        (zephyr-nix.sdk-0_16.override {
          targets = [ "arm-zephyr-eabi" ];
        })
        pkgs.dtc
      ];

      env = {
        PYTHONPATH = "${zephyr-nix.pythonEnv}/${zephyr-nix.pythonEnv.sitePackages}";
      } // lib.optionalAttrs pkgs.stdenv.isLinux {
        LD_LIBRARY_PATH = "${libatomic}/lib";
      };
    };
  };
}
