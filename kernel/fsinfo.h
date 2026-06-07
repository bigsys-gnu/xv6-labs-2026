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
