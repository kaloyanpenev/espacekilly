# Real-Time Trading Simulator & Matching Engine Core

### Order Types Supported

Market/Limit/Stop/Stop-Limit

### Instrument Scope

Any instrument can be traded on the market. There will be an API to register new instruments on the market at start-up using an external DSL.

### Threading Model

Symbol parallelism.
Dedicate a specific thread exlucisvely to one high-volume instrument, or a group of lower-volume symbols. This guarantees that only one thread ever writes to a symbol's order book - no concurrent writes, so no locks inside the core matching logic.

Network threads read incoming packets and parse the payload, then push the order into a SPMC lock-free queue attached to that specific symbol's matching thread. The symbol thread spins a while(true) loop continuously, executes the order upon the event happening.

OS-level APIs like pthread_setaffinity_np will be used to make sure each physical cpu core runs only 1 thread in order to guarantee cache locality. 
TODO: Need to turn off hyper-threading.

Minimal data sharing and cache invalidation will happen across cores (they should almost never share data)

Need to align everything to the cache line to guarantee no false sharing.

We will not be doing any risk management as this will only ever run on a single machine, so no need to worry about checking orders.

### Data Structures Chosen (and why)

SPMC (single producer multiple consumer) queue/ring buffer that uses mmap/shm to move data between processes.
Figure out the best way to do event based trading with that.
If you have a fastqueue, increment the write counter with 1000x messages instead of touching it every time you write to the queue (David Gross, 2024)


### Networking and order submission

IPC (Shared Memory)
Run the C++23 exchange and the Rust ecosystem simulation as separate processes on the same machine reading/writing to /dev/shm
Colocation and Kernel Bypass

Sockets
Consumes data from either a WebSocket or a plain udp socket using some representation, for example protobuf or FlatBuffers.
Wire-to-wire (from receiving the packet to the time your process reading from the mmap sees it needs to be sub 10 us). Need to understand the linux networking stack and scheduler (and how to turn them off).

### OS-level stuff

(Throwing some ideas here, I'm yet to experiment with this)
Linux kernel boot parameters (like isolcpus and nohz_full) to make sure no OS processes run on threads in use
Networking stack and scheduler on Linux
Turning off hypter-threading in kernel
pthread_setaffinity_np
kernel bypass

### Compile-Time Safety Mechanisms

Almost no runtime mutation of data should happen.
Compile-time first.
Template metaprogramming is fun!
C++23 and C++20 idioms and functionality must be preferred.
Functional paradigms and pure functions must be used for almost all aspects of class design.
Object-oriented design where classes own both functionality and state should generally be avoided.
Data-driven design should be employed for all large volume elements.
Memory pre-allocation should be prioritized. Polymorphic allocators must be considered.

## References & Inspiration

- Data arrives in memory without any copies whatsoever, you read some value out, you change some data structures on the side, usually with pooled values that you reuse, you go back and you do it again. You aim to allocate very little in the critical path and you aim to be prepared up front with all the memory you need.
- Use integer arithmetic for all financial elements - better performance than float/double and can also represent floats.
- In order to avoid fragmentation - put everything of a subsystem with high memory locality on a memory allocator's "local arena" so it does not get fragmented with other data (e.g. if you have 3 vectors for one subsystem.). U can use std::pmr::monotonic for this
- Be mindful of moves - if they are across allocators, UB or fragmentation
- Even if your allocation/deallocation speed is irrelevant in a long-running system, partitioning memory corresponding to subsystems having physical and temporal locality can have enormous effects on performance due to diffusion etc.
- Orders need a common representation - finance sector looks at protobuf for this
- Make sure to code this on linux and learn kernel bypass and the networking TCP/IP/UDP/Multicast sockets stack.
- Use Clang Xray for profiling
- Pre-allocate everything - for example, if you have an fastqueue, increment the write counter with 1000x messages instead of touching it every time you write to the queue (David Gross, 2024)
- Learn about the functional collection library in C++ - i.e. Algo/Numerics and operations over views, e.g. fold, map, std::transform, std::to, std::reduce, std::collect?, std::filter...
- Variants allows functional "OR" (choice) types in C++ (or unions in C) - most of the type they are zero-cost abstraction (unless using type erasure), see https://godbolt.org/z/eeed68b5E


Talks

CppCon 2017: John Lakos “Local ('Arena') Memory Allocators - https://www.youtube.com/watch?v=CFzuFNSpycI
How to build an exchange 24:21 26:20 31:00 - https://www.youtube.com/watch?v=b1e4t2k2KJY&t=1s 
When Nanoseconds Matter: Ultrafast Trading Systems in C++ - David Gross  56:10 - CppCon 2024
Functional Design Patterns - Scott Wlaschin - https://www.youtube.com/watch?v=srQt1NAHYC0


Books
Domain Specific Languages, Martin Fowler
Clean Code, Robert C. Martin (this is really overrated)
Efficient Modern C++, Scott Meyers


# Design
Remember: Memory is cheap. Latency is expensive
Main business Logic loop:
- An order comes in on the writer thread. It is either buy or sell. It is pushed onto an SPSC queue.
- The order is then pushed into a queue (we will be inserting and removing from the two ends, we want a queue order) 
- implemented as a preallocated array. This queue contains other orders of its type for the same price.
- But we always want to deal with the oldest orders first. The queue is in an array of queues where the idx of
- the array denotes the "tick" which gives the queue for orders of the same price and type.
- Therefore we need an array of queue(each denoting a price) per order type.

## New order -> Push

ordersForTick:
Ptr + Price + [][P][X][I][] -> P is the last available slot, I is write index. Move P when fullfiling/removing order, Move I when adding order
Ptr + Price + [X][X][X][X][I==P] -> I and P match, therefore buffer is full. We need to start writing to a new buffer which is denoted by Ptr


OrderBook we need a min and max tick
Map of cold order is [tick, ordersForTick] with the allocated area for orderForTick's vector being massive in some far away corner
of memory which also falls back to disk in case it overflows.

The Ptr in the ordersForTick points to a bucket in the cold orders. 


- What happens if there are more than 128 orders with the same price
- What happens if there is an order very far away from the spread (unrealistically so)
- What happens if an order is removed

## Match order -> Pop

## Cancel order -> Pop

## Edit order -> Edit in-place
Cancel + New

- The logic for matching orders around the market type (in the hot path) is:
	- Buy and sell orders get immediately executed
	- Spin on the lowest ask
What needs most frequent updates:
	- If there is no match anywhere, read next order

Invariants:
- It is invalid to not have a spread - matched order should always be processed by the time the next msg comes in
- Orders must always be processed from last to first chronologically in a deterministic fashion.
