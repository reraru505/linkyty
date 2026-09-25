# LinKyty (`lin-kitty`)

[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-E95420.svg?logo=linux&logoColor=white)](#system-requirements)
[![Build System](https://img.shields.io/badge/Build%20System-Zig%200.16.0-F7A41D.svg?logo=zig&logoColor=white)](#building)
[![Renderer](https://img.shields.io/badge/Vulkan-1.3-red.svg?logo=vulkan&logoColor=white)](#developer-information)
[![Synchronization](https://img.shields.io/badge/Sync-Proton--Style%20Futex-brightgreen.svg)](#benchmarks)
[![License](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](LICENSE)

**LinKyty** (pronounced *"Lin-kitty"*, like *lickety-split*) is a ruthlessly stripped, high-performance, Linux-native PlayStation 5 runtime. 

It is an opinionated hard fork of [Kyty](https://github.com/InoriRus/Kyty) and [KytyPS5](https://github.com/KytyPS5/KytyPS5), re-engineered specifically for **Steam, Steam Deck, Lutris, and headless CLI runners**.

> [!CAUTION]
> **DISCLAIMER**: This is a vibe slopped project , I am reviewing the things i can , but some code may not be reviewed.

> [!IMPORTANT]
> LinKyty is not affiliated with Sony Interactive Entertainment or PlayStation. The project does not distribute games, firmware, decryption keys, or copyrighted system software. Use only game files and ELFs that you have obtained legally.

---

## ⚡ Why LinKyty?

Upstream Kyty is an impressive emulator, but carries massive cross-platform baggage: tens of thousands of lines of CMake/Ninja scripts, Qt6 GUI launcher dialogs, Windows MSVC wrappers, and heavyweight synchronization abstractions.

LinKyty strips away over **9,600 lines** of legacy bloat and rebuilds the project around a pure Linux gaming philosophy:

1. **Zero GUI Bloat**: Completely nuked Qt6, desktop launchers, and UI dialogs. LinKyty is a lean, headless runner designed to plug directly into Steam, Lutris, or Heroic.
2. **Pure Zig 0.16 Toolchain**: No CMake, no Ninja, no Makefiles. A single, hermetic [`build.zig`](build.zig) orchestrates the entire C++20/C11 compilation pipeline and automatically compiles & embeds Vulkan SPIR-V compute and blit shaders on the fly.
3. **Proton-Style 3-State CAS + Linux Futex Core**: Replaced heavy host `std::mutex` and forced 10ms condition-variable polling loops with a Drepper 3-state atomic CAS state machine (`0 = UNLOCKED`, `1 = LOCKED`, `2 = CONTENDED`), 16-cycle `_mm_pause()` adaptive spin, and direct Linux `SYS_futex` sleep.
4. **Inline Object Fast-Paths**: Mutexes, rwlocks, and condition variables resolve inline with zero global hash-table lockups (~50ns saved per lock/unlock).
5. **No Idle CPU Churn**: Condition variables adaptively sleep without waking 100 times/second per thread when no signals are pending, silencing background CPU waste in worker thread pools.
6. **Single-Platform Source Tree**: The Windows and macOS branches are gone from `src/` and `tests/` - no `KYTY_PLATFORM` switch, no `windows.h`, no Mach thread ports, no `AddVectoredExceptionHandler`. One platform, one code path, nothing dead to reason about. (The only `WIN32` left is the guest ABI: `libRtc` speaks `FILETIME` because the game does.)

---

## 📊 Benchmarks

Measured on Linux x86_64 comparing the original Kyty baseline against LinKyty (`ReleaseFast`):

| Scenario | Original Baseline | LinKyty (`ReleaseFast`) | Performance Gain |
| :--- | :--- | :--- | :--- |
| **Uncontended Lock/Unlock** | **135.9 ns** (7.36 M ops/s) | **12.8 ns** (78.05 M ops/s) | **10.6x faster** |
| **Contended (2 Threads)** | **179.7 ns** (5.57 M ops/s) | **36.3 ns** (27.54 M ops/s) | **5.0x faster** |
| **Contended (4 Threads)** | **262.0 ns** (3.82 M ops/s) | **61.5 ns** (16.25 M ops/s) | **4.3x faster** |
| **Contended (8 Threads)** | **554.6 ns** (1.80 M ops/s) | **44.8 ns** (22.30 M ops/s) | **12.4x faster** |
| **8-Thread Total Run Time** | **277.3 ms** | **22.4 ms** | **91.9% time reduction** |
| **8-Thread Context Switches** | **179 switches** | **3 switches** | **98.3% fewer context switches** |

Run the benchmark suite locally anytime with:
```bash
zig build bench -Doptimize=ReleaseFast
```

### Additional measurements (current tree)

Ryzen 5 3600, idle machine, `ReleaseFast`, from the in-tree suites (`test-locks`, `bench-sync`,
`bench`). Before/after values come from the same scenario on the same machine.

| Scenario | Before | After | Why |
| :--- | ---: | ---: | :--- |
| Region-tracking lock, 12 threads, independent locks | 9.2 ns/op | **1.0 ns/op** | `alignas(64)` on `TrackingSpinLock`: as 8 bytes it shared one cache line with the region bitmaps other threads read without the lock |
| Same lock with concurrent bitmap readers | 18.7 ns/op | **6.6 ns/op** | plus `_mm_pause()`/`yield` backoff where `atomic_signal_fence` used to sit (a compiler barrier, not a pause) |
| Guest mapping protection change, 1024 live mappings | 19.5 us | **2.7 us** | range/mapping updates edit the affected window in place instead of reallocating and rebuilding the whole sorted vector per call |
| Guest mapping fragmentation (alloc/free churn) | 1976 ns/op | **697 ns/op** | same, on the split/merge paths |
| `Thread::SleepMicro(200 us)` | 250 us | **204 us** | kernel timer slack absorbed by sleeping to `deadline - 50 us` and spinning the remainder |
| Eviction policy, time until the frame rate collapses | 420 s | **540 s** | relaxed age floors and smaller collection batches; peak VRAM -750 MiB, mean -326 MiB. Eager collection re-uploads while the old images are still pending deletion, which both slows the frame loop and raises peak usage |

Two results changed the code more than any micro-optimisation:

* **The shader program cache is healthy** - 335,872 lookups, 335,717 hits (99.95%), 155 misses and 160 new permutations in one session. Shader compilation is not a per-frame cost.
* **Sparse guest reads no longer zero twice** - `GuestBackingStore::TryReadSparseBacking` used to `memset` the whole span and then `memcpy` every mapped region over it, so a fully backed read (the usual GPU buffer upload) cleared the destination for nothing. Only the gaps are zeroed now.

---

## 🔬 Profiling & Regression Tooling

`ptrace` is restricted on many hardened setups, so the fork carries its own in-process sampling and
dump tools. All of it is opt-in and off by default.

| Variable | Effect |
| :--- | :--- |
| `LINKYTY_PC_PROFILE=<path>` | `SIGPROF` PC sampler: per-thread samples with the interrupted PC and a caller guess, plus `<path>.names` with host thread names |
| `LINKYTY_DUMP_RWX=<path>` | Dump readable guest mappings (code and data) for offline disassembly; non-readable mappings are skipped |
| `LINKYTY_TRACE_MEMORY=1` | `DBGMEM`/`DBGTOP` device-memory totals and top images from the texture/buffer caches |
| `LINKYTY_TRACE_LOCKS=1` | `DBGLOCK` acquisition counter for the guest memory-operation lock |
| `LINKYTY_TRACE_SHADER_CACHE=1` | `SHADERCACHE` program-cache lookups/hits/misses/permutations every 4096 lookups |
| `LINKYTY_TRACE_END_OF_PIPE=1` | Re-enable the per-signal `EndOfPipe` trace (off by default; one session produced 920k lines) |
| `LINKYTY_DUMP_FAILED_IR=1` | Write the IR of a shader that fails resource tracking to `failed_shader_<hash>.ir.txt` |

Suites and benchmarks (`zig build <step> -Doptimize=ReleaseFast`):

| Step | Coverage |
| :--- | :--- |
| `test` | synchronization unit tests |
| `test-vm` | guest virtual-memory allocation suite |
| `test-shader` | shader recompiler IR regression suite |
| `test-locks` | spin-lock contention suite: own-lock, shared-lock, region-like layouts plus two control scenarios |
| `bench-sync` | mutex, condvar handoff, poll interval, sleep precision |
| `bench` | synchronization suite, including guest memory bookkeeping and mapping-protection scaling |

### Where the emulator actually spends time in gameplay

Profiled in-process during Demon's Souls gameplay:

| Share of all CPU | Site |
| ---: | :--- |
| 72.6-79% | the guest's own `bpe::threading::JobSystemScheduler` busy-wait (13 job workers) |
| ~13.7% | glibc syscall-return PCs (sampler bias - those threads are syscall-bound) |
| **4.48%** | **all emulator code**: memset/backing reads ~1.5%, relocations 0.9%, guest TLS getter 0.35%, audio decode ~0.4%, shader IR ~0.4% |

The GPU sits at 18-20% during the same window and there is no file I/O, so the frame pipeline is
latency-bound rather than compute-bound: the emulator is a small part of the picture and the game's
own workers spin while it waits. Known pressure points that remain:

* **GPU memory on a 6 GB card** - eviction thrashing under pressure, and a hard `failed to create
  image` exit when the budget is exhausted (no sysmem fallback).
* **The 10 ms guest signal-dispatch poll** in `Common::CondVar` (`Wait` becomes a 10 ms
  `pthread_cond_timedwait` loop while the dispatch hook is installed), which bounds how quickly a
  blocked thread notices a pending guest signal.

---

## 🎮 Screenshots

<table align="center">
  <tr>
    <td align="center">
      <strong>Astro Bot</strong><br>
      <img src="docs/screenshots/ps5-01.png" width="300" alt="Astro Bot running in LinKyty">
    </td>
    <td align="center">
      <strong>Dreaming Sarah</strong><br>
      <img src="docs/screenshots/ps5-03.png" width="300" alt="Dreaming Sarah running in LinKyty">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Neptunia ReVerse</strong><br>
      <img src="docs/screenshots/ps5-04.png" width="300" alt="Neptunia ReVerse running in LinKyty">
    </td>
    <td align="center">
      <strong>SILENT HILL: The Short Message</strong><br>
      <img src="docs/screenshots/ps5-05.png" width="300" alt="SILENT HILL: The Short Message running in LinKyty">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Demon's Souls</strong><br>
      <img src="docs/screenshots/ps5-02.png" width="300" alt="Demon's Souls running in LinKyty">
    </td>
    <td align="center">
      <strong>Hellboy</strong><br>
      <img src="docs/screenshots/ps5-06.png" width="300" alt="Hellboy running in LinKyty">
    </td>
  </tr>
</table>

---

## 🛠️ Building

### System Requirements
- **OS**: Modern Linux distribution (Arch, CachyOS, Fedora, Ubuntu/Debian).
- **CPU**: x86_64 processor with AVX2 support.
- **GPU**: Vulkan 1.3 capable GPU with proprietary/Mesa drivers (AMD RADV, NVIDIA, or Intel ANV).

### Dependencies
Install the required development packages from your package manager:

- **Arch Linux / CachyOS**:
  ```bash
  sudo pacman -S zig glslang vulkan-devel sdl3 ffmpeg zydis spirv-tools python
  ```
- **Fedora**:
  ```bash
  sudo dnf install zig glslang vulkan-loader-devel SDL3-devel ffmpeg-devel zydis-devel spirv-tools-devel python3
  ```
- **Ubuntu 24.04+ / Debian**:
  ```bash
  sudo apt install zig glslang-tools libvulkan-dev libsdl3-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev spirv-tools python3
  ```

### Build Steps

1. Clone the repository and initialize submodules:
   ```bash
   git clone --recursive <your-repo-url>
   cd linkyty
   ```

2. Compile with Zig:
   ```bash
   # Optimized Release Build (recommended for gaming)
   zig build -Doptimize=ReleaseFast

   # Debug Build (with full debug symbols and tracing)
   zig build
   ```

The compiled binary will be placed at `zig-out/bin/linkyty`.

3. Run verification tests:
   ```bash
   zig build test
   ```

---

## 🚀 Usage & Launcher Integration

### Command Line
```bash
./zig-out/bin/linkyty --game /path/to/game_directory --fullscreen
```

Run `./zig-out/bin/linkyty --help` to view all available CLI flags (resolution, presentation modes, AMD instruction patching, Vulkan device index, and ETAHen cheat patches).

### Steam / Steam Deck Integration
1. In Steam, click **Games &rarr; Add a Non-Steam Game to My Library...**
2. Point it to your compiled `linkyty` binary.
3. Open its **Properties**, and in **Launch Options**, specify:
   ```bash
   %command% --fullscreen --screen-width 1920 --screen-height 1080 --game "/path/to/game"
   ```

### Lutris Integration
1. Add a new game and select **Custom / Linux Native** as the runner.
2. Under **Game options**:
   - **Executable**: `/path/to/linkyty`
   - **Arguments**: `--fullscreen --game "/path/to/game"`

---

## 📜 Upstream Attribution & License

LinKyty is licensed under the [GNU General Public License v2.0](LICENSE).

LinKyty is a hard fork based on the pioneering work of:
- **Inori** and contributors of the original [Kyty](https://github.com/InoriRus/Kyty) project.
- The [KytyPS5](https://github.com/KytyPS5/KytyPS5) organization contributors.

All original copyrights and licenses are preserved.
