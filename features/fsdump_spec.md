# xv6 fsdump 구현 스펙

## 수정 / 생성할 파일 목록

| 파일 | 작업 | 내용 |
|------|------|------|
| kernel/fsinfo.h | 새로 생성 | syscall에서 사용할 공용 struct 정의 |
| kernel/sysproc.c | 함수 추가 | sys_get_fsinfo / sys_get_inode_info / sys_get_free_blocks |
| kernel/syscall.h | 3줄 추가 | 새 syscall 번호 정의 |
| kernel/syscall.c | 6줄 추가 | extern 선언 + 테이블 등록 |
| user/usys.pl | 3줄 추가 | user-space stub 생성 |
| user/user.h | 3줄 추가 | 함수 선언 |
| user/fsdump.c | 새로 생성 | 출력 프로그램 |
| Makefile | 1줄 추가 | UPROGS에 fsdump 추가 |

---

## Step 1 — kernel/fsinfo.h (새로 생성)

```c
// kernel/fsinfo.h
#pragma once

struct fsinfo {
  uint size;        // 디스크 전체 블록 수
  uint nblocks;     // 데이터 블록 수
  uint ninodes;     // 최대 inode 수
  uint nlog;        // 로그 블록 수
  uint logstart;    // 로그 시작 블록 번호
  uint inodestart;  // inode 영역 시작 블록 번호
  uint bmapstart;   // bitmap 블록 번호
};
```

---

## Step 2 — Syscall 등록

### kernel/syscall.h — 3줄 추가

```c
#define SYS_get_fsinfo        23
#define SYS_get_inode_info    24
#define SYS_get_free_blocks   25
```

### kernel/syscall.c — extern 선언 추가

```c
extern uint64 sys_get_fsinfo(void);
extern uint64 sys_get_inode_info(void);
extern uint64 sys_get_free_blocks(void);
```

### kernel/syscall.c — syscalls[] 테이블 추가

```c
[SYS_get_fsinfo]       sys_get_fsinfo,
[SYS_get_inode_info]   sys_get_inode_info,
[SYS_get_free_blocks]  sys_get_free_blocks,
```

### user/usys.pl — 3줄 추가

```perl
entry("get_fsinfo");
entry("get_inode_info");
entry("get_free_blocks");
```

### user/user.h — 3줄 추가

```c
int get_fsinfo(struct fsinfo *);
int get_inode_info(int inum, struct dinode *);
int get_free_blocks(void);
```

### Makefile — UPROGS에 추가

```makefile
$U/_fsdump\
```

---

## Step 3 — sys_get_fsinfo() (kernel/sysproc.c)

```c
// kernel/sysproc.c 상단에 include 추가
#include "fsinfo.h"

uint64
sys_get_fsinfo(void)
{
  uint64 addr;
  if(argaddr(0, &addr) < 0) return -1;

  struct fsinfo info;
  info.size       = sb.size;
  info.nblocks    = sb.nblocks;
  info.ninodes    = sb.ninodes;
  info.nlog       = sb.nlog;
  info.logstart   = sb.logstart;
  info.inodestart = sb.inodestart;
  info.bmapstart  = sb.bmapstart;

  if(copyout(myproc()->pagetable, addr, (char*)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
```

---

## Step 4 — sys_get_inode_info() (kernel/sysproc.c)

```c
uint64
sys_get_inode_info(void)
{
  int inum;
  uint64 addr;
  if(argint(0, &inum) < 0 || argaddr(1, &addr) < 0) return -1;
  if(inum <= 0 || inum >= (int)sb.ninodes) return -1;

  struct buf *bp = bread(ROOTDEV, IBLOCK(inum, sb));
  struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
  struct dinode di;
  memmove(&di, dip, sizeof(di));
  brelse(bp);

  if(copyout(myproc()->pagetable, addr, (char*)&di, sizeof(di)) < 0)
    return -1;
  return 0;
}
```

---

## Step 5 — sys_get_free_blocks() (kernel/sysproc.c)

```c
uint64
sys_get_free_blocks(void)
{
  int free_count = 0;

  for(uint b = 0; b < sb.size; b += BPB) {
    struct buf *bp = bread(ROOTDEV, BBLOCK(b, sb));
    for(int bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      int m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0) free_count++;
    }
    brelse(bp);
  }
  return free_count;
}
```

---

## Step 6 — user/fsdump.c (새로 생성)

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fsinfo.h"
#include "user/user.h"

void
print_superblock(struct fsinfo *info)
{
  printf("\n=== xv6 File System ===\n\n");
  printf("[ Superblock ]\n");
  printf("  Total blocks  : %5d    Log start    : %3d\n",
          info->size, info->logstart);
  printf("  Data blocks   : %5d    Inode start  : %3d\n",
          info->nblocks, info->inodestart);
  printf("  Inodes        : %5d    Bitmap block : %3d\n",
          info->ninodes, info->bmapstart);
}

void
print_layout(struct fsinfo *info)
{
  int free_blks = get_free_blocks();
  int used_blks = info->nblocks - free_blks;

  printf("\n[ Disk Layout ]\n");
  printf("  blk   0     | boot block\n");
  printf("  blk   1     | superblock\n");
  printf("  blk %2d-%2d   | log (%d blocks)\n",
          info->logstart, info->logstart + info->nlog - 1, info->nlog);
  printf("  blk %2d-%2d   | inode blocks (%d inodes)\n",
          info->inodestart, info->bmapstart - 1, info->ninodes);
  printf("  blk %2d      | bitmap\n", info->bmapstart);
  printf("  blk %2d-%-4d | data  (used: %d / free: %d)\n",
          info->bmapstart + 1, info->size - 1, used_blks, free_blks);
}

void
print_inodes(struct fsinfo *info)
{
  printf("\n[ Inode Table ]\n");
  printf("  inum  type    nlink  size    blks\n");

  int used = 0;
  for(int i = 1; i < (int)info->ninodes; i++) {
    struct dinode di;
    if(get_inode_info(i, &di) < 0) continue;
    if(di.type == 0) continue;

    char *tstr = di.type == T_FILE   ? "file" :
                 di.type == T_DIR    ? "dir"  :
                 di.type == T_DEVICE ? "dev"  : "???";

    int nblks = 0;
    for(int j = 0; j < NDIRECT; j++)
      if(di.addrs[j]) nblks++;

    if(di.addrs[NDIRECT])
      nblks += (di.size - NDIRECT * BSIZE + BSIZE - 1) / BSIZE;

    printf("  %4d  %-6s  %4d  %6d  %4d\n",
           i, tstr, di.nlink, di.size, nblks);
    used++;
  }
  printf("  Total: %d allocated / %d free\n",
         used, (int)info->ninodes - 1 - used);
}

void
print_bitmap(struct fsinfo *info)
{
  int free_blks = get_free_blocks();
  int used_blks = info->nblocks - free_blks;

  printf("\n[ Bitmap Summary ]\n");
  printf("  Used: %4d (%4.1f%%)  Free: %4d (%4.1f%%)\n",
         used_blks, 100.0*used_blks/info->nblocks,
         free_blks, 100.0*free_blks/info->nblocks);

  printf("  [");
  int W = 20;
  int u = used_blks * W / info->nblocks;
  for(int i = 0; i < W; i++) printf(i < u ? "#" : ".");
  printf("]\n");
}

int
main(void)
{
  struct fsinfo info;
  get_fsinfo(&info);
  print_superblock(&info);
  print_layout(&info);
  print_inodes(&info);
  print_bitmap(&info);
  printf("\n");
  return 0;
}
```
