#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "ipt.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

extern struct spinlock kmem_lock_alias;
extern struct {
  struct spinlock lock;
  struct run *freelist;
  int use_lock;
} kmem;
struct physframe_info {
  uint frame_index;
  int  allocated;
  int  pid;
  uint start_tick;
  int  ref_count;  // (사용자님이 사용한 이름 'ref_count' 유지)
};
extern struct physframe_info pf_info[PFNNUM];

// physframe_info 배열을 사용자 공간으로 복사하는 시스템 콜
int sys_dump_physmem_info(void){
  return 0;
}

// page directory index
#define PDX(va)         (((uint)(va) >> PDXSHIFT) & 0x3FF)

// page table index
#define PTX(va)         (((uint)(va) >> PTXSHIFT) & 0x3FF)

// 현재 프로세스에서 sw_vtop을 호출하는 시스템 콜
int
sys_vtop(void)
{
  void *va;
  uint *pa_out;       // 사용자 공간 포인터 (값)
  uint *flags_out;    // 사용자 공간 포인터 (값)

  uint pa_kernel;     // 커널 내 임시 저장 변수
  uint flags_kernel;  // 커널 내 임시 저장 변수

  // 1. 사용자 공간 포인터 인자를 "정수(주소값)"로 가져오기
  if (argint(0, (int*)&va) < 0 ||
      argint(1, (int*)&pa_out) < 0 ||
      argint(2, (int*)&flags_out) < 0) {
    return -1;
  }
  
  // 2. 현재 프로세스(myproc())의 pgdir을 이용해 sw_vtop 호출
  if (sw_vtop(myproc()->pgdir, va, &pa_kernel, &flags_kernel) < 0) {
    return -1; // 매핑 없음
  }

  // 3. 결과를 사용자 공간 포인터에 안전하게 복사
  if (copyout(myproc()->pgdir, (uint)pa_out, &pa_kernel, sizeof(uint)) < 0) {
    return -1;
  }

  if (copyout(myproc()->pgdir, (uint)flags_out, &flags_kernel, sizeof(uint)) < 0) {
    return -1;
  }

  return 0;
}

// 물리주소(페이지 단위)를 받아서
// 해당 물리주소를 매핑한 (pid, va_page, flags) 리스트를
// 사용자 공간 버퍼에 복사하는 시스템 콜
int
sys_phys2virt(void)
{
  uint pa_page;
  struct vlist *out; // 사용자 공간 포인터
  int max;
  
  uint pfn;
  struct ipt_entry *e;
  struct vlist *k_buf; // 커널 공간 임시 버퍼
  int count = 0;
  int max_per_page;

  // 1. 시스템 콜 인자(물리주소, 유저버퍼, 최대개수) 가져오기
  if (argint(0, (int*)&pa_page) < 0 ||
      argint(1, (int*)&out) < 0 ||
      argint(2, &max) < 0) {
    return -1;
  }
  
  if (max <= 0) 
    return 0;

  // 2. 커널 임시 버퍼 할당
  if ((k_buf = (struct vlist*)kalloc()) == 0) {
    return -1; // 메모리 부족
  }
  
  // kalloc으로 할당받은 한 페이지에 들어갈 수 있는 최대 엔트리 수 계산
  max_per_page = PGSIZE / sizeof(struct vlist);
  if (max > max_per_page) {
    max = max_per_page; // 사용자가 요청한 max가 한 페이지보다 크면, 페이지 크기로 제한
  }

  // 3. PFN(물리 프레임 번호) 계산 및 검증
  pfn = pa_page >> 12; // 0x12345678 -> 0x12345
  if (pfn >= PFNNUM) {
    kfree((char*)k_buf);
    return -1; // 유효하지 않은 PFN
  }

  // 4. IPT 순회 (반드시 락을 잡고 수행해야 함!)
  acquire(&ipt_lock);
  
  e = ipt_by_pfn[pfn]; // PFN 리스트의 헤드
  
  // (e != 0) 리스트의 끝까지, (count < max) 사용자가 요청한 최대 개수까지
  while (e != 0 && count < max) {
    // IPT 엔트리 정보를 k_buf에 복사
    k_buf[count].pid = e->pid;
    k_buf[count].va_page = e->va;
    k_buf[count].flags = e->flags; // ipt_map에서 저장한 플래그
    
    count++;
    e = e->next_pfn; // 다음 엔트리 (COW인 경우 다음 엔트리가 있을 것임)
  }
  
  release(&ipt_lock);

  // 5. 락을 푼 뒤, 수집된 결과를 사용자 공간으로 복사
  if (count > 0) {
    if (copyout(myproc()->pgdir, (uint)out, k_buf, count * sizeof(struct vlist)) < 0) {
      kfree((char*)k_buf);
      return -1; // copyout 실패
    }
  }

  // 6. 커널 임시 버퍼 해제
  kfree((char*)k_buf);

  return count; // 찾은 엔트리 개수 반환
}