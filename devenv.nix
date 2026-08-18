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

  # Python environment for running the module's ztest suites through west
  # twister (the subset of zephyr/scripts/requirements*.txt that the twister
  # core imports unconditionally; see docs/quality.md).
  twisterPython = pkgs.python3.withPackages (ps: with ps; [
    west
    pyyaml
    pykwalify
    pyelftools
    junitparser
    lxml
    natsort
    psutil
    colorama
    ply
    packaging
    anytree
    pytest
  ]);

  # The tidy gates (c:tidy:check, c:firmware:tidy) are tuned against the
  # clang-tidy 22 series. Macro-generated Zephyr/ZMK symbols that newer
  # versions flag as misc-use-internal-linkage carry documented NOLINTs, so
  # the gate also passes on the 21 series (the floating fallback). If a
  # future nixpkgs float introduces new findings on a clean tree: triage
  # them (fix, or a documented NOLINT / check exclusion) — see
  # docs/quality.md.
  clangTidyPinned =
    if pkgs ? llvmPackages_22 then pkgs.llvmPackages_22.clang-tools
    else pkgs.clang-tools;
in
{
  packages = [
    pkgs.nodejs
    pkgs.python3
    pkgs.shellcheck
    pkgs.gcc
    pkgs.cmake
    pkgs.ninja
    clangTidyPinned
    pkgs.cppcheck
    pkgs.python3Packages.lizard
    # twisterPython before compliancePython: `west twister` imports the
    # twister script into the west process itself, so the west binary must
    # come from the environment that carries twister's python deps.
    twisterPython
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
      description = "Run the pinned clang-tidy over the host compile database (any finding fails)";
      exec = "cmake -S host -B build/host -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "
        + "&& cmake --build build/host "
        + "&& python3 tooling/clang-tidy/check_host_tidy.py --db-dir build/host --repo . --clang-tidy \"$(command -v clang-tidy)\"";
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

    # Runs the root .clang-tidy check set over the real firmware compile
    # database (tooling/clang-tidy/); any first-party finding fails. The
    # script is the shared entry point with the CI firmware-tidy job
    # (.github/workflows/firmware-tidy.yml), which runs it under the same
    # firmware profile. Needs the firmware profile for west and the Zephyr
    # SDK: `devenv shell -P firmware`.
    "c:firmware:tidy" = {
      description = "Run the firmware clang-tidy gate (shared with the CI firmware-tidy job); requires the firmware profile";
      exec = "bash tooling/clang-tidy/run_firmware_tidy.sh";
    };

    "zephyr:tests" = {
      description = "Run the module's ztest suites on native_posix via twister";
      exec = ''
        # native_posix builds with the host gcc; the variant env var keeps
        # twister's toolchain verification from requiring the Zephyr SDK
        # (which this profile deliberately does not install). The module is
        # loaded through the ZEPHYR_MODULES env var (read by zephyr_get
        # from the ENV scope; twister's cmake configure has no option to
        # pass arbitrary -D args, and the REMAINDER after -- would reach
        # the test binary, not CMake). Zephyr only runs its module
        # registration script when WEST or ZEPHYR_MODULES is set, and WEST
        # is WEST-NOTFOUND without a west workspace (west topdir is looked
        # up from ZEPHYR_BASE), so the env var also covers CI, where the
        # zephyr tree is fetched without a workspace.
        #
        # Twister is invoked as a plain python script rather than through
        # west: west only registers the twister command once the manifest
        # import chain (config/west.yml -> zmk/app/west.yml) resolves, so
        # the zmk project must be checked out. The script derives
        # ZEPHYR_BASE from its own location and has no west dependency.
        # Its imports come from twisterPython (ordered first in packages).
        export ZEPHYR_TOOLCHAIN_VARIANT=host
        export ZEPHYR_MODULES="$PWD"
        python3 "$PWD/zephyr/scripts/twister" -p native_posix/native/64 -T tests
      '';
    };

    "zephyr:compliance:check" = {
      description = "Run the module-safe Zephyr compliance checks over HEAD~1..HEAD (COMPLIANCE_RANGE overrides the range); see docs/quality.md";
      exec = ''
        range="''${COMPLIANCE_RANGE:-HEAD~1..HEAD}"
        # COMPLIANCE_SCRIPT: the west workspace keeps the script under
        # zephyr/; CI clones the pinned tree to zephyr-tree/ and overrides
        # the path (the repo tracks zephyr/module.yml, so zephyr/ is
        # occupied).
        script="''${COMPLIANCE_SCRIPT:-zephyr/scripts/ci/check_compliance.py}"
        ${compliancePython}/bin/python3 "$script" \
          -c "$range" -m ClangFormat -m DevicetreeBindings -m Nits -m YAMLLint \
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
