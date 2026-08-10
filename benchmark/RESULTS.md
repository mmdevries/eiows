# Native transport ownership: eiows 11.0.0

Measured on 2026-08-10 with Node.js 26.0.0 and OpenSSL 3.6.3 on an Apple
M2 Pro (arm64, macOS). Every server ran in a fresh process on loopback with
100 connections, one in-flight operation per connection and
`permessage-deflate` disabled. Tables report three-run medians. The final
1 KiB and 256 KiB verifications used one second of warm-up and three seconds
of measurement; the 16 KiB, remaining egress and memory matrices used 0.5
seconds of warm-up and two seconds of measurement.

The comparison uses this checkout, `eiows@9.2.0` and `ws@8.21.3`. The ws
process was verified to load `bufferutil@4.1.0`. Bufferutil is used for
masked client ingress above ws's threshold; server egress frames are not
masked, so it is not relevant to those writes.

CPU is process CPU relative to one logical core. The normalized rate is the
most useful capacity number: 0.100 M messages/CPU-s means 100,000 messages for
one consumed CPU second. Loopback throughput can be limited by the clients,
so equal throughput with less server CPU is a real improvement.

## Outcome

The production path now takes ownership after an HTTP upgrade instead of
retaining Node's TCPWrap/TLSWrap graph:

- the descriptor is duplicated with close-on-exec, Node reads are stopped and
  the original Node socket is destroyed;
- a native `uv_poll_t` drives level-triggered TCP/TLS readiness;
- TCP receives into a reusable 64 KiB thread-local buffer and sends frame
  vectors with `sendmsg()` without copying shared payloads;
- TLS retains the negotiated `SSL*` and required `SSL_CTX` references, waits
  for Node to release TLSWrap's SSL reference, then exclusively drives the
  SSL BIO queues;
- TLS plaintext is staged in bounded 60 KiB batches. Ciphertext from multiple
  frame parts and TLS records is gathered into a 64 KiB thread-local scratch
  buffer before raw socket writes. Per-connection ciphertext is allocated only
  when kernel backpressure requires it;
- callbacks and `bufferedAmount` are committed only after the corresponding
  TCP bytes or TLS ciphertext have been accepted by the owned descriptor;
- outgoing Buffer/TypedArray/ArrayBuffer storage is pinned through its V8
  backing store, including if the JavaScript ArrayBuffer is detached while a
  write is backpressured;
- an asynchronous environment cleanup hook closes every active owner during
  Worker shutdown.

The result is not a claim that every workload is faster. Small 1 KiB ingress
still leaves a fixed-cost CPU gap to 9.2.0, especially when current eiows
returns ws-compatible text Buffers while 9.2.0 returns strings. At 16 KiB,
both TCP and TLS ingress are more CPU-efficient than the two comparison
implementations. The Engine.IO pre-encoded `sendFrame()` route is the best
egress path, especially over TLS.

## Receive-only ingress

The server parses, validates, unmasks, dispatches and counts messages without
sending an echo. Text remains a Buffer with `isBinary === false`, matching ws
and Engine.IO's input contract.

### TCP

| Payload | Data | Implementation | Messages/s | CPU | M messages/CPU-s |
|---:|:---|:---|---:|---:|---:|
| 1 KiB | text | current | 45,557 | 50.3% | 0.091 |
|  |  | 9.2.0 | 45,348 | 46.6% | 0.097 |
|  |  | ws | 45,672 | 56.7% | 0.081 |
| 1 KiB | binary | current | 46,793 | 51.7% | 0.091 |
|  |  | 9.2.0 | 46,188 | 50.4% | 0.092 |
|  |  | ws | 46,416 | 57.5% | 0.081 |
| 16 KiB | text | current | 31,045 | 46.7% | 0.067 |
|  |  | 9.2.0 | 31,567 | 52.2% | 0.060 |
|  |  | ws | 31,090 | 50.6% | 0.061 |
| 16 KiB | binary | current | 33,817 | 48.9% | 0.070 |
|  |  | 9.2.0 | 33,974 | 49.3% | 0.069 |
|  |  | ws | 33,676 | 52.2% | 0.065 |

At 1 KiB, current eiows is 6% less CPU-efficient than 9.2.0 for default text
and 1% less for binary, but about 12% more efficient than ws in both cases.
The text comparison is not representation-equivalent: current eiows and ws
return a Buffer, while 9.2.0 returns a string. In a separate five-run check
with `--server-text-output string`, the remaining gap was about 2% for TCP and
3% for TLS. At 16 KiB current eiows is 10% more efficient than 9.2.0 for text
and effectively tied for binary; it is 8–9% ahead of ws.

### TLS

| Payload | Data | Implementation | Messages/s | CPU | M messages/CPU-s |
|---:|:---|:---|---:|---:|---:|
| 1 KiB | text | current | 50,843 | 62.6% | 0.082 |
|  |  | 9.2.0 | 51,539 | 57.5% | 0.090 |
|  |  | ws | 51,060 | 66.4% | 0.076 |
| 1 KiB | binary | current | 53,149 | 63.4% | 0.084 |
|  |  | 9.2.0 | 53,487 | 60.2% | 0.089 |
|  |  | ws | 53,155 | 67.4% | 0.079 |
| 16 KiB | text | current | 28,943 | 66.1% | 0.044 |
|  |  | 9.2.0 | 28,265 | 78.0% | 0.036 |
|  |  | ws | 29,077 | 72.4% | 0.040 |
| 16 KiB | binary | current | 31,994 | 71.3% | 0.045 |
|  |  | 9.2.0 | 31,049 | 81.0% | 0.038 |
|  |  | ws | 31,393 | 76.2% | 0.041 |

At 1 KiB, owned TLS is 6–9% less CPU-efficient than 9.2.0 with the default
Buffer output, but 6–8% more efficient than ws. At 16 KiB it is 18–22% more
CPU-efficient than 9.2.0 and 10–11% more efficient than ws. Median active peak
RSS growth was 0.6–1.1 MiB for current eiows and 8.6–9.0 MiB for ws in those
16 KiB cases. Short active RSS deltas are allocator-sensitive, so they are
supporting evidence rather than a per-connection estimate.

## Broadcast egress

Each round sends one message to 100 clients and waits for every write callback
and every client acknowledgement. This prevents an implementation from
appearing fast by accumulating an unbounded write queue.

`sendFrame()` reuses an already encoded frame and is the route Engine.IO uses
for eligible broadcasts. `send()` includes per-recipient input conversion and
framing.

### Text

| Transport | Payload | Variant | Deliveries/s | CPU | M deliveries/CPU-s |
|:---|---:|:---|---:|---:|---:|
| TCP | 1 KiB | current `sendFrame` | 96,457 | 84.2% | 0.115 |
|  |  | current `send` | 95,443 | 85.2% | 0.111 |
|  |  | 9.2.0 `send` | 97,034 | 80.8% | 0.120 |
|  |  | ws `sendFrame` | 98,312 | 85.3% | 0.115 |
| TCP | 16 KiB | current `sendFrame` | 61,381 | 75.5% | 0.081 |
|  |  | current `send` | 58,896 | 87.9% | 0.067 |
|  |  | 9.2.0 `send` | 61,775 | 80.2% | 0.077 |
|  |  | ws `sendFrame` | 58,542 | 79.3% | 0.074 |
| TLS | 1 KiB | current `sendFrame` | 88,702 | 85.2% | 0.104 |
|  |  | current `send` | 89,759 | 89.2% | 0.099 |
|  |  | 9.2.0 `send` | 87,424 | 77.7% | 0.112 |
|  |  | ws `sendFrame` | 88,158 | 87.3% | 0.101 |
| TLS | 16 KiB | current `sendFrame` | 48,264 | 94.0% | 0.052 |
|  |  | current `send` | 42,682 | 96.1% | 0.045 |
|  |  | 9.2.0 `send` | 39,699 | 95.6% | 0.042 |
|  |  | ws `sendFrame` | 50,893 | 93.0% | 0.055 |

At 16 KiB, TCP `sendFrame()` is 5.5% faster and 10% more CPU-efficient than
ws's equivalent route. TLS `sendFrame()` is 5.2% behind ws in throughput and
5.4% behind in CPU efficiency, but 22% faster and 24% more CPU-efficient than
9.2.0. The TLS gap is now small enough that application work and real network
latency are likely to dominate; it is not described as universally eliminated.

### 256 KiB text

This larger-payload verification used 100 clients, one second of warm-up and
three seconds of measurement per run. Logical and wire payload rates are
effectively equal because compression is disabled. Active RSS is the median
increase from the post-warm-up baseline to the active peak and is therefore
allocator-sensitive.

| Transport | Variant | Deliveries/s | MiB/s | CPU | M deliveries/CPU-s | Delivery p50/p99 | Active RSS +MiB |
|:---|:---|---:|---:|---:|---:|---:|---:|
| TCP | current `sendFrame` | 6,596 | 1,649.0 | 58.1% | 0.01135 | 15.180 / 16.855 ms | 4.7 |
|  | current `send` | 6,975 | 1,743.7 | 78.1% | 0.00896 | 14.361 / 15.760 ms | 4.8 |
|  | 9.2.0 `send` | 6,758 | 1,689.5 | 69.8% | 0.00954 | 14.844 / 16.123 ms | 4.7 |
|  | ws `sendFrame` | 6,819 | 1,704.8 | 59.6% | 0.01143 | 14.673 / 16.990 ms | 4.4 |
|  | ws `send` | 6,589 | 1,647.3 | 82.0% | 0.00804 | 15.207 / 16.469 ms | 4.5 |
| TLS | current `sendFrame` | 5,557 | 1,389.2 | 98.7% | 0.00562 | 17.949 / 20.331 ms | 5.0 |
|  | current `send` | 4,689 | 1,172.3 | 99.4% | 0.00472 | 21.067 / 35.999 ms | 5.2 |
|  | 9.2.0 `send` | 3,275 | 818.7 | 98.8% | 0.00331 | 30.330 / 45.227 ms | 3.9 |
|  | ws `sendFrame` | 6,336 | 1,584.1 | 97.2% | 0.00651 | 15.555 / 18.835 ms | 2.6 |
|  | ws `send` | 5,185 | 1,296.3 | 100.4% | 0.00516 | 19.257 / 20.578 ms | 4.6 |

On TCP, current `sendFrame()` is within 0.7% of ws in CPU efficiency, with
3.3% lower throughput. It is 19.0% more CPU-efficient than the only route
available in 9.2.0, although throughput is 2.4% lower. The ordinary current
`send()` route is 3.2% faster but 6.0% less CPU-efficient than 9.2.0 `send()`.

On TLS, current `sendFrame()` is 69.7% faster and 69.8% more CPU-efficient
than 9.2.0, while its delivery p50/p99 is 40.8%/55.0% lower. Against ws's
equivalent `sendFrame()` route, it remains 12.3% behind in throughput and
13.7% behind in CPU efficiency. Current `send()` also improves over 9.2.0:
43.2% more throughput, 42.6% more CPU efficiency and 30.5% lower median
delivery latency. The short RSS deltas are too close and allocator-sensitive
to infer a stable per-connection memory difference from this run.

### Application-deflated payload

A deterministic 215,000-byte logical source was compressed once with raw
DEFLATE before warm-up, producing a 29,926-byte binary payload. Compression
CPU is intentionally excluded; the measured path is frame reuse and TLS I/O.

| TLS variant | Deliveries/s | CPU | M deliveries/CPU-s | Active RSS +MiB |
|:---|---:|---:|---:|---:|
| current `sendFrame` | 35,775 | 82.6% | 0.0433 | 0.45 |
| 9.2.0 `send` | 34,236 | 96.1% | 0.0357 | 0.42 |
| ws `sendFrame` | 37,848 | 87.9% | 0.0431 | 0.34 |

The multipart memory-BIO staging path gives current eiows essentially the same
CPU efficiency as ws while preserving shared payload storage. Throughput was
5.5% lower than ws and 4.5% higher than 9.2.0.

## Idle TCP memory

RSS was sampled after forced V8 GC at 0, 500, 1,000, 1,500 and 2,000 idle
connections. The reported value is the slope of a least-squares fit.

| Implementation | KiB/connection | Regression R² |
|:---|---:|---:|
| current | 9.80 | 0.897 |
| 9.2.0 | 6.67 | 0.885 |
| ws | 9.78 | 0.929 |

Current eiows and ws have the same measured idle RSS slope. Version 9.2.0 is
about 3.1 KiB lower per connection. The current owner carries explicit
`uv_poll_t`, callback, queue and async-context lifecycle state; the original
Node TCPWrap and socket object are already destroyed and are not part of this
slope.

## Correctness and compatibility evidence

The native tests cover TCP and TLS ownership, upgrade-head delivery, SNI
contexts alongside ordinary Node HTTPS, ordered multipart writes, more than
`IOV_MAX` frame parts, peer-close flushing, TLS ciphertext backpressure,
detached 4 MiB ArrayBuffers under TCP and TLS backpressure, Worker environment
cleanup, Engine.IO Unicode round trips, compression and protocol errors.

The final suite contains 43 tests, including explicit verification that
message callbacks execute in the transport's Node `AsyncResource`. It was
built and passed on:

- Node 22.23.2, Linux glibc, arm64;
- Node 24.18.1, Linux musl/Alpine, arm64;
- Node 26.0.0, macOS, arm64.

CI additionally defines Node 22/24/26 glibc and musl jobs, macOS x64/arm64,
Linux arm64 and FreeBSD x64/arm64 coverage. Because private Node structures are
used deliberately, install compiles from source against the exact running Node
release; a generic Node-API prebuild is not shipped.

## Reproduction

```sh
npm install --prefix benchmark

node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --duration 2 --warmup 0.5 --connections 100 \
  --payloads 1024,16384 --data-modes text,binary \
  --transports tcp,tls

node --expose-gc benchmark/broadcast-run.js --iterations 3 \
  --duration 2 --warmup 0.5 --connections 100 \
  --payloads 1024,16384 --data-modes text \
  --transports tcp,tls

node --expose-gc benchmark/broadcast-run.js --iterations 3 \
  --duration 3 --warmup 1 --connections 100 \
  --payloads 262144 --data-modes text \
  --transports tcp,tls

node --expose-gc benchmark/broadcast-run.js --iterations 3 \
  --duration 2 --warmup 0.5 --connections 100 \
  --payloads 215000 --data-modes app-deflate --transports tls

node --expose-gc benchmark/run.js --iterations 1 \
  --duration 1 --warmup 0.25 --connections 20 \
  --payloads 64 --message-types binary --compression off \
  --memory-connections 2000 --memory-step 500
```

Run on an otherwise idle machine and compare repeated medians. These results
are regression evidence for native transport ownership, not a substitute for
a production-shaped Socket.IO load test with the application's actual message
mix, TLS settings, adapters and connection churn.
