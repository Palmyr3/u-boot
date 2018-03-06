#define DEBUG

#include <common.h>
#include <config.h>
#include <clk.h>
#include <dm/device.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <asm/arcregs.h>
#include <fdt_support.h>


#ifdef CONFIG_CPU_BIG_ENDIAN
	#error "hsdk_go will not work with BIG endian CPU"
#endif

#define HSDKGO_VERSION	"0.9"

#define ceil(x, y) ({ ulong __x = (x), __y = (y); (__x + __y - 1) / __y; })

/* TODO: move to common config */
#define NR_CPUS		4
#define ALL_CPU_MASK	0xF	/* GENMASK(NR_CPUS - 1, 0) */
#define MASTER_CPU	0
#define MAX_CMD_LEN	25
#define HZ_IN_MHZ	1000000

#define NO_CCM		0x10

/* Uncached access macros */
#define arc_read_uncached_32(ptr)	\
({					\
	unsigned int __ret;		\
	__asm__ __volatile__(		\
	"	ld.di %0, [%1]	\n"	\
	: "=r"(__ret)			\
	: "r"(ptr));			\
	__ret;				\
})

#define arc_write_uncached_32(ptr, data)\
({					\
	__asm__ __volatile__(		\
	"	st.di %0, [%1]	\n"	\
	:				\
	: "r"(data), "r"(ptr));		\
})

enum clk_ctl {
	CLK_SET		= BIT(0), /* set frequency */
	CLK_GET		= BIT(1), /* get frequency */
	CLK_ON		= BIT(2), /* enable clock */
	CLK_OFF		= BIT(3), /* disable clock */
	CLK_PRINT	= BIT(4)  /* print frequency */
};

/* TODO: use local */
void smp_set_core_boot_addr(unsigned long addr, int corenr);

enum env_type {
	ENV_DEC,
	ENV_HEX
};

typedef struct {
	u32 val;
	/* use u32 instead bool to all env be aligned for uncached operations */
	u32 set;
} u32_env;

/* Uncached */
struct hsdk_env_core_ctl {
	u32_env entry[NR_CPUS];
	u32_env iccm[NR_CPUS];
	u32_env dccm[NR_CPUS];

	u8 cache_padding[ARCH_DMA_MINALIGN];
} __aligned(ARCH_DMA_MINALIGN);

/* Uncached */
struct hsdk_env_common_ctl {
	bool halt_on_boot;
	u32_env core_mask;
	u32_env cpu_freq;
	u32_env axi_freq;
	u32_env tun_freq;
	u32_env nvlim;
	u32_env icache;
	u32_env dcache;

	u8 cache_padding[ARCH_DMA_MINALIGN];
} __aligned(ARCH_DMA_MINALIGN);

/* Uncached */
struct hsdk_cross_cpu {
	u32 data_flag;
	u32 stack_ptr;
	s32 status[NR_CPUS];

	u8 cache_padding[ARCH_DMA_MINALIGN];
} __aligned(ARCH_DMA_MINALIGN);

struct hsdk_env_map_common {
	const char *const env_name;
	enum env_type type;
	bool mandatory;
	u32 min;
	u32 max;
	u32_env *val;
};

struct hsdk_env_map_core {
	const char *const env_name;
	enum env_type type;
	bool mandatory;
	u32 min[NR_CPUS];
	u32 max[NR_CPUS];
	u32_env (*val)[NR_CPUS];
};

/* Place for slave cpu temporary stack */
static u32 slave_stack[256 * NR_CPUS] __attribute__((aligned(4)));

static struct hsdk_env_common_ctl env_common = {};
static struct hsdk_env_core_ctl env_core = {};
static struct hsdk_cross_cpu cross_cpu_data;

int soc_clk_ctl(const char *name, ulong *rate, enum clk_ctl ctl)
{
	int ret;
	ulong priv_rate;
	struct clk clk;

	/* Dummy fmeas device, just to be able to use standard clk_* api funcs */
	struct udevice fmeas = {
		.name = "clk-fmeas",
		.node = ofnode_path("/clk-fmeas"),
	};

	ret = clk_get_by_name(&fmeas, name, &clk);
	if (ret) {
		pr_err("clock '%s' not found, err=%d\n", name, ret);
		return ret;
	}

	if (ctl & CLK_ON) {
		ret = clk_enable(&clk);
		if (ret && ret != -ENOSYS && ret != -ENOTSUPP)
			return ret;
	}

	if ((ctl & CLK_SET) && rate != NULL) {
		ret = clk_set_rate(&clk, *rate);
		if (ret)
			return ret;
	}

	if (ctl & CLK_OFF) {
		ret = clk_disable(&clk);
		if (ret) {
			pr_err("clock '%s' can't disable, err=%d\n", name, ret);
			return ret;
		}
	}

	priv_rate = clk_get_rate(&clk);

	if ((ctl & CLK_GET) && rate != NULL) {
		*rate = priv_rate;
	}

	clk_free(&clk);

	if (ctl & CLK_PRINT) {
		printf("HSDK: clock '%s' rate %lu MHz\n", name, ceil(priv_rate, HZ_IN_MHZ));
	} else {
		debug("HSDK: clock '%s' rate %lu MHz\n", name, ceil(priv_rate, HZ_IN_MHZ));
	}

	return 0;
}

static bool is_cpu_used(u32 cpu_id)
{
	return !!(env_common.core_mask.val & BIT(cpu_id));
}

static const struct hsdk_env_map_common env_map_common[] = {
	{ "core_mask",		ENV_HEX, true,	0x1, 0xF,	&env_common.core_mask },
	{ "non_volatile_limit", ENV_HEX, true,	0, 0xF, 	&env_common.nvlim },
	{ "icache_ena",		ENV_HEX, true,	0, 1,		&env_common.icache },
	{ "dcache_ena",		ENV_HEX, true,	0, 1,		&env_common.dcache },
	{}
};

static const struct hsdk_env_map_common env_map_clock[] = {
	{ "cpu_freq",		ENV_DEC, false,	100, 1000,	&env_common.cpu_freq },
	{ "axi_freq",		ENV_DEC, false,	200, 800,	&env_common.axi_freq },
	{ "tun_freq",		ENV_DEC, false,	0, 150,		&env_common.tun_freq },
	{}
};

static const struct hsdk_env_map_core env_map_core[] = {
	{ "core_iccm",	ENV_HEX, true,	{NO_CCM, 0, NO_CCM, 0}, {NO_CCM, 0xF, NO_CCM, 0xF},	&env_core.iccm },
	{ "core_dccm",	ENV_HEX, true,	{NO_CCM, 0, NO_CCM, 0}, {NO_CCM, 0xF, NO_CCM, 0xF},	&env_core.dccm },
	{}
};

static const struct hsdk_env_map_common env_map_mask[] = {
	{ "core_mask",		ENV_HEX, false,	0x1, 0xF,	&env_common.core_mask },
	{}
};

static const struct hsdk_env_map_core env_map_go[] = {
	{ "core_entry",	ENV_HEX, true,	{0, 0, 0, 0}, {U32_MAX, U32_MAX, U32_MAX, U32_MAX},	&env_core.entry },
	{}
};

static void env_clear_common(u32 index, const struct hsdk_env_map_common *map)
{
	map[index].val->val = 0;
	map[index].val->set = 1;
}

static int env_read_common(u32 index, const struct hsdk_env_map_common *map)
{
	u32 val;

	if (!env_get_yesno(map[index].env_name)) {
		if (map[index].type == ENV_HEX) {
			val = (u32)env_get_hex(map[index].env_name, 0);
			debug("ENV: %s: = %#x\n", map[index].env_name, val);
		} else {
			val = (u32)env_get_ulong(map[index].env_name, 10, 0);
			debug("ENV: %s: = %d\n", map[index].env_name, val);
		}

		map[index].val->val = val;
		map[index].val->set = 1;
	}

	return 0;
}

static void env_clear_core(u32 index, const struct hsdk_env_map_core *map)
{
	u32 i;

	for (i = 0; i < NR_CPUS; i++) {
		(*map[index].val)[i].val = 0;
		(*map[index].val)[i].set = 1;
	}
}

/* process core specific variables */
static int env_read_core(u32 index, const struct hsdk_env_map_core *map)
{
	u32 i, val;
	char comand[MAX_CMD_LEN];

	for (i = 0; i < NR_CPUS; i++) {
		sprintf(comand, "%s_%u", map[index].env_name, i);
		if (!env_get_yesno(comand)) {
			if (map[index].type == ENV_HEX) {
				val = (u32)env_get_hex(comand, 0);
				debug("ENV: %s: = %#x\n", comand, val);
			} else {
				val = (u32)env_get_ulong(comand, 10, 0);
				debug("ENV: %s: = %d\n", comand, val);
			}

			(*map[index].val)[i].val = val;
			(*map[index].val)[i].set = 1;
		}
	}

	return 0;
}

/* environment common verification */
static int env_validate_common(u32 index, const struct hsdk_env_map_common *map)
{
	u32 value = map[index].val->val;
	bool set = map[index].val->set;
	u32 min = map[index].min;
	u32 max = map[index].max;

	/* Check if environment is mandatory */
	if (map[index].mandatory && !set) {
		pr_err("Variable \'%s\' is mandatory, but it is not defined\n",
			map[index].env_name);

		return -EINVAL;
	}

	/* Check environment boundary */
	if (set && (value < min || value > max)) {
		if (map[index].type == ENV_HEX)
			pr_err("Variable \'%s\' must be between %#x and %#x\n",
				map[index].env_name, min, max);
		else
			pr_err("Variable \'%s\' must be between %u and %u\n",
				map[index].env_name, min, max);

		return -EINVAL;
	}

	return 0;
}

static int env_validate_core(u32 index, const struct hsdk_env_map_core *map)
{
	u32 i;
	u32 value;
	bool set;
	bool mandatory = map[index].mandatory;
	u32 min, max;

	for (i = 0; i < NR_CPUS; i++) {
		set = (*map[index].val)[i].set;
		value = (*map[index].val)[i].val;

		/* Check if environment is mandatory */
		if (is_cpu_used(i) && mandatory && !set) {
			pr_err("CPU %u is used, but \'%s_%u\' is not defined\n",
				i, map[index].env_name, i);

			return -EINVAL;
		}

		min = map[index].min[i];
		max = map[index].max[i];

		/* Check environment boundary */
		if (set && (value < min || value > max)) {
			if (map[index].type == ENV_HEX)
				pr_err("Variable \'%s_%u\' must be between %#x and %#x\n",
					map[index].env_name, i, min, max);
			else
				pr_err("Variable \'%s_%u\' must be between %d and %d\n",
					map[index].env_name, i, min, max);

			return -EINVAL;
		}
	}

	return 0;
}

static void envs_cleanup_common(const struct hsdk_env_map_common *map)
{
	u32 i;

	/* Cleanup env struct first */
	for (i = 0; map[i].env_name; i++) {
		env_clear_common(i, map);
	}
}

static int envs_read_common(const struct hsdk_env_map_common *map)
{
	u32 i;
	int ret;

	for (i = 0; map[i].env_name; i++) {
		ret = env_read_common(i, map);
		if (ret)
			return ret;
	}

	return 0;
}

static int envs_validate_common(const struct hsdk_env_map_common *map)
{
	u32 i;
	int ret;

	for (i = 0; map[i].env_name; i++) {
		ret = env_validate_common(i, map);
		if (ret)
			return ret;
	}

	return 0;
}

static int env_read_validate_common(const struct hsdk_env_map_common *map)
{
	u32 i;
	int ret;

	/* Cleanup env struct first */
	for (i = 0; map[i].env_name; i++) {
		env_clear_common(i, map);
	}

	for (i = 0; map[i].env_name; i++) {
		ret = env_read_common(i, map);
		if (ret)
			return ret;
	}

	for (i = 0; map[i].env_name; i++) {
		ret = env_validate_common(i, map);
		if (ret)
			return ret;
	}

	return 0;
}

static int env_read_validate_core(const struct hsdk_env_map_core *map)
{
	u32 i;
	int ret;

	/* Cleanup env struct first */
	for (i = 0; map[i].env_name; i++) {
		env_clear_core(i, map);
	}

	for (i = 0; map[i].env_name; i++) {
		ret = env_read_core(i, map);
		if (ret)
			return ret;
	}

	for (i = 0; map[i].env_name; i++) {
		ret = env_validate_core(i, map);
		if (ret)
			return ret;
	}

	return 0;
}

static int env_process_and_validate(const struct hsdk_env_map_common *common,
				    const struct hsdk_env_map_core *core)
{
	int ret;

	ret = env_read_validate_common(common);
	if (ret)
		return ret;

	ret = env_read_validate_core(core);
	if (ret)
		return ret;

	return 0;
}

/* TODO: move to explicit external cache header */
void __l1_dc_invalidate_all(void);
void __l1_dc_flush_all(void);

/* Bit values in IC_CTRL */
#define IC_CTRL_CACHE_DISABLE	(1 << 0)
/* Bit values in DC_CTRL */
#define DC_CTRL_CACHE_DISABLE	(1 << 0)
#define DC_CTRL_INV_MODE_FLUSH	(1 << 6)
#define DC_CTRL_FLUSH_STATUS	(1 << 8)
#define APT_SHIFT		28

// TODO: add xCCM runtime check
static void smp_init_slave_cpu_func(u32 core)
{
	unsigned int r;

	/* ICCM move if exists */
	if (env_core.iccm[core].val != NO_CCM) {
		r = ARC_AUX_ICCM_BASE;
		write_aux_reg(r, env_core.iccm[core].val << APT_SHIFT);
	}

	/* DCCM move if exists */
	if (env_core.dccm[core].val != NO_CCM) {
		r = ARC_AUX_DCCM_BASE;
		write_aux_reg(r, env_core.dccm[core].val << APT_SHIFT);
	}

	/* i$ enable if required (it is disabled by default) */
	if (env_common.icache.val) {
		icache_enable();
		invalidate_icache_all();
	}

	/* d$ enable if required (it is disabled by default) */
	if (env_common.dcache.val)
		dcache_enable();
}

static void init_claster_nvlim(void)
{
	u32 val = env_common.nvlim.val << APT_SHIFT;

	flush_dcache_all();
	write_aux_reg(ARC_AUX_NON_VOLATILE_LIMIT, val);
	write_aux_reg(AUX_AUX_CACHE_LIMIT, val);
	flush_n_invalidate_dcache_all();
}

/* TODO: use generic status functions */

static void init_master_icache(void)
{
#ifdef CONFIG_SYS_ICACHE_OFF
	unsigned int r;

	/* enable if required, else - nothing to do */
	if (env_common.icache.val) {
		r = ARC_AUX_IC_CTRL;
		write_aux_reg(r, read_aux_reg(r) & ~IC_CTRL_CACHE_DISABLE);
	}
#else
	/* disable if required, else - nothing to do (we will invalidate i$
	 * just before app launch) */
	if (!env_common.icache.val) {
		/* next code copied from board_hsdk.c */
		/* instruction cache invalidate */
		write_aux_reg(ARC_AUX_IC_IVIC, 0x00000001U);
		/* HS Databook, 5.3.3.2: three NOP's must be inserted inbetween
		 * invalidate and disable  */
		__builtin_arc_nop();
		__builtin_arc_nop();
		__builtin_arc_nop();
		/* instruction cache disable */
		write_aux_reg(ARC_AUX_IC_CTRL, 0x00000001U);
	}
#endif
}

static void init_master_dcache(void)
{
#ifdef CONFIG_SYS_ICACHE_OFF
	unsigned int r;

	/* enable if required, else - nothing to do */
	if (env_common.dcache.val) {
		r = ARC_AUX_DC_CTRL;
		write_aux_reg(r, read_aux_reg(r) & ~(DC_CTRL_CACHE_DISABLE | DC_CTRL_INV_MODE_FLUSH));
	}
#else
	/* disable if required, else - nothing to do (we will flush d$ and sl$
	 * just before app launch) */
	if (!env_common.dcache.val) {
		/* next code copied from board_hsdk.c */
		flush_dcache_all(); /* TODO: it is OK if we flush SL$ too? */
		/* data cache ctrl: invalidate mode to: invalidate dc and flush
		 * dirty entries */
		write_aux_reg(ARC_AUX_DC_CTRL, 0x00000060U);
		/* data cache invalidate */
		write_aux_reg(ARC_AUX_DC_IVDC, 0x00000001U);
		/* data cache disable */
		write_aux_reg(ARC_AUX_DC_CTRL, 0x00000001U);
	}
#endif
}

static int cleanup_cache_before_go(void)
{
	flush_n_invalidate_dcache_all();
	invalidate_icache_all();

	return 0;
}

/* ********************* SMP: START ********************* */
#define	CREG_BASE	(ARC_PERIPHERAL_BASE + 0x1000)
#define	CREG_CPU_START	(CREG_BASE + 0x400)
#define	CPU_START_MASK	0xF

static int cleanup_before_go(void)
{
	disable_interrupts();
	cleanup_cache_before_go();

	return 0;
}

static inline void halt_this_cpu(void)
{
	__builtin_arc_flag(1);
}

static void smp_kick_cpu_x(u32 cpu_id)
{
	int cmd = readl((void __iomem *)CREG_CPU_START);

	if (cpu_id > NR_CPUS)
		return;

	cmd &= ~CPU_START_MASK;
	cmd |= (1 << cpu_id);
	writel(cmd, (void __iomem *)CREG_CPU_START);
}

static u32 prepare_cpu_ctart_reg(void)
{
	int cmd = readl((void __iomem *)CREG_CPU_START);

	cmd &= ~CPU_START_MASK;

	return cmd | env_common.core_mask.val;
}

/* flatten */
__attribute__((naked, noreturn, flatten)) noinline void hsdk_core_init_f(void)
{
	__asm__ __volatile__(	"ld.di	r8,	[%0]\n"
				"mov	%%sp,	r8\n"
				"mov	%%fp,	%%sp\n"
	 			: /* no output */
				: "r" (&cross_cpu_data.stack_ptr));

	arc_write_uncached_32(&cross_cpu_data.status[CPU_ID_GET()], 1);
	smp_init_slave_cpu_func(CPU_ID_GET());

	arc_write_uncached_32(&cross_cpu_data.data_flag, 0x12345678);
	arc_write_uncached_32(&cross_cpu_data.status[CPU_ID_GET()], 2);

	/* Halt the processor untill the master kick us again */
	halt_this_cpu();

	__builtin_arc_nop();
	__builtin_arc_nop();
	__builtin_arc_nop();

	arc_write_uncached_32(&cross_cpu_data.status[CPU_ID_GET()], 3);

	/* get the updated entry - invalidate i$ */
	invalidate_icache_all();

	arc_write_uncached_32(&cross_cpu_data.status[CPU_ID_GET()], 4);

	/* Run our program */
	((void (*)(void))(env_core.entry[CPU_ID_GET()].val))();

	arc_write_uncached_32(&cross_cpu_data.status[CPU_ID_GET()], 5);

	/* Something went terribly wrong */
	while (true)
		halt_this_cpu();
}

static void clear_cross_cpu_data(void)
{
	arc_write_uncached_32(&cross_cpu_data.data_flag, 0);
	arc_write_uncached_32(&cross_cpu_data.stack_ptr, 0);

	for (u32 i = 0; i < NR_CPUS; i++)
		arc_write_uncached_32(&cross_cpu_data.status[i], 0);
}

noinline static void do_init_slave_cpu(u32 cpu_id)
{
	u32 timeout = 5000;
	/* TODO: optimize memory usage, now we have one unused aperture */
	u32 stack_ptr = (u32)(slave_stack + (64 * cpu_id));

	arc_write_uncached_32(&cross_cpu_data.data_flag, 0);

	/* Use global unic place for slave cpu stack */
	arc_write_uncached_32(&cross_cpu_data.stack_ptr, stack_ptr);

	/* TODO: remove useless debug's in do_init_slave_cpu */
	debug("CPU %u: stack pool base: %p\n", cpu_id, slave_stack);
	debug("CPU %u: stack base: %x\n", cpu_id, stack_ptr);
	smp_set_core_boot_addr((unsigned long)hsdk_core_init_f, -1);

	/*
	 * Slave CPUs may start with disabled caches, so
	 * make sure other cores see written value in memory - flush L1, L2
	 */
	flush_dcache_all();

	smp_kick_cpu_x(cpu_id);

	debug("CPU %u: cross-cpu UC flag: %x [before timeout]\n", cpu_id, arc_read_uncached_32(&cross_cpu_data.data_flag));

	/* TODO: add dcache invalidation here */
	while (!arc_read_uncached_32(&cross_cpu_data.data_flag) && timeout) {
		timeout--;
		mdelay(10);
	}

	/* We need to panic here as there is no option to halt slave cpu
	 * (or check that slave cpu is halted) */
	if (!timeout)
		pr_err("CPU %u is not responding after init!\n", cpu_id);

	debug("CPU %u: cross-cpu UC flag: %x [after timeout]\n", cpu_id, arc_read_uncached_32(&cross_cpu_data.data_flag));
	debug("CPU %u: status: %d [after timeout]\n", cpu_id, arc_read_uncached_32(&cross_cpu_data.status[cpu_id]));
}

static void do_init_slave_cpus(void)
{
	u32 i;

	clear_cross_cpu_data();

	for (i = 1; i < NR_CPUS; i++)
		if (is_cpu_used(i))
			do_init_slave_cpu(i);
}

static void do_init_master_cpu(void)
{
	/* Setup master caches even if master isn't used as we want to use
	 * same cache configuration on all running CPUs */
	init_master_icache();
	init_master_dcache();
}

enum hsdk_axi_masters {
	M_HS_CORE = 0,
	M_HS_RTT,
	M_AXI_TUN,
	M_HDMI_VIDEO,
	M_HDMI_AUDIO,
	M_USB_HOST,
	M_ETHERNET,
	M_SDIO,
	M_GPU,
	M_DMAC_0,
	M_DMAC_1,
	M_DVFS
};

#define UPDATE_VAL	1

/*
 * m	master		AXI_M_m_SLV0	AXI_M_m_SLV1	AXI_M_m_OFFSET0	AXI_M_m_OFFSET1
 * 0	HS (CBU)	0x11111111	0x63111111	0xFEDCBA98	0x0E543210
 * 1	HS (RTT)	0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 2	AXI Tunnel	0x88888888	0x88888888	0xFEDCBA98	0x76543210
 * 3	HDMI-VIDEO	0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 4	HDMI-ADUIO	0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 5	USB-HOST	0x77777777	0x77999999	0xFEDCBA98	0x76DCBA98
 * 6	ETHERNET	0x77777777	0x77999999	0xFEDCBA98	0x76DCBA98
 * 7	SDIO		0x77777777	0x77999999	0xFEDCBA98	0x76DCBA98
 * 8	GPU		0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 9	DMAC (port #1)	0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 10	DMAC (port #2)	0x77777777	0x77777777	0xFEDCBA98	0x76543210
 * 11	DVFS 		0x00000000	0x60000000	0x00000000	0x00000000
 */

#define CREG_AXI_M_SLV0(m)  ((void __iomem *)(CREG_BASE + 0x020 * (m)))
#define CREG_AXI_M_SLV1(m)  ((void __iomem *)(CREG_BASE + 0x020 * (m) + 0x004))
#define CREG_AXI_M_OFT0(m)  ((void __iomem *)(CREG_BASE + 0x020 * (m) + 0x008))
#define CREG_AXI_M_OFT1(m)  ((void __iomem *)(CREG_BASE + 0x020 * (m) + 0x00C))
#define CREG_AXI_M_UPDT(m)  ((void __iomem *)(CREG_BASE + 0x020 * (m) + 0x014))

#define CREG_AXI_M_HS_CORE_BOOT	((void __iomem *)(CREG_BASE + 0x010))

#define CREG_PAE	((void __iomem *)(CREG_BASE + 0x180))
#define CREG_PAE_UPDT	((void __iomem *)(CREG_BASE + 0x194))

void init_memory_bridge(void)
{
	u32 reg;

	/*
	 * M_HS_CORE has one unic register - BOOT.
	 * We need to clean boot mirror (BOOT[1:0]) bits in them.
	 */
	reg = readl(CREG_AXI_M_HS_CORE_BOOT) & (~0x3);
	writel(reg, CREG_AXI_M_HS_CORE_BOOT);
	writel(0x11111111, CREG_AXI_M_SLV0(M_HS_CORE));
	writel(0x63111111, CREG_AXI_M_SLV1(M_HS_CORE));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_HS_CORE));
	writel(0x0E543210, CREG_AXI_M_OFT1(M_HS_CORE));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_HS_CORE));

	writel(0x77777777, CREG_AXI_M_SLV0(M_HS_RTT));
	writel(0x77777777, CREG_AXI_M_SLV1(M_HS_RTT));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_HS_RTT));
	writel(0x76543210, CREG_AXI_M_OFT1(M_HS_RTT));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_HS_RTT));

	writel(0x88888888, CREG_AXI_M_SLV0(M_AXI_TUN));
	writel(0x88888888, CREG_AXI_M_SLV1(M_AXI_TUN));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_AXI_TUN));
	writel(0x76543210, CREG_AXI_M_OFT1(M_AXI_TUN));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_AXI_TUN));

	writel(0x77777777, CREG_AXI_M_SLV0(M_HDMI_VIDEO));
	writel(0x77777777, CREG_AXI_M_SLV1(M_HDMI_VIDEO));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_HDMI_VIDEO));
	writel(0x76543210, CREG_AXI_M_OFT1(M_HDMI_VIDEO));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_HDMI_VIDEO));

	writel(0x77777777, CREG_AXI_M_SLV0(M_HDMI_AUDIO));
	writel(0x77777777, CREG_AXI_M_SLV1(M_HDMI_AUDIO));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_HDMI_AUDIO));
	writel(0x76543210, CREG_AXI_M_OFT1(M_HDMI_AUDIO));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_HDMI_AUDIO));

	writel(0x77777777, CREG_AXI_M_SLV0(M_USB_HOST));
	writel(0x77999999, CREG_AXI_M_SLV1(M_USB_HOST));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_USB_HOST));
	writel(0x76DCBA98, CREG_AXI_M_OFT1(M_USB_HOST));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_USB_HOST));

	writel(0x77777777, CREG_AXI_M_SLV0(M_ETHERNET));
	writel(0x77999999, CREG_AXI_M_SLV1(M_ETHERNET));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_ETHERNET));
	writel(0x76DCBA98, CREG_AXI_M_OFT1(M_ETHERNET));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_ETHERNET));

	writel(0x77777777, CREG_AXI_M_SLV0(M_SDIO));
	writel(0x77999999, CREG_AXI_M_SLV1(M_SDIO));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_SDIO));
	writel(0x76DCBA98, CREG_AXI_M_OFT1(M_SDIO));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_SDIO));

	writel(0x77777777, CREG_AXI_M_SLV0(M_GPU));
	writel(0x77777777, CREG_AXI_M_SLV1(M_GPU));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_GPU));
	writel(0x76543210, CREG_AXI_M_OFT1(M_GPU));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_GPU));

	writel(0x77777777, CREG_AXI_M_SLV0(M_DMAC_0));
	writel(0x77777777, CREG_AXI_M_SLV1(M_DMAC_0));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_DMAC_0));
	writel(0x76543210, CREG_AXI_M_OFT1(M_DMAC_0));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_DMAC_0));

	writel(0x77777777, CREG_AXI_M_SLV0(M_DMAC_1));
	writel(0x77777777, CREG_AXI_M_SLV1(M_DMAC_1));
	writel(0xFEDCBA98, CREG_AXI_M_OFT0(M_DMAC_1));
	writel(0x76543210, CREG_AXI_M_OFT1(M_DMAC_1));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_DMAC_1));

	writel(0x00000000, CREG_AXI_M_SLV0(M_DVFS));
	writel(0x60000000, CREG_AXI_M_SLV1(M_DVFS));
	writel(0x00000000, CREG_AXI_M_OFT0(M_DVFS));
	writel(0x00000000, CREG_AXI_M_OFT1(M_DVFS));
	writel(UPDATE_VAL, CREG_AXI_M_UPDT(M_DVFS));

	writel(0x00000000, CREG_PAE);
	writel(UPDATE_VAL, CREG_PAE_UPDT);
}

static void setup_clocks(void)
{
	ulong rate;

	/* Setup CPU clock */
	if (env_common.cpu_freq.set) {
		rate = env_common.cpu_freq.val * HZ_IN_MHZ;
		soc_clk_ctl("cpu-clk", &rate, CLK_ON | CLK_SET);
	}

	/* Setup TUN clock */
	if (env_common.tun_freq.set) {
		rate = env_common.tun_freq.val * HZ_IN_MHZ;
		if (rate)
			soc_clk_ctl("tun-clk", &rate, CLK_ON | CLK_SET);
		else
			soc_clk_ctl("tun-clk", NULL, CLK_OFF);
	}

	if (env_common.axi_freq.set) {
		rate = env_common.axi_freq.val * HZ_IN_MHZ;
		soc_clk_ctl("axi-clk", &rate, CLK_SET | CLK_ON);
	}
}

static void do_init_claster(void)
{
	/* A multi-core ARC HS configuration always includes only one
	 * ARC_AUX_NON_VOLATILE_LIMIT register, which is shared by all the cores. */
	init_claster_nvlim();

	init_memory_bridge();
}

/* ********************* SMP: END ********************* */

static int check_master_cpu_id(void)
{
	if (CPU_ID_GET() == MASTER_CPU)
		return 0;

	pr_err("u-boot runs on non-master cpu with id: %lu\n", CPU_ID_GET());

	return -ENOENT;
}

static int prepare_cpus(void)
{
	int ret;

	ret = check_master_cpu_id();
	if (ret)
		return ret;

	ret = env_process_and_validate(env_map_common, env_map_core);
	if (ret)
		return ret;

	printf("CPU start mask is %#x\n", env_common.core_mask.val);

	do_init_slave_cpus();

	do_init_master_cpu();

	do_init_claster();

	return 0;
}

static int hsdk_go_run(u32 cpu_start_reg)
{
	/* Cleanup caches, disable interrupts */
	cleanup_before_go();

	if (env_common.halt_on_boot)
		halt_this_cpu();

	__builtin_arc_nop();
	__builtin_arc_nop();
	__builtin_arc_nop();

	/* Kick chosen slave CPUs */
	writel(cpu_start_reg, (void __iomem *)CREG_CPU_START);

	if (is_cpu_used(MASTER_CPU))
		((void (*)(void))(env_core.entry[MASTER_CPU].val))();
	else
		halt_this_cpu();

	pr_err("u-boot still runs on cpu [%ld]\n", CPU_ID_GET());

	/* We will never return after executing our program if master cpu used
	 * otherwise halt master cpu manually */
	while (true)
		halt_this_cpu();

	return 0;
}

int board_prep_linux(bootm_headers_t *images)
{
	u32 i;
	int ret;
	char dt_cpu_path[30];

	ret = env_read_validate_common(env_map_mask);
	if (ret)
		return ret;

	/* Rollback to default values */
	if (!env_common.core_mask.set) {
		env_common.core_mask.val = ALL_CPU_MASK;
		env_common.core_mask.set = true;
	}

	printf("CPU start mask is %#x\n", env_common.core_mask.val);

	if (!is_cpu_used(MASTER_CPU))
		pr_err("ERR: try to launch linux with CPU[0] disabled! It doesn't work for ARC.\n");

	if (!IMAGE_ENABLE_OF_LIBFDT || !images->ft_len) {
		if (env_common.core_mask.val != ALL_CPU_MASK) {
			pr_err("WARN: core_mask setup will work properly only with external DTB!\n");

			return 0;
		}
	}

	for (i = 0; i < NR_CPUS; i++) {
		if (!is_cpu_used(i)) {
			sprintf(dt_cpu_path, "/cpus/cpu@%u", i);
			ret = fdt_status_disabled_by_alias(images->ft_addr, dt_cpu_path);
			debug("patched '%s' node status: ret=%d%s\n", dt_cpu_path,
			      ret, ret == 0 ? "(OK)" : "");
		}
	}

	return 0;
}

void board_jump_and_run(ulong entry, int zero, int arch, uint params)
{
	void (*kernel_entry)(int zero, int arch, uint params);
	u32 cpu_start_reg, i;

	kernel_entry = (void (*)(int, int, uint))entry;

	/* Prepare CREG_CPU_START for kicking chosen CPUs */
	cpu_start_reg = prepare_cpu_ctart_reg();

	/* In case of run without hsdk_init */
	smp_set_core_boot_addr(entry, -1);

	/* In case of run with hsdk_init */
	for (i = 0; i < NR_CPUS; i++) {
		env_core.entry[i].val = entry;
		env_core.entry[i].set = 1;
	}

	/* Entry goes to slave cpu icache so we need to flush master cpu dcache
	 * as there is no coherency between icache and dcache */
	flush_dcache_all();

	/* Kick chosen slave CPUs */
	writel(cpu_start_reg, (void __iomem *)CREG_CPU_START);

	if (is_cpu_used(0))
		kernel_entry(zero, arch, params);
}

static int hsdk_go_prepare_and_run(void)
{
	u32 reg;

	/* Prepare CREG_CPU_START for kicking chosen CPUs */
	reg = prepare_cpu_ctart_reg();

	if (env_common.halt_on_boot)
		printf("CPU will halt before application start, start application with debugger.\n");

	return hsdk_go_run(reg);
}

static int do_hsdk_go(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	int ret;

	/* TODO: delete after release */
#ifdef PRINT_HSDK_CMD_VERSION
	printf("HSDK: hsdk_go version: %s\n", HSDKGO_VERSION);
#endif

	/* Check for 'halt' parameter. 'halt' = enter halt-mode just before
	 * starting the application; can be used for debug */
	if (argc > 1) {
		env_common.halt_on_boot = !strcmp(argv[1], "halt");
		if (!env_common.halt_on_boot) {
			pr_err("Unrecognised parameter: \'%s\'\n", argv[1]);
			return CMD_RET_FAILURE;
		}
	}

	ret = check_master_cpu_id();
	if (ret)
		return ret;

	ret = env_process_and_validate(env_map_mask, env_map_go);
	if (ret)
		return ret;

	ret = hsdk_go_prepare_and_run();

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	hsdk_go, 3, 0, do_hsdk_go,
	"Synopsys HSDK specific command",
	"     - Boot stand-alone application on HSDK\n"
	"hsdk_go halt - Boot stand-alone application on HSDK, halt CPU just before application run\n"
);

static int do_hsdk_init(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	static bool done = false;
	int ret;

	/* TODO: delete after release */
#ifdef PRINT_HSDK_CMD_VERSION
	printf("HSDK: hsdk_init version: %s\n", HSDKGO_VERSION);
#endif

	/* hsdk_init can be run only once */
	if (done) {
		printf("HSDK HW is already initialized! Please reset the board if you want to change the configuration.\n");
		return CMD_RET_FAILURE;
	}

	ret = prepare_cpus();
	if (!ret)
		done = true;

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	hsdk_init, 1, 0, do_hsdk_init,
	"Synopsys HSDK specific command",
	"- Init HSDK HW\n"
);

static int hsdk_read_args_search(const struct hsdk_env_map_common *map, int argc, char * const argv[])
{
	int i;

	for (i = 0; map[i].env_name; i++) {
		if (!strcmp(argv[0], map[i].env_name))
			return i;
	}

	pr_err("Unexpected argument '%s', can't parse\n", argv[0]);

	return -ENOENT;
}

static int arg_read_set(const struct hsdk_env_map_common *map, u32 i, int argc, char *const argv[])
{
	char *endp = argv[1];

	map[i].val->set = 1;

	if (map[i].type == ENV_HEX)
		map[i].val->val = simple_strtoul(argv[1], &endp, 16);
	else
		map[i].val->val = simple_strtoul(argv[1], &endp, 10);

	if (*endp == '\0')
		return 0;

	pr_err("Unexpected argument '%s', can't parse\n", argv[1]);

	map[i].val->set = 0;

	return -EINVAL;
}

static int hsdk_args_enumerate(const struct hsdk_env_map_common *map, int enum_by, int (*act)(const struct hsdk_env_map_common *, u32, int, char *const []), int argc, char * const argv[])
{
	u32 i;

	if (argc % enum_by) {
		pr_err("unexpected argument number: %d\n", argc);
		return -EINVAL;
	}

	while (argc > 0) {
		i = hsdk_read_args_search(map, argc, argv);
		if (i < 0)
			return i;

//		printf("PAL: %s: found '%s' with index %d\n", __func__, map[i].env_name, i);

		if (i < 0) {
			pr_err("unknown arg: %s\n", argv[0]);
			return -EINVAL;
		}

		if (act(map, i, argc, argv))
			return -EINVAL;

//		printf("PAL: %s: value.s '%s' == %#x\n", __func__, argv[1], map[i].val->val);

		argc -= enum_by;
		argv += enum_by;
	}

	return 0;
}

static int do_hsdk_clock_set(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	int ret = 0;

	/* Strip off leading subcommand argument */
	argc--;
	argv++;

	envs_cleanup_common(env_map_clock);

	if (!argc) {
		printf("Set clocks to values specified in environment\n");
		ret = envs_read_common(env_map_clock);
	} else {
		printf("Set clocks to values specified in args\n");
		ret = hsdk_args_enumerate(env_map_clock, 2, arg_read_set, argc, argv);
	}

	if (ret)
		return CMD_RET_FAILURE;

	ret = envs_validate_common(env_map_clock);
	if (ret)
		return CMD_RET_FAILURE;

	/* Setup clock tree HW */
	setup_clocks();

	return CMD_RET_SUCCESS;
}

int env_set_hexi(const char *varname, ulong value)
{
	char str[17];

	sprintf(str, "%#lx", value);
	return env_set(varname, str);
}

static int do_hsdk_clock_get(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	int ret = 0;
	ulong rate;

	/* TODO: get rid of ret */
	ret = soc_clk_ctl("cpu-clk", &rate, CLK_GET);
	if (ret)
		return CMD_RET_FAILURE;

	ret = env_set_ulong("cpu_freq", ceil(rate, HZ_IN_MHZ));
	if (ret)
		return CMD_RET_FAILURE;

	ret = soc_clk_ctl("tun-clk", &rate, CLK_GET);
	if (ret)
		return CMD_RET_FAILURE;

	ret = env_set_ulong("tun_freq", ceil(rate, HZ_IN_MHZ));
	if (ret)
		return CMD_RET_FAILURE;

	ret = soc_clk_ctl("axi-clk", &rate, CLK_GET);
	if (ret)
		return CMD_RET_FAILURE;

	ret = env_set_ulong("axi_freq", ceil(rate, HZ_IN_MHZ));
	if (ret)
		return CMD_RET_FAILURE;

	printf("Clock values are saved to environment\n");

	return CMD_RET_SUCCESS;
}

static int do_hsdk_clock_print(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	/* Main clocks */
	soc_clk_ctl("cpu-clk", NULL, CLK_PRINT);
	soc_clk_ctl("tun-clk", NULL, CLK_PRINT);
	soc_clk_ctl("axi-clk", NULL, CLK_PRINT);
	soc_clk_ctl("ddr-clk", NULL, CLK_PRINT);

	return CMD_RET_SUCCESS;
}

static int do_hsdk_clock_print_all(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	/* CPU clock domain */
	soc_clk_ctl("cpu-pll", NULL, CLK_PRINT);
	soc_clk_ctl("cpu-clk", NULL, CLK_PRINT);
	printf("\n");

	/* SYS clock domain */
	soc_clk_ctl("sys-pll", NULL, CLK_PRINT);
	soc_clk_ctl("apb-clk", NULL, CLK_PRINT);
	soc_clk_ctl("axi-clk", NULL, CLK_PRINT);
	soc_clk_ctl("eth-clk", NULL, CLK_PRINT);
	soc_clk_ctl("usb-clk", NULL, CLK_PRINT);
	soc_clk_ctl("sdio-clk", NULL, CLK_PRINT);
/*	soc_clk_ctl("hdmi-sys-clk", NULL, CLK_PRINT); */
	soc_clk_ctl("gfx-core-clk", NULL, CLK_PRINT);
	soc_clk_ctl("gfx-dma-clk", NULL, CLK_PRINT);
	soc_clk_ctl("gfx-cfg-clk", NULL, CLK_PRINT);
	soc_clk_ctl("dmac-core-clk", NULL, CLK_PRINT);
	soc_clk_ctl("dmac-cfg-clk", NULL, CLK_PRINT);
	soc_clk_ctl("sdio-ref-clk", NULL, CLK_PRINT);
	soc_clk_ctl("spi-clk", NULL, CLK_PRINT);
	soc_clk_ctl("i2c-clk", NULL, CLK_PRINT);
/*	soc_clk_ctl("ebi-clk", NULL, CLK_PRINT); */
	soc_clk_ctl("uart-clk", NULL, CLK_PRINT);
	printf("\n");

	/* DDR clock domain */
	soc_clk_ctl("ddr-clk", NULL, CLK_PRINT);
	printf("\n");

	/* HDMI clock domain */
/*	soc_clk_ctl("hdmi-pll", NULL, CLK_PRINT);
	soc_clk_ctl("hdmi-clk", NULL, CLK_PRINT);
	printf("\n"); */

	/* TUN clock domain */
	soc_clk_ctl("tun-pll", NULL, CLK_PRINT);
	soc_clk_ctl("tun-clk", NULL, CLK_PRINT);
	soc_clk_ctl("rom-clk", NULL, CLK_PRINT);
	soc_clk_ctl("pwm-clk", NULL, CLK_PRINT);
	printf("\n");

	return CMD_RET_SUCCESS;
}

cmd_tbl_t cmd_hsdk_clock[] = {
	U_BOOT_CMD_MKENT(set, 3, 0, do_hsdk_clock_set, "", ""),
	U_BOOT_CMD_MKENT(get, 3, 0, do_hsdk_clock_get, "", ""),
	U_BOOT_CMD_MKENT(print, 4, 0, do_hsdk_clock_print, "", ""),
	U_BOOT_CMD_MKENT(print_all, 4, 0, do_hsdk_clock_print_all, "", ""),
};

static int do_hsdk_clock(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	cmd_tbl_t *c;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* Strip off leading 'hsdk_clock' command argument */
	argc--;
	argv++;

	c = find_cmd_tbl(argv[0], cmd_hsdk_clock, ARRAY_SIZE(cmd_hsdk_clock));
	if (!c)
		return CMD_RET_USAGE;

	return c->cmd(cmdtp, flag, argc, argv);
}

U_BOOT_CMD(
	hsdk_clock, CONFIG_SYS_MAXARGS, 0, do_hsdk_clock,
	"Synopsys HSDK specific clock command",
	"set   - Set clock to values specified in environment / command line arguments\n"
	"hsdk_clock get   - Save clock values to environment\n"
	"hsdk_clock print - Print main clock values to console\n"
	"hsdk_clock print_all - Print all clock values to console\n"
);
