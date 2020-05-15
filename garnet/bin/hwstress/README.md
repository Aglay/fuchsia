# `hwstress`

Tool for exercising hardware components, such as CPU and RAM, to try and detect
faulty hardware.

Example usage:

```sh
hwstress
```

See also:

*  `loadgen` (`zircon/system/uapp/loadgen`) which has many threads performing
   cycles of idle / CPU work, useful for exercising the kernel's scheduler.

*  `kstress` (`zircon/system/uapp/kstress`) which is designed to stress test
   the kernel itself.
