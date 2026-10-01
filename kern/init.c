/* See COPYRIGHT for copyright information. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/monitor.h>
#include <kern/console.h>

// Test the stack backtrace function (lab 1 only)
void
test_backtrace(int x)
{
	cprintf("entering test_backtrace %d\n", x);
	if (x > 0)
		test_backtrace(x-1);
	else
		mon_backtrace(0, 0, 0);
	cprintf("leaving test_backtrace %d\n", x);
}

void
i386_init(void)
{
	extern char edata[], end[];

	// Before doing anything else, complete the ELF loading process.
	// Clear the uninitialized global data (BSS) section of our program.
	// This ensures that all static/global variables start out zero.
	memset(edata, 0, end - edata);

	// Initialize the console.
	// Can't call cprintf until after we do this!
	cons_init();

	cprintf("6828 decimal is %o octal!\n", 6828);

    //cprintf("\33[04m RED \33[0m\n");      // 黑底红字

	// Test the stack backtrace function (lab 1 only)
	test_backtrace(5);

    // 测试彩色输出
    // 格式: \33[XYm，X=背景色，Y=前景色，均为十六进制 VGA 颜色码
    // VGA: 0黑 1蓝 2绿 3青 4红 5品红 6棕/黄 7白
    //      8暗灰 9亮蓝 A亮绿 B亮青 C亮红 D亮品红 E亮黄 F亮白

    cprintf("\33[04mThis is RED\33[0m\n");      // 黑底红字
    cprintf("\33[02mThis is GREEN\33[0m\n");    // 黑底绿字
    cprintf("\33[06mThis is YELLOW\33[0m\n");   // 黑底黄字
    cprintf("\33[01mThis is BLUE\33[0m\n");     // 黑底蓝字
    cprintf("\33[05mThis is MAGENTA\33[0m\n");  // 黑底品红字
    cprintf("\33[03mThis is CYAN\33[0m\n");     // 黑底青字
    cprintf("\33[07mThis is WHITE\33[0m\n");    // 黑底白字

    // 混合颜色
    cprintf("\33[04mRed \33[02mGreen \33[01mBlue\33[0m\n");

    // 背景色
    cprintf("\33[47mWhite on Red\33[0m\n");     // 红底白字

    // 高亮色（VGA 8-F）
    cprintf("\33[0CmBright Red\33[0m\n");       // 黑底亮红字
    cprintf("\33[CFmBright White on Bright Red\33[0m\n");  // 亮红底亮白字

	// Drop into the kernel monitor.
	while (1)
		monitor(NULL);
}


/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
const char *panicstr;

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	// Be extra sure that the machine is in as reasonable state
	asm volatile("cli; cld");

	va_start(ap, fmt);
	cprintf("kernel panic at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* break into the kernel monitor */
	while (1)
		monitor(NULL);
}

/* like panic, but don't */
void
_warn(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);
	cprintf("kernel warning at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);
}
