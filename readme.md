#### cmake commands

1. cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
2. ninja -C build
3. .\build\src\exchange.exe

#### why not using `new` keyword:

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
