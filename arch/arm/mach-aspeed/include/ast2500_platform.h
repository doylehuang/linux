/*
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

#ifndef _AST2500_PLATFORM_H_
#define _AST2500_PLATFORM_H_                 1

#define AST_DRAM_BASE				0x80000000

#define AST_SRAM_SIZE				(SZ_32K)

#define AST_AHBC_BASE				0x1E600000	/* AHB CONTROLLER */

#define AST_FMC_BASE        			0x1E620000	/* NEW SMC CONTROLLER */
#define AST_FMC_SPI0_BASE			0x1E630000	/* NEW SMC CONTROLLER */
#define AST_FMC_SPI1_BASE			0x1E631000	/* NEW SMC CONTROLLER */

/* */
#define AST_MIC_BASE					0x1E650000	/* Memory Integrity Check Controller */
#define AST_MAC0_BASE				0x1E660000	/* MAC1 */ 
#define AST_MAC1_BASE				0x1E680000	/* MAC2 */ 
/* */
#define AST_USB20_BASE				0x1E6A0000	/* USB 2.0 VIRTUAL HUB CONTROLLER */
#define AST_EHCI0_BASE				0x1E6A1000	/* USB 2.0 HOST CONTROLLER */
#define AST_UDC1_BASE				0x1E6A2000	/* USB 2.0 Device CONTROLLER */
#define AST_EHCI1_BASE				0x1E6A3000	/* USB 2.0 HOST CONTROLLER */
#define AST_UHCI_BASE				0x1E6B0000	/* USB 1.1 HOST CONTROLLER */
#define AST_VIC_BASE					0x1E6C0000	/* VIC */
/* */
#define AST_SDMC_BASE				0x1E6E0000	/* MMC SDRAM*/
#define AST_HID_BASE					0x1E6E1000	/* USB 1.1 Controller */
#define AST_SCU_BASE				0x1E6E2000	/* SCU */
#define AST_CRYPTO_BASE				0x1E6E3000	/* Crypto */
#define AST_JTAG_BASE				0x1E6E4000	/* JTAG */
#define AST_CRT0_BASE				0x1E6E6000	/* CRT0 */
#define AST_XDMA_BASE				0x1E6E7000	/* XDMA */
#define AST_MCTP_BASE				0x1E6E8000	/* MCTP */
#define AST_ADC_BASE				0x1E6E9000	/* ADC */
/* */
#define AST_LPC_PLUS_BASE			0x1E6EC000	/* LPC+ */
#define AST_PCIE_PLDA_BASE			0x1E6ED000	/* PCIE PLDA Bridge */
#define AST_ESPI_BASE				0x1E6EE000	/* e-SPI  */
#define AST_BSRAM_BASE				0x1E6EF000	/* Battery Backup SRAM  */
/* */
/* */
#define AST_VIDEO_BASE				0x1E700000	/* VIDEO ENGINE */
#define AST_SRAM_BASE				0x1E720000	/* SRAM */
#define AST_SDHC_BASE				0x1E740000	/* SD */
#define AST_2D_BASE					0x1E760000	/* 2D */
#define AST_GPIO_BASE				0x1E780000	/* GPIO */
#define AST_SGPIO_BASE				0x1E780200	/* SGPIO */
#define AST_SGPIO_S_BASE			0x1E780300	/* SGPIO Slave*/
#define AST_RTC_BASE					0x1E781000	/* RTC */
#define AST_TIMER_BASE				0x1E782000	/* TIMER #0~2*/
#define AST_UART1_BASE				0x1E783000	/* UART1 */
#define AST_UART0_BASE				0x1E784000	/* UART5 */
#define AST_WDT_BASE				0x1E785000	/* WDT */
#define AST_PWM_BASE				0x1E786000	/* PWM */
#define AST_VUART0_BASE				0x1E787000	/* VUART1 */
#define AST_PUART_BASE				0x1E788000	/* PUART */
#define AST_LPC_BASE					0x1E789000	/* LPC */
#define AST_MBX_BASE				0x1E789200	/* Mailbox */
#define AST_I2C_BASE					0x1E78A000	/* I2C */
#define AST_PECI_BASE				0x1E78B000	/* PECI */
#define AST_PCIARBITER_BASE			0x1E78C000	/* PCI ARBITER */
#define AST_UART2_BASE				0x1E78D000	/* UART2 */
#define AST_UART3_BASE				0x1E78E000	/* UART3 */
#define AST_UART4_BASE				0x1E78F000	/* UART4 */
#define AST_UART5_BASE				0x1E790000	/* UART6 */
#define AST_UART6_BASE				0x1E791000	/* UART7 */
#define AST_UART7_BASE				0x1E792000	/* UART8 */
#define AST_UART8_BASE				0x1E793000	/* UART9 */
#define AST_UART9_BASE				0x1E794000	/* UART10 */
#define AST_UART10_BASE				0x1E795000	/* UART11 */
#define AST_UART11_BASE				0x1E796000	/* UART12 */
#define AST_UART12_BASE				0x1E797000	/* UART13 */
#define AST_UART_SDMA_BASE			0x1E79E000	/* UART SDMA */

#define AST_FMC_CS0_BASE		              0x20000000	/* CS0 */
#define AST_FMC_CS1_BASE		              0x28000000	/* CS3 */
#define AST_FMC_CS2_BASE		              0x2a000000	/* CS4 */

#define AST_FMC_SPI0_CS0_BASE			0x30000000	/* SPI 2 Flash CS 0 Memory */
#define AST_FMC_SPI0_CS1_BASE			0x32000000	/* SPI 2 Flash CS 1 Memory */
#define AST_FMC_SPI1_CS0_BASE			0x38000000	/* SPI 3 Flash CS 0 Memory */
#define AST_FMC_SPI1_CS1_BASE			0x3a000000	/* SPI 3 Flash CS 1 Memory */

#define AST_CRT0_MEM_SIZE				SZ_16M
#define AST_CRT0_MEM_BASE				(AST_DRAM_BASE + (SZ_1M * 496))

#define ASR_VIDEO_MEM 					(AST_DRAM_BASE + SZ_256M)

#define AST_LPC_BRIDGE				0x60000000
#define AST_LPC_PLUS_BRIDGE			0x70000000

#endif
