# xemu Performance Research

Potential optimizations for running xemu on low-end hardware (Intel UHD 505 / Celeron).

---

## The Core Problem

xemu uses QEMU's TCG (Tiny Code Generator) which has ~17x overhead translating x86 guest to x86 host. The Xbox's single Pentium III 733MHz is emulated on one host core - this cannot be parallelized.

**Cxbx-Reloaded** runs Xbox code natively (no translation) but is Windows-only.

---

## Techniques from RPCS3 (SPU Scheduling)

RPCS3 deals with similar bottlenecks on Cell SPEs. Key techniques:

| Technique | Description | Benefit |
|-----------|-------------|---------|
| **Wake-Up Delay** | Adds microsecond delays at sync points | CPU catches up, fixes desyncs |
| **Postponed Notifications** | Batches thread wake-ups | Reduces context switches |
| **Lock-Line Reservation** | Skip notifications when data unchanged | Eliminates spurious wake-ups |
| **Futex over Mutexes** | Direct kernel wait vs spin-wait | Lower CPU overhead |
| **Thread Desync** | Staggers identical threads | Reduces lock contention |
| **Exclusive Reservations** | Deduplicates notifications | Less redundant work |

### Applicable to xemu
- Wake-up delay concept for GPU/CPU sync
- Batch notifications instead of per-event
- Futex-based waiting (Linux native)

---

## Memory Access Optimizations

### Fastmem
Use host MMU + signal handlers instead of software TLB lookup.
- PCSX2 got **5-10% gains** from this
- Guest memory access compiles to simple `[base + address]`
- SIGSEGV handler catches MMIO, backpatches code

### Memory Barriers
- Use weakest barrier that maintains correctness
- x86-on-x86 can skip many barriers (strong memory model)
- Barrier coalescing in TCG optimizer

---

## IRQ/Interrupt Handling

### IRQ Coalescing
- Batch interrupt processing instead of one-per-event
- Trade latency for throughput
- Adaptive coalescing based on event rate

### NAPI-style Polling
- Switch from interrupt-per-event to polling under load
- Reduces context switches significantly

---

## Timing/Scheduling Optimizations

### Catch-Up Synchronization
Instead of cycle-by-cycle sync:
1. Run CPU for N cycles
2. Catch up GPU/APU to same point
3. Only sync when components actually interact

SNES emulator (bsnes) reduced syncs from millions/sec to thousands/sec.

### JIT Synchronization
Only synchronize when CPU reads from shared memory region with another component.

### Cooperative Threading
Userland context switches instead of kernel transitions.
- Kernel switch: thousands of cycles
- Cooperative switch: ~12 assembly instructions (x86)

---

## CPU Affinity Strategy (SPITBALL)

**Idea:** Can't parallelize Xbox CPU, but CAN isolate it from overhead.

### Bind xemu to dedicated core
```bash
# Pin xemu to CPU 0
taskset -c 0 /opt/xbox/xemu ...
```

### Bind system services to OTHER cores
```bash
# Move dnsmasq to CPU 1
taskset -c 1 $(pgrep dnsmasq)

# Move X11/Xorg to CPU 1
taskset -c 1 $(pgrep Xorg)

# Move PulseAudio to CPU 1
taskset -c 1 $(pgrep pulseaudio)

# Move all IRQ handling to CPU 1
echo 2 > /proc/irq/default_smp_affinity
for irq in /proc/irq/*/smp_affinity; do
  echo 2 > $irq 2>/dev/null
done
```

### Isolate CPU at boot (kernel parameter)
```
isolcpus=0 nohz_full=0 rcu_nocbs=0
```
This tells Linux to NOT schedule anything on CPU 0 except what you explicitly pin there.

### Real-time priority
```bash
# Give xemu real-time FIFO scheduling
sudo chrt -f 50 taskset -c 0 /opt/xbox/xemu ...
```

### CPU Governor
```bash
# Force max frequency
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

---

## Potential Code Changes to xemu

These would require modifying xemu source:

1. **Implement fastmem** - Host MMU + signal handlers
2. **Add wake-up delay** - Configurable sync delay like RPCS3
3. **IRQ batching** - Coalesce interrupt handling
4. **GPU sync optimization** - Only sync when GPU reads CPU data
5. **TCG tuning** - Block size, chaining aggressiveness

---

## Quick Wins (No Code Changes)

1. **Vulkan renderer** - Lower CPU overhead than OpenGL
2. **Shader caching** - Enable in xemu settings
3. **Lower resolution** - Less GPU work
4. **CPU governor** - Set to performance
5. **CPU affinity** - Pin xemu, move everything else away
6. **Disable compositor** - Raw X11, no desktop effects
7. **Kernel isolcpus** - Reserve core for xemu only

---

## Hardware Reality

The UHD 505 is a Celeron/Pentium with weak single-thread performance. Software optimizations can help but won't overcome fundamental hardware limits.

**Best single-thread CPUs for emulation:**
- AMD Ryzen 7 9800X3D (3D V-Cache helps)
- Intel Core i9-14900K (high clocks)
- Apple M4 (excellent IPC)

---

## Sources

- RPCS3 GitHub PRs: #8485, #12523, #14491, #15934, #17457
- PCSX2 fastmem: PR #5821
- QEMU MTTCG documentation
- Dolphin performance guide
- bsnes cooperative threading design

---

**Last Updated:** 2025-12-17
