/*
 * Copyright (C) 2013-2014 Synopsys, Inc. All rights reserved.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <config.h>
#include <common.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <asm/arcregs.h>
#include <asm/arc-bcr.h>
#include <asm/cache.h>

DECLARE_GLOBAL_DATA_PTR;

static inline void __ic_entire_invalidate(void);
static inline void __dc_entire_op(const int cacheop);

/* Bit values in IC_CTRL */
#define IC_CTRL_CACHE_DISABLE	BIT(0)

/* Bit values in DC_CTRL */
#define DC_CTRL_CACHE_DISABLE	BIT(0)
#define DC_CTRL_INV_MODE_FLUSH	BIT(6)
#define DC_CTRL_FLUSH_STATUS	BIT(8)

#define OP_INV			BIT(0)
#define OP_FLUSH		BIT(1)
#define OP_FLUSH_N_INV		(OP_FLUSH | OP_INV)

/* Bit val in SLC_CONTROL */
#define SLC_CTRL_DIS		0x001
#define SLC_CTRL_IM		0x040
#define SLC_CTRL_BUSY		0x100
#define SLC_CTRL_RGN_OP_INV	0x200

#define CACHE_LINE_MASK		(~(gd->arch.l1_line_sz - 1))

static inline bool pae_exists(void)
{
	/* TODO: should we compare mmu version from BCR and from CONFIG? */
#if (CONFIG_ARC_MMU_VER >= 4)
	union bcr_mmu_4 mmu4;

	mmu4.word = read_aux_reg(ARC_AUX_MMU_BCR);

	if (mmu4.fields.pae)
		return true;
#endif /* (CONFIG_ARC_MMU_VER >= 4) */

	return false;
}

static inline bool icache_exists(void)
{
	union bcr_di_cache ibcr;

	ibcr.word = read_aux_reg(ARC_BCR_IC_BUILD);
	return !!ibcr.fields.ver;
}

static inline bool dcache_exists(void)
{
	union bcr_di_cache dbcr;

	dbcr.word = read_aux_reg(ARC_BCR_DC_BUILD);
	return !!dbcr.fields.ver;
}

static inline bool slc_exists(void)
{
	if (is_isa_arcv2()) {
		union bcr_generic sbcr;

		sbcr.word = read_aux_reg(ARC_BCR_SLC);
		return !!sbcr.fields.ver;
	}

	return false;
}

static inline bool ioc_exists(void)
{
	if (is_isa_arcv2()) {
		union bcr_clust_cfg cbcr;

		cbcr.word = read_aux_reg(ARC_BCR_CLUSTER);
		return cbcr.fields.c;
	}

	return false;
}

static inline bool ioc_status(void)
{
	/*
	 * We check only CONFIG option instead of IOC HW state check as IOC
	 * must be disabled by default.
	 */
	if (is_ioc_enabled())
		return ioc_exists();

	return false;
}

static inline bool slc_status(void)
{
	/* TODO: HS 3.0 supports SLC disable so we need to check it here */
	return slc_exists();
}

static void __slc_entire_op(const int op)
{
	unsigned int ctrl;

	if (!slc_status())
		return;

	ctrl = read_aux_reg(ARC_AUX_SLC_CTRL);

	if (!(op & OP_FLUSH))		/* i.e. OP_INV */
		ctrl &= ~SLC_CTRL_IM;	/* clear IM: Disable flush before Inv */
	else
		ctrl |= SLC_CTRL_IM;

	write_aux_reg(ARC_AUX_SLC_CTRL, ctrl);

	if (op & OP_INV)	/* Inv or flush-n-inv use same cmd reg */
		write_aux_reg(ARC_AUX_SLC_INVALIDATE, 0x1);
	else
		write_aux_reg(ARC_AUX_SLC_FLUSH, 0x1);

	/* Make sure "busy" bit reports correct stataus, see STAR 9001165532 */
	read_aux_reg(ARC_AUX_SLC_CTRL);

	/* Important to wait for flush to complete */
	while (read_aux_reg(ARC_AUX_SLC_CTRL) & SLC_CTRL_BUSY);
}

static void slc_upper_region_init(void)
{
	/*
	 * ARC_AUX_SLC_RGN_START1 and ARC_AUX_SLC_RGN_END1 register exist
	 * only if PAE exists in current HW. So we had to check pae_exist
	 * before using them.
	 */
	if (!pae_exists())
		return;

	/*
	 * ARC_AUX_SLC_RGN_END1 and ARC_AUX_SLC_RGN_START1 are always == 0
	 * as we don't use PAE40.
	 */
	write_aux_reg(ARC_AUX_SLC_RGN_END1, 0);
	write_aux_reg(ARC_AUX_SLC_RGN_START1, 0);
}

static void __slc_rgn_op(unsigned long paddr, unsigned long sz, const int op)
{
	unsigned int ctrl;
	unsigned long end;

	if (!slc_status())
		return;

	/*
	 * The Region Flush operation is specified by CTRL.RGN_OP[11..9]
	 *  - b'000 (default) is Flush,
	 *  - b'001 is Invalidate if CTRL.IM == 0
	 *  - b'001 is Flush-n-Invalidate if CTRL.IM == 1
	 */
	ctrl = read_aux_reg(ARC_AUX_SLC_CTRL);

	/* Don't rely on default value of IM bit */
	if (!(op & OP_FLUSH))		/* i.e. OP_INV */
		ctrl &= ~SLC_CTRL_IM;	/* clear IM: Disable flush before Inv */
	else
		ctrl |= SLC_CTRL_IM;

	if (op & OP_INV)
		ctrl |= SLC_CTRL_RGN_OP_INV;	/* Inv or flush-n-inv */
	else
		ctrl &= ~SLC_CTRL_RGN_OP_INV;

	write_aux_reg(ARC_AUX_SLC_CTRL, ctrl);

	/*
	 * Lower bits are ignored, no need to clip
	 * END needs to be setup before START (latter triggers the operation)
	 * END can't be same as START, so add (l2_line_sz - 1) to sz
	 */
	end = paddr + sz + gd->arch.slc_line_sz - 1;

	/*
	 * Upper addresses (ARC_AUX_SLC_RGN_END1 and ARC_AUX_SLC_RGN_START1)
	 * are always == 0 as we don't use PAE40, so we only setup lower ones
	 * (ARC_AUX_SLC_RGN_END and ARC_AUX_SLC_RGN_START)
	 */
	write_aux_reg(ARC_AUX_SLC_RGN_END, end);
	write_aux_reg(ARC_AUX_SLC_RGN_START, paddr);

	/* Make sure "busy" bit reports correct stataus, see STAR 9001165532 */
	read_aux_reg(ARC_AUX_SLC_CTRL);

	while (read_aux_reg(ARC_AUX_SLC_CTRL) & SLC_CTRL_BUSY);
}

static void arc_ioc_setup(void)
{
	/* IOC Aperture start is equal to DDR start */
	unsigned int ap_base = CONFIG_SYS_SDRAM_BASE;
	/* IOC Aperture size is equal to DDR size */
	long ap_size = CONFIG_SYS_SDRAM_SIZE;

	flush_n_invalidate_dcache_all();

	if (!is_power_of_2(ap_size) || ap_size < 4096)
		panic("IOC Aperture size must be power of 2 and bigger 4Kib");

	/*
	 * IOC Aperture size decoded as 2 ^ (SIZE + 2) KB,
	 * so setting 0x11 implies 512M, 0x12 implies 1G...
	 */
	write_aux_reg(ARC_AUX_IO_COH_AP0_SIZE,
		      order_base_2(ap_size / 1024) - 2);

	/* IOC Aperture start must be aligned to the size of the aperture */
	if (ap_base % ap_size != 0)
		panic("IOC Aperture start must be aligned to the size of the aperture");

	write_aux_reg(ARC_AUX_IO_COH_AP0_BASE, ap_base >> 12);
	write_aux_reg(ARC_AUX_IO_COH_PARTIAL, 1);
	write_aux_reg(ARC_AUX_IO_COH_ENABLE, 1);
}

static void read_decode_cache_bcr_arcv2(void)
{
	union bcr_slc_cfg slc_cfg;

	if (slc_exists()) {
		slc_cfg.word = read_aux_reg(ARC_AUX_SLC_CONFIG);
		gd->arch.slc_line_sz = (slc_cfg.fields.lsz == 0) ? 128 : 64;
	}
}

void read_decode_cache_bcr(void)
{
	int dc_line_sz = 0, ic_line_sz = 0;
	union bcr_di_cache ibcr, dbcr;

	ibcr.word = read_aux_reg(ARC_BCR_IC_BUILD);
	if (ibcr.fields.ver) {
		gd->arch.l1_line_sz = ic_line_sz = 8 << ibcr.fields.line_len;
		if (!ic_line_sz)
			panic("Instruction exists but line length is 0\n");
	}

	dbcr.word = read_aux_reg(ARC_BCR_DC_BUILD);
	if (dbcr.fields.ver) {
		gd->arch.l1_line_sz = dc_line_sz = 16 << dbcr.fields.line_len;
		if (!dc_line_sz)
			panic("Data cache exists but line length is 0\n");
	}

	if (ic_line_sz && dc_line_sz && (ic_line_sz != dc_line_sz))
		panic("Instruction and data cache line lengths differ\n");
}

void cache_init(void)
{
	read_decode_cache_bcr();

	if (is_isa_arcv2())
		read_decode_cache_bcr_arcv2();

	if (is_isa_arcv2() && ioc_status())
		arc_ioc_setup();

	if (is_isa_arcv2() && slc_exists())
		slc_upper_region_init();
}

int icache_status(void)
{
	if (!icache_exists())
		return 0;

	if (read_aux_reg(ARC_AUX_IC_CTRL) & IC_CTRL_CACHE_DISABLE)
		return 0;
	else
		return 1;
}

void icache_enable(void)
{
	if (icache_exists())
		write_aux_reg(ARC_AUX_IC_CTRL, read_aux_reg(ARC_AUX_IC_CTRL) &
			      ~IC_CTRL_CACHE_DISABLE);
}

void icache_disable(void)
{
	if (!icache_exists())
		return;

	__ic_entire_invalidate();

	write_aux_reg(ARC_AUX_IC_CTRL, read_aux_reg(ARC_AUX_IC_CTRL) |
		      IC_CTRL_CACHE_DISABLE);
}

/* IC supports only invalidation */
static inline void __ic_entire_invalidate(void)
{
	if (!icache_status())
		return;

	/* Any write to IC_IVIC register triggers invalidation of entire I$ */
	write_aux_reg(ARC_AUX_IC_IVIC, 1);
	/*
	 * As per ARC HS databook (see chapter 5.3.3.2)
	 * it is required to add 3 NOPs after each write to IC_IVIC.
	 */
	__builtin_arc_nop();
	__builtin_arc_nop();
	__builtin_arc_nop();
	read_aux_reg(ARC_AUX_IC_CTRL);  /* blocks */
}

void invalidate_icache_all(void)
{
	__ic_entire_invalidate();

	/*
	 * We don't to invalidate SLC here as we can lose some data from
	 * SLC (especially if L1 D$ is disabled) because SLC is shared for
	 * data and instructions.
	 */
}

int dcache_status(void)
{
	if (!dcache_exists())
		return 0;

	if (read_aux_reg(ARC_AUX_DC_CTRL) & DC_CTRL_CACHE_DISABLE)
		return 0;
	else
		return 1;
}

void dcache_enable(void)
{
	if (!dcache_exists())
		return;

	write_aux_reg(ARC_AUX_DC_CTRL, read_aux_reg(ARC_AUX_DC_CTRL) &
		      ~(DC_CTRL_INV_MODE_FLUSH | DC_CTRL_CACHE_DISABLE));
}

void dcache_disable(void)
{
	if (!dcache_exists())
		return;

	__dc_entire_op(OP_FLUSH_N_INV);

	write_aux_reg(ARC_AUX_DC_CTRL, read_aux_reg(ARC_AUX_DC_CTRL) |
		      DC_CTRL_CACHE_DISABLE);
}

/* Common Helper for Line Operations on D-cache */
static inline void __dcache_line_loop(unsigned long paddr, unsigned long sz,
				      const int cacheop)
{
	unsigned int aux_cmd;
	int num_lines;

	/* d$ cmd: INV (discard or wback-n-discard) OR FLUSH (wback) */
	aux_cmd = cacheop & OP_INV ? ARC_AUX_DC_IVDL : ARC_AUX_DC_FLDL;

	sz += paddr & ~CACHE_LINE_MASK;
	paddr &= CACHE_LINE_MASK;

	num_lines = DIV_ROUND_UP(sz, gd->arch.l1_line_sz);

	while (num_lines-- > 0) {
#if (CONFIG_ARC_MMU_VER == 3)
		write_aux_reg(ARC_AUX_DC_PTAG, paddr);
#endif
		write_aux_reg(aux_cmd, paddr);
		paddr += gd->arch.l1_line_sz;
	}
}

static void __before_dc_op(const int op)
{
	unsigned int ctrl;

	ctrl = read_aux_reg(ARC_AUX_DC_CTRL);

	/* IM bit implies flush-n-inv, instead of vanilla inv */
	if (op == OP_INV)
		ctrl &= ~DC_CTRL_INV_MODE_FLUSH;
	else
		ctrl |= DC_CTRL_INV_MODE_FLUSH;

	write_aux_reg(ARC_AUX_DC_CTRL, ctrl);
}

static void __after_dc_op(const int op)
{
	if (op & OP_FLUSH)	/* flush / flush-n-inv both wait */
		while (read_aux_reg(ARC_AUX_DC_CTRL) & DC_CTRL_FLUSH_STATUS);
}

static inline void __dc_entire_op(const int cacheop)
{
	int aux;

	if (!dcache_status())
		return;

	__before_dc_op(cacheop);

	if (cacheop & OP_INV)	/* Inv or flush-n-inv use same cmd reg */
		aux = ARC_AUX_DC_IVDC;
	else
		aux = ARC_AUX_DC_FLSH;

	write_aux_reg(aux, 0x1);

	__after_dc_op(cacheop);
}

static inline void __dc_line_op(unsigned long paddr, unsigned long sz,
				const int cacheop)
{
	if (!dcache_status())
		return;

	__before_dc_op(cacheop);
	__dcache_line_loop(paddr, sz, cacheop);
	__after_dc_op(cacheop);
}

void invalidate_dcache_range(unsigned long start, unsigned long end)
{
	if (start >= end)
		return;

	/*
	 * ARCv1                  -> call __dc_line_op
	 * ARCv2 && no IOC        -> call __dc_line_op; call __slc_rgn_op
	 * ARCv2 && IOC enabled   -> nothing
	 */
	if (!is_isa_arcv2() || !ioc_status())
		__dc_line_op(start, end - start, OP_INV);

	if (is_isa_arcv2() && !ioc_status())
		__slc_rgn_op(start, end - start, OP_INV);
}

void flush_dcache_range(unsigned long start, unsigned long end)
{
	if (start >= end)
		return;

	/*
	 * ARCv1                  -> call __dc_line_op
	 * ARCv2 && no IOC        -> call __dc_line_op; call __slc_rgn_op
	 * ARCv2 && IOC enabled   -> nothing
	 */
	if (!is_isa_arcv2() || !ioc_status())
		__dc_line_op(start, end - start, OP_FLUSH);

	if (is_isa_arcv2() && !ioc_status())
		__slc_rgn_op(start, end - start, OP_FLUSH);
}

void flush_cache(unsigned long start, unsigned long size)
{
	flush_dcache_range(start, start + size);
}

/*
 * As invalidate_dcache_all() is not used in generic U-Boot code and as we
 * don't need it in arch/arc code alone (invalidate without flush) we implement
 * flush_n_invalidate_dcache_all (flush and invalidate in 1 operation) because
 * it's much safer.
 */
void flush_n_invalidate_dcache_all(void)
{
	__dc_entire_op(OP_FLUSH_N_INV);

	if (is_isa_arcv2())
		__slc_entire_op(OP_FLUSH_N_INV);
}

void flush_dcache_all(void)
{
	__dc_entire_op(OP_FLUSH);

	if (is_isa_arcv2())
		__slc_entire_op(OP_FLUSH);
}

/*
 * This is function for making I/D Caches consistent when modifying u-boot code
 * (relocation, etc...)
 */
void sync_icache_dcache_all(void)
{
	__dc_entire_op(OP_FLUSH);
	__ic_entire_invalidate();
}

void sync_n_cleanup_cache_all(void)
{
	__dc_entire_op(OP_FLUSH_N_INV);

	if (is_isa_arcv2())
		__slc_entire_op(OP_FLUSH_N_INV);

	__ic_entire_invalidate();
}
