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
