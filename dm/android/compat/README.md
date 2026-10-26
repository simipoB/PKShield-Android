# Darwin stand-in headers for the Android host

The host (`dm/host`, `dm/gfx`) is written against Darwin. On Android these headers stand in for the Darwin ones it includes, so the sources build without per-file `#ifdef`s:
monotonic time for `mach_absolute_time`, `process_vm_readv` for `vm_read_overwrite`, POSIX semaphores and a timer thread for the dispatch calls, `malloc_usable_size` for `malloc_size`,
and failures (`KERN_FAILURE`) for what Android has no equivalent of (Mach thread suspension, task info): the host's diagnostics that use them then report nothing instead of crashing.
`compat.c` holds the out-of-line parts. Only `dm/android/Makefile.host` puts this folder on the include path.
