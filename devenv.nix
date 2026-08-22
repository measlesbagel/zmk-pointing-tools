{ pkgs, inputs, lib, ... }:

let
  zephyr-nix = inputs.zephyr-nix.packages.${pkgs.stdenv.system};

  # Some Zephyr host tools dynamically load libatomic on Linux. Keep the
  # workaround narrow instead of exposing the compiler's entire library tree.
  libatomic = pkgs.runCommand "zmk-libatomic" { } ''
    mkdir -p "$out/lib"
    cp -d ${pkgs.stdenv.cc.cc.lib}/lib/libatomic.so* "$out/lib/"
  '';

  # Python environment for Zephyr's compliance script. check_compliance.py
  # imports these at the top level even when only a subset of checks runs;
  # junitparser's JUnitXml support additionally needs lxml. See
  # docs/quality.md for the check subset and why it is limited.
  compliancePython = pkgs.python3.withPackages (ps: with ps; [
    unidiff
    yamllint
    junitparser
    lxml
    python-magic
    west
  ]);

  # check_compliance.py hardcodes "clang-format-diff.py" on PATH; the nix
  # clang-tools package ships the same script as "clang-format-diff". Expose
  # the name the script expects.
  clangFormatDiffPy = pkgs.runCommand "clang-format-diff-py" { } ''
    mkdir -p "$out/bin"
    ln -s ${pkgs.clang-tools}/bin/clang-format-diff "$out/bin/clang-format-diff.py"
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
    pkgs.clang-tools
    pkgs.cppcheck
    pkgs.python3Packages.lizard
    compliancePython
    clangFormatDiffPy
  ];

  processes.tuner.exec = "python -m http.server 8787 --directory web";

  tasks = {
    "c:format" = {
      description = "Apply clang-format to C sources";
      exec = "find src include host -type f \\( -name '*.c' -o -name '*.h' \\) -print0 | xargs -0 clang-format -i";
    };

    "c:format:check" = {
      description = "Check C source formatting";
      exec = "find src include host -type f \\( -name '*.c' -o -name '*.h' \\) -print0 | xargs -0 clang-format --dry-run --Werror";
    };

    "c:complexity:check" = {
      description = "Check C cyclomatic complexity gates";
      exec = "lizard -l c -C 15 -w src include && lizard -l c -C 15 -w host";
    };

    "host:test:asan" = {
      description = "Build and run host tests with ASan/UBSan";
      exec = "cmake -S host -B build/host-asan -G Ninja -DZPT_ENABLE_SANITIZERS=ON && cmake --build build/host-asan && ctest --test-dir build/host-asan --output-on-failure";
    };

    "c:tidy:check" = {
      description = "Run clang-tidy over the host compile database";
      exec = "cmake -S host -B build/host -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build/host && run-clang-tidy -p build/host";
    };

    "c:cppcheck:check" = {
      description = "Run cppcheck over C sources";
      exec = ''
        suppressions=$(grep -vE '^[[:space:]]*(#|$)' .cppcheck-suppressions | sed 's/^/--suppress=/' | tr '\n' ' ')
        cppcheck --enable=warning,performance,portability --std=c11 \
          -I include --include=tooling/cppcheck/preinclude.h \
          --inline-suppr --suppress=missingIncludeSystem \
          $suppressions \
          --error-exitcode=1 src include host
      '';
    };

    "zephyr:compliance:check" = {
      description = "Run the module-safe Zephyr compliance checks over HEAD~1..HEAD (see docs/quality.md)";
      exec = ''
        ${compliancePython}/bin/python3 zephyr/scripts/ci/check_compliance.py \
          -m ClangFormat -m DevicetreeBindings -m Nits -m YAMLLint \
          -m GitDiffCheck -m TextEncoding -m BinaryFiles
      '';
    };

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
      after = [
        "c:format:check"
        "c:complexity:check"
        "c:tidy:check"
        "c:cppcheck:check"
        "javascript:check"
        "javascript:test"
        "host:test"
      ];
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
