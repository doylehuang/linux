/*
 *  arch/arm/plat-aspeed/include/plat/irqs.h
 *
 *  Copyright (C) 2012-2020  ASPEED Technology Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "include/aspeed.h"

#include "include/ast2500_irqs.h"

#include "include/ast_gpio_irqs.h"
//#include <mach/ast_lpc_irqs.h>

/*********************************************************************************/
//CVIC
#if defined(CONFIG_ARCH_AST1070)
//Companion chip irq
#include <mach/ast1070_irqs.h>
#endif

#if defined(CONFIG_AST2400_BMC)
#include <mach/ext_ast2400_irqs.h>
#endif
