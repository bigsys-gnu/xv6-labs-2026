# Fragmentation Lab — Claude Code 실제 구현 정리

> 이 문서는 `fragmentation_implementation.md`(원본 스펙)을 기준으로
> **Claude Code에서 실제로 구현·수정된 내용**을 정리한 것입니다.
> Claude AI 채팅에서 lab 문제를 작성할 때, 원본 스펙이 아니라
> **이 문서의 내용**을 정답/구현 기준으로 반영하세요.
>
> 브랜치: `features/fragmentation` · 빌드: `make clean && make qemu CPUS=1`

---

## 0. 원본 스펙 대비 핵심 변경점 (요약)

| # | 항목 | 원본 스펙 | 실제 구현 |
|---|------|-----------|-----------|
| 1 | `sys_get_fragmentation` 위치 | `kernel/sysproc.c` | **`kernel/sysfile.c`** (argfd가 static, file/inode 헬퍼가 이미 scope 안에 있음) |
| 2 | 추가 syscall | 없음 | **`get_free_runs()` (SYS 27) 추가** — 비트맵의 free run 개수로 "디스크가 이미 aged 되었는지" 판별 |
| 3 | `sysfile.c` include | — | `#include "buf.h"` 추가 (bread/struct buf 사용) |
| 4 | syscall 번호 | 26 한 개 | **26, 27 두 개** (`get_fragmentation`=26, `get_free_runs`=27) |
| 5 | `aging.c` 파일명 생성 | `snprintf()` 사용 | **`mkname()` 직접 구현** (xv6 user lib에 snprintf 없음) |
| 6 | 테스트 파일 구조 | `fresh.txt` + `aged.txt` 2개 | **`frag.txt` 단일 파일** — 매 실행마다 unlink 후 재생성하여 신규 할당 강제 |
| 7 | 디스크 상태 안내 | 없음 | aging/fragtest 모두 `get_free_runs()`로 "fresh / AGED" 상태 출력 (fs.img가 재부팅 후에도 유지됨을 반영) |
| 8 | 블록 번호 가정 | 주석에 "block 47부터" 하드코딩 | 하드코딩 제거 — 실제 시작 블록에 의존하지 않는 설명 |
| 9 | `exit()` | `return 0;` | `exit(0);` |

---

## 1. 변경된 파일 목록

| 파일 | 동작 |
|------|------|
| `kernel/syscall.h` | `#define` 2줄 추가 (26, 27) |
| `kernel/syscall.c` | extern 2개 + 테이블 2개 |
| `kernel/sysfile.c` | `#include "buf.h"` + `sys_get_fragmentation()` |
| `kernel/sysproc.c` | `sys_get_free_runs()` |
| `user/usys.pl` | `entry()` 2줄 |
| `user/user.h` | 선언 2개 |
| `user/aging.c` | **신규 생성** |
| `user/fragtest.c` | **신규 생성** |
| `Makefile` | `$U/_aging`, `$U/_fragtest` UPROGS에 추가 |

전체: 7개 파일 수정 + 2개 신규, +110 라인 (커널 측).

---

## 2. `kernel/syscall.h`

```c
#define SYS_get_fragmentation 26
#define SYS_get_free_runs     27
```
> Lab 1(fsdump)이 23, 24, 25를 사용했으므로 26, 27로 이어짐.

## 3. `kernel/syscall.c`

```c
extern uint64 sys_get_fragmentation(void);
extern uint64 sys_get_free_runs(void);
```
```c
[SYS_get_fragmentation] sys_get_fragmentation,
[SYS_get_free_runs]    sys_get_free_runs,
```

## 4. `user/usys.pl`

```perl
entry("get_fragmentation");
entry("get_free_runs");
```

## 5. `user/user.h`

```c
// Returns 0-100 (fragmentation percentage), or -1 on error.
// fd must refer to a regular file opened with open().
int get_fragmentation(int fd);
// Number of free-block "runs" (regions) in the bitmap. ~1-2 on a fresh
// disk, many (~20+) once aging has punched holes. Quick aging gauge.
int get_free_runs(void);
```

## 6. `Makefile`

```makefile
UPROGS=\
	...\
	$U/_fsdump\
	$U/_aging\
	$U/_fragtest\
```

---

## 7. `kernel/sysfile.c` — `sys_get_fragmentation()`

> **원본과 가장 큰 차이: sysproc.c가 아니라 sysfile.c에 배치.**
> 이유 — `argfd()`가 sysfile.c 내부의 static 함수이고, `struct file`/inode
> 헬퍼(`ilock`/`bread`/`brelse` 등)가 이미 이 파일 scope에 있기 때문.
> 그래서 파일 상단에 `#include "buf.h"`도 함께 추가됨.

```c
// (파일 상단 include 추가)
#include "buf.h"
```

```c
// Measure how scattered a file's data blocks are on disk.
// Collects the file's block addresses in logical order (direct blocks
// then the indirect block), counts adjacent pairs whose physical block
// numbers are not consecutive, and normalizes to 0-100%.
//
// User call: get_fragmentation(fd)
//   0%   = all blocks physically consecutive (ideal)
//   100% = every adjacent logical pair is non-consecutive (worst)
// Returns -1 on error (bad fd or non-regular file).
//
// NOTE: placed in sysfile.c (not sysproc.c) because argfd() is static
// to this file and struct file/inode helpers are already in scope.
uint64
sys_get_fragmentation(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;

  // Only regular files (FD_INODE) have addrs[] block arrays.
  if(f->type != FD_INODE)
    return -1;

  struct inode *ip = f->ip;

  uint blocks[MAXFILE];
  int n = 0;

  ilock(ip);

  // Direct blocks: addrs[0..NDIRECT-1]. A 0 marks end of allocation.
  for(int i = 0; i < NDIRECT; i++){
    if(ip->addrs[i] == 0)
      break;
    blocks[n++] = ip->addrs[i];
  }

  // Indirect block: addrs[NDIRECT] points to a block of NINDIRECT uints.
  if(ip->addrs[NDIRECT]){
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *a = (uint*)bp->data;
    for(int i = 0; i < NINDIRECT; i++){
      if(a[i] == 0)
        break;
      blocks[n++] = a[i];
    }
    brelse(bp);
  }

  iunlock(ip);

  // 0 or 1 blocks: no adjacent pairs, so no fragmentation.
  if(n <= 1)
    return 0;

  // Count non-consecutive adjacent pairs (transitions).
  int transitions = 0;
  for(int i = 1; i < n; i++){
    if(blocks[i] != blocks[i-1] + 1)
      transitions++;
  }

  // Normalize: multiply before dividing to avoid integer truncation.
  return (transitions * 100) / (n - 1);
}
```

핵심 알고리즘은 원본과 동일:
1. direct(addrs[0..11]) + indirect(addrs[12]→256개) 블록 주소를 논리 순서로 수집
2. 인접 쌍이 `blocks[i] != blocks[i-1] + 1`이면 transition 카운트
3. `(transitions * 100) / (n - 1)` 로 0~100% 정규화 (곱셈 먼저 → 정수 truncation 방지)

---

## 8. `kernel/sysproc.c` — `sys_get_free_runs()` (신규 추가 syscall)

> **원본 스펙에 없던 추가 기능.** 비트맵을 스캔하여 "연속된 free 블록 덩어리(run)"
> 개수를 셈. fresh 디스크는 free 공간이 한 덩어리(~1-2개)지만, aging이 구멍을
> 뚫으면 run 개수가 급증(~20+). fs.img가 재부팅 후에도 유지되므로
> "이 디스크가 이미 aged 상태인가?"를 빠르게 판별하는 게이지로 사용.

```c
// Count the number of free "runs" in the bitmap: maximal spans of
// consecutive free blocks. A fresh disk keeps its free space in one big
// contiguous run (~1-2 regions); after aging punches alternating holes
// the count jumps to many (~20+). Used as a quick "is the disk already
// aged?" gauge, since fs.img persists across reboots.
uint64
sys_get_free_runs(void)
{
  int runs = 0;
  int prev_free = 0;   // was the previous block free?

  for(uint b = 0; b < sb.size; b += BPB){
    struct buf *bp = bread(ROOTDEV, BBLOCK(b, sb));
    for(int bi = 0; bi < BPB && b + bi < sb.size; bi++){
      int m = 1 << (bi % 8);
      int is_free = (bp->data[bi/8] & m) == 0;
      if(is_free && !prev_free)
        runs++;          // start of a new free run
      prev_free = is_free;
    }
    brelse(bp);
  }
  return runs;
}
```

---

## 9. `user/aging.c` (실제 구현)

원본과의 차이:
- `snprintf` 대신 **`mkname()` 헬퍼 직접 구현** (xv6 user lib엔 snprintf 없음)
- 시작 시 **`get_free_runs()`로 디스크 상태 출력** ("fresh" vs "already AGED")
- **`frag.txt`를 먼저 unlink** → 그 블록들이 free pool로 돌아가 hole 패턴에 흡수됨
- 블록 번호(47~166 등) 하드코딩된 출력 제거
- `return 0` → `exit(0)`

```c
// user/aging.c — Aging Workload
//
// Simulates a "used" disk that has had many files created and deleted,
// leaving non-consecutive free blocks (holes) in the bitmap. A file
// allocated afterwards is forced to use those scattered holes, so it
// ends up fragmented.
//
// Mechanism (xv6 balloc is lowest-free-block-first):
//   1. Remove frag.txt so its blocks rejoin the free pool.
//   2. Create f0..f39, each 3 blocks -> they fill the lowest free blocks
//      consecutively: f0=[b,b+1,b+2], f1=[b+3,b+4,b+5], ...
//   3. Delete the even files (f0,f2,...) -> alternating 3-block holes
//      become the lowest free blocks on the disk.
//   Now any new file (e.g. frag.txt from fragtest) is scattered across
//   those holes instead of getting one contiguous run.
//
// Run sequence:
//   $ fragtest      <- baseline on a clean disk (low %)
//   $ aging         <- punch holes
//   $ fragtest      <- now frag.txt lands in the holes (high %)

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Build the filename "f<i>" into buf (xv6 user lib has no snprintf).
// i is in 0..39, so at most two digits.
static void
mkname(char *buf, int i)
{
  char *p = buf;
  *p++ = 'f';
  if(i >= 10)
    *p++ = '0' + i / 10;
  *p++ = '0' + i % 10;
  *p = 0;
}

int
main(void)
{
  char name[16];
  char buf[1024];

  // Content does not matter — only which blocks get allocated.
  memset(buf, 0x41, sizeof(buf));  // 'A'

  // Report the incoming disk state so it's clear whether this run is
  // aging a fresh disk or a disk that's already aged (fs.img persists
  // across reboots, so the holes survive).
  int runs = get_free_runs();
  printf("Disk state before aging: %d free region(s) -- %s\n", runs,
         runs <= 3 ? "fresh" : "already AGED");

  // Free the baseline test file so its (contiguous) blocks rejoin the
  // pool and get folded into the alternating hole pattern below.
  unlink("frag.txt");

  // ── Step 1: Create 40 small files, each 3 KB (3 blocks) ──────────
  for(int i = 0; i < 40; i++){
    mkname(name, i);

    int fd = open(name, O_CREATE|O_WRONLY);
    if(fd < 0){
      printf("aging: could not create %s\n", name);
      exit(1);
    }

    // Write 3 KB = 3 blocks; each block boundary triggers balloc().
    for(int b = 0; b < 3; b++)
      write(fd, buf, sizeof(buf));

    close(fd);
  }
  printf("Step 1: created 40 files (f0..f39), each 3 KB.\n");

  // ── Step 2: Delete every other file ──────────────────────────────
  // Deletes f0, f2, ... f38 — leaves alternating free/used 3-block runs
  // as the lowest free blocks on the disk.
  for(int i = 0; i < 40; i += 2){
    mkname(name, i);
    unlink(name);
  }
  printf("Step 2: deleted f0, f2, f4 ... f38 (20 files).\n");
  printf("  Bitmap now has alternating free/used 3-block runs.\n");
  printf("Disk state after aging: %d free region(s).\n", get_free_runs());
  printf("Aging complete. Run 'fragtest' to measure fragmentation.\n");

  exit(0);
}
```

---

## 10. `user/fragtest.c` (실제 구현)

원본과의 차이:
- `fresh.txt` + `aged.txt` 2개 → **`frag.txt` 단일 파일**, 매 실행마다 unlink 후 재생성
- 측정도 1회 (전/후는 aging을 사이에 두고 fragtest를 두 번 실행해서 대비)
- **`get_free_runs()`로 디스크 상태 출력** + "rm fs.img to reset" 안내

```c
// user/fragtest.c — Fragmentation Measurement Test
//
// Creates a single 8 KB test file (frag.txt) and reports how scattered
// its data blocks are on disk via get_fragmentation().
//
// The contrast comes from running it before vs. after aging:
//   $ fragtest      <- clean disk: blocks are contiguous   -> low %
//   $ aging         <- punch alternating holes in the bitmap
//   $ fragtest      <- frag.txt now lands in those holes    -> high %
//
// We unlink frag.txt first so each run forces a fresh allocation:
// reopening an existing file would just reuse its old block layout.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Display a 20-character ASCII bar for a percentage value.
static void
print_bar(int score)
{
  int filled = score * 20 / 100;
  printf("  [");
  for(int i = 0; i < 20; i++)
    printf(i < filled ? "#" : ".");
  printf("]  %d%%\n", score);
}

int
main(void)
{
  char buf[1024];
  memset(buf, 0x42, sizeof(buf));  // 'B'

  // Report the incoming disk state. fs.img persists across qemu reboots,
  // so a disk aged in a previous session stays aged: this line tells you
  // whether you're measuring a fresh disk or one that 'aging' already hit.
  int runs = get_free_runs();
  printf("Disk state: %d free region(s) -- %s\n", runs,
         runs <= 3 ? "fresh" : "AGED (rm fs.img to reset)");

  // Force a fresh allocation: drop any previous frag.txt first.
  unlink("frag.txt");

  int fd = open("frag.txt", O_CREATE|O_RDWR);
  if(fd < 0){
    printf("fragtest: cannot create frag.txt\n");
    exit(1);
  }
  for(int i = 0; i < 8; i++)
    write(fd, buf, 1024);

  int score = get_fragmentation(fd);
  close(fd);

  printf("frag.txt (8 KB):\n");
  print_bar(score);
  printf("  Low %% on a clean disk; higher %% after running 'aging'.\n");

  exit(0);
}
```

---

## 11. 실행 순서 & 기대 동작

```sh
make clean && make qemu CPUS=1
```

xv6 셸에서:

```
$ fragtest        # 깨끗한 디스크: "fresh", 낮은 % (연속 할당)
$ aging           # 구멍 뚫기: free region 수 급증
$ fragtest        # frag.txt가 구멍에 흩어짐: "AGED", 높은 %
```

> fs.img는 재부팅 후에도 유지되므로, 다시 처음부터 측정하려면 `rm fs.img`
> 후 재빌드. `get_free_runs()` 출력의 "fresh / AGED" 표시로 현재 상태 확인 가능.

---

## 12. 체크리스트

- [x] `kernel/syscall.h` — 26, 27 두 개 정의
- [x] `kernel/syscall.c` — extern 2 + 테이블 2
- [x] `kernel/sysfile.c` — `#include "buf.h"` + `sys_get_fragmentation()`
- [x] `kernel/sysproc.c` — `sys_get_free_runs()`
- [x] `user/usys.pl` — entry 2
- [x] `user/user.h` — 선언 2
- [x] `user/aging.c` — 생성
- [x] `user/fragtest.c` — 생성
- [x] `Makefile` — `$U/_aging`, `$U/_fragtest`
