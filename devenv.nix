{ pkgs, ... }:

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
}
