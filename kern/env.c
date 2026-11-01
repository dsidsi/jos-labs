/* See COPYRIGHT for copyright information. */
// 版权信息请参见 COPYRIGHT。

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>

struct Env *envs = NULL;		// All environments
					// 所有用户环境
struct Env *curenv = NULL;		// The current env
					// 当前用户环境
static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)
					// 空闲用户环境链表
					// （通过 Env->env_link 链接）

#define ENVGENSHIFT	12		// >= LOGNENV
					// 大于等于 LOGNENV

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
// 全局描述符表。
//
// 设置全局描述符表（GDT），为内核态和用户态分别设置独立的段。
// 段在 x86 上有多种用途。我们不使用它们的内存映射功能，
// 但需要借助它们来切换特权级。
//
// 除 DPL 外，内核段和用户段完全相同。
// 加载 SS 寄存器时，CPL 必须等于 DPL。
// 因此，我们必须为用户态和内核态分别复制这些段。
//
// 具体而言，在 gdt 的定义中，SEG 宏的最后一个参数指定了
// 该描述符的描述符特权级（DPL）：内核为 0，用户为 3。
//
struct Segdesc gdt[] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	// 0x0——未使用（总是产生故障，用于捕获 NULL 远指针）
	SEG_NULL,

	// 0x8 - kernel code segment
	// 0x8——内核代码段
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	// 0x10——内核数据段
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	// 0x18——用户代码段
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	// 0x20——用户数据段
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// 0x28 - tss, initialized in trap_init_percpu()
	// 0x28——TSS，在 trap_init_percpu() 中初始化
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
// 将 envid 转换为 Env 指针。
// 如果设置了 checkperm，指定的用户环境必须是当前用户环境，
// 或者当前用户环境的直接子环境。
//
// 返回值
//   成功时返回 0，出错时返回 -E_BAD_ENV。
//   成功时，将 *env_store 设置为该用户环境。
//   出错时，将 *env_store 设置为 NULL。
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	// 如果 envid 为 0，则返回当前用户环境。
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	// 通过 envid 的索引部分查找 Env 结构，
	// 然后检查该 struct Env 中的 env_id 字段，
	// 以确保 envid 没有过期（即它并不指向_此前_使用
	// envs[] 数组中同一槽位的用户环境）。
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	// 检查调用方用户环境是否拥有操作指定用户环境的合法权限。
	// 如果设置了 checkperm，指定的用户环境必须是当前用户环境，
	// 或者当前用户环境的直接子环境。
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the env_free_list.
// Make sure the environments are in the free list in the same order
// they are in the envs array (i.e., so that the first call to
// env_alloc() returns envs[0]).
//
// 将 'envs' 中的所有用户环境标记为空闲，把它们的 env_id 设为 0，
// 并将它们插入 env_free_list。
// 确保各用户环境在空闲链表中的顺序与其在 envs 数组中的顺序相同
// （也就是说，第一次调用 env_alloc() 时应返回 envs[0]）。
//
void
env_init(void)
{
	// Set up envs array
	// LAB 3: Your code here.
	// 设置 envs 数组
	// 实验 3：请在此处编写代码。
	int i;
	for (i = NENV - 1; i >= 0; i--) {
		envs[i].env_link = env_free_list;
		env_free_list = &envs[i];
	}

	// Per-CPU part of the initialization
	// 初始化中针对每个 CPU 的部分
	env_init_percpu();
}

// Load GDT and segment descriptors.
// 加载 GDT 和段描述符。
void
env_init_percpu(void)
{
	lgdt(&gdt_pd);
	// The kernel never uses GS or FS, so we leave those set to
	// the user data segment.
	// 内核从不使用 GS 或 FS，因此让它们保持指向用户数据段。
	asm volatile("movw %%ax,%%gs" : : "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" : : "a" (GD_UD|3));
	// The kernel does use ES, DS, and SS.  We'll change between
	// the kernel and user data segments as needed.
	// 内核确实会使用 ES、DS 和 SS。我们将根据需要
	// 在内核数据段和用户数据段之间切换。
	asm volatile("movw %%ax,%%es" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));
	// Load the kernel text segment into CS.
	// 将内核代码段加载到 CS。
	asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT));
	// For good measure, clear the local descriptor table (LDT),
	// since we don't use it.
	// 为稳妥起见，清除局部描述符表（LDT），因为我们不使用它。
	lldt(0);
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page directory, set e->env_pgdir accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
// 初始化用户环境 e 的内核虚拟内存布局。
// 分配一个页目录，据此设置 e->env_pgdir，
// 并初始化新用户环境地址空间中的内核部分。
// 暂时不要在该用户环境虚拟地址空间的用户部分映射任何内容。
//
// 成功时返回 0，出错时返回 < 0。错误包括：
//	-E_NO_MEM：无法分配页目录或页表。
//
static int
env_setup_vm(struct Env *e)
{
	int i;
	struct PageInfo *p = NULL;

	// Allocate a page for the page directory
	// 为页目录分配一个物理页
	if (!(p = page_alloc(ALLOC_ZERO)))
		return -E_NO_MEM;

	// Now, set e->env_pgdir and initialize the page directory.
	//
	// Hint:
	//    - The VA space of all envs is identical above UTOP
	//	(except at UVPT, which we've set below).
	//	See inc/memlayout.h for permissions and layout.
	//	Can you use kern_pgdir as a template?  Hint: Yes.
	//	(Make sure you got the permissions right in Lab 2.)
	//    - The initial VA below UTOP is empty.
	//    - You do not need to make any more calls to page_alloc.
	//    - Note: In general, pp_ref is not maintained for
	//	physical pages mapped only above UTOP, but env_pgdir
	//	is an exception -- you need to increment env_pgdir's
	//	pp_ref for env_free to work correctly.
	//    - The functions in kern/pmap.h are handy.
	// 现在设置 e->env_pgdir 并初始化页目录。
	//
	// 提示：
	//    - 所有用户环境的虚拟地址空间在 UTOP 以上的部分都相同
	//	（UVPT 除外，我们将在下面对其进行设置）。
	//	权限和布局请参见 inc/memlayout.h。
	//	能把 kern_pgdir 用作模板吗？提示：可以。
	//	（确保你在实验 2 中正确设置了权限。）
	//    - UTOP 以下的初始虚拟地址为空。
	//    - 你不需要再调用 page_alloc。
	//    - 注意：通常不会为仅映射到 UTOP 以上的物理页
	//	维护 pp_ref，但 env_pgdir 是个例外——
	//	你需要增加 env_pgdir 的 pp_ref，
	//	以便 env_free 正常工作。
	//    - kern/pmap.h 中的函数很好用。

	// LAB 3: Your code here.
	// 实验 3：请在此处编写代码。
	e->env_pgdir = (pde_t *) page2kva(p);
	p->pp_ref++;

	for (i = PDX(UTOP); i < NPDENTRIES; i++)
		e->env_pgdir[i] = kern_pgdir[i];

	// UVPT maps the env's own page table read-only.
	// Permissions: kernel R, user R
	// UVPT 以只读方式映射用户环境自身的页表。
	// 权限：内核可读，用户可读
	e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;

	return 0;
}

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENV environments are allocated
//	-E_NO_MEM on memory exhaustion
//
// 分配并初始化一个新的用户环境。
// 成功时，新用户环境会被存入 *newenv_store。
//
// 成功时返回 0，失败时返回 < 0。错误包括：
//	-E_NO_FREE_ENV：所有 NENV 个用户环境均已分配
//	-E_NO_MEM：内存耗尽
//
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;

	// Allocate and set up the page directory for this environment.
	// 为该用户环境分配并设置页目录。
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// Generate an env_id for this environment.
	// 为该用户环境生成 env_id。
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
				// 不要创建负的 env_id。
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// Set the basic status variables.
	// 设置基本状态变量。
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	// 清除所有已保存的寄存器状态，
	// 防止曾占用此 Env 结构的先前用户环境的寄存器值
	// “泄漏”到我们的新用户环境中。
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and
	// GD_UT is the user text segment selector (see inc/memlayout.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.  When
	// we switch privilege levels, the hardware does various
	// checks involving the RPL and the Descriptor Privilege Level
	// (DPL) stored in the descriptors themselves.
	// 为段寄存器设置适当的初始值。
	// GD_UD 是 GDT 中的用户数据段选择子，
	// GD_UT 是用户代码段选择子（见 inc/memlayout.h）。
	// 每个段寄存器的低 2 位包含请求者特权级（RPL）；
	// 3 表示用户态。切换特权级时，硬件会进行各种检查，
	// 其中涉及 RPL 以及存储在描述符自身中的描述符特权级（DPL）。
	e->env_tf.tf_ds = GD_UD | 3;
	e->env_tf.tf_es = GD_UD | 3;
	e->env_tf.tf_ss = GD_UD | 3;
	e->env_tf.tf_esp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | 3;
	// You will set e->env_tf.tf_eip later.
	// 你将在稍后设置 e->env_tf.tf_eip。

	// commit the allocation
	// 提交此次分配
	env_free_list = e->env_link;
	*newenv_store = e;

	cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Does not zero or otherwise initialize the mapped pages in any way.
// Pages should be writable by user and kernel.
// Panic if any allocation attempt fails.
//
// 为用户环境 env 分配 len 字节的物理内存，
// 并将其映射到该用户环境地址空间中的虚拟地址 va。
// 不以任何方式将映射的页面清零或进行其他初始化。
// 这些页面应当可由用户和内核写入。
// 如果任何一次分配失败，则触发 panic。
//
static void
region_alloc(struct Env *e, void *va, size_t len)
{
	// LAB 3: Your code here.
	// (But only if you need it for load_icode.)
	//
	// Hint: It is easier to use region_alloc if the caller can pass
	//   'va' and 'len' values that are not page-aligned.
	//   You should round va down, and round (va + len) up.
	//   (Watch out for corner-cases!)
	// 实验 3：请在此处编写代码。
	// （但仅在 load_icode 需要时才这样做。）
	//
	// 提示：如果调用者可以传入未按页对齐的 'va' 和 'len' 值，
	//   region_alloc 会更易于使用。
	//   你应该将 va 向下对齐，并将 (va + len) 向上对齐。
	//   （注意边界情况！）
	uintptr_t start = ROUNDDOWN((uintptr_t) va, PGSIZE);
	uintptr_t end = ROUNDUP((uintptr_t) va + len, PGSIZE);
	struct PageInfo *p;

	for (; start < end; start += PGSIZE) {
		if (!(p = page_alloc(0)))
			panic("region_alloc: out of memory");
		if (page_insert(e->env_pgdir, p, (void *) start, PTE_W | PTE_U) < 0)
			panic("region_alloc: page_insert failed");
	}
}

//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// All this is very similar to what our boot loader does, except the boot
// loader also needs to read the code from disk.  Take a look at
// boot/main.c to get ideas.
//
// Finally, this function maps one page for the program's initial stack.
//
// load_icode panics if it encounters problems.
//  - How might load_icode fail?  What might be wrong with the given input?
//
// 为用户进程设置初始程序二进制映像、栈和处理器标志。
// 此函数仅在内核初始化期间、运行第一个用户态用户环境之前调用。
//
// 此函数将 ELF 二进制映像中的所有可加载段加载到用户环境的用户内存中，
// 加载位置为 ELF 程序头中指定的相应虚拟地址。
// 同时，它会将这些段中在程序头里被标记为需要映射、
// 但实际并不存在于 ELF 文件中的部分清零，即程序的 bss 节。
//
// 这一切与我们的引导加载程序所做的工作非常相似，
// 只是引导加载程序还需要从磁盘读取代码。
// 可以查看 boot/main.c 来获取思路。
//
// 最后，此函数为程序的初始栈映射一个页面。
//
// load_icode 遇到问题时触发 panic。
//  - load_icode 可能如何失败？给定的输入可能存在什么问题？
//
static void
load_icode(struct Env *e, uint8_t *binary)
{
	// Hints:
	//  Load each program segment into virtual memory
	//  at the address specified in the ELF segment header.
	//  You should only load segments with ph->p_type == ELF_PROG_LOAD.
	//  Each segment's virtual address can be found in ph->p_va
	//  and its size in memory can be found in ph->p_memsz.
	//  The ph->p_filesz bytes from the ELF binary, starting at
	//  'binary + ph->p_offset', should be copied to virtual address
	//  ph->p_va.  Any remaining memory bytes should be cleared to zero.
	//  (The ELF header should have ph->p_filesz <= ph->p_memsz.)
	//  Use functions from the previous lab to allocate and map pages.
	//
	//  All page protection bits should be user read/write for now.
	//  ELF segments are not necessarily page-aligned, but you can
	//  assume for this function that no two segments will touch
	//  the same virtual page.
	//
	//  You may find a function like region_alloc useful.
	//
	//  Loading the segments is much simpler if you can move data
	//  directly into the virtual addresses stored in the ELF binary.
	//  So which page directory should be in force during
	//  this function?
	//
	//  You must also do something with the program's entry point,
	//  to make sure that the environment starts executing there.
	//  What?  (See env_run() and env_pop_tf() below.)
	// 提示：
	//  将每个程序段加载到虚拟内存中
	//  ELF 段头指定的地址处。
	//  你只应加载 ph->p_type == ELF_PROG_LOAD 的段。
	//  每个段的虚拟地址可在 ph->p_va 中找到，
	//  其在内存中的大小可在 ph->p_memsz 中找到。
	//  应将 ELF 二进制文件中从 'binary + ph->p_offset' 开始的
	//  ph->p_filesz 个字节复制到虚拟地址 ph->p_va。
	//  其余内存字节都应清零。
	//  （ELF 头中应满足 ph->p_filesz <= ph->p_memsz。）
	//  使用上一个实验中的函数来分配和映射页面。
	//
	//  暂时将所有页保护位设置为用户可读写。
	//  ELF 段不一定按页对齐，但对于此函数，
	//  你可以假设任意两个段都不会触及同一虚拟页。
	//
	//  你可能会发现 region_alloc 这样的函数很有用。
	//
	//  如果能将数据直接移动到 ELF 二进制文件中存储的虚拟地址处，
	//  加载段会简单得多。
	//  那么，在此函数执行期间应启用哪个页目录？
	//
	//  你还必须对程序的入口点做些处理，
	//  以确保用户环境从那里开始执行。
	//  做什么？（见下面的 env_run() 和 env_pop_tf()。）

	// LAB 3: Your code here.
	// 实验 3：请在此处编写代码。
	struct Elf *elf = (struct Elf *) binary;
	struct Proghdr *ph, *eph;

	if (elf->e_magic != ELF_MAGIC)
		panic("load_icode: invalid ELF");

	lcr3(PADDR(e->env_pgdir));

	ph = (struct Proghdr *) (binary + elf->e_phoff);
	eph = ph + elf->e_phnum;
	for (; ph < eph; ph++) {
		if (ph->p_type != ELF_PROG_LOAD)
			continue;
		region_alloc(e, (void *) ph->p_va, ph->p_memsz);
		memcpy((void *) ph->p_va, binary + ph->p_offset, ph->p_filesz);
		memset((void *) (ph->p_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
	}

	e->env_tf.tf_eip = elf->e_entry;

	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.
	// 现在为程序的初始栈映射一个页面，
	// 虚拟地址为 USTACKTOP - PGSIZE。

	// LAB 3: Your code here.
	// 实验 3：请在此处编写代码。
	region_alloc(e, (void *) (USTACKTOP - PGSIZE), PGSIZE);

	lcr3(PADDR(kern_pgdir));
}

//
// Allocates a new env with env_alloc, loads the named elf
// binary into it with load_icode, and sets its env_type.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
//
// 使用 env_alloc 分配一个新的用户环境，
// 使用 load_icode 将指定的 ELF 二进制文件加载到其中，
// 并设置其 env_type。
// 此函数仅在内核初始化期间、运行第一个用户态用户环境之前调用。
// 新用户环境的父环境 ID 被设为 0。
//
void
env_create(uint8_t *binary, enum EnvType type)
{
	// LAB 3: Your code here.
	// 实验 3：请在此处编写代码。
	struct Env *e;

	if (env_alloc(&e, 0) < 0)
		panic("env_create: env_alloc failed");
	load_icode(e, binary);
	e->env_type = type;
}

//
// Frees env e and all memory it uses.
//
// 释放用户环境 e 及其使用的所有内存。
//
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	// 如果正在释放当前用户环境，则在释放页目录之前
	// 切换到 kern_pgdir，以防该页面被重用。
	if (e == curenv)
		lcr3(PADDR(kern_pgdir));

	// Note the environment's demise.
	// 记录该用户环境的终止。
	cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	// Flush all mapped pages in the user portion of the address space
	// 清除地址空间用户部分中所有已映射的页面
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		// 只检查已映射的页表
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		// 查找页表的物理地址（pa）和虚拟地址（va）
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		// 取消此页表中所有 PTE 的映射
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		// 释放页表本身
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	// 释放页目录
	pa = PADDR(e->env_pgdir);
	e->env_pgdir = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	// 将用户环境归还到空闲链表
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
//
// 释放用户环境 e。
//
void
env_destroy(struct Env *e)
{
	env_free(e);

	cprintf("Destroyed the only environment - nothing more to do!\n");
	while (1)
		monitor(NULL);
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
// 使用 'iret' 指令恢复陷阱帧（Trapframe）中的寄存器值。
// 这会退出内核，并开始执行某个用户环境的代码。
//
// 此函数不返回。
//
void
env_pop_tf(struct Trapframe *tf)
{
	asm volatile(
		"\tmovl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
				      // 跳过 tf_trapno 和 tf_errcode
		"\tiret\n"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
			       // 主要是为了消除编译器的警告
}

//
// Context switch from curenv to env e.
// Note: if this is the first call to env_run, curenv is NULL.
//
// This function does not return.
//
// 将上下文从 curenv 切换到用户环境 e。
// 注意：如果这是第一次调用 env_run，则 curenv 为 NULL。
//
// 此函数不返回。
//
void
env_run(struct Env *e)
{
	// Step 1: If this is a context switch (a new environment is running):
	//	   1. Set the current environment (if any) back to
	//	      ENV_RUNNABLE if it is ENV_RUNNING (think about
	//	      what other states it can be in),
	//	   2. Set 'curenv' to the new environment,
	//	   3. Set its status to ENV_RUNNING,
	//	   4. Update its 'env_runs' counter,
	//	   5. Use lcr3() to switch to its address space.
	// Step 2: Use env_pop_tf() to restore the environment's
	//	   registers and drop into user mode in the
	//	   environment.
	// 步骤 1：如果这是一次上下文切换（即有一个新的用户环境开始运行）：
	//	   1. 如果有当前用户环境，且其状态为 ENV_RUNNING，
	//	      则将其状态改回 ENV_RUNNABLE
	//	      （想想它还可能处于哪些状态），
	//	   2. 将 'curenv' 设置为新的用户环境，
	//	   3. 将其状态设为 ENV_RUNNING，
	//	   4. 更新其 'env_runs' 计数器，
	//	   5. 使用 lcr3() 切换到它的地址空间。
	// 步骤 2：使用 env_pop_tf() 恢复用户环境的寄存器，
	//	   并进入该用户环境的用户态。

	// Hint: This function loads the new environment's state from
	//	e->env_tf.  Go back through the code you wrote above
	//	and make sure you have set the relevant parts of
	//	e->env_tf to sensible values.
	// 提示：此函数从 e->env_tf 加载新用户环境的状态。
	//	回头检查你在上面编写的代码，
	//	确保已将 e->env_tf 的相关部分设置为合理的值。

	// LAB 3: Your code here.
	// 实验 3：请在此处编写代码。
	if (curenv && curenv->env_status == ENV_RUNNING)
		curenv->env_status = ENV_RUNNABLE;
	curenv = e;
	e->env_status = ENV_RUNNING;
	e->env_runs++;
	lcr3(PADDR(e->env_pgdir));

	env_pop_tf(&e->env_tf);
}

