# Performance investigation: eiows 10.1.0

Measured on 2026-08-09 with Node.js 26.0.0 on an Apple M2 Pro (arm64). Each
server ran in an isolated process on loopback with 100 connections, one
in-flight message per connection and `permessage-deflate` disabled. Unless a
section says otherwise, tables contain medians of three three-second
measurements after one second of warm-up.

CPU is process CPU relative to one logical core: 100% means one fully occupied
core. The CPU-normalized rate is the most useful capacity metric because equal
throughput at lower CPU leaves more time for Engine.IO and application work.
Short-run RSS deltas are allocator- and GC-sensitive; controlled A/B results
and repeated trends carry more weight than one absolute sample.

The comparison uses the 10.1.0 release candidate from this checkout,
`eiows@9.2.0`, and `ws@8.21.3`. The ws process was verified to load
`bufferutil@4.1.0`. `utf-8-validate@6.0.6` is installed and loadable, but Node
26 deliberately selects its built-in `Buffer.isUtf8()` implementation.

## Release outcome

- ARM64 receive processing now uses NEON for frame unmasking and long ASCII
  runs in UTF-8 validation. In the controlled 16 KiB text A/B, server CPU fell
  from 61.5% to 56.7% at unchanged throughput; CPU efficiency improved 8.6%.
- Native receive events are released as soon as their JavaScript values own or
  reference the payload. A controlled 320 KiB text A/B reduced active peak RSS
  from 34.3 MiB to 10.7 MiB (69%) without a material speed or CPU regression.
- `Server` supplies text as a Buffer with `isBinary === false`, after
  which Engine.IO performs its normal synchronous `data.toString()`. This
  avoids the expensive Node-API string conversion for Unicode-heavy input and
  matches the existing Engine.IO configuration and `ws` message semantics.
- The uncompressed send path avoids duplicate conversion and length work,
  caches immutable frame headers and writes header/payload pairs without a
  temporary array or callback wrapper when none is needed.
- eiows 10.1.0 is effectively tied with ws on raw throughput for ASCII-dominant
  receive traffic, is close for mixed Unicode in Engine.IO mode, and generally
  shows lower active RSS in these tests. Version 9.2.0 remains more
  CPU-efficient for 1 KiB and 16 KiB ingress because it owns the native socket;
  that architecture is intentionally not restored in eiows 10.

## Root cause and ARM64 fix

CPU profiles of 16 KiB ingress showed about 42% of visible active samples in
the current native `_consume` path. The bundled protocol parser already had
AVX2 fast paths on x64, but Apple Silicon used scalar unmasking and scalar ASCII
scanning. The release candidate adds AArch64 NEON paths and retains scalar
fallbacks for short data.

Controlled five-iteration A/B, 16 KiB ASCII text:

| Build | Messages/s | Server CPU | k messages/CPU-s |
|:---|---:|---:|---:|
| Scalar ARM64 | 31,178 | 61.5% | 50.7 |
| ARM64 NEON | 31,177 | 56.7% | 55.2 |

Binary efficiency changed by less than 1%, which is expected: binary frames
benefit from NEON unmasking but do not run UTF-8 validation.

## Receive-only text ingress

The handler normalizes text to a JavaScript string, as Engine.IO does. This
puts eiows and ws on the same application-visible representation.

### 10.1.0 versus ws, ASCII fixture

| Payload | Implementation | Messages/s | CPU | k messages/CPU-s | Active peak RSS delta |
|---:|:---|---:|---:|---:|---:|
| 1 KiB | 10.1.0 | 48,464 | 61.6% | 78.7 | 0.2 MiB |
|  | ws | 48,354 | 58.5% | 82.7 | 1.5 MiB |
| 16 KiB | 10.1.0 | 31,049 | 56.5% | 55.0 | 5.3 MiB |
|  | ws | 31,044 | 55.7% | 55.7 | 23.9 MiB |
| 320 KiB | 10.1.0 | 3,959 | 73.7% | 5.37 | 12.8 MiB |
|  | ws | 4,056 | 87.2% | 4.65 | 57.1 MiB |

Raw throughput is tied at 1 KiB and 16 KiB. ws is about 5% more CPU-efficient
at 1 KiB and 1% at 16 KiB. At 320 KiB, 10.1.0 was about 15% more
CPU-efficient, but throughput was 2.4% lower. The large-frame results varied
more between iterations and should not be read as a universal advantage.

### 10.1.0 versus 9.2.0, paired ASCII run

| Payload | Implementation | Messages/s | CPU | k messages/CPU-s | Active peak RSS delta |
|---:|:---|---:|---:|---:|---:|
| 1 KiB | 10.1.0 | 48,464 | 61.6% | 78.7 | 0.2 MiB |
|  | 9.2.0 | 47,806 | 45.8% | 104.4 | 0.0 MiB |
| 16 KiB | 10.1.0 | 31,049 | 56.5% | 55.0 | 5.3 MiB |
|  | 9.2.0 | 31,155 | 52.4% | 59.5 | 1.3 MiB |
| 320 KiB | 10.1.0 | 3,959 | 73.7% | 5.37 | 12.8 MiB |
|  | 9.2.0 | 4,153 | 81.0% | 5.13 | 0.0 MiB |

The native socket ownership in 9.2.0 is still valuable at small and medium
sizes. At 320 KiB, 10.1.0 was slightly more CPU-efficient but processed about
5% fewer messages. Version 9.2.0 is retained only as a historical performance
reference; the production choice must also account for its older Node
integration and maintenance model.

## Default Engine.IO Buffer-backed text mode

`Server` returns a Buffer by default and leaves the one required conversion to
Engine.IO. Existing `wsEngine: eiows.Server` configurations receive this path
without application changes. The wire frame is still text and `isBinary`
remains false. Direct consumers that require the 10.0.1 string behavior can set
`textAsString: true`.

For a mixed Latin/CJK/emoji 16 KiB fixture:

| Implementation/mode | Messages/s | CPU | k messages/CPU-s | Active peak RSS delta |
|:---|---:|---:|---:|---:|
| 10.1.0 legacy native string | 18,561 | 80.8% | 23.0 | 4.4 MiB |
| 10.1.0 default Buffer → string | 18,165 | 57.9% | 31.4 | 4.4 MiB |
| ws Buffer → string | 18,085 | 54.9% | 32.9 | 16.1 MiB |

Moving conversion to Engine.IO cuts 10.1.0 CPU by about 28% for this Unicode
fixture while keeping speed and measured RSS essentially unchanged. The final
gap to ws is about 5% in CPU efficiency and is attributed mainly to full UTF-8
validation after the ASCII fast path.

For ASCII-only 16 KiB input, Buffer mode was within normal noise of direct
string output: 29,895 messages/s at 56.4% CPU versus ws at 30,031 messages/s and
55.3% CPU. It therefore has little downside for Engine.IO while protecting the
Unicode case.

### RSS during Engine.IO conversion

| Payload | Native string RSS delta | Engine.IO conversion RSS delta |
|---:|---:|---:|
| 30 KiB | 4.1 MiB | 5.5 MiB |
| 320 KiB | 8.7 MiB | 16.8 MiB |

At the workload's 30 KiB receive ceiling, Engine.IO conversion added about
1.4 MiB of peak RSS across 100 active connections. The 320 KiB result confirms
that simultaneous Buffer and string lifetimes become more visible at much
larger sizes. RSS may stay at its high-water mark for allocator reuse; that is
not by itself evidence of a leak.

## Echo: receive and send together

Final 10.1.0 candidate versus ws, text frames:

| Payload | Implementation | RTT/s | CPU | k RTT/CPU-s | p99 RTT | Active peak RSS delta |
|---:|:---|---:|---:|---:|---:|---:|
| 1 KiB | 10.1.0 | 33,979 | 76.7% | 44.4 | 5.34 ms | 0.0 MiB |
|  | ws | 33,869 | 75.7% | 44.9 | 5.29 ms | 8.0 MiB |
| 16 KiB | 10.1.0 | 17,077 | 64.0% | 26.6 | 10.77 ms | 0.3 MiB |
|  | ws | 17,182 | 62.7% | 26.8 | 10.91 ms | 11.9 MiB |
| 320 KiB | 10.1.0 | 1,488 | 48.6% | 3.0 | 80.74 ms | 7.5 MiB |
|  | ws | 1,295 | 50.2% | 2.6 | 75.96 ms | 51.5 MiB |

The 1 KiB and 16 KiB cases are effectively tied; ws is about 1% and 0.8% more
CPU-efficient respectively. The 320 KiB samples varied substantially between
iterations, so the apparent throughput lead is not treated as a stable
conclusion. Active RSS was consistently lower for 10.1.0.

## Binary ingress, capped at 30 KiB

These results predate only text-specific NEON ASCII scanning; the retained send
changes do not affect receive-only binary processing.

| Payload | Implementation | Messages/s | CPU | k messages/CPU-s |
|---:|:---|---:|---:|---:|
| 1 KiB | 10.1.0 | 47,381 | 59.9% | 79.1 |
|  | 9.2.0 | 47,363 | 50.9% | 93.4 |
|  | ws | 47,683 | 57.6% | 82.8 |
| 16 KiB | 10.1.0 | 32,476 | 53.0% | 61.3 |
|  | 9.2.0 | 32,422 | 48.6% | 66.8 |
|  | ws | 32,394 | 51.8% | 62.6 |
| 30 KiB | 10.1.0 | 30,389 | 54.3% | 56.0 |
|  | 9.2.0 | 30,375 | 51.4% | 59.1 |
|  | ws | 30,397 | 54.3% | 56.0 |

At 30 KiB, 10.1.0 and ws are effectively identical. Version 9.2.0 used about
three CPU percentage points less at the same speed.

## Application-compressed binary ingress

The source is compressed once with raw DEFLATE before warm-up. Compression CPU
is excluded, WebSocket compression stays off, and the server receives ordinary
binary frames. The largest 215,000-byte source becomes 29,926 bytes on the wire.

| Logical source / wire | Implementation | Messages/s | CPU | k messages/CPU-s |
|---:|:---|---:|---:|---:|
| 1,024 / 228 B | 10.1.0 | 47,116 | 58.9% | 80.2 |
|  | 9.2.0 | 47,098 | 49.6% | 94.8 |
|  | ws | 47,552 | 56.2% | 84.5 |
| 16,384 / 2,429 B | 10.1.0 | 46,141 | 60.3% | 76.5 |
|  | 9.2.0 | 46,096 | 51.2% | 90.2 |
|  | ws | 46,047 | 57.9% | 79.6 |
| 215,000 / 29,926 B | 10.1.0 | 30,915 | 55.1% | 56.4 |
|  | 9.2.0 | 30,874 | 51.4% | 59.9 |
|  | ws | 30,969 | 55.1% | 56.2 |

The receive library does not decompress these frames. Application inflate CPU
and memory must be included in a true end-to-end workload test.

## Broadcast and `sendFrame()`

Each broadcast delivers to 100 clients. A new broadcast starts only after all
write callbacks and client acknowledgements, preventing unbounded buffering.

### Text

| Source | Variant | Deliveries/s | CPU | k deliveries/CPU-s | p99 delivery |
|---:|:---|---:|---:|---:|---:|
| 1 KiB | 10.1.0 `sendFrame` | 94,087 | 86.2% | 109 | 1.31 ms |
|  | 10.1.0 `send` | 91,632 | 90.4% | 101 | 1.34 ms |
|  | 9.2.0 `send` | 95,362 | 81.0% | 118 | 1.31 ms |
|  | ws `sendFrame` | 94,378 | 85.7% | 110 | 1.32 ms |
| 16 KiB | 10.1.0 `sendFrame` | 57,098 | 80.4% | 71 | 2.77 ms |
|  | 10.1.0 `send` | 55,199 | 89.8% | 61 | 2.80 ms |
|  | 9.2.0 `send` | 56,950 | 82.7% | 69 | 2.75 ms |
|  | ws `sendFrame` | 57,423 | 79.7% | 72 | 2.74 ms |
| 320 KiB | 10.1.0 `sendFrame` | 5,559 | 61.7% | 9.0 | 19.20 ms |
|  | 10.1.0 `send` | 5,438 | 83.2% | 6.5 | 19.47 ms |
|  | 9.2.0 `send` | 5,510 | 70.7% | 7.8 | 19.38 ms |
|  | ws `sendFrame` | 5,565 | 62.0% | 9.0 | 19.03 ms |

For text, `sendFrame()` improves 10.1.0 CPU efficiency over `send()` by 7.8%
at 1 KiB, 15.8% at 16 KiB and 38.2% at 320 KiB. It matters because Engine.IO
encodes one frame and reuses it across recipients.

For application-compressed binary, `send()` already writes the unmasked header
and original Buffer as a corked pair. At the 29,926-byte wire maximum,
`sendFrame()` and `send()` both delivered about 55–56 thousand messages/s at
about 82% CPU; the difference was run noise.

## Idle memory

The longer original least-squares measurement over 500, 1,000, 1,500 and 2,000
idle connections, with WebSocket compression off, was:

| Implementation | Idle RSS/connection | Regression R² |
|:---|---:|---:|
| 10.x | 10.0 KiB | 0.967 |
| 9.2.0 | 6.8 KiB | 0.888 |
| ws | 9.9 KiB | 0.922 |

The final 200-connection packaging smoke measured 15.2 KiB for 10.1.0 and 13.8
KiB for ws, but the smaller sample has a weaker regression fit. The longer run
is the better estimate: 10.1.0 and ws have comparable idle RSS slopes.

## Production interpretation

For Engine.IO/Socket.IO on ARM64, keep using `Server` with
`perMessageDeflate: false`. The default protects Unicode receive CPU, enables Engine.IO's
pre-encoded `sendFrame()` broadcast path and avoids changing direct eiows API
semantics. Already-compressed application data should remain binary.

The remaining meaningful optimization opportunity is a proven full SIMD UTF-8
validator for non-ASCII-heavy input. It should only be adopted after differential
fuzzing and ARM64/x64 benchmarks. Restoring native socket ownership could recover
more of the 9.2.0 small-message advantage but would reintroduce a separate TLS,
lifecycle and backpressure architecture and is outside a safe patch release.

## What p99 round-trip latency means

The p99 value is the 99th percentile: 99% of sampled round trips completed in
that time or less, while the slowest 1% took longer. It is a tail-latency value,
not an average.

## Reproduction

```sh
npm install --prefix benchmark
node --expose-gc benchmark/run.js

node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,327680 --data-modes text \
  --text-consumption string

node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,327680 --data-modes text \
  --source-content mixed-utf8 --server-text-output buffer \
  --text-consumption string

node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,30720 --data-modes binary

node --expose-gc benchmark/broadcast-run.js
```

Set `EIOWS_BENCHMARK_CPU_PROF_DIR` to an output directory to collect V8 CPU
profiles. Run final measurements on an otherwise idle machine and compare
multiple medians; loopback results are regression evidence, not a substitute
for a production-shaped Socket.IO load test.
