/* See COPYRIGHT for copyright information. */
// 版权信息请参见 COPYRIGHT。

#ifndef JOS_INC_ENV_H
#define JOS_INC_ENV_H

#include <inc/types.h>
#include <inc/trap.h>
#include <inc/memlayout.h>

typedef int32_t envid_t;

// An environment ID 'envid_t' has three parts:
//
// +1+---------------21-----------------+--------10--------+
// |0|          Uniqueifier             |   Environment    |
// | |                                  |      Index       |
// +------------------------------------+------------------+
//                                       \--- ENVX(eid) --/
//
// The environment index ENVX(eid) equals the environment's index in the
// 'envs[]' array.  The uniqueifier distinguishes environments that were
// created at different times, but share the same environment index.
//
// All real environments are greater than 0 (so the sign bit is zero).
// envid_ts less than 0 signify errors.  The envid_t == 0 is special, and
// stands for the current environment.
// 用户环境（即进程）ID 'envid_t' 由三部分组成：
//
// 上图各部分含义：符号位；唯一标识符（Uniqueifier）；用户环境索引（Environment Index）。
// ENVX(eid) 标注了用户环境索引所在的位。
//
// 用户环境索引 ENVX(eid) 等于该用户环境在
// 'envs[]' 数组中的索引。唯一标识符用于区分
// 创建时间不同但共用同一用户环境索引的用户环境。
//
// 所有真实用户环境的 ID 都大于 0（因此符号位为 0）。
// 小于 0 的 envid_t 表示错误。envid_t == 0 是一个特殊值，
// 代表当前用户环境。

#define LOG2NENV		10
#define NENV			(1 << LOG2NENV)
#define ENVX(envid)		((envid) & (NENV - 1))

// Values of env_status in struct Env
// struct Env 中 env_status 的取值
enum {
	ENV_FREE = 0,
	ENV_DYING,
	ENV_RUNNABLE,
	ENV_RUNNING,
	ENV_NOT_RUNNABLE
};

// Special environment types
// 特殊用户环境类型
enum EnvType {
	ENV_TYPE_USER = 0,
};

struct Env {
	struct Trapframe env_tf;	// Saved registers
					// 已保存的寄存器
	struct Env *env_link;		// Next free Env
					// 空闲链表中的下一个 Env
	envid_t env_id;			// Unique environment identifier
					// 用户环境的唯一标识符
	envid_t env_parent_id;		// env_id of this env's parent
					// 该用户环境父环境的 env_id
	enum EnvType env_type;		// Indicates special system environments
					// 标识特殊的系统用户环境
	unsigned env_status;		// Status of the environment
					// 用户环境的状态
	uint32_t env_runs;		// Number of times environment has run
					// 用户环境已运行的次数

	// Address space
	// 地址空间
	pde_t *env_pgdir;		// Kernel virtual address of page dir
					// 页目录的内核虚拟地址
};

#endif // !JOS_INC_ENV_H
