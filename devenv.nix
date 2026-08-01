{ pkgs, ... }:

{
  packages = [
    pkgs.nodejs
    pkgs.python3
    pkgs.shellcheck
  ];

  processes.tuner.exec = "python -m http.server 8787 --directory web";

  tasks = {
    "web:check" = {
      description = "Check browser JavaScript syntax";
      exec = "npm run check";
    };

    "web:test" = {
      description = "Run host protocol parser tests";
      exec = "npm test";
    };

    "repository:check" = {
      description = "Run repository checks";
      exec = "git diff --check";
      after = [ "web:check" "web:test" ];
      before = [ "devenv:enterTest" ];
    };
  };
}
