#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fsinfo.h"
#include "user/user.h"

// xv6's printf has no width specifiers or floats, so format
// fields by hand into rotating static buffers and print with %s.

// Format n padded to width w. Right-aligned, or left-aligned if left != 0.
char*
fmtnum(int n, int w, int left)
{
  static char bufs[8][16];
  static int next;
  char *buf = bufs[next++ % 8];
  char digs[12];
  int i = 0, len, pad, neg = 0;

  if(n < 0){
    neg = 1;
    n = -n;
  }
  do {
    digs[i++] = '0' + n % 10;
    n /= 10;
  } while(n > 0);
  if(neg)
    digs[i++] = '-';

  len = i;
  pad = w > len ? w - len : 0;
  i = 0;
  if(!left)
    while(pad-- > 0) buf[i++] = ' ';
  for(int j = len - 1; j >= 0; j--)
    buf[i++] = digs[j];
  if(left)
    while(pad-- > 0) buf[i++] = ' ';
  buf[i] = 0;
  return buf;
}

// Format s left-aligned in width w (like %-6s).
char*
fmtstr(char *s, int w)
{
  static char bufs[4][16];
  static int next;
  char *buf = bufs[next++ % 4];
  int i = 0;

  while(*s && i < 15)
    buf[i++] = *s++;
  while(i < w && i < 15)
    buf[i++] = ' ';
  buf[i] = 0;
  return buf;
}

// Format part/total as a percentage with one decimal, width 4 (like %4.1f).
char*
fmtpct(int part, int total)
{
  static char bufs[4][8];
  static int next;
  char *buf = bufs[next++ % 4];
  int p = total > 0 ? (part * 1000 + total/2) / total : 0;
  char *whole = fmtnum(p / 10, 2, 0);

  buf[0] = whole[0];
  buf[1] = whole[1];
  buf[2] = '.';
  buf[3] = '0' + p % 10;
  buf[4] = 0;
  return buf;
}

void
print_superblock(struct fsinfo *info)
{
  printf("\n=== xv6 File System ===\n\n");
  printf("[ Superblock ]\n");
  printf("  Total blocks  : %s    Log start    : %s\n",
          fmtnum(info->size, 5, 0), fmtnum(info->logstart, 3, 0));
  printf("  Data blocks   : %s    Inode start  : %s\n",
          fmtnum(info->nblocks, 5, 0), fmtnum(info->inodestart, 3, 0));
  printf("  Inodes        : %s    Bitmap block : %s\n",
          fmtnum(info->ninodes, 5, 0), fmtnum(info->bmapstart, 3, 0));
}

void
print_layout(struct fsinfo *info)
{
  int free_blks = get_free_blocks();
  int used_blks = info->nblocks - free_blks;

  printf("\n[ Disk Layout ]\n");
  printf("  blk   0     | boot block\n");
  printf("  blk   1     | superblock\n");
  printf("  blk %s-%s   | log (%d blocks)\n",
          fmtnum(info->logstart, 2, 0),
          fmtnum(info->logstart + info->nlog - 1, 2, 0), info->nlog);
  printf("  blk %s-%s   | inode blocks (%d inodes)\n",
          fmtnum(info->inodestart, 2, 0),
          fmtnum(info->bmapstart - 1, 2, 0), info->ninodes);
  printf("  blk %s      | bitmap\n", fmtnum(info->bmapstart, 2, 0));
  printf("  blk %s-%s | data  (used: %d / free: %d)\n",
          fmtnum(info->bmapstart + 1, 2, 0),
          fmtnum(info->size - 1, 4, 1), used_blks, free_blks);
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

    printf("  %s  %s  %s  %s  %s\n",
           fmtnum(i, 4, 0), fmtstr(tstr, 6), fmtnum(di.nlink, 4, 0),
           fmtnum(di.size, 6, 0), fmtnum(nblks, 4, 0));
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
  printf("  Used: %s (%s%%)  Free: %s (%s%%)\n",
         fmtnum(used_blks, 4, 0), fmtpct(used_blks, info->nblocks),
         fmtnum(free_blks, 4, 0), fmtpct(free_blks, info->nblocks));

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
