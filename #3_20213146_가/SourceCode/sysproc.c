#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

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
extern struct physframe_info pf_info[PFNNUM];

int sys_dump_physmem_info(void){
  void *uaddr; int maxn;
  if(argptr(0,(void*)&uaddr,sizeof(void*))<0) return -1;
  if(argint(1,&maxn)<0) return -1;
  if(maxn <= 0) return 0;

  int n = maxn < PFNNUM ? maxn : PFNNUM;

  // 스냅샷 일관성 확보: 복사 구간 동안 kmem.lock으로 고정
  if(kmem.use_lock) acquire(&kmem.lock);
  int rc = copyout(myproc()->pgdir, (uint)uaddr,
                   (void*)pf_info, sizeof(struct physframe_info)*n);
  if(kmem.use_lock) release(&kmem.lock);

  return rc < 0 ? -1 : n;
}

// page directory index
#define PDX(va)         (((uint)(va) >> PDXSHIFT) & 0x3FF)

// page table index
#define PTX(va)         (((uint)(va) >> PTXSHIFT) & 0x3FF)

// int sw_vtop(pde_t *pgdir, void *va, unsigned *pa_out, unsigned *flags_out) {
//   pde_t pde = pgdir[PDX(va)];
//   if(!(pde & PTE_P)) {
//     return -1; // Page directory entry not present
//   }

//   pte_t *ptab = (pte_t*)P2V(PTE_ADDR(pde));
//   pte_t pte = ptab[PTX(va)];
//   if(!(pte & PTE_P)) {
//     return -1; // Page table entry not present
//   }

//   if(pa_out) {
//     *pa_out = PTE_ADDR(pte) | ((uint)va & 0xFFF);
//   }
//   if(flags_out) {
//     *flags_out = PTE_FLAGS(pte);
//   }

//   return 0; // Success
// }

// int sys_vtop(void *va, unsigned *pa_out, unsigned *flags_out) {
//   pde_t *pgdir = myproc()->pgdir;
//   if(!pgdir || !va || !pa_out) {
//     return -1; // Invalid arguments
//   }

//   int rc = sw_vtop(pgdir, va, pa_out, flags_out);
//   if(rc < 0) {
//     return -1; // Page not present
//   }

//   return 0; // Success
// }

