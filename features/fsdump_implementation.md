# xv6 fsdump — Implementation Guide for Claude Code

## Overview

Add a `fsdump` command to xv6 that prints the internal structure of the file system.
Requires 3 new syscalls and one user-space program.

**Repository:** xv6-riscv (riscv branch)  
**Run with:** `make clean && make qemu CPUS=1`  
**Test:** `$ fsdump` at the xv6 shell prompt

---

## Files to Change

| File | Action |
|------|--------|
| `kernel/fsinfo.h` | **Create new** |
| `kernel/syscall.h` | Add 3 `#define` lines |
| `kernel/syscall.c` | Add 3 extern + 3 table entries |
| `kernel/sysproc.c` | Add 3 new functions |
| `user/usys.pl` | Add 3 `entry()` lines |
| `user/user.h` | Add includes + 3 declarations |
| `user/fsdump.c` | **Create new** |
| `Makefile` | Add `$U/_fsdump\` to UPROGS |

---

## 1. Create `kernel/fsinfo.h`

Create this file from scratch.

```c
// kernel/fsinfo.h
// Shared struct between kernel (sysproc.c) and user space (fsdump.c).
// sys_get_fsinfo() fills this struct from the global superblock sb,
// then copies it to user space with copyout().

#pragma once
#include "kernel/types.h"

struct fsinfo {
  uint size;        // total disk blocks  (sb.size,       e.g. 2000)
  uint nblocks;     // data blocks        (sb.nblocks,    e.g. 1953)
  uint ninodes;     // max inode count    (sb.ninodes,    e.g. 200)
  uint nlog;        // log block count    (sb.nlog,       e.g. 30)
  uint logstart;    // first log block    (sb.logstart,   e.g. 2)
  uint inodestart;  // first inode block  (sb.inodestart, e.g. 33)
  uint bmapstart;   // bitmap block       (sb.bmapstart,  e.g. 46)
};
// sizeof(struct fsinfo) = 7 × 4 = 28 bytes
```

---

## 2. Edit `kernel/syscall.h`

Add three lines at the **end** of the file.
Check the last existing syscall number and continue from there.
(In stock xv6, `SYS_close = 21`.)

```c
#define SYS_get_fsinfo        23
#define SYS_get_inode_info    24
#define SYS_get_free_blocks   25
```

---

## 3. Edit `kernel/syscall.c`

### 3a. Add extern declarations

Add alongside the existing `extern uint64 sys_*` declarations:

```c
extern uint64 sys_get_fsinfo(void);
extern uint64 sys_get_inode_info(void);
extern uint64 sys_get_free_blocks(void);
```

### 3b. Add table entries

Add inside the `syscalls[]` array:

```c
[SYS_get_fsinfo]       sys_get_fsinfo,
[SYS_get_inode_info]   sys_get_inode_info,
[SYS_get_free_blocks]  sys_get_free_blocks,
```

---

## 4. Edit `kernel/sysproc.c`

Add `#include "fsinfo.h"` near the top of the file (after existing includes).

Then add these three functions at the **end** of the file.

```c
// ─────────────────────────────────────────────────────────────────────
// sys_get_fsinfo()
//
// Reads the kernel's global superblock variable sb and copies it to
// the user-provided struct fsinfo via copyout().
//
// User call:  get_fsinfo(&my_info)
// Arg 0:      uint64 address of the struct fsinfo in user space
// ─────────────────────────────────────────────────────────────────────
uint64
sys_get_fsinfo(void)
{
  uint64 addr;

  // argaddr(n, &addr): read syscall argument n as a pointer (user VA).
  if(argaddr(0, &addr) < 0)
    return -1;

  // Build the fsinfo struct in kernel memory from the global superblock sb.
  // sb is populated by fsinit() on the first scheduler tick at boot.
  struct fsinfo info;
  info.size       = sb.size;
  info.nblocks    = sb.nblocks;
  info.ninodes    = sb.ninodes;
  info.nlog       = sb.nlog;
  info.logstart   = sb.logstart;
  info.inodestart = sb.inodestart;
  info.bmapstart  = sb.bmapstart;

  // copyout(pagetable, dstva, src, len):
  //   copies len bytes from kernel address src to user VA dstva.
  //   Required because user and kernel have separate page tables in xv6.
  if(copyout(myproc()->pagetable, addr, (char*)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}

// ─────────────────────────────────────────────────────────────────────
// sys_get_inode_info()
//
// Reads one dinode from the inode region on disk, given an inode number,
// and copies it to the user-provided struct dinode buffer.
//
// User call:  get_inode_info(inum, &my_dinode)
// Arg 0:      int  inum  — inode number to read (1 = root directory)
// Arg 1:      uint64 address of struct dinode buffer in user space
// ─────────────────────────────────────────────────────────────────────
uint64
sys_get_inode_info(void)
{
  int    inum;
  uint64 addr;

  if(argint(0, &inum) < 0 || argaddr(1, &addr) < 0)
    return -1;

  // inode 0 is reserved and never used; inum must be < ninodes.
  if(inum <= 0 || inum >= (int)sb.ninodes)
    return -1;

  // IBLOCK(i, sb) = i / IPB + sb.inodestart
  //   IPB = BSIZE / sizeof(struct dinode) = 1024 / 64 = 16
  //   Computes which disk block contains dinode number inum.
  //   Example: inum=5  → 5/16 + 33 = 33  (first inode block)
  //   Example: inum=20 → 20/16 + 33 = 34 (second inode block)
  struct buf *bp = bread(ROOTDEV, IBLOCK(inum, sb));

  // Find the dinode slot within the block.
  //   Each 1 KB block holds 16 dinodes (0..15).
  //   inum % IPB gives the slot index (0-based) within the block.
  //   Pointer arithmetic advances by sizeof(struct dinode) = 64 bytes per step.
  struct dinode *dip = (struct dinode *)bp->data + inum % IPB;

  struct dinode di;
  memmove(&di, dip, sizeof(di));  // kernel-to-kernel safe copy
  brelse(bp);                     // always release buffer after bread

  if(copyout(myproc()->pagetable, addr, (char*)&di, sizeof(di)) < 0)
    return -1;

  return 0;
}

// ─────────────────────────────────────────────────────────────────────
// sys_get_free_blocks()
//
// Scans the bitmap (block 46) and counts bits that are 0 (= free blocks).
//
// User call:  get_free_blocks()
// Returns:    number of free data blocks
// ─────────────────────────────────────────────────────────────────────
uint64
sys_get_free_blocks(void)
{
  int free_count = 0;

  // Outer loop: one iteration per bitmap block.
  //   BPB = BSIZE * 8 = 8192: each bitmap block tracks 8192 data blocks.
  //   xv6 has only 2000 total blocks, so this loop runs exactly once.
  for(uint b = 0; b < sb.size; b += BPB) {

    // BBLOCK(b, sb) = b / BPB + sb.bmapstart
    //   Returns the disk block number of the bitmap block that tracks block b.
    //   Example: b=0, bmapstart=46 → BBLOCK(0,sb) = 0/8192 + 46 = 46
    struct buf *bp = bread(ROOTDEV, BBLOCK(b, sb));

    // Inner loop: examine each bit in this bitmap block.
    for(int bi = 0; bi < BPB && b + bi < sb.size; bi++) {

      // bi / 8 : which byte in bp->data[] holds this block's bit
      // bi % 8 : which bit within that byte (0 = least significant)
      // 1 << (bi%8): mask with exactly that bit set
      int m = 1 << (bi % 8);

      // Bit 0 → block is free (not yet allocated by balloc).
      // Bit 1 → block is in use (balloc set it with |= m).
      if((bp->data[bi/8] & m) == 0)
        free_count++;
    }
    brelse(bp);
  }

  return free_count;
}
```

---

## 5. Edit `user/usys.pl`

Add three lines at the **end** of the file:

```perl
entry("get_fsinfo");
entry("get_inode_info");
entry("get_free_blocks");
```

---

## 6. Edit `user/user.h`

### 6a. Add includes near the top of the syscalls section

```c
#include "kernel/fsinfo.h"  // struct fsinfo
#include "kernel/fs.h"      // struct dinode, NDIRECT, BSIZE
```

### 6b. Add function declarations in the syscalls section

```c
int get_fsinfo(struct fsinfo *);
int get_inode_info(int inum, struct dinode *);
int get_free_blocks(void);
```

---

## 7. Create `user/fsdump.c`

Create this file from scratch.

```c
// user/fsdump.c
//
// xv6 File System Dump Utility.
//
// Displays the internal structure of the xv6 file system by calling
// three kernel syscalls added in this lab:
//   get_fsinfo()       — reads the superblock (disk block 1)
//   get_inode_info()   — reads one dinode from the inode region
//   get_free_blocks()  — counts free blocks by scanning the bitmap
//
// Usage:  $ fsdump   (at the xv6 shell prompt)

#include "kernel/types.h"
#include "kernel/stat.h"    // T_FILE=2, T_DIR=1, T_DEVICE=3
#include "kernel/fs.h"      // NDIRECT, NINDIRECT, BSIZE, struct dinode
#include "kernel/fsinfo.h"  // struct fsinfo
#include "user/user.h"      // printf, get_fsinfo, exit, etc.

// ─────────────────────────────────────────────────────────────────────
// print_superblock: display the key superblock fields.
//
// Shows the Phase 1 disk layout numbers: total blocks, regions, etc.
// Data comes from get_fsinfo() → sys_get_fsinfo() → global sb variable.
// ─────────────────────────────────────────────────────────────────────
static void
print_superblock(struct fsinfo *info)
{
  printf("\n=== xv6 File System ===\n\n");
  printf("[ Superblock ]\n");

  // Each field mirrors a field in struct superblock (kernel/fs.h).
  printf("  Total blocks  : %5d    Log start    : %3d\n",
         info->size,    info->logstart);
  printf("  Data blocks   : %5d    Inode start  : %3d\n",
         info->nblocks, info->inodestart);
  printf("  Inodes        : %5d    Bitmap block : %3d\n",
         info->ninodes, info->bmapstart);
  printf("  Log blocks    : %5d    (blk %d to blk %d)\n",
         info->nlog,
         info->logstart,
         info->logstart + info->nlog - 1);
}

// ─────────────────────────────────────────────────────────────────────
// print_layout: show which block ranges serve which purpose.
//
// Calls get_free_blocks() to split the data region into used vs free.
// The output directly corresponds to the Phase 1 disk layout diagram.
// ─────────────────────────────────────────────────────────────────────
static void
print_layout(struct fsinfo *info)
{
  // sys_get_free_blocks() scans the bitmap and counts 0-bits.
  int free_blks = get_free_blocks();
  int used_blks = info->nblocks - free_blks;  // used = total - free

  printf("\n[ Disk Layout ]\n");
  printf("  blk   0         | boot block\n");
  printf("  blk   1         | superblock\n");

  printf("  blk %2d-%-2d       | log (%d blocks)\n",
         info->logstart,
         info->logstart + info->nlog - 1,
         info->nlog);

  // IPB = BSIZE / sizeof(struct dinode) = 1024 / 64 = 16
  printf("  blk %2d-%-2d       | inode blocks (%d inodes, IPB=%d)\n",
         info->inodestart,
         info->bmapstart - 1,
         info->ninodes,
         (int)(BSIZE / sizeof(struct dinode)));

  // BPB = BSIZE * 8 = 8192: one bitmap block tracks 8192 data blocks.
  printf("  blk %2d           | bitmap (tracks %d blocks per block)\n",
         info->bmapstart,
         BSIZE * 8);

  printf("  blk %2d-%-4d      | data  (used: %d / free: %d)\n",
         info->bmapstart + 1,
         info->size - 1,
         used_blks,
         free_blks);
}

// ─────────────────────────────────────────────────────────────────────
// print_inodes: display the inode table.
//
// Loops over all 200 possible inode numbers (inum 1 to ninodes-1).
// For each allocated inode (type != 0), prints type, links, size, blocks.
// Calls get_inode_info(inum, &di) which uses IBLOCK() to locate the dinode.
// ─────────────────────────────────────────────────────────────────────
static void
print_inodes(struct fsinfo *info)
{
  printf("\n[ Inode Table ]\n");
  printf("  inum  type    nlink   size    blks\n");
  printf("  %-4s  %-6s  %-5s  %-6s  %-4s\n",
         "----", "------", "-----", "------", "----");

  int used = 0;  // count of allocated inodes

  // inode 0 is reserved and never allocated; start from 1.
  for(int i = 1; i < (int)info->ninodes; i++) {
    struct dinode di;

    // get_inode_info(inum, &di):
    //   reads dinode inum from disk via IBLOCK() and copies it here.
    if(get_inode_info(i, &di) < 0)
      continue;

    // dinode.type == 0 means this slot is free (unallocated or freed).
    if(di.type == 0)
      continue;

    // Map the type integer to a human-readable label.
    // kernel/stat.h: T_DIR=1, T_FILE=2, T_DEVICE=3
    char *ts = di.type == T_FILE   ? "file"   :
               di.type == T_DIR    ? "dir"    :
               di.type == T_DEVICE ? "device" : "???";

    // Count allocated data blocks.
    //
    // Direct blocks: addrs[0..NDIRECT-1] = addrs[0..11].
    //   Each non-zero entry is one allocated 1 KB data block.
    //   addrs[j] == 0 means that slot was never allocated (lazy alloc).
    int nblks = 0;
    for(int j = 0; j < NDIRECT; j++)
      if(di.addrs[j])
        nblks++;

    // Indirect block: addrs[NDIRECT] = addrs[12].
    //   Non-zero means the file exceeds 12 KB.
    //   Estimate indirect data block count from file size:
    //     bytes beyond 12 direct blocks, rounded UP to BSIZE.
    //   Formula: (remainder + BSIZE - 1) / BSIZE  (ceiling division)
    if(di.addrs[NDIRECT])
      nblks += (di.size - NDIRECT * BSIZE + BSIZE - 1) / BSIZE;

    printf("  %4d  %-6s  %4d  %6d  %4d\n",
           i, ts, di.nlink, di.size, nblks);
    used++;
  }

  // inode 0 is reserved, so free slots = ninodes - 1 - used.
  printf("\n  Total: %d allocated / %d free\n",
         used, (int)info->ninodes - 1 - used);
}

// ─────────────────────────────────────────────────────────────────────
// print_bitmap: show bitmap usage with a percentage and ASCII bar.
//
// get_free_blocks() counts 0-bits in the bitmap block (disk block 46).
// ─────────────────────────────────────────────────────────────────────
static void
print_bitmap(struct fsinfo *info)
{
  int free_blks = get_free_blocks();
  int used_blks = info->nblocks - free_blks;

  printf("\n[ Bitmap Summary ]\n");
  printf("  Used: %4d (%4.1f%%)  Free: %4d (%4.1f%%)\n",
         used_blks, 100.0 * used_blks / info->nblocks,
         free_blks, 100.0 * free_blks / info->nblocks);

  // 20-character ASCII bar: '#' for used, '.' for free.
  int W = 20;
  int u = used_blks * W / info->nblocks;
  printf("  [");
  for(int i = 0; i < W; i++)
    printf(i < u ? "#" : ".");
  printf("]\n");
}

// ─────────────────────────────────────────────────────────────────────
// main: call all four sections in order.
// ─────────────────────────────────────────────────────────────────────
int
main(void)
{
  struct fsinfo info;

  // get_fsinfo() calls sys_get_fsinfo() which reads the global sb
  // variable in kernel/fs.c and copies its fields to user space.
  if(get_fsinfo(&info) < 0) {
    printf("fsdump: get_fsinfo failed\n");
    exit(1);
  }

  print_superblock(&info);
  print_layout(&info);
  print_inodes(&info);
  print_bitmap(&info);

  printf("\n");
  exit(0);
}
```

---

## 8. Edit `Makefile`

Find the `UPROGS` block and add one line:

```makefile
UPROGS=\
    ...existing entries...\
    $U/_fsdump\
    ...existing entries...\
```

The backslash `\` at the end of each line is required (line continuation).
The order within the list does not matter.

---

## Expected Output

```
$ fsdump

=== xv6 File System ===

[ Superblock ]
  Total blocks  :  2000    Log start    :   2
  Data blocks   :  1953    Inode start  :  33
  Inodes        :   200    Bitmap block :  46
  Log blocks    :    30    (blk 2 to blk 31)

[ Disk Layout ]
  blk   0         | boot block
  blk   1         | superblock
  blk  2-31       | log (30 blocks)
  blk 33-45       | inode blocks (200 inodes, IPB=16)
  blk 46           | bitmap (tracks 8192 blocks per block)
  blk 47-1999     | data  (used: 49 / free: 1904)

[ Inode Table ]
  inum  type    nlink   size    blks
  ----  ------  -----  ------  ----
     1  dir         1     192     1
     2  file        1   26624    26
     3  file        1   13568    14
   ...

  Total: 23 allocated / 176 free

[ Bitmap Summary ]
  Used:   49 ( 2.5%)  Free: 1904 (97.5%)
  [##..................]
```

---

## Quick Checklist

- [ ] `kernel/fsinfo.h` created
- [ ] `kernel/syscall.h` — 3 `#define` lines added
- [ ] `kernel/syscall.c` — 3 `extern` + 3 table entries added
- [ ] `kernel/sysproc.c` — `#include "fsinfo.h"` + 3 functions added
- [ ] `user/usys.pl` — 3 `entry()` lines added
- [ ] `user/user.h` — includes + 3 declarations added
- [ ] `user/fsdump.c` created
- [ ] `Makefile` — `$U/_fsdump\` added to UPROGS
- [ ] `make clean && make qemu CPUS=1` succeeds
- [ ] `$ fsdump` produces output
