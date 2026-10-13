# 5. Users and groups: no /etc/passwd or /etc/group; `getgrset` returns a static buffer

**Status:** behaviour observed with probes. Some of it is a difference from AIX documentation rather than a defect.

## Findings
- There is no `/etc/passwd` or `/etc/group`; users and groups come from IBM i user profiles.
- `getgrset(user)` returns the group ids of a user as text (for example `"102,103"`). AIX documents that the returned pointer must be freed. On PASE it is a **static buffer** that must not be freed. A call takes about 8 ms.
- `getgrouplist` does not exist.
- The port's notes also record that `getgrset` is not thread-safe. *Unverified*: no probe in this directory demonstrates it.

## Reproducers
- `src/getgrset_probe.c`: `gcc-12 -maix64 -o getgrset_probe getgrset_probe.c`; `./getgrset_probe USER [iterations [mode]]`. Mode 0 calls and frees each time, 1 never frees (memory growth shows whether the result is allocated per call), 2 calls twice and compares the pointers.
- `src/group_probe.c`: `gcc-12 -maix64 -o group_probe group_probe.c` (add `-ldl` if the link fails); `./group_probe [USER]`. Prints which user and group files exist, the process ids, the results of `getpwuid`, `getpwnam`, `getgrgid`, `getgrnam` and `getgroups`, which of `getgrset`, `getgrouplist`, `setgrent`, `getgrent`, `endgrent` exist and what they return, and walks the whole group database. Run it once per profile of interest.
