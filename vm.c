#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];    // defined by kernel.ld
pde_t *kpgdir;    // for use in scheduler()
uint pgref[PHYSTOP >> PTXSHIFT];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
    struct cpu *c;

    // Map "logical" addresses to virtual addresses using identity map.
    // Cannot share a CODE descriptor for both kernel and user
    // because it would have to have DPL_USR, but the CPU forbids
    // an interrupt from CPL=0 to DPL=3.
    c = &cpus[cpuid()];
    c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
    c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
    c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
    c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
    lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.    If alloc!=0,
// create any required page table pages.
pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
    pde_t *pde;
    pte_t *pgtab;

    pde = &pgdir[PDX(va)];
    if(*pde & PTE_P){
        pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
    } else {
        if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
            return 0;
        // Make sure all those PTE_P bits are zero.
        memset(pgtab, 0, PGSIZE);
        // The permissions here are overly generous, but they can
        // be further restricted by the permissions in the page table
        // entries, if necessary.
        *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
    }
    return &pgtab[PTX(va)];
}



// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//     0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                                phys memory allocated by the kernel
//     KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//     KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                                for the kernel's instructions and r/o data
//     data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                                                    rw data + free physical memory
//     0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
    void *virt;
    uint phys_start;
    uint phys_end;
    int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,                         EXTMEM,        PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},         // kern text+rodata
 { (void*)data,         V2P(data),         PHYSTOP,     PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,            0,                 PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
    pde_t *pgdir;
    struct kmap *k;

    if((pgdir = (pde_t*)kalloc()) == 0)
        return 0;
    memset(pgdir, 0, PGSIZE);
    if (P2V(PHYSTOP) > (void*)DEVSPACE)
        panic("PHYSTOP too high");
    for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
        if(mapregion(pgdir, k->virt, k->phys_end - k->phys_start,
                                (uint)k->phys_start, k->perm) < 0) {
            freekvm();
            return 0;
        }
    return pgdir;
}

// copy kernel part of a page table
pde_t*
copykvm(void)
{
    pde_t *pgdir;
    if((pgdir = (pde_t*)kalloc()) == 0)
        return 0;
    memmove(pgdir, kpgdir, PGSIZE);
    return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
    kpgdir = setupkvm();
    switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
    lcr3(V2P(kpgdir));     // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
    if(p == 0)
        panic("switchuvm: no process");
    if(p->kstack == 0)
        panic("switchuvm: no kstack");
    if(p->pgdir == 0)
        panic("switchuvm: no pgdir");

    pushcli();
    mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                                                sizeof(mycpu()->ts)-1, 0);
    mycpu()->gdt[SEG_TSS].s = 0;
    mycpu()->ts.ss0 = SEG_KDATA << 3;
    mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
    // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
    // forbids I/O instructions (e.g., inb and outb) from user space
    mycpu()->ts.iomb = (ushort) 0xFFFF;
    ltr(SEG_TSS << 3);
    lcr3(V2P(p->pgdir));    // switch to process's address space
    popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
    char *mem;

    if(sz >= PGSIZE)
        panic("inituvm: more than a page");
    mem = kalloc();
    memset(mem, 0, PGSIZE);
    mappage(pgdir, 0, V2P(mem), PTE_W|PTE_U);
    memmove(mem, init, sz);
}

// Load a program segment into pgdir.    addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
    uint i, pa, n;
    pte_t *pte;

    if((uint) addr % PGSIZE != 0)
        panic("loaduvm: addr must be page aligned");
    for(i = 0; i < sz; i += PGSIZE){
        if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
            panic("loaduvm: address should exist");
        pa = PTE_ADDR(*pte);
        if(sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;
        if(readi(ip, P2V(pa), offset+i, n) != n)
            return -1;
    }
    return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.    Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint vstart, uint vend)
{
    char *mem;
    uint a;

    for(a = vstart; a < vend; a += PGSIZE){
        mem = kalloc();
        if(mem == 0){
            cprintf("allocuvm out of memory\n");
            return -1;
        }
        memset(mem, 0, PGSIZE);
        if(mappage(pgdir, (char*)a, V2P(mem), PTE_W|PTE_U) < 0){
            cprintf("allocuvm out of memory (2)\n");
            deallocuvm(pgdir, vstart, vend);
            kfree(mem);
            return -1;
        }
    }
    return 0;
}

int expandheap(pde_t *pgdir, struct mm_area *area, int sz)
{
    int trueend;

    if(sz < 0 || area->start + area->sz + sz > KERNBASE)
        return -1;
    area->sz += sz; 
    trueend = area->start + area->sz;    
    if(PGROUNDUP(trueend) > area->end){
        area->end = PGROUNDUP(trueend);
    }
    return 0;
}

int shrinkheap(pde_t *pgdir, struct mm_area *area, int sz)
{
    int trueend, oldend;

    if(sz > 0 || sz > area->sz)
        return -1;
    area->sz -= sz;
    trueend = area->start + area->sz;    
    if(PGROUNDUP(trueend) < area->end){
        oldend = area->end;
        area->end = PGROUNDUP(trueend);
        deallocuvm(pgdir, area->end, oldend);
    }
    return 0;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.    oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.    oldsz can be larger than the actual
// process size.    Returns the new process size.
void
deallocuvm(pde_t *pgdir, uint vstart, uint vend)
{
    pte_t *pte;
    uint a;

    pte = 0;
    for(a = vstart; a < vend; a += PGSIZE){
        unmappage(pgdir, (void *)a, &pte);
        if(!pte)
            a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    }
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
    uint i;

    if(pgdir == 0)
        panic("freevm: no pgdir");
    deallocuvm(pgdir, 0, KERNBASE);
    for(i = 0; i < (KERNBASE >> PDXSHIFT); i++){
        if(pgdir[i] & PTE_P){
            char * v = P2V(PTE_ADDR(pgdir[i]));
            kfree(v);
        }
    }
    kfree((char*)pgdir);
}


// This function is only intended to set up the kernal mapping section
// As such, it should *not* change the pgref field on the
// mapped pages.
int mapregion(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
    pte_t *pte;
    uint cnt, s;

    cnt= (size >> PTXSHIFT);
	for(s = 0; s < cnt; s += 1){
		if((pte = walkpgdir(pgdir, va, 1)) == 0)
            return -1;
		*pte = pa | perm | PTE_P;
		va += PGSIZE;
		pa += PGSIZE;
	}	
    return 0;
}

// this is used to build user process. So it should increase the pgref.
int mappage(pde_t *pgdir, void *va, uint pa, int perm)
{
	pte_t *pte;

    if((pte = walkpgdir(pgdir, va, 1)) == 0)
        return -1;
    ++pgref[PGNUM(pa)];
	if((*pte & PTE_P))
		unmappage(pgdir, va, 0);
	*pte = pa | perm | PTE_P;
	pgdir[PDX(va)] |= perm | PTE_P; 
	return 0;
}

// this is used to unmap a user page. So it should decrease the pgref.
void unmappage(pde_t *pgdir, void *va, pte_t **ptestrore)
{
    pte_t *pte;
    uint pa;
    
    pte = walkpgdir(pgdir, va, 0);
    if(ptestrore)
        *ptestrore = pte;
    if(!pte)
        return;
    if(*pte & PTE_P){
        pa = PTE_ADDR(*pte);
        *pte = 0;
        kdecref(pa);
	}		
}



void
freekvm(void)
{
    uint i;
    for(i = 0; i < NPDENTRIES; i++){
        if(kpgdir[i] & PTE_P){
            char * v = P2V(PTE_ADDR(kpgdir[i]));
            kfree(v);
        }
    }
    kfree((char*)kpgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if(pte == 0)
        panic("clearpteu");
    *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(struct proc *pp)
{
    pde_t *d, *pgdir;

    if((d = copykvm()) == 0)
        return 0;
    pgdir = pp->pgdir; 
    if((d = copyseg(pgdir, d, &pp->text_data)) == 0)
        return 0;
    if((d = copyseg(pgdir, d, &pp->stack)) == 0)
        return 0;
    if((d = copyseg(pgdir, d, &pp->heap)) == 0)
        return 0;         
    return d;
}

pde_t*
copyseg(pde_t *opgdir, pde_t *npgdir, struct mm_area *seg)
{
    uint i;
    pte_t *pte1;
    uint flag, pa;

    for(i = seg->start; i < seg->end; i += PGSIZE){
        if((pte1 = walkpgdir(opgdir, (void *) i, 0)) == 0){
            i = PGADDR(PDX(i) + 1, 0, 0) - PGSIZE;
            continue;
        }
        if((*pte1 & PTE_P)){     
            if(!(*pte1 & PTE_U))
                panic("copyseg");
            if((*pte1 & (PTE_W | PTE_COW))){
                *pte1 |= PTE_COW;
                *pte1 &= ~PTE_W; 
            }
            flag = PTE_FLAGS(*pte1);
            pa = PTE_ADDR(*pte1);
            if(mappage(npgdir, (void*)i, pa, flag) < 0)
                goto bad;
        }
    }
    return npgdir;

bad:
    freevm(npgdir);
    return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if((*pte & PTE_P) == 0)
        return 0;
    if((*pte & PTE_U) == 0)
        return 0;
    return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
    char *buf, *pa0;
    uint n, va0;

    buf = (char*)p;
    while(len > 0){
        va0 = (uint)PGROUNDDOWN(va);
        pa0 = uva2ka(pgdir, (char*)va0);
        if(pa0 == 0)
            return -1;
        n = PGSIZE - (va - va0);
        if(n > len)
            n = len;
        memmove(pa0 + (va - va0), buf, n);
        len -= n;
        buf += n;
        va = va0 + PGSIZE;
    }
    return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

