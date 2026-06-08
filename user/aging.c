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
