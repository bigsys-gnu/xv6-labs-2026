# xv6 Locality-Aware Allocation — Implementation Guide for Claude Code

## Overview

Modify `balloc()` in `kernel/fs.c` to accept a **hint** block number and search
near that hint first (Pass 1) before falling back to first-fit (Pass 2).
Modify `bmap()` to compute and pass a hint to `balloc()` when extending a file.

**Prerequisite:** Lab 1 (fsdump) and Lab 2 (get_fragmentation) must be implemented.  
**Repository:** xv6-riscv (riscv branch)  
**Run with:** `make clean && make qemu CPUS=1`  
**Verify:** `$ loctest` — post-aging score should drop from ~70% to ~20%

---

## Files to Change

| File | Action |
|------|--------|
| `kernel/param.h` | Add `#define BALLOC_WINDOW 32` |
| `kernel/fs.c` | Modify `balloc()` signature + body |
| `kernel/fs.c` | Modify `bmap()` — 3 call sites |
| `user/loctest.c` | **Create new** verification program |
| `Makefile` | Add `$U/_loctest\` to UPROGS |

---

## 1. Edit `kernel/param.h`

Add one line anywhere in the file:

```c
// BALLOC_WINDOW: preferred block search range for locality-aware allocation.
// balloc() first searches blocks [hint, hint+BALLOC_WINDOW) before
// falling back to first-fit.  32 = 32 KB window, good balance of
// locality benefit vs. Pass 1 overhead.
#define BALLOC_WINDOW  32
```

---

## 2. Modify `balloc()` in `kernel/fs.c`

**Replace** the entire existing `balloc()` function with this version.

The key change: add a `uint hint` parameter and a two-pass search.

```c
// balloc(dev, hint) — Locality-Aware Block Allocation
//
// Allocates one free disk block and returns its block number.
// Returns 0 if the disk is full.
//
// Two-pass search:
//   Pass 1 (preferred): search blocks [hint, hint+BALLOC_WINDOW)
//                       → keeps file blocks near each other on disk
//   Pass 2 (fallback):  original first-fit scan from block 0
//                       → guarantees allocation always succeeds
//
// hint == 0 means "no preference" — Pass 1 is skipped entirely.
// Pass 2 is identical to the original xv6 balloc().
static uint
balloc(uint dev, uint hint)
{
  int b, bi, m;
  struct buf *bp;

  // ════ PASS 1: Preferred window search ════════════════════════════
  //
  // Only enter Pass 1 when the caller has a preference (hint > 0).
  // hint is typically: (address of the previous block of this file) + 1.
  // Searching near hint keeps a file's blocks physically adjacent,
  // reducing seek time on rotational disks and fragmentation score.
  if(hint > 0) {
    uint lo = hint;                  // window start = hint
    uint hi = hint + BALLOC_WINDOW;  // window end   = hint + 32
    if(hi > sb.size)
      hi = sb.size;  // clamp to disk boundary

    // Align b to the start of the bitmap block covering lo.
    // (lo/BPB)*BPB rounds lo down to the nearest BPB multiple,
    // so we always start reading from a complete bitmap block.
    for(b = (lo / BPB) * BPB; b <= hi; b += BPB) {
      bp = bread(dev, BBLOCK(b, sb));

      for(bi = 0; bi < BPB && b + bi < sb.size; bi++) {
        // Skip blocks that are outside our preferred window.
        if(b + bi < lo || b + bi > hi)
          continue;

        // Check if block b+bi is free (same bitmap test as original balloc).
        //   bi/8:    which byte in bp->data[] holds this block's bit
        //   bi%8:    which bit within that byte
        //   1<<(bi%8): mask with exactly that bit set
        //   & m == 0:  bit is 0 → block is free
        m = 1 << (bi % 8);
        if((bp->data[bi/8] & m) == 0) {
          // Found a free block inside the window.
          // Mark it as allocated: set the bit to 1.
          bp->data[bi/8] |= m;
          log_write(bp);      // persist the bitmap change via the log
          brelse(bp);         // release the buffer cache entry
          bzero(dev, b + bi); // zero the block (security + correctness:
                              //   new owner must not see old content)
          return b + bi;      // ← success: return the allocated block number
        }
      }
      brelse(bp);  // no free block in this bitmap block — try next
    }
    // Pass 1 failed: no free block found in [hint, hint+BALLOC_WINDOW).
    // Fall through to first-fit.
  }

  // ════ PASS 2: First-fit fallback ═════════════════════════════════
  //
  // Identical to the original xv6 balloc() body.
  // Scans the bitmap from block 0, returns the FIRST free block found.
  // Guarantees allocation succeeds as long as any free block exists.
  //
  // Note: Pass 2 may revisit blocks already checked in Pass 1.
  // That is intentional — Pass 1 may have skipped free blocks that
  // were outside the preferred window but are still valid choices.
  for(b = 0; b < sb.size; b += BPB) {
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0) {
        // Free block found via first-fit.
        bp->data[bi/8] |= m;
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }

  printf("balloc: out of blocks\n");
  return 0;  // disk is full
}
```

---

## 3. Modify `bmap()` in `kernel/fs.c`

Update the three `balloc()` call sites inside `bmap()` to pass a hint.

### 3a. Direct block allocation (bn < NDIRECT)

Find this existing code block in `bmap()`:

```c
if(bn < NDIRECT){
  if((addr = ip->addrs[bn]) == 0){
    addr = balloc(ip->dev);
    ...
  }
```

Replace the `balloc()` call with hint-passing logic:

```c
if(bn < NDIRECT) {
  if((addr = ip->addrs[bn]) == 0) {
    // Compute hint: the block right after the previously allocated
    // direct block of this file.
    //
    // If bn == 0 (first block of file):       hint = 0 (no preference)
    // If bn > 0 and addrs[bn-1] is allocated: hint = addrs[bn-1] + 1
    //   → tells balloc() to search right after the previous block
    // If bn > 0 but addrs[bn-1] == 0 (hole):  hint = 0 (no preference)
    //
    // Example: bn=3, addrs[2]=89
    //   hint = 90 → balloc searches [90..121] first
    //   If block 90 is free → file gets [87,88,89,90] → consecutive!
    uint hint = (bn > 0 && ip->addrs[bn-1]) ? ip->addrs[bn-1] + 1 : 0;

    addr = balloc(ip->dev, hint);
    if(addr == 0)
      return 0;
    ip->addrs[bn] = addr;
  }
  return addr;
}
```

### 3b. Indirect block itself (allocating addrs[NDIRECT])

Find this code inside the `bn >= NDIRECT` branch:

```c
if((addr = ip->addrs[NDIRECT]) == 0){
  addr = balloc(ip->dev);
  ...
}
```

Replace `balloc(ip->dev)` with `balloc(ip->dev, 0)`:

```c
if((addr = ip->addrs[NDIRECT]) == 0) {
  // Allocate the indirect block itself.
  // No hint: we have no preference for where the indirect block lives.
  // (It is metadata, not file data — locality is less critical here.)
  addr = balloc(ip->dev, 0);
  if(addr == 0)
    return 0;
  ip->addrs[NDIRECT] = addr;
}
```

### 3c. Indirect data block (entries inside the indirect block)

Find this code inside the `bn < NINDIRECT` section:

```c
if((addr = a[bn]) == 0){
  addr = balloc(ip->dev);
  ...
}
```

Replace with hint-passing logic:

```c
if((addr = a[bn]) == 0) {
  // Compute hint for an indirect data block.
  //   Same principle as 3a, but using the indirect block's entry array a[].
  //   bn here is already rebased: "bn -= NDIRECT" was applied above,
  //   so bn=0 means the first entry in the indirect block (logical block 12).
  //
  // If bn == 0 (first indirect entry):      hint = 0 (no previous entry)
  // If bn > 0 and a[bn-1] is allocated:     hint = a[bn-1] + 1
  // If bn > 0 but a[bn-1] == 0 (hole):      hint = 0
  uint hint2 = (bn > 0 && a[bn-1]) ? a[bn-1] + 1 : 0;

  addr = balloc(ip->dev, hint2);
  if(addr) {
    a[bn] = addr;
    log_write(bp);  // persist the indirect block update
  }
}
```

---

## 4. Verify All Call Sites

After making the changes, confirm there are no remaining old-style calls:

```bash
grep -n "balloc(ip->dev)" kernel/fs.c
```

This should return **zero results**.
All three call sites should now be either:
- `balloc(ip->dev, hint)`
- `balloc(ip->dev, 0)`
- `balloc(ip->dev, hint2)`

If the compiler reports "too few arguments to function balloc", find and fix
the remaining old-style call.

---

## 5. Create `user/loctest.c`

Create this verification program from scratch.

```c
// user/loctest.c — Locality-Aware Allocation Verification
//
// Measures fragmentation before and after an aging workload.
// Run this program TWICE:
//
//   Run 1 — original first-fit (before Lab 3 changes):
//     make clean && make qemu CPUS=1   ← original balloc
//     $ loctest                         ← record baseline score
//
//   Run 2 — locality-aware (after Lab 3 changes):
//     make clean && make qemu CPUS=1   ← new balloc with hint
//     $ loctest                         ← compare with baseline
//
// Expected improvement: post-aging score drops from ~60-80% to ~10-35%.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Helper: print a 20-char ASCII fragmentation bar.
static void
print_bar(int score)
{
  int f = score * 20 / 100;
  printf("  [");
  for(int i = 0; i < 20; i++) printf(i < f ? "#" : ".");
  printf("]  %d%%\n", score);
}

// Helper: create a file, write nblocks × 1 KB, measure fragmentation.
// Opens with O_RDWR so get_fragmentation() can read the file descriptor.
static int
measure(char *name, int nblocks)
{
  char buf[1024];
  memset(buf, 0xCC, sizeof(buf));  // arbitrary fill pattern

  int fd = open(name, O_CREATE|O_RDWR);
  if(fd < 0) {
    printf("loctest: cannot create %s\n", name);
    exit(1);
  }
  // Each write() may cross a block boundary, triggering bmap() → balloc().
  // The hint passed to balloc() comes from the previous block's address.
  for(int i = 0; i < nblocks; i++)
    write(fd, buf, sizeof(buf));

  // get_fragmentation() traverses ip->addrs[] and the indirect block,
  // counts non-consecutive adjacent pairs, returns 0-100%.
  int score = get_fragmentation(fd);
  close(fd);
  return score;
}

int
main(void)
{
  char name[16];
  char buf[1024];
  memset(buf, 0xAA, sizeof(buf));

  printf("=== Locality-Aware Allocation Test ===\n\n");

  // ── Phase A: Baseline (fresh disk) ───────────────────────────────
  //
  // On an empty disk, both first-fit and locality-aware allocate
  // consecutively from block 47 — the hint just happens to equal the
  // next available block.  Score should be 0% in both cases.
  int fresh = measure("target.txt", 12);
  printf("Fresh disk, 12-block file:\n");
  print_bar(fresh);
  printf("  Expected: 0%% (consecutive on empty disk)\n\n");

  // ── Phase B: Create disk holes (aging) ───────────────────────────
  //
  // Creates files of varying sizes (2-4 blocks) and deletes every
  // third one, producing irregular free-block patterns in the bitmap.
  // Irregular holes better stress-test locality-aware allocation
  // compared to the uniform 3-block holes in aging.c.
  for(int i = 0; i < 30; i++) {
    snprintf(name, sizeof(name), "a%d", i);
    int fd = open(name, O_CREATE|O_WRONLY);
    int sz = 2 + (i % 3);  // 2, 3, or 4 blocks per file
    for(int b = 0; b < sz; b++) write(fd, buf, 1024);
    close(fd);
  }
  for(int i = 0; i < 30; i += 3) {
    snprintf(name, sizeof(name), "a%d", i);
    unlink(name);  // delete every third file → irregular holes
  }
  printf("Aging: 30 files created, 10 deleted (irregular holes).\n\n");

  // ── Phase C: Measure post-aging fragmentation ─────────────────────
  //
  // A 12-block file written into an aged disk:
  //
  // First-fit:
  //   balloc() always starts from block 0.
  //   Fills holes left by deleted files in low-address first order.
  //   Blocks end up scattered: [47,48,53,54,59,60,65,66,71,72,77,78]
  //   Many transitions → high score (60-85%).
  //
  // Locality-aware:
  //   For block N, hint = (block N-1 address) + 1.
  //   Pass 1 searches near the previous block first.
  //   If the next-door block happens to be free → consecutive allocation.
  //   Fewer transitions → lower score (10-35%).
  int aged = measure("target2.txt", 12);
  printf("Post-aging, 12-block file:\n");
  print_bar(aged);
  printf("  First-fit expected:        60-85%%\n");
  printf("  Locality-aware expected:   10-35%%\n\n");

  // ── Result ─────────────────────────────────────────────────────────
  if(aged == 0)
    printf("Score: 0%% — perfect! (disk may not be sufficiently aged)\n");
  else if(aged < 35)
    printf("Result: GOOD  — locality-aware is working correctly.\n");
  else if(aged < 60)
    printf("Result: PARTIAL — some improvement. Check BALLOC_WINDOW value.\n");
  else
    printf("Result: CHECK — high score. Re-verify balloc() and bmap() changes.\n");

  return 0;
}
```

---

## 6. Edit `Makefile`

Add one line to the `UPROGS` block:

```makefile
UPROGS=\
    ...existing entries...\
    $U/_loctest\
    ...existing entries...\
```

---

## Expected Output

```
─── BEFORE Lab 3 (original first-fit) ───────────────────────────────

$ loctest
=== Locality-Aware Allocation Test ===

Fresh disk, 12-block file:
  [......................]  0%
  Expected: 0% (consecutive on empty disk)

Aging: 30 files created, 10 deleted (irregular holes).

Post-aging, 12-block file:
  [##############......]  71%
  First-fit expected:        60-85%
  Locality-aware expected:   10-35%

Result: CHECK — high score. Re-verify balloc() and bmap() changes.


─── AFTER Lab 3 (locality-aware) ─────────────────────────────────────

$ loctest
=== Locality-Aware Allocation Test ===

Fresh disk, 12-block file:
  [......................]  0%
  Expected: 0% (consecutive on empty disk)

Aging: 30 files created, 10 deleted (irregular holes).

Post-aging, 12-block file:
  [#####...............]  21%
  First-fit expected:        60-85%
  Locality-aware expected:   10-35%

Result: GOOD — locality-aware is working correctly.

Improvement: 71% → 21%  (50 percentage points)
```

---

## Design Notes

### Why the hint is `addrs[bn-1] + 1`

When a file needs logical block N, the ideal physical location is
immediately after block N-1 (address = `addrs[N-1] + 1`).
`bmap()` already has `addrs[N-1]` available — it is the address stored
in the previous direct slot (or previous indirect entry).
Passing `addrs[N-1] + 1` as the hint tells `balloc()` to look there first.

### Why the indirect block itself gets `hint = 0`

The indirect block is file metadata, not file data.
Its location does not affect how fast file content is read sequentially.
Passing `hint = 0` skips Pass 1 for this allocation.

### BALLOC_WINDOW trade-off

| Window size | Pass 1 cost | Locality benefit |
|-------------|-------------|-----------------|
| 1 | Nearly free | Minimal |
| 32 | ~32 bit checks | Good (covers most small files) |
| 256 | ~256 bit checks | Excellent, but slower |
| FSSIZE | Full scan | Same as best-fit; very slow |

A window of 32 blocks (32 KB) is a good starting point for xv6's 2 MB disk.

---

## Quick Checklist

- [ ] `kernel/param.h` — `#define BALLOC_WINDOW 32` added
- [ ] `kernel/fs.c` — `balloc()` signature changed to `balloc(uint dev, uint hint)`
- [ ] `kernel/fs.c` — `balloc()` body: Pass 1 window search + Pass 2 fallback
- [ ] `kernel/fs.c` — `bmap()` direct block: `balloc(ip->dev, hint)` with `(bn>0 && ip->addrs[bn-1]) ? ip->addrs[bn-1]+1 : 0`
- [ ] `kernel/fs.c` — `bmap()` indirect block alloc: `balloc(ip->dev, 0)`
- [ ] `kernel/fs.c` — `bmap()` indirect data block: `balloc(ip->dev, hint2)` with `(bn>0 && a[bn-1]) ? a[bn-1]+1 : 0`
- [ ] `grep "balloc(ip->dev)" kernel/fs.c` returns zero results
- [ ] `user/loctest.c` created
- [ ] `Makefile` — `$U/_loctest\` added to UPROGS
- [ ] `make clean && make qemu CPUS=1` succeeds
- [ ] `$ loctest` post-aging score is below 40%
