# Testing and CI Contract

## Required local checks

```sh
make clean all
make test
make analyze
make sanitize
```

CI compiles with GCC and Clang, promotes warnings to errors, and runs unit and integration tests with both compilers. The sanitizer job rebuilds and runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer.

## Test layers

- **Unit tests:** isolated index candidate generation and snapshot store read/write/rejection behavior.
- **Integration tests:** CLI behavior over a temporary filesystem, content-index fallback, concurrent daemon clients, and a partial-request client that must not block ping.
- **Sanitizers:** detect supported classes of memory-safety and undefined-behavior defects on exercised paths.
- **Static analysis:** GCC `-fanalyzer` diagnostics over production translation units.

## Rules for adding features

1. Add a focused unit test for each new algorithm or format invariant.
2. Add an integration regression for each user-visible behavior or fixed bug.
3. Test malformed and truncated persistent input before trusting it.
4. Keep tests deterministic, self-contained, and independent of network access or personal filesystem contents.
5. Do not suppress a failing test by weakening its assertion. Document intentional limitations and test them explicitly.
6. CI status is authoritative only for checks it executes. Passing CI does not imply daemon correctness, continuous indexing, large-scale performance, or race-free filesystem snapshots.

## Current coverage gaps

Dedicated tests are still needed for overlapping roots; root and child symlink policy; unreadable files and directories; traversal error reporting; binary-file policy and chunk boundaries; read failures; write/rename/fsync failures; malformed snapshot fuzzing; stale temporary files; worker saturation and shutdown-under-load; and large-index performance. Newline-containing paths are covered with NUL output in integration tests, but JSON output is not implemented.
