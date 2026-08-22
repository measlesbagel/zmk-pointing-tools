/* SPDX-License-Identifier: MIT */
/* Pre-included shim for cppcheck runs (see docs/quality.md).
 *
 * cppcheck analyzes the tree without Zephyr's generated headers, so a few
 * build-system macros must be stubbed to keep the preprocessor happy. The
 * real definitions come from Zephyr in the firmware build (build.yml) and
 * the firmware clang-tidy job (issue #76). */
#ifndef ZPT_CPPCHECK_PREINCLUDE_H
#define ZPT_CPPCHECK_PREINCLUDE_H

/* Zephyr's "is this Kconfig option enabled" predicate; undefined options
 * evaluate to false, matching cppcheck's treatment of unknown #if operands. */
#ifndef IS_ENABLED
#define IS_ENABLED(option) (option)
#endif

/* Zephyr's per-device-instance for-each macro; the argument is the macro
 * receiving each instance. The empty expansion means the per-instance
 * bodies (Zephyr glue) are not analyzed by cppcheck; they are covered by
 * the firmware build. */
#ifndef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(macro)
#endif

/* Devicetree "chosen node" presence predicate; stubbed to true so the
 * compile-time #error guard (the chosen node is required) is satisfied,
 * matching the real build. */
#ifndef DT_HAS_CHOSEN
#define DT_HAS_CHOSEN(node) 1
#endif

/* Instance-node indirection and property-presence predicate, used together
 * in preprocessor conditions by the device table. Conditional directives are
 * evaluated while reading the file even inside macro bodies cppcheck never
 * expands, so both must resolve here; every instance is then analyzed in its
 * first declared capability form. The firmware build and unit tests cover
 * every form. */
#ifndef DT_DRV_INST
#define DT_DRV_INST(inst) inst
#endif

#ifndef DT_NODE_HAS_PROP
#define DT_NODE_HAS_PROP(node, prop) 1
#endif

#endif
