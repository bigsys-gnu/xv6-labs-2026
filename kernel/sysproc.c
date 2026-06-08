#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"
#include "fsinfo.h"

extern struct superblock sb;

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Copy the superblock's layout fields to a user struct fsinfo.
uint64
sys_get_fsinfo(void)
{
  uint64 addr;
  struct fsinfo info;

  argaddr(0, &addr);

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

// Copy the on-disk inode inum to a user struct dinode.
uint64
sys_get_inode_info(void)
{
  int inum;
  uint64 addr;
  struct buf *bp;
  struct dinode di;

  argint(0, &inum);
  argaddr(1, &addr);
  if(inum <= 0 || inum >= (int)sb.ninodes)
    return -1;

  bp = bread(ROOTDEV, IBLOCK(inum, sb));
  memmove(&di, (struct dinode*)bp->data + inum % IPB, sizeof(di));
  brelse(bp);

  if(copyout(myproc()->pagetable, addr, (char*)&di, sizeof(di)) < 0)
    return -1;
  return 0;
}

// Count zero bits in the free-block bitmap.
uint64
sys_get_free_blocks(void)
{
  int free_count = 0;

  for(uint b = 0; b < sb.size; b += BPB){
    struct buf *bp = bread(ROOTDEV, BBLOCK(b, sb));
    for(int bi = 0; bi < BPB && b + bi < sb.size; bi++){
      int m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0)
        free_count++;
    }
    brelse(bp);
  }
  return free_count;
}

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
