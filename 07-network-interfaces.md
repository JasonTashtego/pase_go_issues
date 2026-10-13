# 7. Network interface enumeration: no `getifaddrs`; interface ioctls accept only the IP address text as the name

**Status:** reproduced.

## Findings
- No `<ifaddrs.h>` and no `getifaddrs`. `if_nameindex` exists.
- `getkerninfo(KINFO_RT_IFLIST)` works and lists interfaces by line name (`ETHLINE1`, `*LOOPBACK`).
- `ioctl(SIOCGIFMTU)` (and FLAGS, ADDR, NETMASK) with that line name fails with `EINVAL`. The ioctls accept only the interface's IP address text, as `SIOCGIFCONF` returns it (for example `10.150.0.37`).
- The routing table has no hardware address, and no multicast list is available.

## Reproducers
- `src/if_probe.c`: `gcc-12 -maix64 -O1 -o if_probe if_probe.c`; `./if_probe`. Tries `getkerninfo`, `SIOCGIFCONF`, `if_nameindex` and a `getifaddrs` symbol check, and prints each result.
- `src/if_probe2.c`: `gcc-12 -maix64 -O1 -o if_probe2 if_probe2.c`; `./if_probe2`. Decodes the kernel table the way Go does, then runs the per-interface ioctls with the line name and with the IP-address name.

## Workaround in Go
`net/interface_aix.go` retries the MTU ioctl with the interface's first IPv4 address text.
