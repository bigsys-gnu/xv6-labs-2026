# xv6 get_fragmentation — Implementation Guide for Claude Code

## Overview

Add a `get_fragmentation(fd)` syscall that measures how scattered a file's
data blocks are on disk, returning a score from 0% (perfectly contiguous)
to 100% (completely scattered).

Also add two user programs: `aging` (creates disk holes) and `fragtest`
(measures fragmentation before and after aging).

**Prerequisite:** Lab 1 (fsdump) must already be implemented.  
**Repository:** xv6-riscv (riscv branch)  
**Run with:** `make clean && make qemu CPUS=1`  
**Test:** `$ fragtest` → `$ aging` → `$ fragtest`

---

## Files to Change

| File | Action |
|------|--------|
| `kernel/syscall.h` | Add 1 `#define` line |
| `kernel/syscall.c` | Add 1 extern + 1 table entry |
| `kernel/sysproc.c` | Add `sys_get_fragmentation()` function |
| `user/usys.pl` | Add 1 `entry()` line |
| `user/user.h` | Add 1 function declaration |
| `user/aging.c` | **Create new** |
| `user/fragtest.c` | **Create new** |
| `Makefile` | Add `$U/_aging\` and `$U/_fragtest\` to UPROGS |

---

## 1. Edit `kernel/syscall.h`

Add one line at the end of the file.
(Lab 1 used numbers 23, 24, 25; continue from 26.)

```c
#define SYS_get_fragmentation  26
```

---

## 2. Edit `kernel/syscall.c`

### 2a. Add extern declaration

```c
extern uint64 sys_get_fragmentation(void);
```

### 2b. Add table entry

```c
[SYS_get_fragmentation]  sys_get_fragmentation,
```

---

## 3. Edit `kernel/sysproc.c`

Add this function at the end of the file.

```c
// ─────────────────────────────────────────────────────────────────────
// sys_get_fragmentation()
//
// Measures how scattered a file's data blocks are on disk.
// Traverses the inode's addrs[] array and indirect block to collect
// all allocated block addresses in logical order, then counts
// non-consecutive adjacent pairs ("transitions").
//
// User call:  get_fragmentation(fd)
// Arg 0:      int fd  — file descriptor of an open regular file
// Returns:    0–100 (fragmentation percentage), or -1 on error
//
// Score meaning:
//   0%   = all blocks are physically consecutive on disk (ideal)
//   100% = every adjacent logical block pair is non-consecutive (worst)
// ─────────────────────────────────────────────────────────────────────
uint64
sys_get_fragmentation(void)
{
  int fd;
  struct file *f;

  // argfd(n, &fd, &f): reads syscall argument n as a file descriptor.
  //   Sets fd = integer fd, f = pointer to the struct file it refers to.
  //   Returns -1 if fd is invalid or not open.
  if(argfd(0, &fd, &f) < 0)
    return -1;

  // Only regular files (FD_INODE) have addrs[] arrays.
  // Pipes (FD_PIPE) store data in a ring buffer, not disk blocks.
  if(f->type != FD_INODE)
    return -1;

  struct inode *ip = f->ip;

  // ilock(ip): acquires ip->lock (sleeplock).
  //   If ip->valid == 0, reads the dinode from disk (via IBLOCK)
  //   and copies fields including addrs[] into the in-memory inode.
  //   After ilock(), ip->addrs[] is valid and safe to read.
  ilock(ip);

  // Collect all allocated disk block numbers in logical order.
  //   blocks[0] = physical address of the file's logical block 0
  //   blocks[1] = physical address of logical block 1, etc.
  //   MAXFILE = NDIRECT + NINDIRECT = 12 + 256 = 268 (maximum blocks/file)
  uint blocks[MAXFILE];
  int n = 0;  // number of addresses collected

  // ── Collect direct block addresses ───────────────────────────────
  // addrs[0] through addrs[NDIRECT-1] = addrs[0] through addrs[11].
  // Each non-zero entry holds a real disk block number.
  // Lazy allocation: once we see a 0, no further direct blocks exist.
  // (xv6 does not create sparse files — a 0 means end of allocation.)
  for(int i = 0; i < NDIRECT; i++) {
    if(ip->addrs[i] == 0)
      break;  // no more direct blocks
    blocks[n++] = ip->addrs[i];
  }

  // ── Collect indirect block addresses ─────────────────────────────
  // addrs[NDIRECT] = addrs[12] is the indirect block pointer.
  //   Value 0: file is ≤ 12 KB, no indirect block needed.
  //   Non-zero: holds the disk block number of a 1 KB block that
  //   contains 256 uint entries (a[0]..a[255]), each a data block address.
  if(ip->addrs[NDIRECT]) {
    // bread(dev, block#): loads the indirect block into the buffer cache.
    //   ip->addrs[NDIRECT] is the disk block number of the indirect block.
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *a = (uint *)bp->data;  // treat 1024 bytes as 256 uint values

    for(int i = 0; i < NINDIRECT; i++) {
      if(a[i] == 0)
        break;  // unallocated indirect entry → stop
      blocks[n++] = a[i];
    }
    brelse(bp);  // always release the buffer after bread
  }

  // iunlock: releases ip->lock so other processes can access this inode.
  // Must be called even if we found no blocks.
  iunlock(ip);

  // Edge cases: 0 or 1 blocks → no adjacent pairs → fragmentation = 0.
  if(n <= 1)
    return 0;

  // ── Count non-consecutive adjacent pairs (transitions) ───────────
  //
  // Two logically adjacent blocks are "consecutive" on disk when
  // their physical block numbers differ by exactly 1.
  //
  // Example: blocks = [47, 48, 49, 200, 201]
  //   i=1: 48 == 47+1  → consecutive  (no transition)
  //   i=2: 49 == 48+1  → consecutive
  //   i=3: 200 != 49+1 → NON-CONSECUTIVE → transitions++ = 1
  //   i=4: 201 == 200+1 → consecutive
  //   Total: 1 transition
  int transitions = 0;
  for(int i = 1; i < n; i++) {
    if(blocks[i] != blocks[i-1] + 1)
      transitions++;
  }

  // ── Normalize to 0–100% ──────────────────────────────────────────
  //
  // Maximum possible transitions = n-1 (every block is isolated).
  // Multiply by 100 BEFORE dividing to avoid integer truncation:
  //   Wrong: (3 / 7) * 100 = 0 * 100 = 0   (integer truncation)
  //   Right: (3 * 100) / 7 = 300 / 7 = 42  (correct: 42%)
  //
  // Score = 0%:   all blocks consecutive (e.g. [47,48,49,50])
  // Score = 100%: all blocks non-consecutive (e.g. [47,89,52,200])
  return (transitions * 100) / (n - 1);
}
```

---

## 4. Edit `user/usys.pl`

Add one line at the end of the file:

```perl
entry("get_fragmentation");
```

---

## 5. Edit `user/user.h`

Add one declaration in the syscalls section:

```c
// Returns 0-100 (fragmentation percentage), or -1 on error.
// fd must refer to a regular file opened with open().
int get_fragmentation(int fd);
```

---

## 6. Create `user/aging.c`

Create this file from scratch.

```c
// user/aging.c — Aging Workload
//
// Simulates a "used" disk that has had many files created and deleted,
// leaving non-consecutive free blocks (holes) in the bitmap.
//
// How aging creates fragmentation:
//   Before aging: free blocks start at 47 and continue consecutively.
//   Create f0 → blocks [47,48,49], f1 → [50,51,52], f2 → [53,54,55] ...
//   Delete f0, f2, f4 ...: free [47-49], [53-55], [59-61] (alternating gaps)
//   Next balloc() from block 0: fills 47, 48, 49, then 53, 54, 55 ...
//   → A new 6-block file gets: [47,48,49,53,54,55] → 1 transition → 20%
//
// Run sequence:
//   $ fragtest      ← measure baseline (expect 0%)
//   $ aging         ← create holes
//   $ fragtest      ← measure after aging (expect 40-80%)

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int
main(void)
{
  char name[16];
  char buf[1024];

  // Fill write buffer with ASCII 'A' (0x41).
  // Content does not matter — we only care about which blocks are allocated.
  memset(buf, 0x41, sizeof(buf));

  // ── Step 1: Create 40 small files, each 3 KB (3 blocks) ──────────
  //
  // With first-fit on a mostly-empty disk, consecutive allocation:
  //   f0  → blocks [47, 48, 49]
  //   f1  → blocks [50, 51, 52]
  //   f2  → blocks [53, 54, 55]
  //   ...
  //   f39 → blocks [164, 165, 166]
  //
  // After this step, blocks 47..166 are all in use (120 blocks).
  for(int i = 0; i < 40; i++) {
    snprintf(name, sizeof(name), "f%d", i);

    // O_CREATE: allocate a new inode, set type=T_FILE, add directory entry.
    // O_WRONLY: mark file as write-only (required for write() to succeed).
    int fd = open(name, O_CREATE|O_WRONLY);
    if(fd < 0) {
      printf("aging: could not create %s\n", name);
      exit(1);
    }

    // Write 3 KB = 3 blocks.
    // Each write() that crosses a block boundary causes bmap() to call
    // balloc() for the new block (lazy allocation).
    for(int b = 0; b < 3; b++)
      write(fd, buf, sizeof(buf));

    close(fd);
  }
  printf("Step 1: created 40 files (f0..f39), each 3 KB.\n");
  printf("  Blocks used: 47 to %d\n", 47 + 40*3 - 1);

  // ── Step 2: Delete every other file ──────────────────────────────
  //
  // Deletes f0, f2, f4, ... f38 (20 even-numbered files).
  // Leaves f1, f3, f5, ... f39 (20 odd-numbered files) intact.
  //
  // Creates alternating free/used 3-block patterns:
  //   blk 47-49: FREE  (was f0)   blk 50-52: USED (f1)
  //   blk 53-55: FREE  (was f2)   blk 56-58: USED (f3)
  //   blk 59-61: FREE  (was f4)   blk 62-64: USED (f5) ...
  //
  // The next balloc() scan from block 0 will fill these gaps in order:
  //   1st call → 47,  2nd → 48,  3rd → 49,  4th → 53,  5th → 54 ...
  // A new 6-block file gets [47,48,49,53,54,55] → 1 transition → 20%
  for(int i = 0; i < 40; i += 2) {
    snprintf(name, sizeof(name), "f%d", i);

    // unlink(path): removes the directory entry.
    // When nlink drops to 0 and no process holds the file open,
    // itrunc() is called → bfree() clears all bitmap bits for the file's blocks.
    unlink(name);
  }
  printf("Step 2: deleted f0, f2, f4 ... f38 (20 files).\n");
  printf("  Bitmap now has alternating free/used 3-block runs.\n");
  printf("Aging complete. Run 'fragtest' to measure fragmentation.\n");

  return 0;
}
```

---

## 7. Create `user/fragtest.c`

Create this file from scratch.

```c
// user/fragtest.c — Fragmentation Measurement Test
//
// Demonstrates the effect of disk aging on file block layout.
// Creates files before and after running 'aging' and compares scores.
//
// Run sequence:
//   $ fragtest      ← creates fresh.txt, prints score (expect 0%)
//   $ aging         ← creates disk holes
//   $ fragtest      ← creates aged.txt, prints score (expect > 0%)
//
// The two scores prove that first-fit allocation causes fragmentation
// after a realistic create/delete workload.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// print_bar: display a 20-character ASCII bar for a percentage value.
static void
print_bar(int score)
{
  int filled = score * 20 / 100;  // how many '#' characters
  printf("  [");
  for(int i = 0; i < 20; i++)
    printf(i < filled ? "#" : ".");
  printf("]  %d%%\n", score);
}

int
main(void)
{
  char buf[1024];
  memset(buf, 0x42, sizeof(buf));  // fill with 'B'
  int score;

  // ── Test 1: Fresh file ───────────────────────────────────────────
  //
  // On a freshly booted xv6, blocks are allocated consecutively
  // starting at block 47 (the first data block).
  // An 8-block file gets: [47, 48, 49, 50, 51, 52, 53, 54]
  // All consecutive → 0 transitions → score = 0%
  int fd = open("fresh.txt", O_CREATE|O_RDWR);
  if(fd < 0) {
    printf("fragtest: cannot create fresh.txt\n");
    exit(1);
  }
  for(int i = 0; i < 8; i++)
    write(fd, buf, 1024);

  // get_fragmentation(fd) calls sys_get_fragmentation():
  //   1. Collects addrs[0..7] from the inode
  //   2. Counts transitions between adjacent block numbers
  //   3. Returns (transitions * 100) / (n-1)
  score = get_fragmentation(fd);
  printf("Fresh file (8 KB):\n");
  print_bar(score);
  printf("  Expected: 0%% (consecutive allocation on empty disk)\n\n");
  close(fd);

  printf("  -> Run 'aging' to create disk holes, then run fragtest again.\n\n");

  // ── Test 2: File created after aging ─────────────────────────────
  //
  // After aging creates alternating free/used gaps, first-fit fills them:
  //   New 8-block file might get: [47,48,49, 53,54,55, 59,60]
  //   Transitions at index 3 (49→53) and 6 (55→59) → 2/7 * 100 = 28%
  //   Exact score depends on how many other files already exist.
  fd = open("aged.txt", O_CREATE|O_RDWR);
  if(fd < 0) {
    printf("fragtest: cannot create aged.txt\n");
    exit(1);
  }
  for(int i = 0; i < 8; i++)
    write(fd, buf, 1024);

  score = get_fragmentation(fd);
  printf("Post-aging file (8 KB):\n");
  print_bar(score);
  printf("  Expected: 40-80%% (blocks scattered into freed holes)\n");
  close(fd);

  return 0;
}
```

---

## 8. Edit `Makefile`

Add two lines to the `UPROGS` block:

```makefile
UPROGS=\
    ...existing entries...\
    $U/_aging\
    $U/_fragtest\
    ...existing entries...\
```

---

## Expected Output

```
Run 1 (fresh disk, before aging):

$ fragtest
Fresh file (8 KB):
  [......................]  0%
  Expected: 0% (consecutive allocation on empty disk)

  -> Run 'aging' to create disk holes, then run fragtest again.

Post-aging file (8 KB):
  [......................]  0%
  (aged.txt was also created on a fresh disk)

─────────────────────────────────────────────

$ aging
Step 1: created 40 files (f0..f39), each 3 KB.
  Blocks used: 47 to 166
Step 2: deleted f0, f2, f4 ... f38 (20 files).
  Bitmap now has alternating free/used 3-block runs.
Aging complete. Run 'fragtest' to measure fragmentation.

─────────────────────────────────────────────

Run 2 (after aging):

$ fragtest
Fresh file (8 KB):
  [......................]  0%    (fresh.txt existed before aging)

Post-aging file (8 KB):
  [###########.........]  57%   (typical — varies by workload)
  Expected: 40-80% (blocks scattered into freed holes)
```

---

## Quick Checklist

- [ ] `kernel/syscall.h` — `#define SYS_get_fragmentation 26` added
- [ ] `kernel/syscall.c` — extern + table entry added
- [ ] `kernel/sysproc.c` — `sys_get_fragmentation()` function added
- [ ] `user/usys.pl` — `entry("get_fragmentation");` added
- [ ] `user/user.h` — `int get_fragmentation(int fd);` declared
- [ ] `user/aging.c` — created
- [ ] `user/fragtest.c` — created
- [ ] `Makefile` — `$U/_aging\` and `$U/_fragtest\` added to UPROGS
- [ ] `make clean && make qemu CPUS=1` succeeds
- [ ] `$ fragtest` then `$ aging` then `$ fragtest` produces different scores
