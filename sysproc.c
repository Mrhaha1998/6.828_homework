#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

struct callerregs {
    uint eax;
    uint ecx;
    uint edx;
    uint eip;
};

int
sys_fork(void)
{
    return fork();
}

int
sys_exit(void)
{
    exit();
    return 0;    // not reached
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
    int a, n;
    struct proc *curproc;

    if(argint(0, &n) < 0)
        return -1;
    curproc = myproc();    
    if(n < 0)
        return shrinkheap(curproc->pgdir, &curproc->heap, -n);
    a = curproc->heap.start + curproc->heap.sz;
    if(expandheap(curproc->pgdir, &curproc->heap, n) < 0)
        return -1;
    return a;
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

int
sys_date(void)
{
    char *date;
    if(argptr(0, &date, sizeof(struct rtcdate)) < 0) {
        return -1;
    }
    cmostime((struct rtcdate*)date);
    return 0;
}

int
sys_alarm(void)
{
    int ticks;
    uint handler;
    uint handlerret;

    if(argptr(-1, (char**)&handlerret, 0) < 0)
        return -1; 
    if(argint(1, &ticks) < 0)
        return -1;
    if(argptr(2, (char**)&handler, 0) < 0)
        return -1;
    myproc()->alarmticks = ticks;
    myproc()->alarmticksleft = ticks;
    myproc()->alarmhandler = handler;
    myproc()->alarmhandlerret = handlerret;
    return 0;
}

int
sys_rstoregs(void)
{

    /*  ----------------------
        |eip edx ecx eax      |
        ----------------------
        |保存的寄存器的值的地址|
        ----------------------
        |rstoregs 地址       |
        --------------------  <--esp
    */
    struct callerregs *regs;
    struct trapframe *tf;

    if(argptr(-1, (char **)&regs, sizeof(struct callerregs)) < 0) {
        return -1;
    }
    tf = myproc()->tf;
    tf->eip = regs->eip;
    tf->eax = regs->eax;
    tf->ecx = regs->ecx;
    tf->edx = regs->edx;
    tf->esp +=  20;
    myproc()->inalarmhandler = 0;
    return tf->eax;
}
