
Sandbox for playing with cache, branches, allocation, and data structures.

### Numbers

Non-representative of a real workload because it's all synthetic data but the ratio should be right. It's probably less bursty and therefore more cache-favourable than real data.

Numbers captured on an isolated core locked at 2.9GHz on Ryzen 7 4800H (Zen 2) on Ubuntu 24.04 with g++-14.
First 100 entries ignored to warm up caches and BTB.
These numbers look to be real as they are reproducible within 10% across every run with the same seed.


```
p99.999, idx 14999750: 4649ns
p99.99, idx 14998400: 2555ns
p99.9, idx 14984900: 771ns
p99, idx 14849901: 140ns
p95, idx 14249905: 50ns
p50, idx 7499950: 30ns
last: 7865ns
executed_limits: 7501163, resting_crosses: 10608, fully_filled_crosses: 372370, resting: 7128793
no ops (aggressive order found the opposite side empty): 2
executed_markets: 899710
executed_cancels: 6599127, of which not found (already filled): 833165
book state: ask: 82, bid: 81
book width: asks: 12542, bids: 7921
book ladder (non-empty levels):
    bids  bidLots price   asks  askLots
       0        0   109      1        1
       0        0   105      2        6
       0        0   103      1       10
       0        0   102      4       71
       0        0   101      9       60
       0        0   100     18      311
       0        0    99     38      396
       0        0    98     78      813
       0        0    97    131     1116
       0        0    96    303     2960
       0        0    95    643     6355
       0        0    94   1181    10961
       0        0    93   2491    22997
       0        0    92   4811    44403
       0        0    91   1919    17282
       0        0    90    104     1170
       0        0    89     62      497
       0        0    88     76      555
       0        0    87    119     1413
       0        0    86    243     2471
       0        0    85    259     2721
       0        0    84     24      263
       0        0    83     11      207
       0        0    82     14      124  <- best ask
       6       37    81      0        0  <- best bid
      34      414    80      0        0
      59      529    79      0        0
     175     1542    78      0        0
     185     1879    77      0        0
     187     1679    76      0        0
     206     1817    75      0        0
     101      744    74      0        0
      49      363    73      0        0
      35      190    72      0        0
      29      180    71      0        0
      15       82    70      0        0
      16      164    69      0        0
       6       26    68      0        0
       4       10    67      0        0
      18      305    66      0        0
      24      299    65      0        0
      31      392    64      0        0
      11       47    63      0        0
      10       42    62      0        0
       6       82    61      0        0
      10      340    60      0        0
       7       28    59      0        0
      35      493    58      0        0
      50      653    57      0        0
      47      430    56      0        0
      94      836    55      0        0
     135     1241    54      0        0
     128     1163    53      0        0
      56      577    52      0        0
     172     1720    51      0        0
      75      960    50      0        0
      42      776    49      0        0
      67      482    48      0        0
      55      886    47      0        0
      40      596    46      0        0
      54      497    45      0        0
      24      128    44      0        0
      55      365    43      0        0
      27      206    42      0        0
      20      152    41      0        0
      22      311    40      0        0
     116     1326    39      0        0
      83      811    38      0        0
      46      281    37      0        0
     230     1924    36      0        0
    2498    22279    35      0        0
    1226    12358    34      0        0
     621     5418    33      0        0
     337     3406    32      0        0
     171     1961    31      0        0
      88      831    30      0        0
      39      251    29      0        0
      23      201    28      0        0
      14       84    27      0        0
       3       12    26      0        0
       2       11    25      0        0
       1        1    23      0        0
       1        2    22      0        0
```

### To run

A bit janky as of right now, but first build and run market_generator - it creates the market in shared memory and gives you how to attach from the main exchange. Reason for this is because I wanted to do IPC for this.

```
$ ./market_generator <seed> 
attach with: ./exchange /proc/390268/fd/3   (15000000 requests, 960495616 bytes)
generated 15000000 requests: 10001148 orders, 4998852 cancels

```

### TODOs

1. Implement a proper protocol so I can test against a real market feed and profile end-to-end properly (currently only synthetic data)
2. SPSC directly in shared memory rather than app-side
3. Only 128 tick levels right now mimicking a spread, handle colder orders and movement of the spread
4. More symbols and stop order book
5. Fix up fulfilled orders
6. Address code TODOs

Notes for myself here-on-after:


## Profiling with Clang XRay

XRay is a Clang instrumentation framework that inserts lightweight probes at function entry/exit for precise latency tracing.

### Build with XRay enabled
```bash
cmake -S . -B build -DENABLE_XRAY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

### Collect a trace
```bash
XRAY_OPTIONS="patch_premain=true xray_mode=xray-basic verbosity=1" ./src/exchange
```
This produces a binary log file named `xray-log.exchange.<PID>`.

### Analyse the trace
```bash
# Table of function timings sorted by median duration (descending)
llvm-xray account xray-log.exchange.* --instr_map=./src/exchange --sort=med --sortorder=dsc

# Or export to Chrome/Perfetto trace format for a visual flame chart
llvm-xray convert --symbolize --instr_map=./src/exchange --output-format=trace_event xray-log.exchange.* > trace.json
```
Open `trace.json` in `chrome://tracing` or [Perfetto UI](https://ui.perfetto.dev/).