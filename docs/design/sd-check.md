# SD card checking: design note for `sd check`

Decision (the user's, 2026-11-27): **SOS gets an on-device filesystem checker, `sd check`, that
reads the FAT32 or exFAT structures itself, and it runs only while the card is unmounted.**
Nothing else reaches the card while a check runs: no mount, no file command, no SFTP, no log
writer. The checker comes first and only reports problems. Repair follows in its own PRs, and is
tested by injecting faults (§10).

Further decisions (the user's, 2026-11-27, on #213): `sd check` is console only (§2.2); the
cluster window is 16 KB by default (§6.2).

This note sets the rules so that the phases (§9) can be built without deciding them again.
Where this note and the code differ in detail, the code and its header comments are the
reference.

Status (2026-11-27): phases 1 (the FAT32 checker, #223), 2 (exFAT, #232), 3 (`sd check` on the
device, #226) and 5a (the test-build fault commands, #231) are merged; phase 4 (the fault
harness and the repairs, `src/files/fsck_repair.c`) is built for FAT32 (in review, #238) and
exFAT (on top of it). Phase 5b (`sd check fix` on the device) is to come. Tracking issue: #213.
Revised after Reviewer's review of #212: §2, §4, §5 and §10.2.

## 1. What exists today

- **FatFs** (R0.15, `lib/sdcard/src/ff15`) mounts exFAT and FAT32 with long names
  (`FF_USE_LFN 3`, `FF_FS_EXFAT 1`). It has no journal and no checker: a power cut during a
  write can leave lost clusters, a file whose size doesn't match its cluster chain, or a FAT that
  disagrees with the exFAT allocation bitmap. FatFs neither notices nor repairs these. The log
  writer (`core/logfile_board.c`) appends to `/var/log` all the time, so the card is usually
  being written when power goes.
- **Formatting** (`on_format_confirm()` in [src/files/sdcard.c](../../src/files/sdcard.c)):
  `f_mkfs` with `n_fat = 1` and `align = 8192`, without `FM_SFD`, so the card gets a partition
  table with one partition. With `FF_LBA64 1` and `FF_MIN_GPT 0x10000000`, a volume of 128 GB or
  more gets **GPT**, and a smaller one gets **MBR**. A card formatted on a PC may have no
  partition table (a boot sector at sector 0), two FATs, or other cluster sizes. FatFs mounts
  all of these, so the checker has to read all of them.
- **Cluster counts:** f_mkfs uses 32 KB clusters on a 32 GB card, whether exFAT or FAT32
  (exFAT: 64 sectors per cluster from 512 K to 64 M sectors; FAT32: the `cst32` table). f_mkfs
  leaves 63 sectors before an MBR partition and `GPT_ALIGN` before a GPT one; the checker reads
  the partition table, so the gap doesn't matter. That is about 1 M clusters, so a bitmap of the
  whole card is 128 KB, too much to hold beside Wi-Fi, TLS and SSH. §6 handles this.
- **The encryption layer** sits in `sdcard.c`'s block hooks. It is set up by `detect()`, which
  runs when the card is initialized (`init_layer()`), not when FatFs mounts it. So
  `disk_initialize(0)` followed by `disk_read(0, …)` returns the decrypted volume with no FatFs
  mount at all. An encrypted card reads exactly like a plain one, offset by 8192 sectors.
- **Mount state:** `sdcard_mount()` is called on demand by `files.c`, SFTP (`ssh/sshd.c`), Lua,
  the editor, `man`, the TLS root loader (`tls/certstore.c`) and the log writer at start.
  `sdcard_mount()` **clears `unmounted_by_hand`**, so after `sd unmount` any of these mounts the
  card again; only the log writer's once-a-minute retry (`sdcard_mount_retry()`) respects the
  flag. `sd unmount` (console only, refused while `files_busy()`) does `f_unmount`, sets
  `unmounted_by_hand`, forgets the key (`crypt_off()`) and sets `STA_NOINIT`.

## 2. The rule: only unmounted

`sd check` runs only on an unmounted card. Enforced in `sdcard.c` like this:

1. **Refused while mounted.** If `card.state.mounted`, `sd check` prints
   `the card is mounted: run 'sd unmount' first` and does nothing. It never unmounts the card
   itself: unmounting is a separate, deliberate step that `sd unmount` already gates (console
   only, nothing open).
2. **Console only**, like `sd unmount` and `sd format`: it shuts every other user out of the
   card for the whole check.
3. **A `checking` flag** in `sdcard.c`, set for the whole check. While it is set:
   - `sdcard_mount()` and `sdcard_mount_retry()` return false without touching the card. With
     a shell, `sdcard_mount()` prints `the card is being checked (sd check)`.
   - `sd`/`sd info` (`cmd_info()` mounts), `sd format`, `sd test`, `sd speed` and `sd unmount`
     refuse with the same message.
   - `sdcard_owner_gone()` (run by `config reset`) turns the layer off (`crypt_off()`). After
     that the checker would read ciphertext. Under a check it doesn't refuse: the
     configuration is already erased when it runs. Instead it sets `check_stop`, and the job
     tests that flag **before every `fsck_step()`**. The main loop is single-threaded, so
     owner_gone can only run between steps, never inside one. The check then ends with
     `stopped: the configuration was erased (the card is locked now)`.
   - Callers already handle a failed mount, as they do when there's no card: file commands
     print an error, SFTP returns errors, the TLS root loader has no roots (an `http` request
     fails), and the log writer keeps lines in its buffers and writes them after the next mount. As
     whenever the card is out, lines that don't fit are dropped and counted (`log_file_lost()`).
4. **Starting:** the part of `mount()` that comes before `f_mount` (sdcard.c, from
   `disk_initialize(0)` through the `KIND_LOCKED`/`KIND_UNSUPPORTED` refusal) becomes one
   helper, `card_open()`, called by both `mount()` and the check. That way the check gets
   the same retries a mount does:
   - a card locked for lack of a key is looked at again once a key exists;
   - a card locked for lack of memory is tried again;
   - a card whose kind is `KIND_UNKNOWN` runs `detect()`.

   After `sd unmount`, `STA_NOINIT` is set, so `disk_initialize(0)` is a real initialization:
   `init_layer()` runs `detect()`, which opens an encrypted card's layer with the card key. A
   locked, unsupported or missing card ends the check with the reason (`kind_why`) and no
   problems reported.
5. **Ending** (finished, Ctrl-C, stopped, or an error): `crypt_off()` (forget the key),
   `STA_NOINIT` set, `checking` cleared, and **`unmounted_by_hand` set to true**. A check can
   start on a card that hasn't mounted since boot, where the flag is false, so the check sets
   it rather than leaving it as it was. Whoever just checked the card may be about to pull it,
   and the log writer's quiet retry shouldn't remount it under them. The card ends up as
   `sd unmount` leaves it: the next file command mounts it, and the quiet retry doesn't.
6. **Reads only** in the check phases. The checker gets a read callback and no write callback.
   Repair (§10) adds writes later, and only under the same rule.

So the checker never runs alongside FatFs: there is no cached FAT sector, no open file, and no
FSInfo or allocation bitmap update in flight that it could disagree with or corrupt.

## 3. Shape of the code

- **`src/files/fsck.c` / `fsck.h`: pure C, host tested.** It knows nothing of SPI, FatFs or
  the shell. It is given:

  ```c
  typedef struct {
      // Read `count` 512-byte sectors starting at `lba` (in the decrypted volume) into `buf`.
      bool (*read)(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
      // One problem found, already formatted ("/var/log/messages: size 70000 needs 3 clusters,
      // chain has 2"). Called at most FSCK_REPORT_MAX times; later problems are only counted.
      void (*report)(void *ctx, const char *line);
      // Write sectors: NULL for a check. Only repair (§10) passes one.
      bool (*write)(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);
      void *ctx;
      uint64_t sectors;         // size of the device as the layer shows it
  } fsck_io_t;

  typedef struct fsck fsck_t;   // the checker's state, allocated by fsck_new()

  fsck_t *fsck_new(const fsck_io_t *io, void *mem, size_t mem_len);
  // Do a bounded amount of work (at most `budget` sector reads). FSCK_MORE: call again;
  // FSCK_DONE: finished, see fsck_result(); FSCK_FAILED: couldn't go on (read error, no
  // volume), see fsck_result()->why.
  int fsck_step(fsck_t *f, uint32_t budget);
  const fsck_result_t *fsck_result(const fsck_t *f);
  ```

  All memory comes from `mem`: the caller decides how much (§6). No `malloc`, no recursion:
  the directory walk uses an explicit stack inside `mem`.
- **`sdcard.c`: the device side.** It handles `sd check`, enforces §2, allocates from
  `HEAP_FILES`, and runs the checker as a shell job. Each job step calls `fsck_step()` with a
  budget sized for about 20 ms (like `files.c`'s `STEP_US`), so SSH, the console and the
  watchdog keep running. Ctrl-C ends the job (§2.5). The read callback is
  `disk_read(0, …)`, and `io->sectors` is `get_num_sectors_layer()`: the card's size, less
  8192 sectors on an encrypted card.
- **Host:** `tests/test_fsck.c` runs the same code on image files (§8), and a fuzz target runs
  it on mutated images.

## 4. Finding the volume

The checker finds volume 0 the way FatFs's `find_volume()` and `check_fs()` do (ff.c), so that
it checks exactly the volume FatFs would mount:

- **VBR test** (`check_fs()`), applied to one sector:
  - *exFAT*: bytes 0–10 are `EB 76 90 "EXFAT   "` and bytes 510–511 are `55 AA`.
  - *FAT*: a jump byte (`EB`, `E9` or `E8`) at 0, and either `55 AA` with `"FAT32   "` at
    offset 82, or a plausible BPB.
  - *Not a VBR*: anything else, split into *a valid boot sector* (`55 AA`) or *invalid*.
1. **Sector 0** passes the VBR test: the volume starts at 0 (no partition table).
2. **GPT:** otherwise, if the first MBR entry's type byte is `0xEE`, the header at LBA 1 must
   pass `test_gpt_header()`: signature and version 1.0, header size 92–512, header CRC32, entry
   size 128, at most 128 entries. If it doesn't, there is no volume. The volume is the **first
   entry with the Microsoft basic data type GUID whose first LBA passes the VBR test**. FatFs
   doesn't check the entry array's CRC32. The checker does, and reports a mismatch as a
   problem, then goes on.
3. **MBR:** otherwise the type bytes are ignored, as FatFs ignores them. The checker takes the
   four entries' start LBAs in order, skips any that are 0, and uses the first that passes the
   VBR test.

Failing to find a volume ends the check with `FSCK_FAILED: no FAT32 or exFAT volume`.

Then the boot sector. Its checks fall into two groups, the same split FatFs makes.

**Refused** (`FSCK_FAILED`, with the reason). These are the things FatFs itself refuses to
mount, so nothing further can be trusted:

- bytes per sector not 512 (`FF_MAX_SS`);
- sectors per cluster 0 or not a power of two;
- reserved sectors 0;
- number of FATs not 1 or 2;
- FAT too small for the cluster count;
- cluster count 0;
- the volume not fitting in `io->sectors`.

For FAT, a cluster count of **65 525 or fewer** is FAT12/16 (`MAX_FAT16`, ff.c): FatFs would
mount it, but SOS never makes one, so it is refused as `not FAT32 or exFAT`. FAT32 is 65 526 up
to `0x0FFFFFF5` clusters. On exFAT the FAT offset and length, the cluster heap offset and count,
and the root directory cluster must be in range.

**Reported, then the check goes on from the primary copy.** These are redundancy mismatches:
FatFs mounts the volume whatever their state, so refusing would refuse cards FatFs uses every
day.

- **FAT32:** the backup boot sector (normally sector 6) differs from sector 0. The FSInfo
  signatures are missing or wrong; FatFs then only distrusts the free count and the
  next-free hint.
- **exFAT:** the boot region checksum. That is sector 11 of the 12-sector boot region,
  computed over sectors 0–10 as the spec says; only f_mkfs writes it, and `mount_volume()`
  computes none. Also the backup boot region (sectors 12–23) differing from the main one.
  The `VolumeFlags` dirty bit is read and noted (§7).
- **GPT:** the entry array's CRC32 (above).

## 5. What is checked

### Both kinds

For every directory entry, from the root down:

- The chain (§6.1): every cluster number is in range (2 … count+1), the chain doesn't loop, and
  it ends in an end-of-chain mark. Reaching a free entry (0) or a bad-cluster mark
  (`0x0FFFFFF7` / `0xFFFFFFF7`) is a broken chain.
- **Size against chain:** a file needs `ceil(size / cluster_size)` clusters (0 for size 0, with
  start cluster 0). A chain that is shorter means data is lost. A chain that is longer means
  clusters are wasted (a common result of an interrupted write; harmless but counted).
  **FAT32 directories are exempt.** Their entries carry size 0, so a directory's chain is only
  checked for range, loops and its end mark. exFAT directories have a `DataLength` and are
  checked like files.
- **Directories:** a directory's chain is walked as entries. A directory nested more than
  `FSCK_DEPTH_MAX` (64) levels deep is reported and not entered. That also catches a
  subdirectory whose start cluster points back at an ancestor, which is also checked directly
  against the start clusters on the stack.
- **Names:** characters FatFs refuses (`"*/:<>?\|` and controls), a name repeated in the same
  directory (case-insensitive, as FatFs compares them), `.`/`..` entries where they don't
  belong.
- **Cluster accounting (§6):** clusters used by two entries (*cross-linked*), clusters marked
  used that nothing reaches (*lost*), and clusters reached but marked free. A FAT32 cluster
  marked bad (`0x0FFFFFF7`) is neither lost nor free. Bad clusters are counted in the summary,
  never reported as lost, and never freed (§10.1).
- **Free count:** FAT32's FSInfo free count, when it isn't `0xFFFFFFFF` (unknown), against the
  free clusters counted. A mismatch is a note, not a problem: FatFs recounts on mount when it
  doesn't trust it. exFAT has no stored free count.

### FAT32

- Short entries: the long-name entries before a short entry have the right sequence numbers
  (last one flagged `0x40`, counting down to 1) and their checksum matches the short name. An
  orphaned long-name entry is reported.
- `.` points at the directory itself, `..` at its parent (0 when the parent is the root).
- With two FATs: the copies are compared sector by sector, once, during the first pass.
- Entries 0 and 1 of the FAT: the media byte, and the clean-shutdown and no-error bits of
  entry 1 (reported, §7).

### exFAT

- **Entry sets:** a File entry (`0x85`), then a Stream Extension (`0xC0`), then
  `ceil(name_length / 15)` File Name entries (`0xC1`). The secondary count covers them all. The
  set checksum (the File entry's `SetChecksum`, computed over the set without its own two
  bytes) matches. The name hash in the stream entry matches the up-cased name. `ValidDataLength`
  ≤ `DataLength`.
- **NoFatChain** (the stream entry's `GeneralSecondaryFlags` bit 1): the file is contiguous from
  its first cluster for `ceil(DataLength / cluster_size)` clusters, and the FAT entries for it
  are not looked at (the spec says they are undefined). Without the flag the chain is followed
  through the FAT.
- **Root directory critical entries:** exactly one Allocation Bitmap entry (`0x81`; FatFs uses
  one bitmap, `NumberOfFats` = 1) and one Up-case Table entry (`0x82`) with a matching table
  checksum. The bitmap and the up-case table count as used clusters. A volume label entry
  (`0x83`) is optional.
- **Bitmap against chains:** the bitmap's set bits are compared with the clusters reached. A
  cluster reached but clear in the bitmap is *reached but free*; set but not reached is *lost*.
  On exFAT the bitmap, not the FAT, is the record of what is in use.
- Unknown critical entry types (bit 5 clear, not one of the above) are reported. Unknown benign
  types are skipped, as FatFs does.

## 6. Cluster accounting in windows

### 6.1 Following a chain

A chain is followed through a one-sector FAT cache (128 entries on FAT32, the same on exFAT).
Loops are caught without memory by counting steps: a chain longer than the cluster count has a
loop. When the step count passes the file's own expected length, the checker switches to Brent's
cycle detection, which keeps two cluster numbers. So a looping chain costs at most about twice
its length in reads before it is reported, and a long, correct chain costs nothing extra.

### 6.2 Windows

A bit per cluster for the whole card doesn't fit (§1). The checker therefore accounts for
clusters one **window** at a time: a bitmap of `W` bytes covers `8W` clusters, and the clusters
are covered in `ceil(count / 8W)` passes:

- **Pass 1** does everything in §5 that isn't accounting (the per-entry checks, the boot region,
  the FAT copy comparison) and marks the clusters reached in window 1.
- **Passes 2 …** walk the tree again and only mark clusters. Per-entry problems are reported
  once, in pass 1.
- In each pass a cluster is marked when a chain (or a NoFatChain run) reaches it. A bit that is
  already set is a **cross-link**, reported with the path of the second entry to reach it (one
  line per later entry, in the pass whose window holds the shared run's last cluster, so the
  lines don't depend on the window size). On exFAT a NoFatChain run can share any part of
  another entry's clusters, so the shared clusters needn't include either entry's last one:
  an entry is named when its own last cluster was already reached; when none can be named,
  one line gives the count, and the summary always counts every shared cluster (`fsck.h`). When a pass ends, the window is
  compared with the FAT (FAT32: entry non-zero and not marked → lost) or with the
  allocation bitmap (exFAT, same rule). Lost clusters are counted, and reported as runs
  (`lost: clusters 81920-81935 (512 KB)`), up to the report limit.
- **Window size:** `sdcard.c` asks `HEAP_FILES` for 16 KB, and falls back to 8 KB and then 4 KB.
  If 4 KB isn't available it refuses, naming what it needs: `fsck_state_size()` plus 4 KB,
  about 10 KB. On a 32 GB card with 1 M clusters, 16 KB means 8 passes, 4 KB means 32. The
  checker's own state (`sizeof(fsck_t)`, 7.0 KB on the board since phase 2) holds the
  directory stack (64 levels × 28 bytes), the sector buffers, two name buffers (255 UTF-16
  characters each), the repeated-name filter, exFAT's entry set reader, the path and the report
  line. The encryption layer's 7.5 KB is allocated by
  `detect()` as for any mount.
- **Time:** each pass reads every directory and every FAT sector that a chain passes through,
  plus its window's share of the FAT or bitmap. A NoFatChain exFAT file costs no FAT reads, and
  FatFs writes exFAT files contiguously when it can. **Measured on dev-1** (2026-11-27): exFAT,
  7.5 GB, 246 528 clusters of 32 KB, encrypted, a nearly empty card (`/var` and `/man`, about
  60 files): 2 passes in 0.45 s with the 16 KB window, 8 passes in 0.67 s with 4 KB. Pass 1 is
  the slowest: it also reads the names and the up-case table. A full card will be slower,
  chiefly by its directory and FAT reads, and how much is **unmeasured**. The check shows
  progress (`pass 3/8`) on the console.

## 7. Output

```
sd check
checking exFAT, 29.7 GB, 970752 clusters of 32 KB, encrypted (8 passes)
/var/log/messages: 3 clusters for 70000 bytes, chain has 2
lost: clusters 81920-81935 (512 KB)
pass 8/8
2 problems; 512 KB lost, 0 cross-linked; volume flagged dirty
```

- One line per problem, the path first when there is one. After `FSCK_REPORT_MAX` (50) lines,
  only a count. The summary line always comes last: `no problems` or `N problems`, followed by
  the totals.
- **Notes, not problems:** a stale FSInfo free count, extra clusters on a chain longer than its
  size, the dirty or clean-shutdown flags. FatFs never sets those flags, so on a card only
  written by SOS they only show that a PC or camera left the card dirty.
- The summary also goes to the log (`sd: check: …`) once the card is mounted again. During the
  check, log lines wait in the log writer's buffers, as they do whenever the card is out.
- `sd check` never writes to the card. Repair is a separate command, `sd check fix` (§10).

## 8. Tests

- **Images without host tools.** No mkfs or fsck tools are installed on the dev machine, and the
  checker should agree with what the board writes anyway. So `tests/test_fsck.c` compiles
  FatFs's `ff.c` and `ffunicode.c` for the host over a RAM disk (`diskio` in the test). Images
  are made with `f_mkfs` using the same `MKFS_PARM` as `sd format` (FAT32 and exFAT, with and
  without a partition table, one GPT case at a size over `FF_MIN_GPT`, written sparse), and
  filled with `f_write`/`f_mkdir`. FatFs is a pure C library, so this is the same formatter and
  writer as on the board. `ffconf.h` has no board includes and `FF_FS_REENTRANT` is 0;
  `FF_USE_LFN 3` needs `ff_memalloc`/`ff_memfree`, from `ffsystem.c` or two lines in the test.
- **Damage, one kind per case**, written directly into the image, each with the exact problem
  line expected: lost chain, cross-link (two files sharing a cluster), size larger and smaller
  than the chain, chain into a free cluster, chain loop, directory loop to an ancestor, bad
  cluster number, wrong exFAT set checksum, wrong name hash, bitmap bit cleared under a file,
  bitmap bit set for nothing, NoFatChain file running past the heap, broken boot region
  checksum, backup boot region different, orphaned long-name entry, FAT copies different, GPT
  header CRC wrong. Plus a clean image of every kind, which must report `no problems`.
- **Windows:** every case also runs with the smallest window (4 KB, or less in the test), so the
  multi-pass path is checked against the single-pass result.
- **Agreement with fsck tools** when they're installed (`fsck.fat -n`, `fsck.exfat -n`, from
  dosfstools and exfatprogs): the test skips this part without them. It's a cross-check, not
  the specification.
- **Fuzz:** `tests/fuzz/fuzz_fsck.c` runs the checker on a mutated small image, with seeds from
  the test images (`seeds.py`). The corpus replay runs in ctest like the others. The checker
  reads untrusted bytes, so it must not crash, loop forever (the step budget and §6.1 bound
  the work) or read outside its memory.
- **Board** (phase 3, in a dev-1 slot): check the dev-1 card. Then pull power during a `cp`,
  unmount, check, and compare with the same image checked on a PC (decrypted with
  `tools/host/sdcrypt.c`). Also check that `ls`, SFTP and `http` fail cleanly during a check and
  that the log writer's buffered lines arrive afterwards.

## 9. Phases

1. **Core and FAT32** (`fsck.c`, `fsck.h`, §4 for FAT32, MBR, GPT and no partition table, §5
   both-kinds and FAT32 checks, §6). `test_fsck.c` with the host FatFs image builder, the FAT32
   damage cases, and the fuzz target. Host only; no board.
2. **exFAT:** §4 and §5 for exFAT, the exFAT damage cases. Host only. *Done:* `fsck_exfat.c`
   (2a) and its walk in `fsck.c` (2b); `fsck.h` and `fsck_exfat.h` list the rules decided.
   Departures from this note: on exFAT a cross-linked entry is named only when its last cluster
   is shared (a count otherwise, §6.2), clusters reached but clear in the bitmap are reported
   too, and the dirty flag, a media byte that doesn't match FAT entry 0 and an up-case table
   unlike FatFs's are notes.
3. **`sd check` on the device:** §2 (the `checking` flag in `sdcard_mount()`,
   `sdcard_mount_retry()` and the other `sd` commands), §3's job, §7's output, the `sd` man page
   (`man/sd.txt`, then the regenerated bundle and `docs/commands.md`), [docs/files.md](../files.md)
   and [src/files/README.md](../../src/files/README.md). Board-tested in a dev-1 slot, including
   the timing in §6.2.

4. **Repair on the host:** the fault-injection harness of §10.4 (write counting, cut and failure
   after N writes, the random damage generator) comes first, in the same PR or one before it.
   Then the repairs of §10.1 with the write order of §10.2. Host only. *Built*, FAT32 first
   (#238) and exFAT on top (`src/files/fsck_repair.c`, `tests/test_fsck_repair.c`; the rules it
   settles are in `fsck_repair.h` and §10.1, §10.2). exFAT reuses FAT32's trim hooks; its other
   hooks in `fsck.c` are the bitmap comparison (one write per bitmap sector and pass), the
   volume, and two more places where the walk can't see all that FatFs reads (a directory too
   deep, a chain breaking on a free or bad entry).
5. **`sd check fix` on the device:** §10.3's command, the test-build-only fault commands of
   §10.4, man page and docs. Board-tested in a dev-1 slot, with injected faults. *5a, the fault
   commands (`sd fault`, test builds only), is merged (#231)*; 5b, the command itself, is to
   come.

Each phase is a PR that leaves main working. Phases 1, 2 and 4 are host code only. `fsck.c`
enters the firmware image in phase 3, and repair's code reaches the board in phase 5b.

## 10. Repair

Decided (#213): repair is in scope, in its own PRs after the checker, and it is tested by
injecting faults. Everything in §2 still holds: the card stays unmounted, so repair's writes
can't race FatFs's cache. That is the main reason for checking unmounted.

### 10.1 What is repaired

Only fixes that can't lose data that is still reachable:

- **Lost clusters** are freed: on FAT32, FAT entries set to 0; on exFAT, bitmap bits cleared
  (the FAT entries of free clusters don't matter on exFAT). FAT32 bad-cluster marks
  (`0x0FFFFFF7`) are left alone.
- **Chains longer than their file** are trimmed to `ceil(size / cluster_size)` clusters: an
  end-of-chain mark at the last needed cluster, then the tail freed, but only where nothing
  else uses it (§10.2). For exFAT this covers files with a FAT chain and, for NoFatChain files,
  bitmap bits past the run. FAT32 directories (size 0, §5) are never trimmed.
- **exFAT clusters reached but clear in the bitmap** get their bitmap bit set, so FatFs won't
  hand that cluster to another file later. This is done even when frees are skipped (below):
  it only marks used a cluster something reaches.
- **On exFAT** the end mark of a trim is `0xFFFFFFFF` (all 32 bits), written to the one FAT;
  the tail's bits are then cleared as lost clusters, and its FAT entries are left (FatFs doesn't
  read the entries of free clusters). A NoFatChain file has no chain to end: its clusters past
  the run are lost clusters already, and only their bits are cleared. Directories, the bitmap
  and the up-case table aren't trimmed. There is no FSInfo and no second FAT.
- **FAT32's FSInfo free count** is set to `0xFFFFFFFF` (unknown) before any other write, so
  FatFs recounts on its next mount.
- **A second FAT copy** (FAT32 with two FATs) gets every change written to the first copy:
  the first copy's whole sector, as FatFs's `sync_window()` writes it. A sector where the second
  copy differs from the first is repaired from the first, since FatFs reads only the first copy
  and would overwrite the second at its next write there.

Left alone, though §10.1 would otherwise repair them:

- **When part of the tree wasn't walked**, nothing is freed or trimmed: a directory whose chain
  loops or breaks (FatFs still reads its entries up to the break, and those of the cluster it
  breaks on), a directory nested deeper than the checker reads, or a walk stopped because
  chains overlap too much. What such a directory holds would look lost. FSInfo and the second
  FAT are still repaired, and exFAT bits are still set.
- **On exFAT, when any chain breaks on a free or bad FAT entry**, nothing is freed or trimmed
  either. FatFs reads the cluster the chain breaks on, but the checker doesn't count it (§5),
  so it looks lost; on FAT32 that cluster is already free or marked bad, but on exFAT its bit
  is what keeps FatFs from handing it to another file.
- **An empty file with a chain** (size 0, a start cluster) isn't trimmed: trimming it to no
  clusters would mean writing its directory entry. It stays a note.
- **FSInfo** is written only when it is valid, lies inside the reserved area (else it is a FAT
  sector too), isn't already `0xFFFFFFFF`, and is either stale or the repair writes anything
  else.

Only reported, never changed: cross-links, chains shorter than their file or broken, damaged
entry sets or names, a damaged boot region, GPT or MBR. Each of those needs a choice about whose
data wins, and that is for a person with a PC and a backup.

### 10.2 Write order

Every repair is a sequence of single-sector writes, ordered so that **stopping after any
prefix of the sequence leaves the volume no worse than before**: a check then reports a subset
of the original problems, plus at most:

- (a) a "FAT copies differ" note;
- (b) lost clusters that are the tail of a chain being trimmed: its end mark was written and
  the tail not yet freed. That costs space, not data, and running repair again frees it.

Every file still reads the same bytes as before the repair. (b) can't be avoided: no order of
single-sector writes ends a chain early and frees a tail that runs past the end mark's FAT
sector without a lost or a broken chain in between, and a broken chain would be worse.

- FSInfo is invalidated first.
- A trim writes the new end-of-chain mark before freeing anything. Until the tail is freed,
  it is a run of lost clusters, which the checker already handles.
- On FAT32, the first FAT copy is written before the second.
- On exFAT a bit is set before any bit is cleared in the same sector, and a sector's new
  bitmap state is written in a single write: sets and clears land together. Each bit changed
  takes one cluster from a problem to none (a set only for a cluster something reaches, a clear
  only for one nothing reaches), whatever the other bits, so any prefix of the writes leaves a
  subset of the problems, counted cluster by cluster: a run of lost or reached-but-free
  clusters that crosses a sector or a window may be left in part, and is then reported shorter.
  The order of sectors doesn't matter. A sector that two windows share is written in each of
  the two passes, the part of it in that pass's window.

Repair runs inside the passes of §6.2.

- **A full check comes first.** `sd check fix` first runs an ordinary check (all passes, no
  writes) inside the same job, under the same `checking` flag, so the card can't change
  between it and the repair. Its summary is the plan shown before the `yes` (§10.3).
- **Trims happen only if that check found no cross-link anywhere.** When chains share
  clusters, FAT's one-successor-per-cluster rule means they share everything from the merge
  point on. If the merge point is at or before a file's last needed cluster L, the new
  end-of-chain mark at L would also cut the other file short. A per-cluster "tail" tag
  doesn't help, because the harm is the end mark itself, not the freeing. With cross-links
  present, trims are skipped and the summary says so (`trims skipped: cross-linked files,
  fix those on a PC first`). Freeing lost clusters and setting exFAT bitmap bits still
  happen: neither touches a cluster anything reaches.
- **With no cross-links**, every cluster belongs to at most one chain. In pass 1, when a chain
  turns out longer than its file, the end-of-chain mark at L is written at once. That is safe:
  the tail becomes a lost run, which is a state the checker already handles. The tail isn't
  marked, so it is freed as lost clusters in the passes whose windows hold it.
- **A second repair run:** pass 1 marks a chain's clusters in window 1 before the walk knows
  the chain is too long, so the first repair run doesn't free the part of a trimmed tail that
  lies in window 1. When it trimmed anything, a second repair run of pass 1 alone frees it (it
  only frees: nothing else is left to do). Without trims there is one repair run.
- **Lost clusters** are freed at the end of the pass whose window holds them, a FAT sector at a
  time, before the checker's FAT cache moves to the next sector.

When the last pass ends, a full check runs again by itself, and its summary is what
`sd check fix` reports. It must show no problem of a kind §10.1 repairs.

### 10.3 The command

`sd check fix`: console only and only unmounted, like `sd check`. It runs the check of §10.2,
prints what it found and what it will fix (or that there is nothing to fix), explains that
cross-links and damaged entries are only reported, then asks for `yes`, as `sd format`
does. The `checking` flag stays set from the first check, through the question and the repair,
to the final check; answering anything but `yes` ends it there. A read or write error stops
repair at once: no further writes, and the card is left as §10.2 guarantees. The summary and every fix (as a count per kind) go to the
log.

### 10.4 Fault injection

Repair is tested by injecting two kinds of fault: damage to the volume before repair, and
failures of repair's own writes.

- **Damage catalogue:** every §8 damage case, including an over-long chain cross-linked with
  a later file (the trim must be skipped and both files must read the same), with repair run on it. After repair the check
  must show no problem of a repaired kind. Files the damage didn't touch must read the same
  bytes as before the damage (the test keeps a hash of each). Host FatFs must mount the
  volume, write a new file and read everything back.
- **Random damage:** a seeded generator applies one to four faults of the repairable kinds at
  random places (lost chains, over-long chains, bitmap bits cleared under files, stale FSInfo,
  FAT copies different), mixed with faults that are only reported. Same properties as above,
  a few hundred seeds per ctest run, and the seed is printed when a case fails.
- **Cut after N writes:** the test's write callback counts writes. For every N from 0 up to
  the repair's total, it repairs a fresh copy of the damaged image and stops after write N,
  as a power cut would. Then it checks that §10.2's guarantee holds, runs repair again to the
  end, and checks the result is clean of repaired kinds.
- **Failed write N:** the same, but write N returns an error. Repair must stop with nothing
  written after it.
- **Read errors:** a read callback that fails at read N. Repair must stop, and a later check
  must show no change.
- **Fuzz:** `fuzz_fsck` gets a repair mode: repair a mutated image on a RAM disk, then check it
  again. It must not crash, must not loop, and must leave no problem of a repaired kind.
- **On the board** (phase 5): a test-only command, built only with a test option as the OOM
  test tool's `mem hold` was (#190), such as `sd fault fail <N>` (the layer fails the Nth write
  from now) and `sd fault reboot <N>` (watchdog reboot at the Nth write, the nearest thing to a
  power cut that can be scripted). The board run damages the card with real power cuts during
  `cp` and log writes, checks, repairs with each fault armed in turn, and checks again. A copy
  of the card is checked on a PC (decrypted with `tools/host/sdcrypt.c`) before and after.
- **Not simulated:** a torn sector, where a power cut inside one sector write leaves it
  half-written. SD cards don't promise that a sector write is atomic, so §10.2 can only reduce
  that risk, not exclude it. A card that has lost power during a repair should be checked
  again before use.

## 11. Later, not in these phases

- **Dirty marking:** SOS sets the volume's dirty flag (exFAT `VolumeFlags`, FAT32 FAT entry 1)
  while files are open for writing and clears it at close and unmount. The next mount after a
  power cut then sees the flag, and prints and logs `the card wasn't unmounted cleanly: consider
  sd check`. Not automatic: a check needs the card unmounted and takes a while, and the log
  writer wants the card right after boot.
- **Reading data clusters** to find unreadable sectors (`sd test` already reads and writes the
  card).
- **FAT12/FAT16:** SOS never formats them, and cards over 2 GB don't use them.

## 12. Decisions

Recorded on #213 (the user's, 2026-11-27):

- **Console only** (§2.2), matching `sd unmount` and `sd format`.
- **Window: 16 KB** by default (§6.2), falling back to 8 KB and 4 KB.
- **Repair** (§10): in scope, in its own PRs after the checker (phases 4 and 5), tested by
  injecting faults (§10.4).
