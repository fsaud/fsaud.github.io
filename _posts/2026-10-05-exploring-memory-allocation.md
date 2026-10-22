---
layout: post
title: "Does every malloc call ask Linux for memory? A small experiment with glibc"
date: 2026-10-05 12:00:00 +0500
categories: [cpp, memory]
---

When you call `malloc(16)`, does your program immediately ask Linux for
16 more bytes? And when you call `free`, does that memory go straight
back to the kernel?

Let's find out by watching a few allocations in GDB. We will follow
chunks as they are allocated, freed, and reused—and see why a library
call does not necessarily mean a system call.

## Why start with malloc?

If you write C++, you might usually reach for `new` rather than `malloc`.
In GNU libstdc++, the default, ordinary `::operator new` calls `malloc`,
with additional allocation-failure handling; the corresponding
`::operator delete` calls `free`. This is an implementation choice,
rather than a C++ language requirement. Custom replacements and
over-aligned allocations can take different paths.

On Linux systems that use glibc, `malloc` and `free` normally use glibc's
ptmalloc-based allocator. Alternatives such as TCMalloc can replace it
through linking or `LD_PRELOAD`. Here, we will use glibc's allocator.

## The question

**Can a small allocation be satisfied using memory the allocator already
has, without requesting more from the kernel?**

`malloc` is a library function. It can reuse freed chunks or split space
it already manages, so not every call results in a system call. This
post explores those paths with tcache caching disabled.

Across the series, we will build on these experiments to understand
allocator internals and tunable parameters, and how their tradeoffs
relate to the service we are designing.

## Start with the basics

You might already be familiar with the virtual memory layout of a Linux
process on x86-64:

![Linux x86-64 process virtual memory layout]({{ '/assets/images/linux-x86-64-memory-layout.png' | relative_url }})

For a single-threaded application, glibc normally uses the **main arena**,
which manages the region marked **heap** in this layout. This region can
grow through `brk`/`sbrk`. Some allocations, particularly larger ones, may
instead use separate `mmap` mappings, so not every allocation lives
in that heap region. `sbrk` is a library interface that adjusts the program
break through the underlying `brk` system call.

Before tracing an allocation, let's establish a few terms.

## Arena, heap, and chunk

**Arena:** Allocator state that manages chunks, free lists, and backing
memory. It includes bookkeeping and a lock; it is not the heap region itself.

glibc distinguishes between the main arena and other arenas:

- **Main arena:** The initial arena, normally used by a single-threaded
  application. It manages the process heap shown in the memory layout
  above and remains available when additional threads are created.
- **Other arenas (non-main arenas):** Additional arenas that glibc can
  create to reduce contention between threads. Each has its own
  bookkeeping and lock and manages heaps obtained through `mmap`.
  Threads can share an arena; there is no fixed one-arena-per-thread rule.

We will explore how other arenas are created and used later in this
series through multithreaded experiments.

**Heap:** A region of memory divided into chunks. The main arena normally
uses the process heap; additional arenas can manage multiple mapped heaps.

**Chunk:** An allocator-managed block containing metadata and space for
application data. `malloc` returns a pointer to the data, not the chunk header.

## Where reusable memory is tracked

An arena tracks reusable space through the following structures. Tcache
is included for context, but belongs to a thread rather than an arena:

- **Top chunk:** Free space at the end of the arena's current heap. It can
  be split to satisfy allocations and extended when more memory is needed.
- **Fastbins:** Lists for small freed chunks, grouped by chunk size.
  Adjacent chunks are not immediately merged; consolidation is deferred.
- **Small bins:** Doubly linked free lists, each holding one chunk size.
- **Large bins:** Free lists covering ranges of larger chunk sizes,
  ordered by size to help select a suitable chunk.
- **Unsorted list (unsorted bin):** A staging list for freed or split
  chunks before reuse or placement in a size-specific bin.
- **Tcache (thread cache):** A separate per-thread cache of reusable chunks,
  rather than part of an arena's bin arrays. It can serve allocations before
  the arena's bins are consulted, including in a single-threaded program.
  We will explore it in the next post in this series.

For this post, we will run our experiments with tcache disabled so we can
focus on the arena's allocation behavior. Setting
`glibc.malloc.tcache_count=0` through `GLIBC_TUNABLES` disables tcache
for the program being launched.

With these terms in place, let's begin with a small program.

### Experiment environment

The screenshots below come from this environment. Sizes and allocator
paths discussed here apply to this setup; other glibc versions and
settings can behave differently:

```yaml
Distribution: AlmaLinux 9.6
Kernel: 6.18.40.1-microsoft-standard-WSL2
Architecture / ABI: x86-64, 64-bit executable
glibc package: glibc-2.34-275.el9_8.x86_64
Matching glibc source revision: glibc-2.34-275.el9_8.src.rpm (glibc 2.34 with distribution patches)
```

## The program

Before running it, make a prediction: will requests of 15 and 16 bytes
reuse the same chunk? What about 120 and 128 bytes?

Save the following as `main.cpp`. The final checkpoint gives us a place
to pause before the process exits.

```cpp
#include <stdlib.h>

int main()
{
    void* p = malloc(15);
    void* q = malloc(31);
    void* r = malloc(120);
    if (!p || !q || !r) {
        return 1;
    }

    free(p);
    free(q);
    free(r);
    p = q = r = nullptr;

    p = malloc(16);
    q = malloc(32);
    r = malloc(128);

    volatile int checkpoint = 0; // Set a breakpoint here.
    return checkpoint;
}
```

We will use GEF, an extension for GDB, to debug this program and experiment
with glibc's ptmalloc allocator. By stepping through the allocations and
frees and inspecting chunks, bins, and the top chunk, we will explore how
the program's requests change the allocator's internal state and when it
reuses memory it already manages.

## Build and start GDB

With GDB and GEF installed, build a debug executable and open it:

```bash
g++ -std=c++17 -O0 -g -fno-pie -no-pie main.cpp -o allocation-demo
gdb ./allocation-demo
```

### Disable tcache before running

Before running the program in GDB, set `GLIBC_TUNABLES` as shown below
to disable tcache caching for this experiment:

![Disabling tcache in GDB with GEF]({{ '/assets/images/disable-tcache.png' | relative_url }})

```gdb
set environment GLIBC_TUNABLES glibc.malloc.tcache_count=0
break main
run
```

Enter these commands at the GDB or GEF prompt. The environment setting
must be applied before starting the program because glibc reads the
tunable during startup. If the program is already running, restart it
with `run` for the setting to take effect.

Disabling caching does not necessarily remove tcache's bookkeeping
allocation. In the glibc 2.34 environment used here, glibc can still
allocate the per-thread `tcache_perthread_struct`, which appears as a
`0x290`-byte chunk on x86-64. Seeing this chunk in `heap chunks` does
not mean caching is enabled: `tcache_count=0` prevents freed chunks from
being stored in tcache, while its bookkeeping structure can still exist.

### Load matching glibc debug symbols

GEF can inspect allocator state more reliably with matching glibc debug
symbols. On the AlmaLinux system used here, install them with:

```bash
sudo dnf debuginfo-install glibc-2.34-275.el9_8.x86_64
```

If the subcommand is unavailable, install `dnf-plugins-core` first. Use
the version matching your installed glibc, then start a fresh GDB session.
Without those symbols, GEF may fall back to the start of the `[heap]`
mapping. In this environment, startup storage for the tunables string
can precede the malloc chunks and confuse that fallback heap walk.

At the breakpoint on `main`, the highlighted allocation line has not
executed yet. Use `next` (or `n`) to execute it before inspecting its
result. The same rule applies when stepping over `free`.

## Inspect the main arena

Use `heap arenas` in GEF to display the main arena, the allocator
structure that tracks this heap's state, including its fastbins and
top chunk. The command shows a summary; the bins themselves can be
inspected separately with `heap bins`.

![GEF displaying the main arena at an early checkpoint]({{ '/assets/images/main-arena.png' | relative_url }})

At the early checkpoint shown here, the displayed top pointer is
`top=0x0`, and the memory counters are `mem=0` and `mempeak=0`.
The main arena structure exists, but these fields have not yet been
initialized by allocation activity. This is the state captured here, not
a guarantee for every program: runtime libraries can allocate before
`main`, and other startup code may already have reserved heap space.

## Establish the initial state

Before our allocations, run `heap bins` to inspect the initial state.
All displayed fastbins are empty, as shown below.

![GEF displaying empty heap bins before our allocations]({{ '/assets/images/heap-bins.png' | relative_url }})

Each fastbin index corresponds to a particular chunk size: index 0 holds
`0x20`-byte chunks (32 bytes), index 1 holds `0x30`-byte chunks (48 bytes),
and so on. These are allocator chunk sizes, including overhead and
alignment, not the byte counts passed to `malloc`.

## First allocation

Ready to test the first prediction? Step over `malloc(15)`, then run
`heap chunks` and `p/x p`.
The heap now contains glibc's tcache bookkeeping chunk, the application's
allocation, and the remaining top chunk.

![GEF displaying heap chunks after malloc(15)]({{ '/assets/images/1stchunk-alloc.png' | relative_url }})

Although the application requested 15 bytes, its allocation occupies a
`0x20`-byte chunk (32 bytes), accounting for allocator overhead and
alignment. GEF shows the returned pointer as `0x4052d0` in this run;
this points to the application's usable memory, after the chunk header.
The address can differ between runs.

The tcache bookkeeping chunk can still appear even though caching is
disabled. The top chunk holds the remaining space available for future
allocations.

## All allocations

After stepping over the first three `malloc` calls and before freeing
anything, run `heap chunks`. GEF displays the tcache bookkeeping chunk,
the three application allocations, and the remaining top chunk:

![GEF displaying all three allocations, tcache bookkeeping, and the top chunk]({{ '/assets/images/all-allocated.png' | relative_url }})

The pointer values and chunk sizes in this run are:

| Allocation | Returned pointer | Chunk size |
|---|---|---|
| `p = malloc(15)` | `0x4052d0` | `0x20` (32 bytes) |
| `q = malloc(31)` | `0x4052f0` | `0x30` (48 bytes) |
| `r = malloc(120)` | `0x405320` | `0x80` (128 bytes) |

These chunk sizes account for allocator overhead and alignment, so they
are larger than the requested byte counts. GEF's displayed addresses
match the application pointers printed by `p/x p`, `p/x q`, and `p/x r`;
they point to usable memory after each chunk's header.

The `0x290`-byte chunk at `0x405040` is glibc's tcache bookkeeping
allocation. The remaining top chunk is displayed at `0x4053a0` with
size `0x21c70`. It provides space for subsequent allocations.

### Why do the chunk sizes differ from the requests?

For these small requests on this setup, glibc adds an effective 8-byte
size overhead, rounds up to 16-byte alignment, and enforces a minimum
chunk size of 32 bytes:

```text
chunk size = max(32, round_up(request + 8, 16))

15 bytes  → 0x20     16 bytes  → 0x20
31 bytes  → 0x30     32 bytes  → 0x30
120 bytes → 0x80     128 bytes → 0x90
```

The pointer is 16 bytes after the chunk's header address, but the
previous-size word in the following chunk can be used as payload while
this chunk is allocated. That is why the effective size overhead here
is 8 bytes rather than a flat 16 bytes.

### Reading GEF's chunk flags

In this GEF display, green flag names indicate set bits and red names
indicate unset bits. Thus `PREV_INUSE` is set, while `IS_MMAPPED` and
`NON_MAIN_ARENA` are unset for these main-arena chunks.

- **`PREV_INUSE`:** The preceding physical chunk is treated as in use.
  This describes the previous chunk, not the current one. Chunks held
  in fastbins or tcache remain marked in use for consolidation purposes.
- **`IS_MMAPPED`:** The chunk was allocated through a separate `mmap`
  mapping, outside an arena's normal heap chunks.
- **`NON_MAIN_ARENA`:** The chunk belongs to an arena other than the
  main arena. This differs from `IS_MMAPPED`: non-main arenas can use
  mapped heaps without setting `IS_MMAPPED` on their individual chunks.

## After freeing

Will `free` return these small blocks to Linux? Let's inspect the bins
before making another allocation.

After stepping over `free(p)`, `free(q)`, and `free(r)`, run
`heap bins fast` to inspect the freed chunks:

![GEF displaying the three freed chunks in fastbins]({{ '/assets/images/all-freed.png' | relative_url }})

The three chunks appear in fastbin indexes 0, 1, and 6 respectively:

| Freed pointer | Chunk size | Fastbin index |
|---|---|---|
| `p` | `0x20` (32 bytes) | 0 |
| `q` | `0x30` (48 bytes) | 1 |
| `r` | `0x80` (128 bytes) | 6 |

Their memory remains available to the allocator for reuse. In this run,
the chunks remain separate rather than immediately merging with adjacent
free space: fastbins defer consolidation. The indexes reflect chunk
sizes, not the order in which the chunks were freed.

## Reusing a fastbin chunk

Now test the prediction: does changing the requested size prevent reuse?

Now step over `p = malloc(16)` and inspect `heap bins fast` and `p/x p`:

![GEF showing fastbin index 0 emptied and its chunk returned by malloc(16)]({{ '/assets/images/first-remalloc.png' | relative_url }})

GEF shows `p = 0x4052d0`, the same address returned by the earlier
`malloc(15)`. Both requests map to the same `0x20`-byte chunk size after
allocator overhead and alignment. With tcache disabled, glibc takes the
available chunk from fastbin index 0 and returns it to the application.

Fastbin index 0 is now empty, while indexes 1 and 6 still hold the chunks
freed from `q` and `r`. This allocation reuses memory already managed by
the allocator; it does not need to grow the heap or request a new mapping
from the kernel.

Next, step over `q = malloc(32)` and inspect `p/x q` and
`heap bins fast`:

![GEF showing malloc(32) reusing the chunk from fastbin index 1]({{ '/assets/images/next-fastbinidx-reused.png' | relative_url }})

The returned pointer is `0x4052f0`, the same address previously returned
by `malloc(31)`. Both 31-byte and 32-byte requests require a `0x30`-byte
chunk (48 bytes), so glibc reuses the freed chunk from fastbin index 1.
That bin is now empty, alongside index 0. Index 6 still contains the
`0x80`-byte chunk freed from `r`.

Again, the allocator satisfies the request from an existing free chunk
without growing the heap or obtaining a new mapping.

## A larger request uses the top chunk

Now step over `r = malloc(128)` and inspect the returned pointer,
fastbins, and heap chunks:

![GEF showing a new chunk allocated from the top for malloc(128)]({{ '/assets/images/new-chunk-allocated-for-bigger-request.png' | relative_url }})

`malloc(128)` requires a `0x90`-byte chunk (144 bytes), accounting for
allocator overhead and alignment. The freed `0x80`-byte chunk in fastbin
index 6 is too small, despite its total size being 128 bytes: that total
includes bookkeeping, so it cannot provide 128 usable bytes.

In this run, the `0x80`-byte chunk remains in the fastbin while the
allocator splits a new allocation from the top chunk, returning
`0x4053a0`. The remaining top chunk is now displayed by GEF at
`0x405430`, an advance of `0x90` bytes. As with the other chunk addresses,
GEF displays the address after the header; the top chunk's header begins
at `0x405420`.

This request uses existing space in the top chunk, so it does not require
the allocator to grow the heap merely because the fastbin chunk is too
small.

## What this demonstrates

**Not every `malloc` or `free` call needs to interact with the kernel.**
The allocator can reuse memory it already manages instead of requesting
more space through `brk` or a new `mmap` mapping. Likewise, `free` can
keep a chunk available for reuse rather than immediately returning its
memory to the kernel.

For eligible small allocations, glibc normally checks tcache first.
With tcache disabled, as in this experiment, a matching fastbin is the
next reuse path for fastbin-sized requests. If no matching chunk is
available, the allocator can use other paths, including splitting the
top chunk, before needing more memory from the kernel.

**For the fastbin reuse shown here, the allocator's chunk size class
matters, not an exact match between requested sizes.** In this experiment, requests of 15 and 16
bytes share the `0x20`-byte chunk class, as do 31 and 32 bytes in the
`0x30`-byte class. Requests of 120 and 128 bytes cross a class boundary:
they require `0x80`-byte and `0x90`-byte chunks respectively.

With tcache disabled, we saw `malloc(16)` and `malloc(32)` reuse chunks
from their matching fastbins. The 128-byte request could not use the
freed `0x80`-byte chunk and was instead served from the top chunk.

The debugger snapshots show allocator state and pointer reuse; they are
not a system-call trace. To measure exactly which calls reach the kernel,
we would also need to trace calls such as `brk`, `mmap`, and `munmap`.

## Next experiment

Next, we will repeat the experiment with tcache enabled. We will inspect
how freed chunks enter the per-thread cache and how later allocations
reuse them, then compare this behavior with the fastbin paths observed
here.

## Source references

For the implementation details behind this walkthrough, see glibc
2.34's [malloc.c](https://github.com/bminor/glibc/blob/glibc-2.34/malloc/malloc.c)
and [arena.c](https://github.com/bminor/glibc/blob/glibc-2.34/malloc/arena.c).
These are upstream references; the distribution source RPM listed above
includes the patches matching the installed package. For the C++ entry
points, see libstdc++'s [operator new](https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/libsupc%2B%2B/new_op.cc)
and [operator delete](https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/libsupc%2B%2B/del_op.cc).
