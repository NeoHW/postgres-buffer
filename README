# Modifying PostgreSQL Buffer Replacement Policy

## Overview

This project involves modifying PostgreSQL’s buffer replacement policy to implement **YACLOCK (Yet Another Clock)**, a custom algorithm that replaces the default clock-based strategy.
---

## Contents

- [Benchmarking (Part 1)](#benchmarking-part-1)
- [Buffer Management in PostgreSQL](#buffer-management-in-postgresql)
- [YACLOCK Replacement Policy](#yaclock-replacement-policy)
- [Implementation Guide](#implementation-guide)
- [Testing](#testing)
- [Shared vs Local Buffers](#shared-vs-local-buffers)
- [Free List vs Buffer Pool](#free-list-vs-buffer-pool)
- [Compilation & Execution](#compilation--execution)
- [References](#references)

## Benchmarking (Part 1)

Before modification, PostgreSQL was benchmarked using `pgbench`:

- **Shared Buffers**: 64MB
- **TPS**: ~8234.7
- **Latency**: ~1.214 ms
- **Cache Hit Ratio**: ~99.84%

Command used:
```bash
pgbench -i -s 4
pgbench -c 10 -j 10 -T 360
```

---

## Buffer Management in PostgreSQL

- **Shared Buffers**: Cache pages from permanent tables/indexes.
- **Local Buffers**: Used for session-specific temporary relations.
- **Free List**: Set of unused buffer frames.
- **Buffer Pool**: Entire memory-resident structure containing all buffer frames.

---

## YACLOCK Replacement Policy

YACLOCK uses a **circular queue** of buffer frames, each with a `refBit`, and a **clock hand** called `next`.

### Key Cases:
1. **Page already in buffer** → `refBit = 1`
2. **Page not in buffer, free list not empty** → Insert at tail, `refBit = 0`
3. **Page not in buffer, free list empty** → Use clock-hand-based eviction:
   - Skip pinned frames
   - If `refBit == 1`, set it to 0 and skip
   - If `refBit == 0`, evict and move frame to tail

### Freeing Buffers:
- Update `next` if it points to the removed frame
- Remove frame from the queue
- Reset `next` to NULL if queue is empty

---

## Implementation Guide

Modify **only**:

```c
cs3223_assign1/freelist_yaclock.c
```

Key Functions:

- `StrategyAccessBuffer(int buf_id, int event_num)`
- `StrategyInitialize()`
- `StrategyShmemSize()`
- `StrategyGetBuffer()`
- `StrategyFreeBuffer()`

Use `ShmemInitStruct()` for shared memory. Do **not** use `malloc`.

---

## Testing

Run test cases using:

```bash
$ ./test-yaclock.sh
$ pg_ctl stop
```

Each test file (in `test_bufmgr/testcases/`) simulates operations on a 43-page relation `movies`.

Operation types:
- `read_pin_block(blkno)`
- `read_unpin_block(blkno)`
- `unpin_block(blkno)`

---

## Shared vs Local Buffers

| Feature               | Shared Buffers                   | Local Buffers                     |
|-----------------------|----------------------------------|-----------------------------------|
| Scope                 | Global                           | Per-process                       |
| Concurrency           | Requires locks                   | No locking needed                 |
| Used For              | Permanent tables/indexes         | Temporary session-specific data   |
| Memory Location       | Shared memory                    | Backend-local memory              |

---

## Free List vs Buffer Pool

- **Buffer Pool**: All buffer frames
- **Free List**: Subset of unused buffer frames

Flow:
1. Try buffer pool → cache hit
2. Else, try free list → use unused buffer
3. Else, apply replacement policy (YACLOCK)

---

## Compilation & Execution

### On Server:
```bash
make freelist_yaclock.o
make yaclock
./test-yaclock.sh
pg_ctl stop
```

To restore default Clock policy:
```bash
make clock
```

---

## References

- [PostgreSQL Source Conventions](https://www.postgresql.org/docs/current/source-conventions.html)
- [PostgreSQL Documentation](https://www.postgresql.org/docs/)

---
