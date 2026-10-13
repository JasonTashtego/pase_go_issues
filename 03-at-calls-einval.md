# 3. Every `*at` call fails with EINVAL for a real directory descriptor

**Status:** reproduced (single thread, deterministic).

## Symptom
`openat`, `fstatat`, `mkdirat`, `unlinkat`, `renameat`, `symlinkat`, `readlinkat`, `linkat`, `fchmodat` and `fchownat` all fail with `EINVAL` when the directory argument is a real directory descriptor, with any flags and however the directory was opened. Only `AT_FDCWD` works. The header values (`AT_FDCWD` = -2, `AT_REMOVEDIR` = 1, `AT_SYMLINK_NOFOLLOW` = 1, `O_NOFOLLOW` = 0x1000000, `O_DIRECTORY` = 0x80000, `O_CLOEXEC` = 0x800000) match the ones Go uses.

## Reproducer
`src/at_probe.c`: `gcc-12 -maix64 -O1 -o at_probe at_probe.c`, run `./at_probe`. It prints the header flag values next to Go's, then runs each call with both sets and prints `ok` or `errno N ...` per line. Expected: `errno 22` for each call with a directory descriptor.

## Effect
Go's `os.Root` and the `*at`-based `RemoveAll` cannot be used. The port selects the path-based versions, which are not safe against a directory being renamed or replaced while in use.
