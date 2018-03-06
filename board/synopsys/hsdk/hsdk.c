/*
 * Copyright (C) 2017 Synopsys, Inc. All rights reserved.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dwmmc.h>
#include <malloc.h>
#include <usb.h>

DECLARE_GLOBAL_DATA_PTR;

#define	CREG_BASE	(ARC_PERIPHERAL_BASE + 0x1000)
#define	CREG_PAE	(CREG_BASE + 0x180)
#define	CREG_PAE_UPDATE	(CREG_BASE + 0x194)
#define	CREG_CPU_START	(CREG_BASE + 0x400)

/* TODO: move AXI apertures setup code here from hsdk-cmd.c */
void init_memory_bridge(void);

int board_early_init_f(void)
{
	/* In current chip PAE support for DMA is broken, disabling it. */
	writel(0, (void __iomem *) CREG_PAE);

	/* Really apply settings made above */
	writel(1, (void __iomem *) CREG_PAE_UPDATE);

	/* Setup AXI apertures unconditionally as we want to have DDR
	 * in 0x00000000 region when we are kicking slave cpus */
	init_memory_bridge();

	return 0;
}

#define SDIO_BASE              (ARC_PERIPHERAL_BASE + 0xA000)
#define SDIO_UHS_REG_EXT       (SDIO_BASE + 0x108)
#define SDIO_UHS_REG_EXT_DIV_2 (2 << 30)

int board_mmc_init(bd_t *bis)
{
	struct dwmci_host *host = NULL;

	host = malloc(sizeof(struct dwmci_host));
	if (!host) {
		printf("dwmci_host malloc fail!\n");
		return 1;
	}

	/*
	 * Switch SDIO external ciu clock divider from default div-by-8 to
	 * minimum possible div-by-2.
	 */
	writel(SDIO_UHS_REG_EXT_DIV_2, (void __iomem *) SDIO_UHS_REG_EXT);

	memset(host, 0, sizeof(struct dwmci_host));
	host->name = "Synopsys Mobile storage";
	host->ioaddr = (void *)ARC_DWMMC_BASE;
	host->buswidth = 4;
	host->dev_index = 0;
	host->bus_hz = 50000000;

	add_dwmci(host, host->bus_hz / 2, 400000);

	return 0;
}

int board_early_init_r(void)
{
	/* Init USB to be able read environment from USB MSD */
	/* usb_init(); */ /* Temporary disable init as it break usb in linux */

	/*
	 * Flush all d$ as we want to use uncached area with .di instructions
	 * and we don't want to have any dirty line in L1d$ or SL$ in this area.
	 * It is enough to flush all d$ once here as we access to uncached area
	 * with regular st (non .di) instruction only when we copy data during
	 * u-boot relocation.
	 */
	flush_dcache_all();

	return 0;
}

int board_late_init(void)
{
	/* Populate environment with clock frequency values */
	run_command("hsdk_clock get", 0);

	return 0;
}

#define RESET_VECTOR_ADDR	0x0

void smp_set_core_boot_addr(unsigned long addr, int corenr)
{
	/* All cores have reset vector pointing to 0 */
	writel(addr, (void __iomem *)RESET_VECTOR_ADDR);

	/* Make sure other cores see written value in memory */
	flush_dcache_all();
}

void smp_kick_all_cpus(void)
{
#define BITS_START_CORE1	1
#define BITS_START_CORE2	2
#define BITS_START_CORE3	3

	int cmd = readl((void __iomem *)CREG_CPU_START);

	cmd |= (1 << BITS_START_CORE1) |
	       (1 << BITS_START_CORE2) |
	       (1 << BITS_START_CORE3);
	writel(cmd, (void __iomem *)CREG_CPU_START);
}
