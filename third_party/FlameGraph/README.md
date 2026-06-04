# Vendored FlameGraph

`stackcollapse-perf.pl` and `flamegraph.pl` are vendored verbatim from
Brendan Gregg's FlameGraph project so the M11 flamegraph pipeline
(`scripts/bench_flamegraph.sh`) runs with no network access at bench time.

- Upstream: https://github.com/brendangregg/FlameGraph
- Pinned commit: `41fee1f99f9276008b7cd112fca19dc3ea84ac32`
- License: Common Development and Distribution License (CDDL 1.0), see
  `LICENSE` in this directory (upstream `docs/cddl1.txt`).

Only the two scripts the pipeline needs are vendored, not the full repo.
To update, re-copy both scripts + `docs/cddl1.txt` from the pinned (or a
newer, deliberately re-pinned) upstream commit and update this file.
