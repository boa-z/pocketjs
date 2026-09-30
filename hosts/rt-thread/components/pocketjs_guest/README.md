# RT-Thread QuickJS guest

QuickJS-ng 0.14.0, revision 3c051980ab7e783dfbfb1c70c014ce5e05ecf24c.
The D13x preparation tool verifies the tarball and quickjs.c SHA256 before
applying immutable-buffer and freestanding portability patches. Cache and
generated vendor source stay under .pocket-build. The upstream LICENSE is
preserved there. No quickjs-libc, pthreads, Atomics, host file APIs or module
loader are linked. Date currently reports tick uptime, not calendar time.

The port supplies pjs_host_alloc/free; D13x uses PSRAM_SW for QuickJS allocations,
guest metadata, rejection tracking and UI bridge metadata. There is no SRAM
fallback. The QuickJS memory limit covers the runtime heap; host metadata and
the caller's thread stack are additional allocations. The reference smoke
application selects 512 KiB per guest, a 64 KiB JS stack limit and a separate
128 KiB PSRAM worker stack. The worker is resident; per-guest leak checks compare
allocation counts with their own pre-create baseline.

Each eval/frame starts a cooperative execution budget: at most 10,000 interrupt
polls or one second of RT ticks, and at most 256 Promise jobs. Interrupt or
budget exhaustion makes the realm unusable until destroyed. Ordinary JS errors,
stack overflow and JS heap-limit failures can recover on a later eval. This is
not a preemptive time limit on native methods or a security sandbox.

`pocketjs_guest_eval_with_timeout` accepts **1..60000 ms** for one evaluation,
including its Promise drain. This lets a host admit a package whose cold parse
and initialization exceed the default second. The **10000-poll and 256-job
limits remain active**. Subsequent calls to `pocketjs_guest_eval` or
`pocketjs_guest_frame` use the default second; the override does not persist.
Zero and out-of-range budgets return `-RT_EINVAL` without executing JavaScript.
On interruption, the log reports the cause (`requested`, `poll-limit`, or
`wall-clock`), elapsed milliseconds, selected budget, and consumed polls.

One owner thread performs creation, surface installation, eval, frames, stats
and destruction. Only interrupt may be called from another thread. Destruction
must not race with interruption. Surfaces reserve names once per realm.

Run tools/test-guest-host.py from ports/artinchip-d13x, or use its absolute path.
The runner compiles the same application pjs_js.c used by the board. --ui also
links real Rust retained UI and RGB565 libraries, simulating only framebuffer
I/O; Rust uses its std host allocator for that test. Native GCC, stable Rust and
the matching GNU target are required. Host passes do not prove RV32 stack use,
PSRAM placement, cache coherence or LCD scanout.

The runner also executes `tools/tests/guest-budget.c` with a controlled tick
source. It checks the default deadline, extended evaluation, restoration of
frame/eval deadlines, invalid budgets, polling with a stopped clock, explicit
cancellation, exhausted-realm rejection, and allocation cleanup.
