#### cmake commands

1. cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
2. ninja -C build
3. .\build\src\exchange.exe

#### why not using `new` keyword:

#### zero allocation infrastructure

- we need to find empty place in heap
- take a lock as heap is shared memory
- if space found, takes 30-500ns
- else have to make a sys call, which takes longer
- this also leads to fragmentation of memory, as there are gaps in between filled spaces

#### memory system

- in memory of system we need:
  - O(1) allocation
  - O(1) de-allocation
  - no call to malloc or free
  - cache aligned returns

- it is single threaded
- it has no auto growth, when the memory reaches max.
- we are using every block of same size, so no fragmentation

- we make the list -> intrusive
  - we overide the next ptr. with values when we allocate a slot
  - when we deallocate, head->slotNo->prev. empty slot
- allocation and de-allocation happens at front
- a Slot can be a FreeNode(stores next ptr.) or the object of that Order

#### spsc queue

- we need a queue where producer and consumer never block each other
- also they never call into kernel and never touch the heap (new)

- ring buffer:
  - when we push(write) -> tail advances and start on pop(read)
  - where tail reaches end it comes to starting pos. (wrap around)
- hence producer and consumer do not overlap with each other

- push()
  - tail is only writer in producer so relaxed in push
  - The consumer updates head after removing an item. The producer needs to know whether space has become available.
    Acquire ensures it sees the consumer's latest release update.
- think of acquire as -> now i can safely read
- pop()
  - tail is acquire -> The release-acquire pair guarantees that the consumer never reads uninitialized data.

- not using mpmc:
  - as in case of multiple producer pushing at same time we need lock/mutex (makes slow)
  - or contention loop

#### clocks

- we need two clocks

- can't use wall clock:
  - can be moved backwards (matching engine breaks)
  - is not deterministic (for replays)

- Logical time -> used in sequencer, matching engine(single threaded)
- Real time -> impl using chrono, used by tcp gw, heartbeat supervisor

#### order

- we have a price level
- this is a FIFO queue where the order are stored acc. to time
- when order comes check price and then put in that price level according to TS

- for a order we need:
  - O{1} insert
  - O(1) pop at tail
  - O(1) remove at any place (for cancel order)
- so we use intrusive linked list
