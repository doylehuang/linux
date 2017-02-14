/********************************************************************************
* File Name     : linux/arch/arm/plat-aspeed/dev-adc.c
* Author        : Ryan chen
* Description   : ASPEED ADC Device
*
* Copyright (C) ASPEED Technology Inc.
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by the Free Software Foundation;
* either version 2 of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,  but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

* History      :
*    1. 2012/08/06 ryan chen create this file
*
********************************************************************************/

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/platform_device.h>

#include <asm/irq.h>
#include <asm/io.h>

#include "include/irqs.h"
#include "include/platform.h"
#include "include/devs.h"
#include "include/ast-scu.h"

/* --------------------------------------------------------------------
 *  ADC
 * -------------------------------------------------------------------- */

#define AST_IO_VA 0xf0000000
#define SCU_PROTECT_UNLOCK                       0x1688A8A8
#define AST_SCU_RESET                            0x04            /*      system reset control register */
#define SCU_RESET_ADC                           (0x1 << 23)
#define IO_ADDRESS(__pa)        ((void __iomem *)(((__pa) & 0x001fffff) | AST_IO_VA))
static u32 ast_scu_base = IO_ADDRESS(AST_SCU_BASE);

static inline u32
ast_scu_read(u32 reg)
{
        u32 val;

        val = readl(ast_scu_base + reg);

 //       SCUDBUG("ast_scu_read : reg = 0x%08x, val = 0x%08x\n", reg, val);

        return val;
}


static struct resource ast_adc_resources[] = {
        [0] = {
                .start = AST_ADC_BASE,
                .end = AST_ADC_BASE + SZ_4K - 1,
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = IRQ_ADC,
                .end = IRQ_ADC,
                .flags = IORESOURCE_IRQ,
        },
};

static inline void
ast_scu_write(u32 val, u32 reg)
{
 //       SCUDBUG("ast_scu_write : reg = 0x%08x, val = 0x%08x\n", reg, val);
        //unlock
        writel(SCU_PROTECT_UNLOCK, ast_scu_base);
        writel(val, ast_scu_base + reg);
        //lock
 //       writel(0xaa,ast_scu_base);
}


struct platform_device ast_adc_device = {
        .name = "ast_adc",
        .id = 0,
        .resource = ast_adc_resources,
        .num_resources = ARRAY_SIZE(ast_adc_resources),
};



extern void
ast_scu_init_adc(void)
{
        ast_scu_write(ast_scu_read(AST_SCU_RESET) | SCU_RESET_ADC, AST_SCU_RESET);
        ast_scu_write(ast_scu_read(AST_SCU_RESET) & ~SCU_RESET_ADC, AST_SCU_RESET);
}


void __init ast_add_device_adc(void)
{
        //SCU ADC CTRL Reset
        ast_scu_init_adc();

        platform_device_register(&ast_adc_device);
}

