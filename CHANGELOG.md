# Changelog

## Unreleased

- Correct the CMake generator guidance: Ninja retains
  `CMAKE_BUILD_TYPE=Release`, while multi-config generators use
  `--config Release` for build, test, and install operations.
- Derive every report count and timing maximum from the final bound trace so
  latest-only supersession cannot disappear from aggregate age evidence.
- Add monotonic trace ordinals, an eleven-field trace CSV schema, and a v2
  fingerprint so equal-time zero-duration outcomes retain scheduler order.
- Read workloads as binary data, reject CTRL-Z and malformed carriage-return
  framing, and fail on I/O errors so Windows cannot silently accept a prefix.

## 0.1.0

- Added four deterministic non-preemptive scheduling policies.
- Added freshness/deadline outcomes, diagnoses, trace CSV, and a versioned SHA-256 fingerprint over every trace field.
- Defined and tested strict per-stream sequence ordering for latest-only scheduling.
- Added CMake install/export support and CTest coverage.
