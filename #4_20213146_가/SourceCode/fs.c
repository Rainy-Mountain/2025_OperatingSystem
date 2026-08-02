// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

// Block reference count table
static ushort block_ref[FSSIZE];
static struct spinlock block_ref_lock;


void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Block reference count helpers
void
incref_block(uint bno)
{
  if (bno >= FSSIZE)
    panic("incref_block: invalid block");

  acquire(&block_ref_lock);
  block_ref[bno]++;
  release(&block_ref_lock);
}

int
decref_block(uint bno)
{
  if (bno >= FSSIZE)
    panic("decref_block: invalid block");

  int ret;

  acquire(&block_ref_lock);

  if (block_ref[bno] == 0) {
    release(&block_ref_lock);
    return -1;
  }

  block_ref[bno]--;
  ret = block_ref[bno];

  release(&block_ref_lock);
  return ret;   // 0이면 마지막 참조, 양수면 아직 누가 더 쓰는 중
}


ushort
getref_block(uint bno)
{
  if (bno >= FSSIZE)
    panic("getref_block: invalid block");

  acquire(&block_ref_lock);
  ushort r = block_ref[bno];
  release(&block_ref_lock);
  return r;
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        // Initialize reference count to 1
        incref_block(b + bi);

        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  int r = decref_block(b);
  if (r > 0)
    return;

  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  initlock(&block_ref_lock, "block_ref");
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}


// ip의 bn번째 블럭 주소 반환
// 블럭이 존재하지 않을 경우 Copy-On-Write 방식으로 새로 할당/복사하여 반환
static uint
bmap_cow(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  // 1) 디렉토리는 COW 안 함 (과제 조건)
  if(ip->type == T_DIR) {
    // 그냥 기존 bmap 그대로 써도 됨
    return bmap(ip, bn);
  }

  // ──────────────
  // DIRECT block
  // ──────────────
  if(bn < NDIRECT){
    addr = ip->addrs[bn];

    // 아직 블록 없으면 새로 할당
    if(addr == 0){
      addr = balloc(ip->dev);   // 여기서 refcount = 1
      ip->addrs[bn] = addr;
      return addr;
    }

    // 이미 있는 블록인데 refcount > 1 이면 COW
    if(getref_block(addr) > 1){
      uint newb = balloc(ip->dev);  // refcount 1

      // 내용 복사
      struct buf *bp_old = bread(ip->dev, addr);
      struct buf *bp_new = bread(ip->dev, newb);
      memmove(bp_new->data, bp_old->data, BSIZE);
      log_write(bp_new);
      brelse(bp_old);
      brelse(bp_new);

      // 이전 블록 참조 하나 줄이기
      decref_block(addr);

      // inode가 새 블록을 가리키도록 변경
      ip->addrs[bn] = newb;
      addr = newb;
    }

    return addr;
  }

  // ──────────────
  // INDIRECT block
  // ──────────────
  bn -= NDIRECT;
  if(bn < NINDIRECT){
    // 간접 블록 자체가 없으면 새로 할당
    addr = ip->addrs[NDIRECT];
    if(addr == 0){
      addr = balloc(ip->dev);          // refcount 1
      ip->addrs[NDIRECT] = addr;
    } else if(getref_block(addr) > 1){
      // 간접 블록이 스냅샷과 공유 중이면 COW
      uint newind = balloc(ip->dev);   // refcount 1

      struct buf *bp_old = bread(ip->dev, addr);
      struct buf *bp_new = bread(ip->dev, newind);
      memmove(bp_new->data, bp_old->data, BSIZE);
      log_write(bp_new);
      brelse(bp_old);
      brelse(bp_new);

      decref_block(addr);              // 이전 간접 블록 refcount 감소
      ip->addrs[NDIRECT] = addr = newind;
    }

    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    uint bno = a[bn];

    // 데이터 블록 아직 없으면 새로 할당
    if(bno == 0){
      bno = balloc(ip->dev);           // refcount 1
      a[bn] = bno;
      log_write(bp);
      brelse(bp);
      return bno;
    }

    // 이미 있는 데이터 블록인데 refcount > 1이면 데이터 블록 COW
    if(getref_block(bno) > 1){
      uint newb = balloc(ip->dev);     // refcount 1

      struct buf *bp_old = bread(ip->dev, bno);
      struct buf *bp_new = bread(ip->dev, newb);
      memmove(bp_new->data, bp_old->data, BSIZE);
      log_write(bp_new);
      brelse(bp_old);
      brelse(bp_new);

      decref_block(bno);               // 이전 블록 refcount 감소

      a[bn] = newb;                    // 간접 블록 테이블 갱신
      log_write(bp);
      bno = newb;
    }

    brelse(bp);
    return bno;
  }

  panic("bmap_cow: out of range");
}

void
free_block_if_last(int dev, uint bno)
{
  int r = decref_block(bno);

  if (r < 0) {
    // 이미 0이었는데 또 free하려는 경우 → bfree 호출하지 않고 그냥 무시
    return;
  }

  if (r == 0) {
    // 이제 진짜 마지막 참조가 사라진 시점에만 bfree 호출
    bfree(dev, bno);
  }
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint bn = off / BSIZE;
    uint addr = bmap_cow(ip, bn);  // COW 적용된 bmap 사용
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}



// Snapshot 디렉토리의 inode를 반환
// - 이미 있으면 타입(T_DIR)만 확인하고 그대로 반환
// - 없으면 루트("/") 아래에 "snapshot" 디렉토리를 새로 만든다.
// Snapshot 디렉토리의 inode를 반환
// - 이미 있으면 타입(T_DIR)만 확인하고 그대로 반환
// - 없으면 루트("/") 아래에 "snapshot" 디렉토리를 새로 만든다.
// - 반환되는 inode는 "락이 풀린 상태" (필요하면 호출쪽에서 ilock/iput)
struct inode *
get_snapshot_root(void)
{
  struct inode *ip, *root, *snap;

  // 이미 존재?
  ip = namei("/snapshot");    // 반환 시점엔 UNLOCKED
  if (ip) {
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    iunlock(ip);              // 호출자는 unlock 상태의 inode를 받는다
    return ip;
  }

  // 없으면 "/" 아래에 새로 생성
  root = namei("/");
  if (!root)
    return 0;

  snap = create_in_dir(root, "snapshot", T_DIR, 0, 0);
  iput(root);

  if (!snap)
    return 0;

  iunlock(snap);              // create_in_dir는 LOCKED로 돌려주니까 풀고 반환
  return snap;
}




// 주어진 디렉토리(dp)에 name 이름으로 스냅샷용 디렉토리를/파일을 생성
// 주어진 디렉토리(dp)에 name 이름으로 스냅샷용 디렉토리/파일 생성
// - 이미 같은 타입의 엔트리가 있으면 그 inode를 락 잡힌 상태로 반환
// - 새로 만들면 새 inode를 락 잡힌 상태로 반환
struct inode *
create_in_dir(struct inode *dp, char *name,
              short type, short major, short minor)
{
  struct inode *ip;
  uint off;

  // 부모 디렉토리 먼저 락
  ilock(dp);

  // 1) 이미 존재하는지 체크
  if ((ip = dirlookup(dp, name, &off)) != 0) {
    // dirlookup은 ip를 LOCKED 상태로 반환
    iunlock(dp);   // 부모는 이제 안 필요
    // 타입이 원하는 것과 같은 경우에만 그대로 사용
    if ((type == T_FILE && ip->type == T_FILE) ||
        (type == T_DIR  && ip->type == T_DIR)) {
      return ip;   // ip는 LOCKED 상태
    }
    // 타입이 다르면 쓸 수 없음
    iunlockput(ip);
    return 0;
  }

  // 2) 새 inode 할당
  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlock(dp);
    panic("create_in_dir: ialloc");
  }

  // ialloc은 UNLOCKED inode를 반환하므로 직접 락
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;          // 기본 링크 수
  iupdate(ip);

  // 3) 디렉토리인 경우 "." / ".." 설정 + 부모 nlink 증가
  if (type == T_DIR) {
    dp->nlink++;          // ".." 때문
    iupdate(dp);

    if (dirlink(ip, ".", ip->inum) < 0 ||
        dirlink(ip, "..", dp->inum) < 0)
      panic("create_in_dir: dots");
  }

  // 4) 부모 디렉토리에 (name -> ip->inum) 엔트리 추가
  if (dirlink(dp, name, ip->inum) < 0)
    panic("create_in_dir: dirlink");

  // 부모는 해제, ip는 락 잡힌 상태로 반환
  iunlock(dp);
  return ip;
}




// src 디렉토리 트리를 dst 아래로 복사
//  - 디렉토리는 새로 만들고
//  - 일반 파일은 데이터 블록을 공유 (refcount 증가)
//  - skip_snapshot == 1 인 경우, src == "/" 에서 "snapshot" 디렉토리는 건너뜀
void
clone_tree_for_snapshot(struct inode *src, struct inode *dst, int skip_snapshot)
{
  struct dirent de;
  uint off;
  char name[DIRSIZ];

  ilock(src);
  if (src->type != T_DIR)
    panic("clone_tree_for_snapshot: src not dir");

  for (off = 0; off < src->size; off += sizeof(de)) {

    if (readi(src, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("clone_tree_for_snapshot: readi");

    if (de.inum == 0)
      continue;
    if (namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0)
      continue;
    if (skip_snapshot && namecmp(de.name, "snapshot") == 0)
      continue;

    memmove(name, de.name, DIRSIZ);
    uint inum = de.inum;

    iunlock(src);    // src 잠시 풀고 child, dst 작업

    struct inode *child = iget(src->dev, inum);
    if (child == 0) {
      ilock(src);
      continue;
    }

    ilock(child);

    // ───── DIRECTORY : 구조만 재귀 복제 ─────
    if (child->type == T_DIR) {

      begin_op();   // ★ 이 디렉토리 생성만을 위한 트랜잭션

      struct inode *newdir = create_in_dir(dst, name, T_DIR, 0, 0);
      if (!newdir) {
        iunlock(child);
        iput(child);
        end_op();
        ilock(src);
        continue;
      }

      // newdir 는 LOCKED 상태로 반환됨
      iunlock(newdir);
      end_op();     // 디렉토리 생성 관련 변경 커밋

      iunlock(child);

      // 자식들 복제는 별도 트랜잭션들로 처리
      clone_tree_for_snapshot(child, newdir, 0);

      iput(child);
      iput(newdir);
    }

    // ───── FILE : COW 방식으로 블록 공유 ─────
    else if (child->type == T_FILE) {

      begin_op();   // ★ 이 파일 하나를 위한 트랜잭션

      struct inode *newfile = create_in_dir(dst, name, T_FILE, 0, 0);
      if (!newfile) {
        iunlock(child);
        iput(child);
        end_op();
        ilock(src);
        continue;
      }

      newfile->size = child->size;

      // direct blocks 공유 + refcount 증가
      for (int i = 0; i < NDIRECT; i++) {
        uint a = child->addrs[i];
        newfile->addrs[i] = a;
        if (a)
          incref_block(a);
      }

      // indirect block 공유 + refcount 증가
      if (child->addrs[NDIRECT]) {
        uint ind = child->addrs[NDIRECT];
        newfile->addrs[NDIRECT] = ind;
        incref_block(ind);

        struct buf *bp = bread(child->dev, ind);
        uint *arr = (uint*)bp->data;
        for (int j = 0; j < NINDIRECT; j++) {
          if (arr[j])
            incref_block(arr[j]);
        }
        brelse(bp);
      } else {
        newfile->addrs[NDIRECT] = 0;
      }

      iupdate(newfile);

      iunlock(newfile);
      iput(newfile);

      iunlock(child);
      iput(child);

      end_op();     // 이 파일에 대한 변경 커밋
    }

    // ───── 기타 타입은 그냥 무시 ─────
    else {
      iunlock(child);
      iput(child);
    }

    ilock(src);    // 다음 엔트리 읽기 위해 다시 락
  }

  iunlock(src);
}





// 디렉토리/파일 트리를 재귀적으로 삭제
//  - ip가 파일이면: 바로 itrunc + inode free
//  - ip가 디렉토리면: 자식들에 대해 재귀 호출 후, 자신도 free
// 디렉토리/파일 트리를 재귀적으로 삭제
// - ip가 파일이면: 바로 itrunc + inode free
// - ip가 디렉토리면: 자식들 먼저 모두 delete_tree, 마지막에 자기 자신 삭제
// delete_tree(): in_txn == 1 이면 이미 begin_op 안에 있으므로 새로 열지 않음
void
delete_tree(struct inode *ip, int in_txn)
{
  struct dirent de;
  uint off;

  if (!in_txn)
    begin_op();   // 🔥 최상위 호출에서만 트랜잭션 시작

  ilock(ip);

  // ────────────── FILE ──────────────
  if (ip->type == T_FILE) {
    // direct blocks
    for (int i = 0; i < NDIRECT; i++) {
      uint a = ip->addrs[i];
      if (a)
        free_block_if_last(ip->dev, a);  // ✅ 항상 begin_op 안에서 호출됨
      ip->addrs[i] = 0;
    }

    // indirect blocks
    if (ip->addrs[NDIRECT]) {
      uint ind = ip->addrs[NDIRECT];
      struct buf *bp = bread(ip->dev, ind);
      uint *arr = (uint*)bp->data;

      for (int j = 0; j < NINDIRECT; j++) {
        if (arr[j])
          free_block_if_last(ip->dev, arr[j]); // ✅ begin_op 안
        arr[j] = 0;
      }

      log_write(bp);
      brelse(bp);
      free_block_if_last(ip->dev, ind); // ✅ begin_op 안
      ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
    iunlockput(ip);

    if (!in_txn)
      end_op();   // 🔥 최상위 호출에서만 트랜잭션 닫기
    return;
  }

  // ────────────── DIRECTORY ──────────────
  for (off = 0; off < ip->size; off += sizeof(de)) {
    if (readi(ip, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("delete_tree: read");

    if (de.inum == 0 ||
        namecmp(de.name, ".") == 0 ||
        namecmp(de.name, "..") == 0)
      continue;

    struct inode *child = iget(ip->dev, de.inum);

    struct dirent zero;
    memset(&zero, 0, sizeof(zero));
    writei(ip, (char*)&zero, off, sizeof(zero));

    iunlock(ip);
    delete_tree(child, 1);  // ✅ 자식은 in_txn=1로 호출 (트랜잭션 재사용)
    ilock(ip);
  }

  ip->size = 2 * sizeof(struct dirent);
  iupdate(ip);
  iunlockput(ip);

  if (!in_txn)
    end_op();   // 🔥 최상위 호출만 트랜잭션 닫음
}






// 루트 디렉토리의 내용 중에서 /snapshot 디렉토리를 제외한
// 모든 파일 및 디렉토리를 삭제
// 루트 디렉토리의 내용 중에서 /snapshot 디렉토리를 제외한
// 모든 파일 및 디렉토리를 삭제
void
delete_root_except_snapshot(struct inode *root)
{
  struct dirent de;
  uint off;
  uint size;

  // root->size 는 삭제하면서도 그대로 유지하므로,
  // 루프 시작 전에 snapshot 떠서 사용해도 안전
  size = root->size;

  for (off = 0; off < size; off += sizeof(de)) {

    begin_op();   // ★ 이 엔트리 하나를 위한 트랜잭션

    ilock(root);

    if (readi(root, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("delete_root_except_snapshot: read");

    if (de.inum == 0 ||
        namecmp(de.name, ".") == 0 ||
        namecmp(de.name, "..") == 0 ||
        namecmp(de.name, "snapshot") == 0) {
      iunlock(root);
      end_op();
      continue;
    }

    // 이 엔트리의 inode
    struct inode *child = iget(root->dev, de.inum);

    // dirent 비우기
    struct dirent zero;
    memset(&zero, 0, sizeof(zero));
    if (writei(root, (char*)&zero, off, sizeof(zero)) != sizeof(zero))
      panic("delete_root_except_snapshot: write");

    iunlock(root);

    // child subtree 전체 삭제 (delete_tree 안에서는 begin_op 쓰지 않는다)
    delete_tree(child, 1);

    end_op();
  }
}
