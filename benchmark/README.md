# eiows comparison benchmark

This benchmark compares the current checkout with `eiows@9.2.0` and the
JavaScript `ws` package with its optional `bufferutil` and `utf-8-validate`
native addons installed. Each WebSocket server runs in a fresh child process.
The load generator always uses the same `ws` client with JavaScript masking,
so its cost stays fixed and client memory is excluded from the server RSS
numbers. The `ws` server process clears addon-disabling environment flags and
verifies that `bufferutil.node` was actually loaded. This isolates the effect
of native unmasking in the server under test.

`bufferutil` is automatically used by `ws` for masking and unmasking above its
internal payload thresholds. On current Node versions, `ws` deliberately uses
the built-in `Buffer.isUtf8()` implementation before `utf-8-validate`; the
addon is installed and load-checked but is only selected on Node versions that
do not provide the built-in implementation.

Install the comparison packages once and run the full benchmark:

```sh
npm install --prefix benchmark
node --expose-gc benchmark/run.js
```

The default performance matrix uses 100 concurrent connections, one in-flight
echo per connection, 1 second of warm-up, 3 seconds of measurement, payloads
of 1 KiB, 16 KiB and 320 KiB, text and binary frames, WebSocket compression
off, and two fresh-process iterations. Reported throughput is completed round
trips per second. Payload bandwidth counts the application payload in both
directions. Server CPU is shown both as core utilization and as round trips per
consumed CPU second, so speed and CPU cost can be evaluated together. RTT
percentiles are sampled once per 16 messages. The
`--compression off,on` override remains available for separate experiments.

Optional WebSocket compression uses `permessage-deflate`, a threshold of zero
and no context takeover on either side. Payloads contain repeated ASCII bytes,
so those optional compression cases deliberately represent highly compressible
application data. Text and binary cases use the same bytes to isolate framing
and API cost from differences in content.

The memory case opens 2,000 idle connections in steps of 500 and forces V8 GC
before every sample. The reported KiB/connection is the slope of a least-squares
fit over all RSS samples. It is run separately with compression disabled and
enabled. RSS includes the V8 heap, native allocations and loaded code; it does
not include the separate client process.

For a quick wiring check:

```sh
node --expose-gc benchmark/run.js --iterations 1 --duration 1 --warmup 0.25 \
  --connections 20 --payloads 64 --message-types binary --compression off \
  --memory-connections 100 --memory-step 50
```

## Pre-encoded broadcast benchmark

The broadcast benchmark measures the `_sender.sendFrame()` fast path separately
from echo performance. One WebSocket frame is encoded once and reused across
all recipients. It compares current eiows `sendFrame()` and `send()`, eiows
9.2.0 `send()`, and ws `sendFrame()` and `send()`. Its default source payloads
match the target workload: 1 KiB, 16 KiB and 320 KiB.

```sh
node --expose-gc benchmark/broadcast-run.js
```

Every broadcast round waits for both all socket write callbacks and an
application-level acknowledgement that all clients received the message. Only
then does the next round start, so even a 320 KiB × 100-recipient fan-out cannot
accumulate in kernel or client buffers. Results include broadcasts and delivered
messages per second, logical and wire payload throughput, flush and delivery
p50/p99, server CPU, deliveries per CPU second and active peak RSS.

The `text` mode sends the source bytes as a text frame. The `app-deflate` mode
compresses the same deterministic, varying JSON-record stream once with
`deflateRaw` and sends the result as a binary frame. Application compression
happens before warm-up and its CPU cost is intentionally excluded; both source
and actual wire payload sizes are reported. WebSocket `permessage-deflate`
remains disabled in every case.

For a short end-to-end validation:

```sh
node --expose-gc benchmark/broadcast-run.js --iterations 1 --duration 1 \
  --warmup 0.25 --connections 20 --payloads 1024 \
  --data-modes text,app-deflate
```

The latest three-iteration result set for the target workload is summarized in
[`RESULTS.md`](RESULTS.md).

## Receive-only ingress benchmark

The ingress benchmark isolates the server receive path: clients continuously
send flow-controlled messages and the server parses, validates, dispatches and
counts them without sending an echo. It reports received messages per second,
server CPU, messages per consumed CPU second and active peak RSS for text,
binary and application-deflated binary data. `permessage-deflate` remains off;
application compression happens once before warm-up and is excluded from CPU.
By default, `--text-consumption native` preserves each implementation's message
representation. Use `--text-consumption string` to include conversion to a
JavaScript string for implementations such as ws that emit text as a Buffer.
The default `--source-content ascii` fixture represents ASCII-dominant JSON;
`--source-content mixed-utf8` adds repeated Latin, CJK and emoji content.
The current implementation defaults to Buffer-backed text, matching `ws` and
the production Engine.IO normalization path. Use `--server-text-output string`
to benchmark the legacy eiows 10.0.1 direct-string behavior.

```sh
node --expose-gc benchmark/ingress-run.js
node --expose-gc benchmark/ingress-run.js --iterations 1 --duration 1 \
  --warmup 0.25 --connections 20 --payloads 1024 \
  --data-modes text,app-deflate --source-content mixed-utf8 \
  --server-text-output buffer --text-consumption string
```

For the measured workload, run receive modes separately: text keeps its 320 KiB
large case, binary stops at its real 30 KiB ceiling, and a 215,000-byte logical
source compresses to 29,926 bytes with the deterministic app-deflate fixture:

```sh
node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,327680 --data-modes text
node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,30720 --data-modes binary
node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,215000 --data-modes app-deflate
```

For an application that consumes incoming text as strings or JSON input:

```sh
node --expose-gc benchmark/ingress-run.js --iterations 3 \
  --payloads 1024,16384,327680 \
  --data-modes text --text-consumption string
```

To collect V8 CPU profiles for each selected implementation's isolated server
process, set `EIOWS_BENCHMARK_CPU_PROF_DIR` for the ingress benchmark. The normal
echo benchmark profiles the current implementation:

```sh
EIOWS_BENCHMARK_CPU_PROF_DIR=/tmp/eiows-profiles \
  node --expose-gc benchmark/ingress-run.js \
  --implementations current --payloads 30720 --data-modes binary
```

Useful overrides include:

```sh
node --expose-gc benchmark/run.js \
  --iterations 5 \
  --connections 250 \
  --inflight 2 \
  --duration 10 \
  --warmup 3 \
  --payloads 64,1024,16384 \
  --message-types text,binary \
  --compression off,on \
  --memory-connections 5000 \
  --memory-step 500 \
  --output benchmark/results/local.json
```

Run on an otherwise idle machine, keep the machine on mains power, and compare
medians from multiple iterations. Loopback benchmarks are useful for relative
regressions, but they are not a substitute for an application-level Socket.IO
load test with production message sizes and connection behaviour.
