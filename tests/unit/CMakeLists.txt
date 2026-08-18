# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zpt_unit_tests C)

# The module under test is provided to the app as a Zephyr module via
# the EXTRA_ZEPHYR_MODULES environment variable (see the zephyr:tests
# devenv task and docs/quality.md); its Kconfig-guarded sources are then
# built exactly as in the firmware.
#
# Both files contribute cases to the single zpt_unit suite (defined in
# test_tuning.c). ztest executes suites and cases in name-sorted linker
# order (SORT_BY_NAME in the ztest linker script), so the shared global
# tuning registry is managed by name: the capacity-fill case is named
# z_capacity_fill so it runs last of all (see test_telemetry.c).
target_sources(app PRIVATE
  test_tuning.c
  test_telemetry.c
)
