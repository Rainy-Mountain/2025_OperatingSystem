//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "buf.h"

static int next_snap_id = 1;   // 부팅마다 1부터 시작

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

static void
format_snap_name(int id, char name[3])
{
  if(id < 0) id = -id;
  name[0] = '0' + (id / 10) % 10;
  name[1] = '0' + (id % 10);
  name[2] = 0;
}


// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}


/////////////////////////////
// 스냅샷 관련 시스템 콜 모음 //
////////////////////////////

// 전체 스냅샷 생성 시스템 콜
int
sys_snapshot_create(void)
{
  struct inode *snaproot, *root, *dst;
  int id;
  char name[4];

  // ── 1) /snapshot/NN 디렉토리만 만드는 작은 트랜잭션 ──
  begin_op();

  snaproot = get_snapshot_root();    // 여기서 /snapshot 없으면 만들어줌
  if (!snaproot) {
    end_op();
    return -1;
  }

  id = next_snap_id++;
  format_snap_name(id, name);

  root = namei("/");
  if (!root) {
    iput(snaproot);
    end_op();
    return -1;
  }

  dst = create_in_dir(snaproot, name, T_DIR, 0, 0);
  if (!dst) {
    iput(root);
    iput(snaproot);
    end_op();
    return -1;
  }

  iunlock(dst);
  iput(snaproot);

  end_op();   // 여기까지는 /snapshot/NN 디렉토리 생성만

  // ── 2) 실제 트리 복사는 clone_tree_for_snapshot 안에서
  //       엔트리 하나마다 begin_op/end_op 로 잘게 나눈다 ──
  clone_tree_for_snapshot(root, dst, 1);

  iput(root);
  iput(dst);

  return id;
}




// 전체 스냅샷 삭제 시스템 콜
// 스냅샷 디렉토리 하나(/snapshot/NN)를 통째로 지우는 syscall
int
sys_snapshot_delete(void)
{
  int id;
  char name[4];
  struct inode *snaproot;
  struct inode *snap;
  uint off;
  int isdir;

  if (argint(0, &id) < 0)
    return -1;

  format_snap_name(id, name);

  // /snapshot 디렉토리 찾기
  snaproot = namei("/snapshot");
  if (!snaproot)
    return -1;

  ilock(snaproot);
  if (snaproot->type != T_DIR) {
    iunlockput(snaproot);
    return -1;
  }

  // snaproot 락 상태에서 dirlookup 수행 (snap은 LOCKED로 반환됨)
  snap = dirlookup(snaproot, name, &off);
  if (!snap) {
    iunlock(snaproot);
    iput(snaproot);
    return -1;
  }

  isdir = (snap->type == T_DIR);

  // 핵심: snaproot 락을 먼저 해제
  iunlock(snaproot);

  // 스냅샷 디렉토리 트리 삭제
  delete_tree(snap, 0);   // 여기서 snap은 LOCKED 상태, 내부에서 unlock 후 iput함

  // /snapshot 엔트리 제거
  begin_op();
  ilock(snaproot);

  if (isdir && snaproot->nlink > 0) {
    snaproot->nlink--;
    iupdate(snaproot);
  }

  struct dirent zero;
  memset(&zero, 0, sizeof(zero));
  if (writei(snaproot, (char*)&zero, off, sizeof(zero)) != sizeof(zero))
    panic("snap_delete: write dirent");

  iunlock(snaproot);
  iput(snaproot);
  end_op();

  return 0;
}



// 스냅샷으로 롤백 시스템 콜
int
sys_snapshot_rollback(void)
{
  int id;
  char name[4];
  struct inode *snaproot, *snap, *root;

  if (argint(0, &id) < 0)
    return -1;

  format_snap_name(id, name);

  // 1) /snapshot 디렉토리 찾기 (읽기만, 트랜잭션 필요 없음)
  snaproot = namei("/snapshot");
  if (snaproot == 0)
    return -1;

  ilock(snaproot);
  if (snaproot->type != T_DIR) {
    iunlockput(snaproot);
    return -1;
  }

  snap = dirlookup(snaproot, name, 0);   // snap 은 LOCKED 상태로 반환
  iunlock(snaproot);
  iput(snaproot);

  if (snap == 0)
    return -1;

  // 루트 inode
  root = iget(ROOTDEV, ROOTINO);   // UNLOCKED
  if (root == 0) {
    iput(snap);
    return -1;
  }

  // 2) 루트에서 snapshot 제외하고 모두 삭제
  delete_root_except_snapshot(root);

  // 3) 스냅샷 트리를 루트 아래로 복사
  clone_tree_for_snapshot(snap, root, 1);

  iput(root);
  iput(snap);
  return 0;
}



int
sys_print_addr(void)
{
  char *path;
  struct inode *ip;
  int user_addrs, user_indirect;
  int addrs[NDIRECT+1];
  int indirect[NINDIRECT];
  int i;

  if (argstr(0, &path) < 0)
    return -1;
  if (argint(1, &user_addrs) < 0)
    return -1;
  if (argint(2, &user_indirect) < 0)
    return -1;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  for(i=0; i<NDIRECT+1; i++)
    addrs[i] = ip->addrs[i];

  if(ip->addrs[NDIRECT]) {
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    memmove(indirect, bp->data, sizeof(indirect));
    brelse(bp);
  } else {
    memset(indirect, 0, sizeof(indirect));
  }

  iunlockput(ip);
  end_op();

  struct proc *p = myproc();

  if(copyout(p->pgdir, user_addrs, (char*)addrs, sizeof(addrs)) < 0)
      return -1;
  if(copyout(p->pgdir, user_indirect, (char*)indirect, sizeof(indirect)) < 0)
      return -1;

  return 0;
}
