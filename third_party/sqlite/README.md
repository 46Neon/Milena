# SQLite amalgamation vendored by Milena

- Upstream: https://www.sqlite.org/
- Source artifact: https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
- SQLite release: 3.50.4 (`SQLITE_VERSION_NUMBER` 3050004)
- SHA-256 of the downloaded ZIP: `1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032`
- Vendored files: upstream `sqlite3.c` and `sqlite3.h` from `sqlite-amalgamation-3500400/`.
- License: SQLite's core is dedicated to the public domain. See https://www.sqlite.org/copyright.html. SQLite reports that a separate license is not required; the upstream source contains its copyright/public-domain notices.

The library is built as part of Milena's native C executable, not loaded dynamically. The intended build defines `SQLITE_THREADSAFE=1`, `SQLITE_DQS=0`, and `SQLITE_OMIT_LOAD_EXTENSION`. No Python, R, or external SQLite package is required to build a release.

The release build intentionally excludes extension loading, and the Milena backend rejects PRAGMA and ATTACH/DETACH. Keep this upstream version and digest in sync if the amalgamation is updated; retain this provenance record and include the upstream license notice in binary packages.
