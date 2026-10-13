# 6. `getaddrinfo(NULL, "☺", ...)` succeeds with port 0, while `getservbyname` correctly rejects it

**Status:** reproduced (deterministic).

## Reproducers
- `src/getserv_probe.c`: `gcc-12 -maix64 -o getserv_probe getserv_probe.c`; `./getserv_probe`. `getservbyname` for `"☺"` (UTF-8 `e2 98 ba`), `"nosuchservice"` and `""` returns NULL; `http`, `ftp` and `telnet` resolve.
- `src/getaddr_probe.c`: `gcc-12 -maix64 -o getaddr_probe getaddr_probe.c`; `./getaddr_probe`. Calls `getaddrinfo(NULL, service, hints)` with `ai_flags` 0 and `ai_family` `AF_UNSPEC`, for TCP or UDP, exactly as Go's cgo resolver does. For `"☺"` it returns rc=0 and addresses with port 0, where an unknown-service error is expected.

## Effect
Go tests that expect "unknown service" for port `☺` (`net` TestDialError, TestListenError, TestListenPacketError) fail. The port skips those subtests.
