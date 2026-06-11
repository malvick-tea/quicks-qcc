# module: status

**Responsibility:** define `qcc_status`, the single result type returned by every
fallible function in qcc, and `qcc_status_str` to name a status for logs/tests.

**Public interface:** `status/status.h` (`qcc_status`, `qcc_status_str`).

**Key invariants:** `QCC_OK == 0`; no failure code is 0; codes are distinct.
Relied on across the codebase per
[error-handling.md](../../../Quicks-Meta/docs/standards/error-handling.md).

**Dependencies:** none.
