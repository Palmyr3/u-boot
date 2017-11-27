//#define DEBUG

#include <common.h>
#include <config.h>
#include <clk.h>
#include <dm/device.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <asm/arcregs.h>

#ifdef CONFIG_CPU_BIG_ENDIAN
	#error "hsdk_go will not work with BIG endian CPU"
#endif

#define HSDKGO_VERSION	"0.6"

#define HZ_IN_MHZ	1000000

#define ceil(x, y) ({ ulong __x = (x), __y = (y); (__x + __y - 1) / __y; })

#define NR_CPUS		4
#define MASTER_CPU	0
#define MAX_CMD_LEN	25

#define NO_CCM		0x10

void smp_set_core_boot_addr(unsigned long addr, int corenr);

typedef struct {
	u32 val;
	bool set;
} u32_env;

struct hsdk_env_core_ctl {
	bool used[NR_CPUS];
	u32_env entry[NR_CPUS];
	u32_env iccm[NR_CPUS];
	u32_env dccm[NR_CPUS];
};

struct hsdk_env_common_ctl {
	bool halt_on_boot;
	u32_env core_mask;
	u32_env cpu_freq;
	u32_env axi_freq;
	u32_env tun_freq;
	u32_env nvlim;
	u32_env icache;
	u32_env dcache;
};

struct hsdk_env_map_common {
	const char * const env_name;
	bool mandatory;
	u32 min;
	u32 max;
	u32_env *val;
};

struct hsdk_env_map_core {
	const char * const env_name;
	bool mandatory;
	u32 min[NR_CPUS];
	u32 max[NR_CPUS];
	u32_env (*val)[NR_CPUS];
};

/* Place for slave cpu temporary stack */
static u32 slave_stack[256 * NR_CPUS] __attribute__((aligned(4)));

static struct hsdk_env_common_ctl env_common = {};
static struct hsdk_env_core_ctl env_core = {};

int soc_clk_ctl(const char *name, ulong *rate, bool set)
{
	int ret;
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

	ret = clk_enable(&clk);
	if (ret && ret != -ENOSYS)
		return ret;

	if (set) {
		ret = clk_set_rate(&clk, *rate);
		if (ret)
			return ret;
	}

	*rate = clk_get_rate(&clk);

	clk_free(&clk);

	debug("HSDK: clock '%s' rate %lu MHz\n", name, ceil(*rate, HZ_IN_MHZ));

	return 0;
}

static bool is_cpu_used(u32 cpu_id)
{
	return !!(env_common.core_mask.val & BIT(cpu_id));
}

static const struct hsdk_env_map_common env_map_common[] = {
	{ "core_mask",		true,	0x1, 0xF,	&env_common.core_mask },
	{ "cpu_freq",		false,	100, 1000,	&env_common.cpu_freq },
	{ "axi_freq",		false,	200, 800,	&env_common.axi_freq },
	{ "tun_freq",		false,	0, 150,		&env_common.tun_freq },
	{ "non_volatile_limit", true,	0, 0xF, 	&env_common.nvlim },
	{ "icache_ena",		true,	0, 1,		&env_common.icache },
	{ "dcache_ena",		true,	0, 1,		&env_common.dcache },
	{}
};

static const struct hsdk_env_map_core env_map_core[] = {
	{ "core_entry",	true,	{0, 0, 0, 0}, {U32_MAX, U32_MAX, U32_MAX, U32_MAX},	&env_core.entry },
	{ "core_iccm",	true,	{NO_CCM, 0, NO_CCM, 0}, {NO_CCM, 0xF, NO_CCM, 0xF},	&env_core.iccm },
	{ "core_dccm",	true,	{NO_CCM, 0, NO_CCM, 0}, {NO_CCM, 0xF, NO_CCM, 0xF},	&env_core.dccm },
	{}
};

static int env_read_common(u32 index)
{
	u32 val;

	if (!env_get_yesno(env_map_common[index].env_name)) {
		val = (u32)env_get_hex(env_map_common[index].env_name, 0);
		debug("ENV: %s = %#x\n", env_map_common[index].env_name, val);

		env_map_common[index].val->val = val;
		env_map_common[index].val->set = true;
	}

	return 0;
}

/* process core specific variables */
static int env_read_core(u32 index)
{
	u32 i, val;
	char comand[MAX_CMD_LEN];

	for (i = 0; i < NR_CPUS; i++) {
		sprintf(comand, "%s_%u", env_map_core[index].env_name, i);
		if (!env_get_yesno(comand)) {
			val = (u32)env_get_hex(comand, 0);
			debug("ENV: %s: = %#x\n", comand, val);

			(*env_map_core[index].val)[i].val = val;
			(*env_map_core[index].val)[i].set = true;
		}
	}

	return 0;
}

/* environment common verification */
static int env_validate_common(u32 index)
{
	u32 value = env_map_common[index].val->val;
	bool set = env_map_common[index].val->set;
	u32 min = env_map_common[index].min;
	u32 max = env_map_common[index].max;

	/* Check if environment is mandatory */
	if (env_map_common[index].mandatory && !set) {
		pr_err("Variable \'%s\' is mandatory, but it is not defined\n",
			env_map_common[index].env_name);

		return -EINVAL;
	}

	/* Check environment boundary */
	if (set && (value < min || value > max)) {
		pr_err("Variable \'%s\' must be between %#x and %#x\n",
			env_map_common[index].env_name, min, max);

		return -EINVAL;
	}

	return 0;
}

static int env_validate_core(u32 index)
{
	u32 i;
	u32 value;
	bool set;
	bool mandatory = env_map_core[index].mandatory;
	u32 min, max;

	for (i = 0; i < NR_CPUS; i++) {
		set = (*env_map_core[index].val)[i].set;
		value = (*env_map_core[index].val)[i].val;

		/* Check if environment is mandatory */
		if (is_cpu_used(i) && !(mandatory && set)) {
			pr_err("CPU %u is used, but \'%s_%u\' is not defined\n",
				i, env_map_core[index].env_name, i);

			return -EINVAL;
		}

		min = env_map_core[index].min[i];
		max = env_map_core[index].max[i];

		/* Check environment boundary */
		if (set && (value < min || value > max)) {
			pr_err("Variable \'%s_%u\' must be between %#x and %#x\n",
				env_map_core[index].env_name, i, min, max);

			return -EINVAL;
		}
	}

	return 0;
}

static int env_process_and_validate(void)
{
	u32 i;
	int ret;

	/* Generic read */
	for (i = 0; env_map_common[i].env_name; i++) {
		ret = env_read_common(i);
		if (ret)
			return ret;
	}

	for (i = 0; env_map_core[i].env_name; i++) {
		ret = env_read_core(i);
		if (ret)
			return ret;
	}

	/* Generic validate */
	for (i = 0; env_map_common[i].env_name; i++) {
		ret = env_validate_common(i);
		if (ret)
			return ret;
	}

	for (i = 0; env_map_core[i].env_name; i++) {
		ret = env_validate_core(i);
		if (ret)
			return ret;
	}

	return 0;
}

#define APT_SHIFT		28

/* Bit values in IC_CTRL */
#define IC_CTRL_CACHE_DISABLE	(1 << 0)

/* Bit values in DC_CTRL */
#define DC_CTRL_CACHE_DISABLE	(1 << 0)
#define DC_CTRL_INV_MODE_FLUSH	(1 << 6)

/* TODO: move this to "arch/arc/include/asm/arcregs.h" */
#define AUX_NON_VOLATILE_LIMIT	0x5E
#define ARC_REG_AUX_DCCM	0x18	/* DCCM Base Addr ARCv2 */
#define ARC_REG_AUX_ICCM	0x208	/* ICCM Base Addr (ARCv2) */
#define AUX_VOL			0x5E
#define AUX_CACHE_LIMIT		0x5D

#define AUX_IDENTITY		4

static inline void nop_instr(void)
{
	__asm__ __volatile__("nop");
}

// TODO: add xCCM runtime check
static void smp_init_slave_cpu_func(u32 core)
{
	unsigned int r;

	/* ICCM move if exists */
	if (env_core.iccm[core].val != NO_CCM) {
		r = ARC_REG_AUX_ICCM;
		write_aux_reg(r, env_core.iccm[core].val << APT_SHIFT);
	}

	/* DCCM move if exists */
	if (env_core.dccm[core].val != NO_CCM) {
		r = ARC_REG_AUX_DCCM;
		write_aux_reg(r, env_core.dccm[core].val << APT_SHIFT);
	}

	/* i$ enable if required (it is disabled by default) */
	if (env_common.icache.val) {
		r = ARC_AUX_IC_CTRL;
		write_aux_reg(r, read_aux_reg(r) & ~IC_CTRL_CACHE_DISABLE);
	}

	/* d$ enable if required (it is disabled by default) */
	if (env_common.dcache.val) {
		r = ARC_AUX_DC_CTRL;
		write_aux_reg(r, read_aux_reg(r) & ~(DC_CTRL_CACHE_DISABLE | DC_CTRL_INV_MODE_FLUSH));
	}
}

static void init_master_nvlim(void)
{
	u32 val = env_common.nvlim.val << APT_SHIFT;

	flush_dcache_all();
	write_aux_reg(AUX_NON_VOLATILE_LIMIT, val);
	write_aux_reg(AUX_CACHE_LIMIT, val);
}

// TODO: !! add my own implementation of flush_dcache_all, invalidate_icache_all
// as current implementations depends on CONFIG_SYS_DCACHE_OFF and
// CONFIG_SYS_ICACHE_OFF

static void init_master_icache(void)
{
	unsigned int r;

#ifndef CONFIG_SYS_ICACHE_OFF
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
		nop_instr();
		nop_instr();
		nop_instr();
		/* instruction cache disable */
		write_aux_reg(ARC_AUX_IC_CTRL, 0x00000001U);
	}
#endif
}

static void init_master_dcache(void)
{
	unsigned int r;

#ifndef CONFIG_SYS_ICACHE_OFF
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
	flush_dcache_all();
	invalidate_dcache_all();
	invalidate_icache_all();

	return 0;
}

/* ********************* SMP: START ********************* */

#define ARCNUM_SHIFT	8

static inline u32 get_this_cpu_id(void)
{
	u32 val = read_aux_reg(AUX_IDENTITY);

//	val &= GENMASK(15, ARCNUM_SHIFT);

	val &= 0xFF00;
	val >>= ARCNUM_SHIFT;

	return val;
}

#define	CREG_BASE	(ARC_PERIPHERAL_BASE + 0x1000)
#define	CREG_CPU_START	(CREG_BASE + 0x400)
#define	CPU_START_MASK	0xF

static int cleanup_before_go(void)
{
	disable_interrupts();
	cleanup_cache_before_go();

	return 0;
}

static inline void this_cpu_halt(void)
{
	__asm__ __volatile__("flag  1\n");
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

volatile u32 data_flag;
volatile u32 stack_ptr;

static inline void set_data_flag(void)
{
	__asm__ __volatile__(	"mov	r8,	%%sp\n"
				"st	r8,	%0\n"
	 			: "=m" (data_flag)
				: /* no input */
				: "memory");
}

/* flatten */
__attribute__((naked, noreturn, flatten)) noinline void hsdk_core_init_f(void)
{
	__asm__ __volatile__(	"ld	r8,	%0\n"
				"mov	%%sp,	r8\n"
				"mov	%%fp,	%%sp\n"
	 			: /* no output */
				: "m" (stack_ptr)
				: "memory");

	smp_init_slave_cpu_func(get_this_cpu_id());

	set_data_flag();
	/* Make sure other cores see written value in memory */
	flush_dcache_all();

	/* Halt the processor untill the master kick us again */
	this_cpu_halt();

	nop_instr();
	nop_instr();
	nop_instr();

	/* Run our program */
	((void (*)(void))(env_core.entry[get_this_cpu_id()].val))();

	/* Something went terribly wrong */
	while (true)
		this_cpu_halt();
}

static void do_init_slave_cpu(u32 cpu_id)
{
	u32 timeout = 50000;

	data_flag = 0;

	/* Use global unic place for slave cpu stack */
	/* TODO: optimize memory usage, now we have one unused aperture */
	stack_ptr = (u32)(slave_stack + (64 * cpu_id));

	/* TODO: remove useless debug's in do_init_slave_cpu */
	debug("CPU %u: stack base: %x\n", cpu_id, stack_ptr);
	debug("CPU %u: stack pool base: %p\n", cpu_id, slave_stack);
	smp_set_core_boot_addr((unsigned long)hsdk_core_init_f, -1);

	/* Make sure other cores see written value in memory */
	flush_dcache_all();

	smp_kick_cpu_x(cpu_id);

	debug("CPU %u: FLAG0: %x\n", cpu_id, data_flag);
	while (!data_flag && timeout)
		timeout--;

	/* We need to panic here as there is no option to halt slave cpu
	 * (or check that slave cpu is halted) */
	if (!timeout)
		pr_err("CPU %u is not responding after init!\n", cpu_id);

	debug("CPU %u: FLAG1: %x\n", cpu_id, data_flag);
}

static void do_init_slave_cpus(void)
{
	u32 i;

	for (i = 1; i < NR_CPUS; i++)
		if (env_core.used[i])
			do_init_slave_cpu(i);
}

static void do_init_master_cpu(void)
{
	if (env_core.used[MASTER_CPU]) {
		init_master_icache();
		init_master_dcache();
	}
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

#define CREG_PAE	((void __iomem *)(CREG_BASE + 0x180))
#define CREG_PAE_UPDT	((void __iomem *)(CREG_BASE + 0x194))

static void init_memory_bridge(void)
{
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
	ulong rate, tmp_rate;

	/* Setup CPU clock */
	if (env_common.cpu_freq.set) {
		rate = env_common.cpu_freq.val * HZ_IN_MHZ;
		soc_clk_ctl("cpu-pll", &rate, true); /* 100MHz - 1GHz is OK for PLL */
		soc_clk_ctl("cpu-clk", &rate, true); /* div factor = 1 */
	} else {
		soc_clk_ctl("cpu-clk", &rate, false);
	}
	printf("HSDK: clock '%s' rate %lu MHz\n", "cpu-clk", ceil(rate, HZ_IN_MHZ));

	/* Setup TUN clock */
	if (env_common.tun_freq.set) {
		rate = env_common.tun_freq.val * HZ_IN_MHZ;
		if (rate >= 100 * HZ_IN_MHZ) {
			/* 150 MHz : PLL - 150MHz; DIV = 1 */
			/* 125 MHz : PLL - 125MHz; DIV = 1 */
			/* 100 MHz : PLL - 100MHz; DIV = 1 */
			soc_clk_ctl("tun-pll", &rate, true); /* 100MHz - 150MHz is OK for PLL */
			soc_clk_ctl("tun-clk", &rate, true); /* div factor = 1 */
		} else if (rate > 0) {
			/* 75 MHz  : PLL - 150MHz; DIV = 2 */
			/* 50 MHz  : PLL - 150MHz; DIV = 3 */
			/* 25 MHz  : PLL - 150MHz; DIV = 6 */
			tmp_rate = 150 * HZ_IN_MHZ;
			soc_clk_ctl("tun-pll", &tmp_rate, true);
			soc_clk_ctl("tun-clk", &rate, true); /* div factor - autocalc */
		} else {
			/* 25 MHz  : PLL - DNC;    DIV = OFF */
			// TODO: add
		}
	} else {
		soc_clk_ctl("tun-clk", &rate, false);
	}
	printf("HSDK: clock '%s' rate %lu MHz\n", "tun-clk", ceil(rate, HZ_IN_MHZ));

	if (env_common.axi_freq.set) {
		rate = env_common.axi_freq.val * HZ_IN_MHZ;
		/* firstly we need to increase SYS dividers factors to set
		 * 'safe' freq values */
		tmp_rate = 33333333;
		soc_clk_ctl("sys-apb", &tmp_rate, true);
		soc_clk_ctl("sys-axi", &tmp_rate, true);
		soc_clk_ctl("sys-eth", &tmp_rate, true);
		soc_clk_ctl("sys-usb", &tmp_rate, true);
		soc_clk_ctl("sys-sdio", &tmp_rate, true);
		soc_clk_ctl("sys-hdmi", &tmp_rate, true);
		soc_clk_ctl("sys-gfx-core", &tmp_rate, true);
		soc_clk_ctl("sys-gfx-dma", &tmp_rate, true);
		soc_clk_ctl("sys-gfx-cfg", &tmp_rate, true);
		soc_clk_ctl("sys-dmac-core", &tmp_rate, true);
		soc_clk_ctl("sys-dmac-cfg", &tmp_rate, true);
		soc_clk_ctl("sys-sdio-ref", &tmp_rate, true);
		soc_clk_ctl("sys-spi", &tmp_rate, true);
		soc_clk_ctl("sys-i2c", &tmp_rate, true);
		soc_clk_ctl("sys-uart", &tmp_rate, true);
		soc_clk_ctl("sys-ebi", &tmp_rate, true);

		/* update (increase) PLL clock */
		if (rate == 800 * HZ_IN_MHZ) {
			tmp_rate = 800 * HZ_IN_MHZ;
			soc_clk_ctl("sys-pll", &tmp_rate, true);
			soc_clk_ctl("sys-axi", &tmp_rate, true);
		} else if (rate == 600 * HZ_IN_MHZ) {
			tmp_rate = 600 * HZ_IN_MHZ;
			soc_clk_ctl("sys-pll", &tmp_rate, true);
			soc_clk_ctl("sys-axi", &tmp_rate, true);
		} else if (rate <= 400 * HZ_IN_MHZ) {
			tmp_rate = 400 * HZ_IN_MHZ;
			soc_clk_ctl("sys-pll", &tmp_rate, true);
			soc_clk_ctl("sys-axi", &rate, true); /* div factor - autocalc */
		}

		/* return SYS dividers factors to 'fast' freq values */
		tmp_rate = 200 * HZ_IN_MHZ;
		soc_clk_ctl("sys-apb", &tmp_rate, true);
		tmp_rate = 400 * HZ_IN_MHZ;
		soc_clk_ctl("sys-eth", &tmp_rate, true);
		soc_clk_ctl("sys-usb", &tmp_rate, true);
		soc_clk_ctl("sys-sdio", &tmp_rate, true);
		soc_clk_ctl("sys-hdmi", &tmp_rate, true);
		tmp_rate = 800 * HZ_IN_MHZ;
		soc_clk_ctl("sys-gfx-core", &tmp_rate, true);
		tmp_rate = 400 * HZ_IN_MHZ;
		soc_clk_ctl("sys-gfx-dma", &tmp_rate, true);
		tmp_rate = 200 * HZ_IN_MHZ;
		soc_clk_ctl("sys-gfx-cfg", &tmp_rate, true);
		tmp_rate = 400 * HZ_IN_MHZ;
		soc_clk_ctl("sys-dmac-core", &tmp_rate, true);
		tmp_rate = 200 * HZ_IN_MHZ;
		soc_clk_ctl("sys-dmac-cfg", &tmp_rate, true);
		tmp_rate = 100 * HZ_IN_MHZ;
		soc_clk_ctl("sys-sdio-ref", &tmp_rate, true);
		tmp_rate = 33333333;
		soc_clk_ctl("sys-spi", &tmp_rate, true);
		tmp_rate = 200 * HZ_IN_MHZ;
		soc_clk_ctl("sys-i2c", &tmp_rate, true);
		tmp_rate = 33333333;
		soc_clk_ctl("sys-uart", &tmp_rate, true);
		tmp_rate = 50 * HZ_IN_MHZ;
		soc_clk_ctl("sys-ebi", &tmp_rate, true);
	}
	soc_clk_ctl("sys-axi", &rate, false);
	printf("HSDK: clock '%s' rate %lu MHz\n", "axi-clk", ceil(rate, HZ_IN_MHZ));

	soc_clk_ctl("ddr-clk", &rate, false);
	printf("HSDK: clock '%s' rate %lu MHz\n", "ddr-clk", ceil(rate, HZ_IN_MHZ));
}

static void do_init_claster(void)
{
	/* A multi-core ARC HS configuration always includes only one
	 * AUX_NON_VOLATILE_LIMIT register, which is shared by all the cores. */
	init_master_nvlim();

	init_memory_bridge();
}

/* ********************* SMP: END ********************* */

static int check_master_cpu_id(void)
{
	if (get_this_cpu_id() == MASTER_CPU)
		return 0;

	pr_err("u-boot runs on non-master cpu with id: %u\n", get_this_cpu_id());

	return -ENOENT;
}

static int prepare_cpus(u32 *cpu_start_reg)
{
	u32 i;
	int ret;

	ret = check_master_cpu_id();
	if (ret)
		return ret;

	ret = env_process_and_validate();
	if (ret)
		return ret;

	for (i = 0; i < NR_CPUS; i++) {
		env_core.used[i] = is_cpu_used(i);
	}

	do_init_slave_cpus();

	setup_clocks();

	do_init_claster();

	/* Prepare CREG_CPU_START for kicking chosen CPUs */
	*cpu_start_reg = prepare_cpu_ctart_reg();

	do_init_master_cpu();

	return 0;
}

static int hsdk_go_run(u32 cpu_start_reg)
{
	/* Cleanup caches, disable interrupts */
	cleanup_before_go();

	if (env_common.halt_on_boot)
		this_cpu_halt();

	/* Kick chosen CPUs */
	writel(cpu_start_reg, (void __iomem *)CREG_CPU_START);

	if (env_core.used[MASTER_CPU])
		((void (*)(void))(env_core.entry[MASTER_CPU].val))();
	else
		this_cpu_halt();

	pr_err("u-boot still runs on cpu [%d]\n", get_this_cpu_id());

	/* We will never return after executing our program if master cpu used
	 * otherwise halt master cpu manually */
	while (true)
		this_cpu_halt();

	return 0;
}

static int bootm_run(u32 cpu_start_reg)
{
	debug("bootm cpumask: %#x\n", cpu_start_reg);

	/* Cleanup caches, disable interrupts */
	cleanup_before_go();

	/* Kick chosen CPUs */
	writel(cpu_start_reg, (void __iomem *)CREG_CPU_START);

	return 0;
}

static int hsdk_go_prepare_and_run(void)
{
	int ret;
	u32 reg;

	ret = prepare_cpus(&reg);
	if (ret)
		return ret;

	return hsdk_go_run(reg);
}

int bootm_prepare_and_run(u32 entry)
{
	int ret;
	u32 i, reg;
	char comand[MAX_CMD_LEN];

	/* override core entry env by value from image*/
	for (i = 0; i < NR_CPUS; i++) {
		sprintf(comand, "%s_%u", "core_entry", i);
		env_set_hex(comand, entry);
	}

	ret = prepare_cpus(&reg);
	if (ret)
		return ret;

	return bootm_run(reg);
}

//static int prepare_and_run(void)
//{
//	u32 i, reg;
//	ulong rate;
//	int ret;
//
//	ret = check_master_cpu_id();
//	if (ret)
//		return ret;
//
//	ret = env_process_and_validate();
//	if (ret)
//		return ret;
//
//	for (i = 0; i < NR_CPUS; i++) {
//		env_core.used[i] = is_cpu_used(i);
//	}
//
//	do_init_slave_cpus();
//
//	/* TODO: set frequency, not only read */
//	soc_clk_ctl("cpu-clk", &rate, false);
//	soc_clk_ctl("sys-clk", &rate, false);
//	soc_clk_ctl("ddr-clk", &rate, false);
//	soc_clk_ctl("tun-clk", &rate, false);
//
//	/* A multi-core ARC HS configuration always includes only one
//	 * AUX_NON_VOLATILE_LIMIT register, which is shared by all the cores. */
//	init_master_nvlim();
//
//	/* Prepare CREG_CPU_START for kicking chosen CPUs */
//	reg = prepare_cpu_ctart_reg();
//
//	do_init_master_cpu();
//
//	/* Cleanup caches, disable interrupts */
//	cleanup_before_go();
//
//	if (env_common.halt_on_boot)
//		this_cpu_halt();
//
//	/* Kick chosen CPUs */
//	writel(reg, (void __iomem *)CREG_CPU_START);
//
//	if (env_core.used[MASTER_CPU])
//		((void (*)(void))(env_core.entry[MASTER_CPU].val))();
//	else
//		this_cpu_halt();
//
//	pr_err("u-boot still runs on cpu [%d]\n", get_this_cpu_id());
//
//	/* We will never return after executing our program if master cpu used
//	 * otherwise halt master cpu manually */
//	while (true)
//		this_cpu_halt();
//
//	return 0;
//}

static int do_hsdk_go(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	/* TODO: delete after release */
	printf("HSDK: hsdk_go version: %s\n", HSDKGO_VERSION);

	/* Check for 'halt' parameter. 'halt' = enter halt-mode just before
	 * starting the application; can be used for debug */
	if (argc > 1) {
		env_common.halt_on_boot = !strcmp(argv[1], "halt");
		if (!env_common.halt_on_boot) {
			pr_err("Unrecognised parameter: \'%s\'\n", argv[1]);
			return -EINVAL;
		}
	}

	return hsdk_go_prepare_and_run();
}

U_BOOT_CMD(
	hsdk_go, 3, 0, do_hsdk_go,
	"Synopsys HSDK specific command",
	"hsdk_go                 - Boot stand-alone application on HSDK\n"
);
