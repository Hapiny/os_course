#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/vsyscall.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/vsyscall.h>

#ifndef debug
# define debug 0
#endif

static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

extern void divide_err();
extern void debug_exception();
extern void non_maskable_interrupt();
extern void break_point();
extern void overflow();
extern void bound_check();
extern void illegal_opcode();
extern void device();
extern void double_fault();
extern void task_switch_segment();
extern void segment_not_present(struct Trapframe *tf);
extern void stack_exception();
extern void gen_prot_fault();
extern void page_fault();
extern void floating_point_err();
extern void align_check();
extern void machine_check();
extern void simd_err();
extern void sys_call();
extern void kbd_call();
extern void serial_call();

void 
trap_init(void)
{
	//extern struct Segdesc gdt[];

	// LAB 8: Your code here.
	SETGATE(idt[0], 0, GD_KT, divide_err, 0);
	SETGATE(idt[1], 0, GD_KT, debug_exception, 0);
	SETGATE(idt[2], 0, GD_KT, non_maskable_interrupt, 0);

	SETGATE(idt[3], 0, GD_KT, break_point, 3);
	
	SETGATE(idt[4], 0, GD_KT, overflow, 0);
	SETGATE(idt[5], 0, GD_KT, bound_check, 0);
	SETGATE(idt[6], 0, GD_KT, illegal_opcode, 0);
	SETGATE(idt[7], 0, GD_KT, device, 0);
	SETGATE(idt[8], 0, GD_KT, double_fault, 0);
	SETGATE(idt[10], 0, GD_KT, task_switch_segment, 0);
	SETGATE(idt[11], 0, GD_KT, segment_not_present, 0);
	SETGATE(idt[12], 0, GD_KT, stack_exception, 0);
	SETGATE(idt[13], 0, GD_KT, gen_prot_fault, 0);
	SETGATE(idt[14], 0, GD_KT, page_fault, 0);
	SETGATE(idt[16], 0, GD_KT, floating_point_err, 0);
	SETGATE(idt[17], 0, GD_KT, align_check, 0);
	SETGATE(idt[18], 0, GD_KT, machine_check, 0);
	SETGATE(idt[19], 0, GD_KT, simd_err, 0);
	SETGATE(idt[T_SYSCALL], 0, GD_KT, sys_call, 3);
	// Per-CPU setup 

	SETGATE(idt[IRQ_OFFSET + IRQ_KBD], 0, GD_KT, kbd_call, 0);
	SETGATE(idt[IRQ_OFFSET + IRQ_SERIAL], 0, GD_KT, serial_call, 0);
	trap_init_percpu();
}

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	ts.ts_esp0 = KSTACKTOP;
	ts.ts_ss0 = GD_KD;

	// Initialize the TSS slot of the gdt.
	gdt[GD_TSS0 >> 3] = SEG16(STS_T32A, (uint32_t) (&ts),
					sizeof(struct Taskstate), 0);
	gdt[GD_TSS0 >> 3].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0);

	// Load the IDT
	lidt(&idt_pd);
}


void
clock_idt_init(void)
{
	extern void (*clock_thdlr)(void);
	// init idt structure
	SETGATE(idt[IRQ_OFFSET + IRQ_CLOCK], 0, GD_KT, (int)(&clock_thdlr), 0);
	lidt(&idt_pd);
}


void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	cprintf("  esp  0x%08x\n", tf->tf_esp);
	cprintf("  ss   0x----%04x\n", tf->tf_ss);
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}


static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.

	// Handle spurious interrupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	//
	uint8_t status;

	if (tf->tf_trapno == T_SYSCALL)
	{	
		tf->tf_regs.reg_eax = 
		syscall(tf->tf_regs.reg_eax, 
				tf->tf_regs.reg_edx, 
				tf->tf_regs.reg_ecx, 
				tf->tf_regs.reg_ebx,
				tf->tf_regs.reg_edi,
				tf->tf_regs.reg_esi);
		return;
	}

	if (tf->tf_trapno == T_BRKPT)
	{
		monitor(tf);
		return;
	}
	
	if (tf->tf_trapno == T_PGFLT)
	{
		page_fault_handler(tf);
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	}
	//если произошло прерывания от часов, то:
	//1. Проверяем статус регистра C(читаем CREG функцией rtc_check_status())
	//2. Посылаем сигнал об окончании обрабатывания прерывания EOI
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_CLOCK) {
		status = rtc_check_status();
		pic_send_eoi(IRQ_CLOCK);
		vsys[VSYS_gettime] = gettime();
		sched_yield();
		return;
	}

	// Handle keyboard and serial interrupts.
	// LAB 11: Your code here. 
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_KBD)
	{
		kbd_intr();
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SERIAL)
	{
		serial_intr();
		return;
	}

	print_trapframe(tf);
	if (tf->tf_cs == GD_KT) {
		panic("unhandled trap in kernel");
	} else {
		env_destroy(curenv);
	}
}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if (debug) {
		cprintf("Incoming TRAP frame at %p\n", tf);
	}

	assert(curenv);

	// Garbage collect if current enviroment is a zombie
	if (curenv->env_status == ENV_DYING) {
		env_free(curenv);
		curenv = NULL;
		sched_yield();
	}

	// Copy trap frame (which is currently on the stack)
	// into 'curenv->env_tf', so that running the environment
	// will restart at the trap point.
	curenv->env_tf = *tf;
	// The trapframe on the stack should be ignored from here on.
	tf = &curenv->env_tf;

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// LAB 8: Your code here.
	if (tf->tf_cs == GD_KT)
	{
		panic("page_fault in Kernel-Mode");
	}
	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').

	// LAB 9: Your code here.
	if (curenv->env_pgfault_upcall != NULL)
	{
		uint32_t user_stack = UXSTACKTOP;
		if (tf->tf_esp < UXSTACKTOP && tf->tf_esp >= UXSTACKTOP - PGSIZE)
		{
			user_stack = tf->tf_esp - sizeof(uint32_t);
			user_mem_assert(curenv, (const void *)user_stack, 4, PTE_W | PTE_U);
		}

		user_stack = user_stack - sizeof(struct UTrapframe);

		struct UTrapframe *user_trap = (struct UTrapframe*) user_stack;
        
        user_mem_assert(curenv, user_trap, sizeof(struct UTrapframe), PTE_W | PTE_U);

        user_trap->utf_fault_va = fault_va;
        user_trap->utf_err = tf->tf_err;
        user_trap->utf_regs = tf->tf_regs;
        user_trap->utf_eip = tf->tf_eip;
        user_trap->utf_eflags = tf->tf_eflags;
        user_trap->utf_esp = tf->tf_esp;

        tf->tf_esp = user_stack;
        tf->tf_eip = (uint32_t)curenv->env_pgfault_upcall;
		
		env_run(curenv);

	}
		// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

