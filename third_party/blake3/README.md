# BLAKE3 (vendored)

The official BLAKE3 C implementation, version **1.3.1**, unmodified.

- **Source:** Ubuntu 24.04 package `librust-blake3-dev` 1.3.1-1 (the `c/`
  directory of the upstream `blake3` crate), taken from Ubuntu's signed
  archive. Direct downloads from GitHub were not available in the build
  environment.
- **Upstream:** <https://github.com/BLAKE3-team/BLAKE3>
- **Licence:** CC0-1.0 or Apache-2.0, see `LICENSE`.
- **Integrity:** `SHA256SUMS` lists the hash of every vendored file. Check it
  with `sha256sum -c SHA256SUMS`.
- **Files:** only the C sources (portable plus SSE2/SSE4.1/AVX2/AVX-512/NEON
  intrinsics), not the assembly versions, the tests or the examples.

To update, replace the files with a newer upstream release, regenerate
`SHA256SUMS`, and record the change in `KNOWLEDGE.md`.
