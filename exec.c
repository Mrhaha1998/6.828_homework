#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
    char *s, *last;
    int off;
    uint argc, sp, ustack[3+MAXARG+1];
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    pde_t *pgdir, *oldpgdir;
    struct mm_area text_data, stack, heap;
    struct proc *curproc = myproc();

    begin_op();

    if((ip = namei(path)) == 0){
        end_op();
        cprintf("exec: fail\n");
        return -1;
    }
    ilock(ip);
    pgdir = 0;

    // Check ELF header
    if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;
    if(elf.magic != ELF_MAGIC)
        goto bad;

    if((pgdir = copykvm()) == 0)
        goto bad;

    text_data.start = text_data.sz = 0;
    // Load program into memory. xv6 only use the first program segment
    off = elf.phoff;
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
        goto bad;
    if(ph.type != ELF_PROG_LOAD)
        goto bad;
    if(ph.memsz < ph.filesz)
        goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
        goto bad;
    if(allocuvm(pgdir, text_data.sz, ph.vaddr+ph.memsz) < 0){
        goto bad;    
    }
    text_data.sz = ph.vaddr+ph.memsz;
    if(ph.vaddr % PGSIZE != 0)
        goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
        goto bad;    

    iunlockput(ip);
    end_op();
    ip = 0;

    // Allocate two pages at the next page boundary.
    // Make the first inaccessible.    Use the second as the user stack.
    stack.start = PGROUNDUP(text_data.start + text_data.sz) + MAXSTACK;
    stack.sz = PGSIZE;  
    if(allocuvm(pgdir, stack.start, stack.start + stack.sz) < 0)
        goto bad;
    sp = stack.start + stack.sz;

    heap.start = sp;    
    heap.sz = 0;

    // Push argument strings, prepare rest of stack in ustack.
    for(argc = 0; argv[argc]; argc++) {
        if(argc >= MAXARG)
            goto bad;
        sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
        if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[3+argc] = sp;
    }
    ustack[3+argc] = 0;

    ustack[0] = 0xffffffff;    // fake return PC
    ustack[1] = argc;
    ustack[2] = sp - (argc+1)*4;    // argv pointer

    sp -= (3+argc+1) * 4;
    if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
        goto bad;

    // Save program name for debugging.
    for(last=s=path; *s; s++)
        if(*s == '/')
            last = s+1;
    safestrcpy(curproc->name, last, sizeof(curproc->name));

    // Commit to the user image.
    oldpgdir = curproc->pgdir;
    curproc->pgdir = pgdir;
    curproc->tf->eip = elf.entry;    // main
    curproc->tf->esp = sp;
    curproc->text_data = text_data;
    curproc->stack = stack;
    curproc->heap = heap;
    switchuvm(curproc);
    freevm(oldpgdir);
    return 0;

bad:
    if(pgdir)
        freevm(pgdir);
    if(ip){
        iunlockput(ip);
        end_op();
    }
    return -1;
}
