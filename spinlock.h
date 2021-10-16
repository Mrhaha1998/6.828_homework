// Mutual exclusion lock.
struct spinlock {
    uint locked;             // Is the lock held?
    uint cli;             // when the lock is held, should we cli?
    // For debugging:
    char *name;                // Name of lock.
    struct cpu *cpu;     // The cpu holding the lock.
    uint pcs[10];            // The call stack (an array of program counters)
                                         // that locked the lock.
};

