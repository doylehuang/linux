/*
 *  linux/include/asm-arm/arch-mvp2000/hardware.h
 *
 *  This file contains the hardware definitions of the MVP-2000.
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
#ifndef __ASM_ARCH_HARDWARE_H
#define __ASM_ARCH_HARDWARE_H

#include "include/platform.h"

#define IO_ADDRESS(x)  (x - 0x1e600000 + 0xF8000000) 

//PCIE
#ifdef CONFIG_AST_PCIE
#define PCIBIOS_MIN_IO		0x0
#define PCIBIOS_MIN_MEM		0x0
#define pcibios_assign_all_busses()	1
#endif

#endif	/* __ASM_ARCH_HARDWARE_H END */

