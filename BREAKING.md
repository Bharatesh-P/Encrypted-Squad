---

### 2. `BREAKING.md`

Save this as `encrypted-squad/BREAKING.md`:

```markdown
# Deliverable 3: How We Would Break This (`BREAKING.md`)

## 1. Crash Points Table (Write Path, Compaction Path & Recovery Path)

Every row below references the exact file and function in our C++17 codebase (`src/`).

| # | Exact Code Location | Moment of Process Kill (`SIGKILL` / `TerminateProcess`) | Exact Disk State at Kill | What Next `open()` (`recover_database` in `src/recovery.cpp`) Does |
|:-:|---|---|---|---|
| 1 | `src/recovery.cpp: recover_database()` (initial header write) | Killed after writing $< 12$ bytes of the initial `KVDB` file header on a brand-new file. | File size is between `1` and `11` bytes; no records exist. | Sees `file_size < kFileHeaderSize (12)`, truncates to `0`, writes a complete 12-byte header, and calls `file.sync()`. |
| 2 | `src/engine.cpp: Database::put()` (before `file_.write_all_at`) | Killed during size validation or `serialize_record()` before the OS write syscall starts. | File ends cleanly at the previous transaction boundary (`append_offset_`). | Scans cleanly to EOF with zero bytes truncated; key is absent or retains its previous value (Scenario A1). |
| 3 | `src/engine.cpp: Database::put()` (inside `file_.write_all_at`) | Killed mid-syscall after writing 16 of 32 header bytes, or 400 KB of a 1 MB value payload. | Tail contains a partial header ($< 32$ bytes) or partial payload ($< \text{key\_len} + \text{value\_len}$), or invalid `record_crc32`. | `SequentialLogReader` detects short header/payload or CRC mismatch at `record_start`, calls `repair_tail_at()` to `truncate(record_start)` + `sync()` (Scenarios A2, A7). |
| 4 | `src/engine.cpp: Database::put()` (after `write_all_at`, before `file_.sync()`) | Killed after all bytes of a standalone `PUT`/`DEL` are in the OS page cache, before `fsync`/`FlushFileBuffers` returns. | Since the OS stays up (`kill -9`), the complete record is in the OS cache and flushed to disk; `record_crc32` is valid. | Replays the complete valid record and advances `next_txn_id`. Allowed by Invariant Rule 2 because success was not yet returned (Scenario A3). |
| 5 | `src/engine.cpp: Database::put()` (after `file_.sync()`, before `index_.put` / `ACK`) | Killed one microsecond after `file_.sync()` returns `OK`, before `serve` prints `ACK <op-id>`. | Complete record with valid `record_crc32` is durably on physical disk. | Replays the record and inserts `(value_offset, value_len)` into `index_`. The new value is present (Scenario A4). |
| 6 | `src/engine.cpp: Database::batch()` (member loop `write_all_at`) | Killed after writing $k$ of $N$ (`op_count`) batch member records (`flags = kFlagBatchMember`). | $k$ contiguous batch member records exist at `batch_start_off`, possibly followed by a $(k+1)$-th torn member; no `COMMIT` marker. | Buffers the $k$ members in `pending_ops`, hits EOF or torn $(k+1)$-th member while `in_batch == true`, and calls `repair_tail_at(batch_start_off)` to truncate the entire uncommitted batch (Scenarios B1, B6). |
| 7 | `src/engine.cpp: Database::batch()` (after first `file_.sync()`, before `COMMIT` write) | Killed after all $N$ batch members are synced to disk, before `serialize_commit_marker` is written. | All $N$ member records are valid on disk at `batch_start_off`, but EOF is reached with no `RecordType::COMMIT` record. | Reaches EOF with `in_batch == true`, discards `pending_ops`, and truncates the file back to `batch_start_off`. Zero ops from the batch are visible (Scenario B2). |
| 8 | `src/engine.cpp: Database::batch()` (during `write_all_at` of 36-byte `COMMIT` marker) | Killed after writing only 20 of the 36 bytes of the `COMMIT` marker. | All $N$ batch members are on disk followed by a 20-byte torn `COMMIT` header at EOF. | Detects `< 32` bytes remaining at EOF while `in_batch == true`, and truncates back to `batch_start_off` (not just the marker start!), rolling back all $N$ members (Scenario B3). |
| 9 | `src/engine.cpp: Database::batch()` (after second `file_.sync()` returns `OK`) | Killed after the `COMMIT` marker's `file_.sync()` completes, during the in-memory `index_` loop. | All $N$ batch members and the 36-byte `COMMIT` marker (with valid `batch_crc32`) are durable on disk. | Validates `batch_crc32 == expected_batch_crc` and applies all $N$ operations in `0..op_count-1` order to `index_` (Scenarios B4, B5). |
| 10 | `src/compaction.cpp: compact_database()` (while writing `db.data.tmp`) | Killed while copying live keys to `<path>.tmp` or during `temp_file.sync()`, before `File::atomic_replace()`. | Authoritative `<path>` is completely untouched; an incomplete `<path>.tmp` file sits in the directory. | Replays the untouched authoritative `<path>` file normally, then unlinks the stale `<path>.tmp` at the end of `recover_database()` (Scenario C3). |
| 11 | `src/compaction.cpp: compact_database()` (after `File::atomic_replace()`) | Killed immediately after `MoveFileExW` / `rename` replaces `<path>` with `<path>.tmp`, before `main_file.open()` or RAM index swap. | `<path>` now points to the newly synced compacted file; `<path>.tmp` no longer exists (or is removed on open). | Scans the new compacted `<path>` file from offset `12`, rebuilds `index_` with the new offsets, and starts ready (Scenarios C4, C5). |
| 12 | `src/recovery.cpp: repair_tail_at()` (during `file.truncate(safe_offset)`) | Killed during `open()` right before, during, or right after `ftruncate` / `SetEndOfFile` while repairing a torn tail. | File Either still has the torn tail or has already been shortened to `safe_offset` (a valid transaction boundary). | Next `open()` re-scans from offset `12`: if the torn tail remains, it truncates to the exact same `safe_offset`; if already truncated, it reaches EOF cleanly (Scenarios C1, C2, F2). |

---

## 2. The System-Call Ordering Argument

Our invariant relies on strict ordering between positional writes, OS durability barriers, atomic filesystem replacement, and in-memory visibility:

1. **Single Mutations (`Database::put` / `Database::del`):** While holding `std::unique_lock<std::shared_mutex>`, we call `pwrite()` (POSIX) or `WriteFile()` with explicit `OVERLAPPED` offsets (Windows) to append the 32-byte header, key, and value. We then invoke `fsync(fd)` (POSIX) or `FlushFileBuffers(handle)` (Windows). Only **after** the sync syscall returns `0`/`TRUE` do we mutate the in-memory `Index` (`std::unordered_map`) and return `Status::Ok()` so `main_kvdb.cpp` can print `ACK <op-id>`. Because `ACK` strictly follows `fsync`/`FlushFileBuffers`, any acknowledged write is already on non-volatile storage before the caller hears back.
2. **Two-Phase Atomic Batches (`Database::batch`):** Under the exclusive lock, we write all `op_count` member records with `flags = kFlagBatchMember` and call the **first** `fsync()` / `FlushFileBuffers()`. Next, we append the 36-byte `COMMIT` marker containing `batch_crc32` (computed over all serialized member records) and call the **second** `fsync()` / `FlushFileBuffers()`. Only after the second sync returns do we apply the batch to the RAM index and emit `ACK`. Because recovery buffers `kFlagBatchMember` records in `pending_ops` and rolls back to `batch_start_off` unless a valid `COMMIT` marker is present, a batch can never be partially visible.
3. **Compaction (`compact_database`):** We write all live keys into `<path>.tmp` in the same directory, call `fsync()` / `FlushFileBuffers()` on `<path>.tmp`, and close its handle **before** calling `rename(temp, main)` + `fsync(parent_dir_fd)` on POSIX or `MoveFileExW(temp, main, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows. Flushing the shadow file before replacement guarantees `<path>` never points to an un-synced or half-written file.

---

## 3. Known Weaknesses (Honest Engineering Trade-offs)

1. **Recovery Time Scales Linearly with Uncompacted Log Size (`O(log_bytes)`):** Although our 4 MiB `SequentialLogReader` and value-offset index recover 1,000,000 keys (~64 MiB log) in well under 2 seconds, if a workload performs tens of millions of overwrites without triggering compaction (e.g., when `max_bytes` is unlimited), the log can grow to several gigabytes and eventually exceed the 2-second `open()` budget.
2. **Compaction Requires Up to 2x Physical Disk Space Temporarily:** Because crash-safe compaction builds `<path>.tmp` alongside `<path>` before atomic replacement, compacting a 1 GB database requires up to 1 GB of free physical space on the volume while the shadow file is being constructed.
3. **Windows Handle-Close Window During Compaction Swap:** On POSIX, `rename()` atomically replaces an open file while keeping existing descriptors valid. On Windows, `MoveFileExW` works most reliably when the target file handle is closed first; therefore, `compact_database()` closes `main_file`, calls `MoveFileExW`, and immediately reopens `main_file` while holding the in-process `unique_lock`. If another external process locked the file in that microsecond window, reopening could fail.
4. **RAM Footprint Proportional to Key Strings:** While value payloads (`0` to `1 MB`) remain on disk, all live key strings (`1` to `1,024` bytes) plus a 12-byte `IndexEntry` live in `std::unordered_map` in RAM. At 1,000,000 keys with 1,024-byte keys, the in-memory index would consume ~1.1 GB of RAM.

---

## 4. What We Tried That Failed (And What the Torture Tool Caught)

1. **Bug 1 — Truncating to the Torn Record Instead of `batch_start_off` During a Batch Crash:** In our first recovery prototype, when a crash occurred on member 4 of a 10-op batch (or during the `COMMIT` marker write), our scanner truncated to `record_start` (leaving members 0..3 in the file) instead of `batch_start_off`. On the *subsequent* write after restart, `txn_id` collided and `check` reported a dangling uncommitted batch in the middle of the file. Fixing `rollback_target = in_batch ? batch_start_off : record_start` in `src/recovery.cpp` resolved this.
2. **Bug 2 — Out-of-Order `ACK` Lines Under Multi-Threaded `serve` Workers:** When we added 4 worker threads in `src/main_kvdb.cpp` to drive concurrent `put`/`del`/`batch` calls, Thread A could commit Transaction 5 inside `Database::put`, get preempted by the OS scheduler before writing `ACK` to `stdout`, and let Thread B print `ACK` for Transaction 6 first. Our torture oracle caught this when a kill landed between the two `stdout` writes. We introduced `AckSequencer` ordered by `commit_seq_` assigned inside the database's `unique_lock` critical section.
3. **Bug 3 — Pipe EOF Race in the Torture Supervisor:** Initially, when `torture` called `TerminateProcess`, it immediately joined the writer threads and stopped reading `stdout` before draining bytes already buffered in the OS pipe. As a result, an `ACK` emitted by `kvdb serve` microseconds before termination was occasionally left unread in the pipe, causing false oracle mismatches. Draining `stdout` to EOF after `kill_now()` fixed the race completely.

---

## 5. If We Had 24 More Hours (Top 3 Priorities)

1. **Persisted Checkpoint / Hint File (`<path>.idx`) for `O(1)` Startup on Multi-GB Logs:** Write an optional, checksummed index snapshot during `close()` and `compact()` storing the log offset and `txn_id` watermark, allowing `open()` to load the snapshot and scan only the log tail written after the checkpoint.
2. **Background Incremental Compaction & Automatic Size-Ratio Trigger:** Trigger compaction automatically when `dead_records_ / total_records > 0.5` even when `--max-bytes` is not set, copying chunks under short lock slices so tail latency for concurrent writers stays low.
3. **Full Mid-File Bit-Rot Scrubbing & Page-Level Checksums:** Extend `check` and `get()` with optional on-read CRC verification (currently `get()` trusts the OS disk after recovery validates all record CRCs on `open()`).