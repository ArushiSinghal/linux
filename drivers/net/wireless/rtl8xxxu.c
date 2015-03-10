/*
 * RTL8723au mac80211 USB driver
 *
 * Copyright (c) 2014 Jes Sorensen <Jes.Sorensen@redhat.com>
 *
 * Portions, notably calibration code:
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/wireless.h>
#include <linux/firmware.h>
#include <linux/moduleparam.h>
#include <net/mac80211.h>
#include "rtl8xxxu.h"
#include "rtl8xxxu_regs.h"

#define DRIVER_NAME "rtl8xxxu"

static int rtl8xxxu_debug = 0;

MODULE_AUTHOR("Jes Sorensen <Jes.Sorensen@redhat.com>");
MODULE_DESCRIPTION("RTL8723au USB mac80211 Wireless LAN Driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_A.bin");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_B.bin");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_B_NoBT.bin");

module_param_named(debug, rtl8xxxu_debug, int, 0600);
MODULE_PARM_DESC(debug, "Set debug mask");

#define USB_VENDER_ID_REALTEK		0x0BDA
/* Minimum IEEE80211_MAX_FRAME_LEN */
#define RTL_RX_BUFFER_SIZE		IEEE80211_MAX_FRAME_LEN

static struct usb_device_id dev_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x8724,
				       0xff, 0xff, 0xff)},
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x1724,
				       0xff, 0xff, 0xff)},
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x0724,
				       0xff, 0xff, 0xff)},
	{ }
};

MODULE_DEVICE_TABLE(usb, dev_table);

static struct ieee80211_rate rtl8xxxu_rates[] = {
	{ .bitrate = 10, .hw_value = DESC_RATE_1M, .flags = 0 },
	{ .bitrate = 20, .hw_value = DESC_RATE_2M, .flags = 0 },
	{ .bitrate = 55, .hw_value = DESC_RATE_5_5M, .flags = 0 },
	{ .bitrate = 110, .hw_value = DESC_RATE_11M, .flags = 0 },
	{ .bitrate = 60, .hw_value = DESC_RATE_6M, .flags = 0 },
	{ .bitrate = 90, .hw_value = DESC_RATE_9M, .flags = 0 },
	{ .bitrate = 120, .hw_value = DESC_RATE_12M, .flags = 0 },
	{ .bitrate = 180, .hw_value = DESC_RATE_18M, .flags = 0 },
	{ .bitrate = 240, .hw_value = DESC_RATE_24M, .flags = 0 },
	{ .bitrate = 360, .hw_value = DESC_RATE_36M, .flags = 0 },
	{ .bitrate = 480, .hw_value = DESC_RATE_48M, .flags = 0 },
	{ .bitrate = 540, .hw_value = DESC_RATE_54M, .flags = 0 },
};

static struct ieee80211_channel rtl8xxxu_channels_2g[] = {
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2412,
	  .hw_value = 1, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2417,
	  .hw_value = 2, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2422,
	  .hw_value = 3, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2427,
	  .hw_value = 4, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2432,
	  .hw_value = 5, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2437,
	  .hw_value = 6, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2442,
	  .hw_value = 7, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2447,
	  .hw_value = 8, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2452,
	  .hw_value = 9, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2457,
	  .hw_value = 10, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2462,
	  .hw_value = 11, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2467,
	  .hw_value = 12, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2472,
	  .hw_value = 13, .max_power = 30 },
	{ .band = IEEE80211_BAND_2GHZ, .center_freq = 2484,
	  .hw_value = 14, .max_power = 30 }
};

static struct ieee80211_supported_band rtl8xxxu_supported_band = {
	.channels = rtl8xxxu_channels_2g,
	.n_channels = ARRAY_SIZE(rtl8xxxu_channels_2g),
	.bitrates = rtl8xxxu_rates,
	.n_bitrates = ARRAY_SIZE(rtl8xxxu_rates),
};

static const u32 rtl8xxxu_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

static struct rtl8xxxu_reg8val rtl8723a_mac_init_table[] = {
	{0x420, 0x80}, {0x423, 0x00}, {0x430, 0x00}, {0x431, 0x00},
	{0x432, 0x00}, {0x433, 0x01}, {0x434, 0x04}, {0x435, 0x05},
	{0x436, 0x06}, {0x437, 0x07}, {0x438, 0x00}, {0x439, 0x00},
	{0x43a, 0x00}, {0x43b, 0x01}, {0x43c, 0x04}, {0x43d, 0x05},
	{0x43e, 0x06}, {0x43f, 0x07}, {0x440, 0x5d}, {0x441, 0x01},
	{0x442, 0x00}, {0x444, 0x15}, {0x445, 0xf0}, {0x446, 0x0f},
	{0x447, 0x00}, {0x458, 0x41}, {0x459, 0xa8}, {0x45a, 0x72},
	{0x45b, 0xb9}, {0x460, 0x66}, {0x461, 0x66}, {0x462, 0x08},
	{0x463, 0x03}, {0x4c8, 0xff}, {0x4c9, 0x08}, {0x4cc, 0xff},
	{0x4cd, 0xff}, {0x4ce, 0x01}, {0x500, 0x26}, {0x501, 0xa2},
	{0x502, 0x2f}, {0x503, 0x00}, {0x504, 0x28}, {0x505, 0xa3},
	{0x506, 0x5e}, {0x507, 0x00}, {0x508, 0x2b}, {0x509, 0xa4},
	{0x50a, 0x5e}, {0x50b, 0x00}, {0x50c, 0x4f}, {0x50d, 0xa4},
	{0x50e, 0x00}, {0x50f, 0x00}, {0x512, 0x1c}, {0x514, 0x0a},
	{0x515, 0x10}, {0x516, 0x0a}, {0x517, 0x10}, {0x51a, 0x16},
	{0x524, 0x0f}, {0x525, 0x4f}, {0x546, 0x40}, {0x547, 0x00},
	{0x550, 0x10}, {0x551, 0x10}, {0x559, 0x02}, {0x55a, 0x02},
	{0x55d, 0xff}, {0x605, 0x30}, {0x608, 0x0e}, {0x609, 0x2a},
	{0x652, 0x20}, {0x63c, 0x0a}, {0x63d, 0x0a}, {0x63e, 0x0e},
	{0x63f, 0x0e}, {0x66e, 0x05}, {0x700, 0x21}, {0x701, 0x43},
	{0x702, 0x65}, {0x703, 0x87}, {0x708, 0x21}, {0x709, 0x43},
	{0x70a, 0x65}, {0x70b, 0x87}, {0xffff, 0xff},
};

static struct rtl8xxxu_reg32val rtl8723a_phy_1t_init_table[] = {
	{0x800, 0x80040000}, {0x804, 0x00000003},
	{0x808, 0x0000fc00}, {0x80c, 0x0000000a},
	{0x810, 0x10001331}, {0x814, 0x020c3d10},
	{0x818, 0x02200385}, {0x81c, 0x00000000},
	{0x820, 0x01000100}, {0x824, 0x00390004},
	{0x828, 0x00000000}, {0x82c, 0x00000000},
	{0x830, 0x00000000}, {0x834, 0x00000000},
	{0x838, 0x00000000}, {0x83c, 0x00000000},
	{0x840, 0x00010000}, {0x844, 0x00000000},
	{0x848, 0x00000000}, {0x84c, 0x00000000},
	{0x850, 0x00000000}, {0x854, 0x00000000},
	{0x858, 0x569a569a}, {0x85c, 0x001b25a4},
	{0x860, 0x66f60110}, {0x864, 0x061f0130},
	{0x868, 0x00000000}, {0x86c, 0x32323200},
	{0x870, 0x07000760}, {0x874, 0x22004000},
	{0x878, 0x00000808}, {0x87c, 0x00000000},
	{0x880, 0xc0083070}, {0x884, 0x000004d5},
	{0x888, 0x00000000}, {0x88c, 0xccc000c0},
	{0x890, 0x00000800}, {0x894, 0xfffffffe},
	{0x898, 0x40302010}, {0x89c, 0x00706050},
	{0x900, 0x00000000}, {0x904, 0x00000023},
	{0x908, 0x00000000}, {0x90c, 0x81121111},
	{0xa00, 0x00d047c8}, {0xa04, 0x80ff000c},
	{0xa08, 0x8c838300}, {0xa0c, 0x2e68120f},
	{0xa10, 0x9500bb78}, {0xa14, 0x11144028},
	{0xa18, 0x00881117}, {0xa1c, 0x89140f00},
	{0xa20, 0x1a1b0000}, {0xa24, 0x090e1317},
	{0xa28, 0x00000204}, {0xa2c, 0x00d30000},
	{0xa70, 0x101fbf00}, {0xa74, 0x00000007},
	{0xa78, 0x00000900}, {0xc00, 0x48071d40},
	{0xc04, 0x03a05611}, {0xc08, 0x000000e4},
	{0xc0c, 0x6c6c6c6c}, {0xc10, 0x08800000},
	{0xc14, 0x40000100}, {0xc18, 0x08800000},
	{0xc1c, 0x40000100}, {0xc20, 0x00000000},
	{0xc24, 0x00000000}, {0xc28, 0x00000000},
	{0xc2c, 0x00000000}, {0xc30, 0x69e9ac44},
#if 0	/* Not for USB */
	{0xff0f011f, 0xabcd},
	{0xc34, 0x469652cf},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0xc34, 0x469652af},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0xc38, 0x49795994},
	{0xc3c, 0x0a97971c}, {0xc40, 0x1f7c403f},
	{0xc44, 0x000100b7}, {0xc48, 0xec020107},
	{0xc4c, 0x007f037f}, {0xc50, 0x69543420},
	{0xc54, 0x43bc0094}, {0xc58, 0x69543420},
	{0xc5c, 0x433c0094}, {0xc60, 0x00000000},
#if 0	/* Not for USB */
	{0xff0f011f, 0xabcd},
	{0xc64, 0x7116848b},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0xc64, 0x7112848b},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0xc68, 0x47c00bff},
	{0xc6c, 0x00000036}, {0xc70, 0x2c7f000d},
	{0xc74, 0x018610db}, {0xc78, 0x0000001f},
	{0xc7c, 0x00b91612}, {0xc80, 0x40000100},
	{0xc84, 0x20f60000}, {0xc88, 0x40000100},
	{0xc8c, 0x20200000}, {0xc90, 0x00121820},
	{0xc94, 0x00000000}, {0xc98, 0x00121820},
	{0xc9c, 0x00007f7f}, {0xca0, 0x00000000},
	{0xca4, 0x00000080}, {0xca8, 0x00000000},
	{0xcac, 0x00000000}, {0xcb0, 0x00000000},
	{0xcb4, 0x00000000}, {0xcb8, 0x00000000},
	{0xcbc, 0x28000000}, {0xcc0, 0x00000000},
	{0xcc4, 0x00000000}, {0xcc8, 0x00000000},
	{0xccc, 0x00000000}, {0xcd0, 0x00000000},
	{0xcd4, 0x00000000}, {0xcd8, 0x64b22427},
	{0xcdc, 0x00766932}, {0xce0, 0x00222222},
	{0xce4, 0x00000000}, {0xce8, 0x37644302},
	{0xcec, 0x2f97d40c}, {0xd00, 0x00080740},
	{0xd04, 0x00020401}, {0xd08, 0x0000907f},
	{0xd0c, 0x20010201}, {0xd10, 0xa0633333},
	{0xd14, 0x3333bc43}, {0xd18, 0x7a8f5b6b},
	{0xd2c, 0xcc979975}, {0xd30, 0x00000000},
	{0xd34, 0x80608000}, {0xd38, 0x00000000},
	{0xd3c, 0x00027293}, {0xd40, 0x00000000},
	{0xd44, 0x00000000}, {0xd48, 0x00000000},
	{0xd4c, 0x00000000}, {0xd50, 0x6437140a},
	{0xd54, 0x00000000}, {0xd58, 0x00000000},
	{0xd5c, 0x30032064}, {0xd60, 0x4653de68},
	{0xd64, 0x04518a3c}, {0xd68, 0x00002101},
	{0xd6c, 0x2a201c16}, {0xd70, 0x1812362e},
	{0xd74, 0x322c2220}, {0xd78, 0x000e3c24},
	{0xe00, 0x2a2a2a2a}, {0xe04, 0x2a2a2a2a},
	{0xe08, 0x03902a2a}, {0xe10, 0x2a2a2a2a},
	{0xe14, 0x2a2a2a2a}, {0xe18, 0x2a2a2a2a},
	{0xe1c, 0x2a2a2a2a}, {0xe28, 0x00000000},
	{0xe30, 0x1000dc1f}, {0xe34, 0x10008c1f},
	{0xe38, 0x02140102}, {0xe3c, 0x681604c2},
	{0xe40, 0x01007c00}, {0xe44, 0x01004800},
	{0xe48, 0xfb000000}, {0xe4c, 0x000028d1},
	{0xe50, 0x1000dc1f}, {0xe54, 0x10008c1f},
	{0xe58, 0x02140102}, {0xe5c, 0x28160d05},
	{0xe60, 0x00000008}, {0xe68, 0x001b25a4},
	{0xe6c, 0x631b25a0}, {0xe70, 0x631b25a0},
	{0xe74, 0x081b25a0}, {0xe78, 0x081b25a0},
	{0xe7c, 0x081b25a0}, {0xe80, 0x081b25a0},
	{0xe84, 0x631b25a0}, {0xe88, 0x081b25a0},
	{0xe8c, 0x631b25a0}, {0xed0, 0x631b25a0},
	{0xed4, 0x631b25a0}, {0xed8, 0x631b25a0},
	{0xedc, 0x001b25a0}, {0xee0, 0x001b25a0},
	{0xeec, 0x6b1b25a0}, {0xf14, 0x00000003},
	{0xf4c, 0x00000000}, {0xf00, 0x00000300},
	{0xffff, 0xffffffff},
};

static struct rtl8xxxu_reg32val rtl8723a_agc_1t_init_table[] = {
	{0xc78, 0x7b000001}, {0xc78, 0x7b010001},
	{0xc78, 0x7b020001}, {0xc78, 0x7b030001},
	{0xc78, 0x7b040001}, {0xc78, 0x7b050001},
	{0xc78, 0x7a060001}, {0xc78, 0x79070001},
	{0xc78, 0x78080001}, {0xc78, 0x77090001},
	{0xc78, 0x760a0001}, {0xc78, 0x750b0001},
	{0xc78, 0x740c0001}, {0xc78, 0x730d0001},
	{0xc78, 0x720e0001}, {0xc78, 0x710f0001},
	{0xc78, 0x70100001}, {0xc78, 0x6f110001},
	{0xc78, 0x6e120001}, {0xc78, 0x6d130001},
	{0xc78, 0x6c140001}, {0xc78, 0x6b150001},
	{0xc78, 0x6a160001}, {0xc78, 0x69170001},
	{0xc78, 0x68180001}, {0xc78, 0x67190001},
	{0xc78, 0x661a0001}, {0xc78, 0x651b0001},
	{0xc78, 0x641c0001}, {0xc78, 0x631d0001},
	{0xc78, 0x621e0001}, {0xc78, 0x611f0001},
	{0xc78, 0x60200001}, {0xc78, 0x49210001},
	{0xc78, 0x48220001}, {0xc78, 0x47230001},
	{0xc78, 0x46240001}, {0xc78, 0x45250001},
	{0xc78, 0x44260001}, {0xc78, 0x43270001},
	{0xc78, 0x42280001}, {0xc78, 0x41290001},
	{0xc78, 0x402a0001}, {0xc78, 0x262b0001},
	{0xc78, 0x252c0001}, {0xc78, 0x242d0001},
	{0xc78, 0x232e0001}, {0xc78, 0x222f0001},
	{0xc78, 0x21300001}, {0xc78, 0x20310001},
	{0xc78, 0x06320001}, {0xc78, 0x05330001},
	{0xc78, 0x04340001}, {0xc78, 0x03350001},
	{0xc78, 0x02360001}, {0xc78, 0x01370001},
	{0xc78, 0x00380001}, {0xc78, 0x00390001},
	{0xc78, 0x003a0001}, {0xc78, 0x003b0001},
	{0xc78, 0x003c0001}, {0xc78, 0x003d0001},
	{0xc78, 0x003e0001}, {0xc78, 0x003f0001},
	{0xc78, 0x7b400001}, {0xc78, 0x7b410001},
	{0xc78, 0x7b420001}, {0xc78, 0x7b430001},
	{0xc78, 0x7b440001}, {0xc78, 0x7b450001},
	{0xc78, 0x7a460001}, {0xc78, 0x79470001},
	{0xc78, 0x78480001}, {0xc78, 0x77490001},
	{0xc78, 0x764a0001}, {0xc78, 0x754b0001},
	{0xc78, 0x744c0001}, {0xc78, 0x734d0001},
	{0xc78, 0x724e0001}, {0xc78, 0x714f0001},
	{0xc78, 0x70500001}, {0xc78, 0x6f510001},
	{0xc78, 0x6e520001}, {0xc78, 0x6d530001},
	{0xc78, 0x6c540001}, {0xc78, 0x6b550001},
	{0xc78, 0x6a560001}, {0xc78, 0x69570001},
	{0xc78, 0x68580001}, {0xc78, 0x67590001},
	{0xc78, 0x665a0001}, {0xc78, 0x655b0001},
	{0xc78, 0x645c0001}, {0xc78, 0x635d0001},
	{0xc78, 0x625e0001}, {0xc78, 0x615f0001},
	{0xc78, 0x60600001}, {0xc78, 0x49610001},
	{0xc78, 0x48620001}, {0xc78, 0x47630001},
	{0xc78, 0x46640001}, {0xc78, 0x45650001},
	{0xc78, 0x44660001}, {0xc78, 0x43670001},
	{0xc78, 0x42680001}, {0xc78, 0x41690001},
	{0xc78, 0x406a0001}, {0xc78, 0x266b0001},
	{0xc78, 0x256c0001}, {0xc78, 0x246d0001},
	{0xc78, 0x236e0001}, {0xc78, 0x226f0001},
	{0xc78, 0x21700001}, {0xc78, 0x20710001},
	{0xc78, 0x06720001}, {0xc78, 0x05730001},
	{0xc78, 0x04740001}, {0xc78, 0x03750001},
	{0xc78, 0x02760001}, {0xc78, 0x01770001},
	{0xc78, 0x00780001}, {0xc78, 0x00790001},
	{0xc78, 0x007a0001}, {0xc78, 0x007b0001},
	{0xc78, 0x007c0001}, {0xc78, 0x007d0001},
	{0xc78, 0x007e0001}, {0xc78, 0x007f0001},
	{0xc78, 0x3800001e}, {0xc78, 0x3801001e},
	{0xc78, 0x3802001e}, {0xc78, 0x3803001e},
	{0xc78, 0x3804001e}, {0xc78, 0x3805001e},
	{0xc78, 0x3806001e}, {0xc78, 0x3807001e},
	{0xc78, 0x3808001e}, {0xc78, 0x3c09001e},
	{0xc78, 0x3e0a001e}, {0xc78, 0x400b001e},
	{0xc78, 0x440c001e}, {0xc78, 0x480d001e},
	{0xc78, 0x4c0e001e}, {0xc78, 0x500f001e},
	{0xc78, 0x5210001e}, {0xc78, 0x5611001e},
	{0xc78, 0x5a12001e}, {0xc78, 0x5e13001e},
	{0xc78, 0x6014001e}, {0xc78, 0x6015001e},
	{0xc78, 0x6016001e}, {0xc78, 0x6217001e},
	{0xc78, 0x6218001e}, {0xc78, 0x6219001e},
	{0xc78, 0x621a001e}, {0xc78, 0x621b001e},
	{0xc78, 0x621c001e}, {0xc78, 0x621d001e},
	{0xc78, 0x621e001e}, {0xc78, 0x621f001e},
	{0xffff, 0xffffffff}
};

static struct rtl8xxxu_rfregval rtl8723au_radioa_rf6052_1t_init_table[] = {
	{0x00, 0x00030159}, {0x01, 0x00031284},
	{0x02, 0x00098000},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x03, 0x00018c63},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x03, 0x00039c63},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x04, 0x000210e7}, {0x09, 0x0002044f},
	{0x0a, 0x0001a3f1}, {0x0b, 0x00014787},
	{0x0c, 0x000896fe}, {0x0d, 0x0000e02c},
	{0x0e, 0x00039ce7}, {0x0f, 0x00000451},
	{0x19, 0x00000000}, {0x1a, 0x00030355},
	{0x1b, 0x00060a00}, {0x1c, 0x000fc378},
	{0x1d, 0x000a1250}, {0x1e, 0x0000024f},
	{0x1f, 0x00000000}, {0x20, 0x0000b614},
	{0x21, 0x0006c000}, {0x22, 0x00000000},
	{0x23, 0x00001558}, {0x24, 0x00000060},
	{0x25, 0x00000483}, {0x26, 0x0004f000},
	{0x27, 0x000ec7d9}, {0x28, 0x00057730},
	{0x29, 0x00004783}, {0x2a, 0x00000001},
	{0x2b, 0x00021334}, {0x2a, 0x00000000},
	{0x2b, 0x00000054}, {0x2a, 0x00000001},
	{0x2b, 0x00000808}, {0x2b, 0x00053333},
	{0x2c, 0x0000000c}, {0x2a, 0x00000002},
	{0x2b, 0x00000808}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000003},
	{0x2b, 0x00000808}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000004},
	{0x2b, 0x00000808}, {0x2b, 0x0006b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000005},
	{0x2b, 0x00000808}, {0x2b, 0x00073333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000006},
	{0x2b, 0x00000709}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000007},
	{0x2b, 0x00000709}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000008},
	{0x2b, 0x0000060a}, {0x2b, 0x0004b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000009},
	{0x2b, 0x0000060a}, {0x2b, 0x00053333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000a},
	{0x2b, 0x0000060a}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000b},
	{0x2b, 0x0000060a}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000c},
	{0x2b, 0x0000060a}, {0x2b, 0x0006b333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000d},
	{0x2b, 0x0000060a}, {0x2b, 0x00073333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000e},
	{0x2b, 0x0000050b}, {0x2b, 0x00066666},
	{0x2c, 0x0000001a}, {0x2a, 0x000e0000},
	{0x10, 0x0004000f}, {0x11, 0x000e31fc},
	{0x10, 0x0006000f}, {0x11, 0x000ff9f8},
	{0x10, 0x0002000f}, {0x11, 0x000203f9},
	{0x10, 0x0003000f}, {0x11, 0x000ff500},
	{0x10, 0x00000000}, {0x11, 0x00000000},
	{0x10, 0x0008000f}, {0x11, 0x0003f100},
	{0x10, 0x0009000f}, {0x11, 0x00023100},
	{0x12, 0x00032000}, {0x12, 0x00071000},
	{0x12, 0x000b0000}, {0x12, 0x000fc000},
	{0x13, 0x000287b3}, {0x13, 0x000244b7},
	{0x13, 0x000204ab}, {0x13, 0x0001c49f},
	{0x13, 0x00018493}, {0x13, 0x0001429b},
	{0x13, 0x00010299}, {0x13, 0x0000c29c},
	{0x13, 0x000081a0}, {0x13, 0x000040ac},
	{0x13, 0x00000020}, {0x14, 0x0001944c},
	{0x14, 0x00059444}, {0x14, 0x0009944c},
	{0x14, 0x000d9444},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x15, 0x0000f424},
	{0x15, 0x0004f424},
	{0x15, 0x0008f424},
	{0x15, 0x000cf424},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x15, 0x0000f474}, {0x15, 0x0004f477},
	{0x15, 0x0008f455}, {0x15, 0x000cf455},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x16, 0x00000339}, {0x16, 0x00040339},
	{0x16, 0x00080339},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x16, 0x000c0356},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x16, 0x000c0366},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x00, 0x00010159}, {0x18, 0x0000f401},
	{0xfe, 0x00000000}, {0xfe, 0x00000000},
	{0x1f, 0x00000003}, {0xfe, 0x00000000},
	{0xfe, 0x00000000}, {0x1e, 0x00000247},
	{0x1f, 0x00000000}, {0x00, 0x00030159},
	{0xff, 0xffffffff}
};

static u8 rtl8723au_read8(struct rtl8xxxu_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u8 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val8, sizeof(u8),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = priv->usb_buf.val8;
	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_READ)
		dev_info(&udev->dev, "%s(%04x)   = 0x%02x, len %i\n",
			 __func__, addr, data, len);
	return data;
}

static u16 rtl8723au_read16(struct rtl8xxxu_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u16 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val16, sizeof(u16),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = le16_to_cpu(priv->usb_buf.val16);
	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_READ)
		dev_info(&udev->dev, "%s(%04x)  = 0x%04x, len %i\n",
			 __func__, addr, data, len);
	return data;
}

static u32 rtl8723au_read32(struct rtl8xxxu_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u32 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val32, sizeof(u32),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = le32_to_cpu(priv->usb_buf.val32);
	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_READ)
		dev_info(&udev->dev, "%s(%04x)  = 0x%08x, len %i\n",
			 __func__, addr, data, len);
	return data;
}

static int rtl8723au_write8(struct rtl8xxxu_priv *priv, u16 addr, u8 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val8 = val;
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val8, sizeof(u8),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_WRITE)
		dev_info(&udev->dev, "%s(%04x) = 0x%02x\n",
			 __func__, addr, val);
	return ret;
}

static int rtl8723au_write16(struct rtl8xxxu_priv *priv, u16 addr, u16 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val16 = cpu_to_le16(val);
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val16, sizeof(u16),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_WRITE)
		dev_info(&udev->dev, "%s(%04x) = 0x%04x\n",
			 __func__, addr, val);
	return ret;
}

static int rtl8723au_write32(struct rtl8xxxu_priv *priv, u16 addr, u32 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val32 = cpu_to_le32(val);
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val32, sizeof(u32),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	mutex_unlock(&priv->usb_buf_mutex);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_WRITE)
		dev_info(&udev->dev, "%s(%04x) = 0x%08x\n",
			 __func__, addr, val);
	return ret;
}

static int
rtl8723au_writeN(struct rtl8xxxu_priv *priv, u16 addr, u8 *buf, u16 len)
{
	struct usb_device *udev = priv->udev;
	int ret;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, buf, len, RTW_USB_CONTROL_MSG_TIMEOUT);

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_REG_WRITE)
		dev_info(&udev->dev, "%s(%04x) = %p, len 0x%02x\n",
			 __func__, addr, buf, len);
	return ret;
}

static u32 rtl8723au_read_rfreg(struct rtl8xxxu_priv *priv, u8 reg)
{
	u32 hssia, val32, retval;

	hssia = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	/*
	 * For path B it seems we should be reading REG_FPGA0_XB_HSSI_PARM1
	 * into val32
	 */
	val32 = hssia;
	val32 &= ~FPGA0_HSSI_PARM2_ADDR_MASK;
	val32 |= (reg << FPGA0_HSSI_PARM2_ADDR_SHIFT) |
		FPGA0_HSSI_PARM2_EDGE_READ;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2,
			  hssia &= ~FPGA0_HSSI_PARM2_EDGE_READ);
	udelay(10);
	/* Here use XB for path B */
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(100);
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2,
			  hssia |= FPGA0_HSSI_PARM2_EDGE_READ);
	udelay(10);
	/* Use XB for path B */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM1);
	if (val32 & BIT(8))	/* RF PI enabled */
		retval = rtl8723au_read32(priv, REG_HSPI_XA_READBACK);
	else
		retval = rtl8723au_read32(priv, REG_FPGA0_XA_LSSI_READBACK);

	retval &= 0xfffff;

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_RFREG_READ)
		dev_info(&priv->udev->dev, "%s(%02x) = 0x%06x\n",
			 __func__, reg, retval);
	return retval;
}

static int rtl8723au_write_rfreg(struct rtl8xxxu_priv *priv, u8 reg, u32 data)
{
	int ret, retval;
	u32 dataaddr;

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_RFREG_WRITE)
		dev_info(&priv->udev->dev, "%s(%02x) = 0x%06x\n",
			 __func__, reg, data);

	data &= FPGA0_LSSI_PARM_DATA_MASK;
	dataaddr = (reg << FPGA0_LSSI_PARM_ADDR_SHIFT) | data;

	/* Use XB for path B */
	ret = rtl8723au_write32(priv, REG_FPGA0_XA_LSSI_PARM, dataaddr);
	if (ret != sizeof(dataaddr))
		retval = -EIO;
	else
		retval = 0;

	udelay(1);

	return retval;
}

static int rtl8723a_h2c_cmd(struct rtl8xxxu_priv *priv, struct h2c_cmd *h2c)
{
	struct device *dev = &priv->udev->dev;
	int mbox_nr, retry, retval = 0;
	int mbox_reg, mbox_ext_reg;
	u8 val8;

	mbox_nr = priv->next_mbox;

	mbox_reg = REG_HMBOX_0 + (mbox_nr * 4);
	mbox_ext_reg = REG_HMBOX_EXT_0 + (mbox_nr * 2);

	mutex_lock(&priv->h2c_mutex);
	/*
	 * MBOX ready?
	 */
	retry = 100;
	do {
		val8 = rtl8723au_read8(priv, REG_HMTFR);
		if (!(val8 & BIT(mbox_nr)))
			break;
	} while (retry--);

	if (!retry) {
		dev_dbg(dev, "%s: Mailbox busy\n", __func__);
		retval = -EBUSY;
		goto error;
	}

	/*
	 * Need to swap as it's being swapped again by rtl8723au_write16/32()
	 */
	if (h2c->cmd.cmd & H2C_EXT) {
		rtl8723au_write16(priv, mbox_ext_reg,
				  le16_to_cpu(h2c->raw.ext));
		if (rtl8xxxu_debug & RTL8XXXU_DEBUG_H2C)
			dev_info(dev, "H2C_EXT %04x\n",
				 le16_to_cpu(h2c->raw.ext));
	}
	rtl8723au_write32(priv, mbox_reg, le32_to_cpu(h2c->raw.data));
	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_H2C)
		dev_info(dev, "H2C %08x\n", le16_to_cpu(h2c->raw.data));

	priv->next_mbox = (mbox_nr + 1) % H2C_MAX_MBOX;

error:
	mutex_unlock(&priv->h2c_mutex);
	return retval;
}

static void rtl8723a_enable_rf(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	u32 val32;

	val8 = rtl8723au_read8(priv, REG_SPS0_CTRL);
	val8 |= BIT(0) | BIT(3);
	rtl8723au_write8(priv, REG_SPS0_CTRL, val8);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_PARM);
	val32 &= ~(BIT(4) | BIT(5));
	val32 |= BIT(3);
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_PARM, val32);

	val32 = rtl8723au_read32(priv, REG_OFDM0_TRX_PATH_ENABLE);
	val32 &= ~(BIT(4) | BIT(5) | BIT(6) | BIT(7));
	val32 |= BIT(4);
	rtl8723au_write32(priv, REG_OFDM0_TRX_PATH_ENABLE, val32);

	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	val32 &= ~FPGA_RF_MODE_JAPAN;
	rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

	rtl8723au_write32(priv, REG_RX_WAIT_CCA, 0x631B25A0);

	rtl8723au_write_rfreg(priv, RF6052_REG_AC, 0x32d95);

#if 0
	rtl8723au_write8(priv, REG_SYS_FUNC, 0xE3);
	rtl8723au_write8(priv, REG_APSD_CTRL, 0x00);
	rtl8723au_write8(priv, REG_SYS_FUNC, 0xE2);
	rtl8723au_write8(priv, REG_SYS_FUNC, 0xE3);
#endif
	rtl8723au_write8(priv, REG_TXPAUSE, 0x00);
}

static void rtl8723a_disable_rf(struct rtl8xxxu_priv *priv)
{
	u8 sps0;
	u32 val32;

	rtl8723au_write8(priv, REG_TXPAUSE, 0xff);

	sps0 = rtl8723au_read8(priv, REG_SPS0_CTRL);

	/* RF RX code for preamble power saving */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_PARM);
	val32 &= ~(BIT(3) | BIT(4) | BIT(5));
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_PARM, val32);

	/* Disable all packet detection for all four paths */
	val32 = rtl8723au_read32(priv, REG_OFDM0_TRX_PATH_ENABLE);
	val32 &= ~(BIT(4) | BIT(5) | BIT(6) | BIT(7));
	rtl8723au_write32(priv, REG_OFDM0_TRX_PATH_ENABLE, val32);

	/* Enable power saving */
	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	val32 |= FPGA_RF_MODE_JAPAN;
	rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

	/* AFE control register to power down bits [30:22] */
	rtl8723au_write32(priv, REG_RX_WAIT_CCA, 0x001b25a0);

	/* Power down RF module */
	rtl8723au_write_rfreg(priv, RF6052_REG_AC, 0);

	sps0 &= ~(BIT(0) | BIT(3));
	rtl8723au_write8(priv, REG_SPS0_CTRL, sps0);
}


static void rtl8723a_stop_tx_beacon(struct rtl8xxxu_priv *priv)
{
	u8 val8;

	val8 = rtl8723au_read8(priv, REG_FWHW_TXQ_CTRL + 2);
	val8 &= ~BIT(6);
	val8 = 0x00; /* FIXME */
	rtl8723au_write8(priv, REG_FWHW_TXQ_CTRL + 2, val8);

	rtl8723au_write8(priv, REG_TBTT_PROHIBIT + 1, 0x64);
	val8 = rtl8723au_read8(priv, REG_TBTT_PROHIBIT + 2);
	val8 &= ~BIT(0);
	val8 = 0x00; /* FIXME */
	rtl8723au_write8(priv, REG_TBTT_PROHIBIT + 2, val8);
}


/*
 * The rtl8723a has 3 channel groups for it's efuse settings. It only
 * supports the 2.4GHz band, so channels 1 - 14:
 *  group 0: channels 1 - 3
 *  group 1: channels 4 - 9
 *  group 2: channels 10 - 14
 *
 * Note: We index from 0 in the code
 */
static int rtl8723a_channel_to_group(int channel)
{
	int group;

	if (channel < 4)
		group = 0;
	else if (channel < 10)
		group = 1;
	else
		group = 2;

	return group;
}

static void rtl8723au_config_channel(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	u32 val32, rsr;
	u8 val8, opmode;
	bool ht = true;
	int sec_ch_above;

	val32 = rtl8723au_read_rfreg(priv, RF6052_REG_MODE_AG);
	val32 &= ~MODE_AG_CHANNEL_MASK;
	val32 |= hw->conf.chandef.chan->hw_value;
	rtl8723au_write_rfreg(priv, RF6052_REG_MODE_AG, val32);

	opmode = rtl8723au_read8(priv, REG_BW_OPMODE);
	rsr = rtl8723au_read32(priv, REG_RESPONSE_RATE_SET);

	switch (hw->conf.chandef.width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
		ht = false;
	case NL80211_CHAN_WIDTH_20:
		opmode |= BW_OPMODE_20MHZ;
		rtl8723au_write8(priv, REG_BW_OPMODE, opmode);

		val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
		val32 &= ~FPGA_RF_MODE;
		rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

		val32 = rtl8723au_read32(priv, REG_FPGA1_RF_MODE);
		val32 &= ~FPGA_RF_MODE;
		rtl8723au_write32(priv, REG_FPGA1_RF_MODE, val32);

		val32 = rtl8723au_read32(priv, REG_FPGA0_ANALOG2);
		val32 |= BIT(10);
		rtl8723au_write32(priv, REG_FPGA0_ANALOG2, val32);
		break;
	case NL80211_CHAN_WIDTH_40:
		if (hw->conf.chandef.center_freq1 >
		    hw->conf.chandef.chan->center_freq)
			sec_ch_above = 1;
		else
			sec_ch_above = 0;

		opmode &= ~BW_OPMODE_20MHZ;
		rtl8723au_write8(priv, REG_BW_OPMODE, opmode);
		rsr &= ~RSR_RSC_BANDWIDTH_40M;
		if (sec_ch_above)
			rsr |= RSR_RSC_UPPER_SUB_CHANNEL;
		else
			rsr |= RSR_RSC_LOWER_SUB_CHANNEL;
		rtl8723au_write32(priv, REG_RESPONSE_RATE_SET, rsr);

		val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
		val32 |= FPGA_RF_MODE;
		rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

		val32 = rtl8723au_read32(priv, REG_FPGA1_RF_MODE);
		val32 |= FPGA_RF_MODE;
		rtl8723au_write32(priv, REG_FPGA1_RF_MODE, val32);

		/*
		 * Set Control channel to upper or lower. These settings
		 * are required only for 40MHz
		 */
		val32 = rtl8723au_read32(priv, REG_CCK0_SYSTEM);
		val32 &= ~CCK0_SIDEBAND;
		if (!sec_ch_above)
			val32 |= CCK0_SIDEBAND;
		rtl8723au_write32(priv, REG_CCK0_SYSTEM, val32);

		val32 = rtl8723au_read32(priv, REG_OFDM1_LSTF);
		val32 &= ~(BIT(10) | BIT(11)); /* 0xc00 */
		if (sec_ch_above)
			val32 |= BIT(10);
		else
			val32 |= BIT(11);
		rtl8723au_write32(priv, REG_OFDM1_LSTF, val32);

		val32 = rtl8723au_read32(priv, REG_FPGA0_ANALOG2);
		val32 &= ~BIT(10);
		rtl8723au_write32(priv, REG_FPGA0_ANALOG2, val32);

		val32 = rtl8723au_read32(priv, REG_FPGA0_POWER_SAVE);
		val32 &= ~(FPGA0_PS_LOWER_CHANNEL | FPGA0_PS_UPPER_CHANNEL);
		if (sec_ch_above)
			val32 |= FPGA0_PS_UPPER_CHANNEL;
		else
			val32 |= FPGA0_PS_LOWER_CHANNEL;
		rtl8723au_write32(priv, REG_FPGA0_POWER_SAVE, val32);
		break;

	default:
		break;
	}

	if (ht)
		val8 = 0x0e;
	else
		val8 = 0x0a;

	rtl8723au_write8(priv, REG_SIFS_CCK + 1, val8);
	rtl8723au_write8(priv, REG_SIFS_OFDM + 1, val8);

	rtl8723au_write16(priv, REG_R2T_SIFS, 0x0808);
	rtl8723au_write16(priv, REG_T2T_SIFS, 0x0a0a);

	val32 = rtl8723au_read_rfreg(priv, RF6052_REG_MODE_AG);
	if (hw->conf.chandef.width == NL80211_CHAN_WIDTH_40)
		val32 &= ~MODE_AG_CHANNEL_20MHZ;
	else
		val32 |= MODE_AG_CHANNEL_20MHZ;
	rtl8723au_write_rfreg(priv, RF6052_REG_MODE_AG, val32);
}

static void
rtl8723a_set_tx_power(struct rtl8xxxu_priv *priv, int channel, bool ht40)
{
	struct rtl8723au_efuse *efuse;
	u8 cck[RTL8723A_MAX_RF_PATHS], ofdm[RTL8723A_MAX_RF_PATHS];
	u8 ofdmbase[RTL8723A_MAX_RF_PATHS], mcsbase[RTL8723A_MAX_RF_PATHS];
	u32 val32, ofdm_a, ofdm_b, mcs_a, mcs_b;
	u8 val8;
	int group, i;

	efuse = &priv->efuse_wifi.efuse;

	group = rtl8723a_channel_to_group(channel);

	cck[0] = efuse->cck_tx_power_index_A[group];
	ofdm[0] = efuse->ht40_1s_tx_power_index_A[group];

	if (priv->rf_paths > 1) {
		cck[1] = efuse->cck_tx_power_index_B[group];
		ofdm[1] = efuse->ht40_1s_tx_power_index_B[group];
	} else {
		cck[1] = 0;
		ofdm[1] = 0;
	}

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_CHANNEL)
		dev_info(&priv->udev->dev,
			 "%s: Setting TX power CCK A: %02x, "
			 "CCK B: %02x, OFDM A: %02x, OFDM B: %02x\n",
			 __func__, cck[0], cck[1], ofdm[0], ofdm[1]);

	for (i = 0; i < RTL8723A_MAX_RF_PATHS; i++) {
		if (cck[i] > RF6052_MAX_TX_PWR)
			cck[i] = RF6052_MAX_TX_PWR;
		if (ofdm[i] > RF6052_MAX_TX_PWR)
			ofdm[i] = RF6052_MAX_TX_PWR;
	}

	val32 = rtl8723au_read32(priv, REG_TX_AGC_A_CCK1_MCS32);
	val32 &= 0xffff00ff;
	val32 |= (cck[0] << 8);
	rtl8723au_write32(priv, REG_TX_AGC_A_CCK1_MCS32, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11);
	val32 &= 0xff;
	val32 |= ((cck[0] << 8) | (cck[0] << 16) | (cck[0] << 24));
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11);
	val32 &= 0xffffff00;
	val32 |= cck[1];
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK1_55_MCS32);
	val32 &= 0xff;
	val32 |= ((cck[1] << 8) | (cck[1] << 16) | (cck[1] << 24));
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK1_55_MCS32, val32);

	ofdmbase[0] = ofdm[0] +	efuse->ofdm_tx_power_index_diff[group].a;
	mcsbase[0] = ofdm[0];
	if (!ht40)
		mcsbase[0] += efuse->ht20_tx_power_index_diff[group].a;

	ofdmbase[1] = ofdm[1] +	efuse->ofdm_tx_power_index_diff[group].b;
	mcsbase[1] = ofdm[1];
	if (!ht40)
		mcsbase[1] += efuse->ht20_tx_power_index_diff[group].b;

	ofdm_a = ofdmbase[0] | ofdmbase[0] << 8 |
		ofdmbase[0] << 16 | ofdmbase[0] << 24;
	ofdm_b = ofdmbase[1] | ofdmbase[1] << 8 |
		ofdmbase[1] << 16 | ofdmbase[1] << 24;
	rtl8723au_write32(priv, REG_TX_AGC_A_RATE18_06, ofdm_a);
	rtl8723au_write32(priv, REG_TX_AGC_B_RATE18_06, ofdm_b);

	rtl8723au_write32(priv, REG_TX_AGC_A_RATE54_24, ofdm_a);
	rtl8723au_write32(priv, REG_TX_AGC_B_RATE54_24, ofdm_b);

	mcs_a = mcsbase[0] | mcsbase[0] << 8 |
		mcsbase[0] << 16 | mcsbase[0] << 24;
	mcs_b = mcsbase[1] | mcsbase[1] << 8 |
		mcsbase[1] << 16 | mcsbase[1] << 24;

	rtl8723au_write32(priv, REG_TX_AGC_A_MCS03_MCS00, mcs_a);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS03_MCS00, mcs_b);

	rtl8723au_write32(priv, REG_TX_AGC_A_MCS07_MCS04, mcs_a);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS07_MCS04, mcs_b);

	rtl8723au_write32(priv, REG_TX_AGC_A_MCS11_MCS08, mcs_a);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS11_MCS08, mcs_b);

	rtl8723au_write32(priv, REG_TX_AGC_A_MCS15_MCS12, mcs_a);
	for (i = 0; i < 3; i++) {
		if (i != 2)
			val8 = (mcsbase[0] > 8) ? (mcsbase[0] - 8) : 0;
		else
			val8 = (mcsbase[0] > 6) ? (mcsbase[0] - 6) : 0;
		rtl8723au_write8(priv, REG_OFDM0_XC_TX_IQ_IMBALANCE + i, val8);
	}
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS15_MCS12, mcs_b);
	for (i = 0; i < 3; i++) {
		if (i != 2)
			val8 = (mcsbase[1] > 8) ? (mcsbase[1] - 8) : 0;
		else
			val8 = (mcsbase[1] > 6) ? (mcsbase[1] - 6) : 0;
		rtl8723au_write8(priv, REG_OFDM0_XD_TX_IQ_IMBALANCE + i, val8);
	}
}

static void rtl8xxxu_set_linktype(struct rtl8xxxu_priv *priv,
				  enum nl80211_iftype linktype)
{
	u16 val8;

	val8 = rtl8723au_read16(priv, REG_MSR);
	val8 &= ~MSR_LINKTYPE_MASK;

	switch (linktype) {
	case NL80211_IFTYPE_UNSPECIFIED:
		val8 |= MSR_LINKTYPE_NONE;
		break;
	case NL80211_IFTYPE_ADHOC:
		val8 |= MSR_LINKTYPE_ADHOC;
		break;
	case NL80211_IFTYPE_STATION:
		val8 |= MSR_LINKTYPE_STATION;
		break;
	case NL80211_IFTYPE_AP:
		val8 |= MSR_LINKTYPE_AP;
		break;
	default:
		goto out;
	}

	rtl8723au_write8(priv, REG_MSR, val8);
out:
	return;
}

static void
rtl8xxxu_set_retry(struct rtl8xxxu_priv *priv, u16 short_retry, u16 long_retry)
{
	u16 val16;

	val16 = ((short_retry << RETRY_LIMIT_SHORT_SHIFT) &
		 RETRY_LIMIT_SHORT_MASK) |
		((long_retry << RETRY_LIMIT_LONG_SHIFT) &
		 RETRY_LIMIT_LONG_MASK);

	rtl8723au_write16(priv, REG_RETRY_LIMIT, val16);
}

static void
rtl8xxxu_set_spec_sifs(struct rtl8xxxu_priv *priv, u16 cck, u16 ofdm)
{
	u16 val16;

	val16 = ((cck << SPEC_SIFS_CCK_SHIFT) & SPEC_SIFS_CCK_MASK) |
		((ofdm << SPEC_SIFS_OFDM_SHIFT) & SPEC_SIFS_OFDM_MASK);

	rtl8723au_write16(priv, REG_SPEC_SIFS, val16);
}

static void rtl8xxxu_8723au_identify_chip(struct rtl8xxxu_priv *priv)
{
	struct device *dev = &priv->udev->dev;
	u32 val32;
	u16 val16;
	char *cut;

	val32 = rtl8723au_read32(priv, REG_SYS_CFG);
	priv->chip_cut = (val32 & SYS_CFG_CHIP_VERSION_MASK) >>
		SYS_CFG_CHIP_VERSION_SHIFT;
	switch (priv->chip_cut) {
	case 0:
		cut = "A";
		break;
	case 1:
		cut = "B";
		break;
	default:
		cut = "unknown";
	}

	val32 = rtl8723au_read32(priv, REG_GPIO_OUTSTS);
	priv->rom_rev = (val32 & GPIO_RF_RL_ID) >> 28;

	val32 = rtl8723au_read32(priv, REG_MULTI_FUNC_CTRL);
	if (val32 & MULTI_WIFI_FUNC_EN)
		priv->has_wifi = 1;
	if (val32 & MULTI_BT_FUNC_EN)
		priv->has_bluetooth = 1;
	if (val32 & MULTI_GPS_FUNC_EN)
		priv->has_gps = 1;

	if (val32 & SYS_CFG_VENDOR_ID)
		priv->vendor_umc = 1;

	/* The rtl8192 presumably can have 2 */
	priv->rf_paths = 1;

	val16 = rtl8723au_read16(priv, REG_NORMAL_SIE_EP_TX);
	if (val16 & NORMAL_SIE_EP_TX_HIGH_MASK) {
		priv->ep_tx_high_queue = 1;
		priv->ep_tx_count++;
	}

	if (val16 & NORMAL_SIE_EP_TX_NORMAL_MASK) {
		priv->ep_tx_normal_queue = 1;
		priv->ep_tx_count++;
	}

	if (val16 & NORMAL_SIE_EP_TX_LOW_MASK) {
		priv->ep_tx_low_queue = 1;
		priv->ep_tx_count++;
	}

	dev_info(dev, "RTL8723au rev %s, features: WiFi=%i, BT=%i, GPS=%i\n",
		 cut, priv->has_wifi, priv->has_bluetooth, priv->has_gps);

	dev_info(dev, "%s: RTL8723au number of TX queues: %i\n",
		 __func__, priv->ep_tx_count);
}

static int
rtl8xxxu_read_efuse8(struct rtl8xxxu_priv *priv, u16 offset, u8 *data)
{
	int i;
	u8 val8;
	u32 val32;

	/* Write Address */
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 1, offset & 0xff);
	val8 = rtl8723au_read8(priv, REG_EFUSE_CTRL + 2);
	val8 &= 0xfc;
	val8 |= (offset >> 8) & 0x03;
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 2, val8);

	val8 = rtl8723au_read8(priv, REG_EFUSE_CTRL + 3);
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 3, val8 & 0x7f);

	/* Poll for data read */
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
	for (i = 0; i < RTL8XXXU_MAX_REG_POLL; i++) {
		val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
		if (val32 & BIT(31))
			break;
	}

	if (i == RTL8XXXU_MAX_REG_POLL)
		return -EIO;

	udelay(50);
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);

	*data = val32 & 0xff;
	return 0;
}

static int rtl8xxxu_read_efuse(struct rtl8xxxu_priv *priv)
{
	struct device *dev = &priv->udev->dev;
	int i, ret = 0;
	u8 val8, word_mask, header, extheader;
	u16 val16, efuse_addr, offset;
	u32 val32;

	val16 = rtl8723au_read16(priv, REG_9346CR);
	if (val16 & EEPROM_ENABLE)
		priv->has_eeprom = 1;
	if (val16 & EEPROM_BOOT)
		priv->boot_eeprom = 1;

	val32 = rtl8723au_read32(priv, REG_EFUSE_TEST);
	val32 = (val32 & ~EFUSE_SELECT_MASK) | EFUSE_WIFI_SELECT;
	rtl8723au_write32(priv, REG_EFUSE_TEST, val32);

	dev_dbg(dev, "Booting from %s\n",
		priv->boot_eeprom ? "EEPROM" : "EFUSE");

	rtl8723au_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_ENABLE);

	/*  1.2V Power: From VDDON with Power Cut(0x0000[15]), default valid */
	val16 = rtl8723au_read16(priv, REG_SYS_ISO_CTRL);
	if (!(val16 & SYS_ISO_PWC_EV12V)) {
		val16 |= SYS_ISO_PWC_EV12V;
		rtl8723au_write16(priv, REG_SYS_ISO_CTRL, val16);
	}
	/*  Reset: 0x0000[28], default valid */
	val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
	if (!(val16 & SYS_FUNC_ELDR)) {
		val16 |= SYS_FUNC_ELDR;
		rtl8723au_write16(priv, REG_SYS_FUNC, val16);
	}

	/*
	 * Clock: Gated(0x0008[5]) 8M(0x0008[1]) clock from ANA, default valid
	 */
	val16 = rtl8723au_read16(priv, REG_SYS_CLKR);
	if (!(val16 & SYS_CLK_LOADER_ENABLE) || !(val16 & SYS_CLK_ANA8M)) {
		val16 |= (SYS_CLK_LOADER_ENABLE | SYS_CLK_ANA8M);
		rtl8723au_write16(priv, REG_SYS_CLKR, val16);
	}

	/* Default value is 0xff */
	memset(priv->efuse_wifi.raw, 0xff, EFUSE_MAP_LEN_8723A);

	efuse_addr = 0;
	while (efuse_addr < EFUSE_REAL_CONTENT_LEN_8723A) {
		ret = rtl8xxxu_read_efuse8(priv, efuse_addr++, &header);
		if (ret || header == 0xff)
			goto exit;

		if ((header & 0x1f) == 0x0f) {	/* extended header */
			offset = (header & 0xe0) >> 5;

			ret = rtl8xxxu_read_efuse8(priv, efuse_addr++,
						   &extheader);
			if (ret)
				goto exit;
			/* All words disabled */
			if ((extheader & 0x0f) == 0x0f)
				continue;

			offset |= ((extheader & 0xf0) >> 1);
			word_mask = extheader & 0x0f;
		} else {
			offset = (header >> 4) & 0x0f;
			word_mask = header & 0x0f;
		}

		if (offset < EFUSE_MAX_SECTION_8723A) {
			u16 map_addr;
			/* Get word enable value from PG header */

			/* We have 8 bits to indicate validity */
			map_addr = offset * 8;
			if (map_addr >= EFUSE_MAP_LEN_8723A) {
				dev_warn(dev, "%s: Illegal map_addr (%04x), "
					 "efuse corrupt!\n",
					 __func__, map_addr);
				ret = -EINVAL;
				goto exit;
			}
			for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
				/* Check word enable condition in the section */
				if (!(word_mask & BIT(i))) {
					ret = rtl8xxxu_read_efuse8(priv,
								   efuse_addr++,
								   &val8);
					priv->efuse_wifi.raw[map_addr++] = val8;

					ret = rtl8xxxu_read_efuse8(priv,
								   efuse_addr++,
								   &val8);
					priv->efuse_wifi.raw[map_addr++] = val8;
				} else
					map_addr += 2;
			}
		} else {
			dev_warn(dev,
				 "%s: Illegal offset (%04x), efuse corrupt!\n",
				 __func__, offset);
			ret = -EINVAL;
			goto exit;
		}
	}

exit:
	rtl8723au_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_DISABLE);

	if (priv->efuse_wifi.efuse.rtl_id != cpu_to_le16(0x8129))
		ret = EINVAL;

	return ret;
}

static int rtl8xxxu_start_firmware(struct rtl8xxxu_priv *priv)
{
	struct device *dev = &priv->udev->dev;
	int ret = 0, i;
	u32 val32;

	/* Poll checksum report */
	for (i = 0; i < RTL8XXXU_FIRMWARE_POLL_MAX; i++) {
		val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
		if (val32 & MCU_FW_DL_CSUM_REPORT)
			break;
	}

	if (i == RTL8XXXU_FIRMWARE_POLL_MAX) {
		dev_warn(dev, "Firmware checksum poll timed out\n");
		ret = -EAGAIN;
		goto exit;
	}

	val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
	val32 |= MCU_FW_DL_READY;
	val32 &= ~MCU_WINT_INIT_READY;
	rtl8723au_write32(priv, REG_MCU_FW_DL, val32);

	/* Wait for firmware to become ready */
	for (i = 0; i < RTL8XXXU_FIRMWARE_POLL_MAX; i++) {
		val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
		if (val32 & MCU_WINT_INIT_READY)
			break;

		udelay(100);
	}

	if (i == RTL8XXXU_FIRMWARE_POLL_MAX) {
		dev_warn(dev, "Firmware failed to start\n");
		ret = -EAGAIN;
		goto exit;
	}

exit:
	return ret;
}

static int rtl8xxxu_download_firmware(struct rtl8xxxu_priv *priv)
{
	int pages, remainder, i, ret;
	u8 val8;
	u16 val16;
	u32 val32;
	u8 *fwptr;

	val8 = rtl8723au_read8(priv, REG_SYS_FUNC + 1);
	val8 |= 4;
	rtl8723au_write8(priv, REG_SYS_FUNC + 1, val8);

	/* 8051 enable */
	val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
	rtl8723au_write16(priv, REG_SYS_FUNC, val16 | SYS_FUNC_CPU_ENABLE);

	/* MCU firmware download enable */
	val8 = rtl8723au_read8(priv, REG_MCU_FW_DL);
	rtl8723au_write8(priv, REG_MCU_FW_DL, val8 | MCU_FW_DL_ENABLE);

	/* 8051 reset */
	val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
	rtl8723au_write32(priv, REG_MCU_FW_DL, val32 & ~BIT(19));

	/* Reset firmware download checksum */
	val8 = rtl8723au_read8(priv, REG_MCU_FW_DL);
	rtl8723au_write8(priv, REG_MCU_FW_DL, val8 | MCU_FW_DL_CSUM_REPORT);

	pages = priv->fw_size / RTL_FW_PAGE_SIZE;
	remainder = priv->fw_size % RTL_FW_PAGE_SIZE;

	fwptr = priv->fw_data->data;

	for (i = 0; i < pages; i++) {
		val8 = rtl8723au_read8(priv, REG_MCU_FW_DL + 2) & 0xF8;
		rtl8723au_write8(priv, REG_MCU_FW_DL + 2, val8 | i);

		ret = rtl8723au_writeN(priv, REG_8723A_FW_START_ADDRESS,
				       fwptr, RTL_FW_PAGE_SIZE);
		if (ret != RTL_FW_PAGE_SIZE) {
			ret = -EAGAIN;
			goto fw_abort;
		}

		fwptr += RTL_FW_PAGE_SIZE;
	}

	if (remainder) {
		val8 = rtl8723au_read8(priv, REG_MCU_FW_DL + 2) & 0xF8;
		rtl8723au_write8(priv, REG_MCU_FW_DL + 2, val8 | i);
		ret = rtl8723au_writeN(priv, REG_8723A_FW_START_ADDRESS,
				       fwptr, remainder);
		if (ret != remainder) {
			ret = -EAGAIN;
			goto fw_abort;
		}
	}

	ret = 0;
fw_abort:
	/* MCU firmware download disable */
	val16 = rtl8723au_read16(priv, REG_MCU_FW_DL);
	rtl8723au_write16(priv, REG_MCU_FW_DL,
			  val16 & (~MCU_FW_DL_ENABLE & 0xff));

	return ret;
}

static int rtl8xxxu_load_firmware(struct rtl8xxxu_priv *priv)
{
	struct device *dev = &priv->udev->dev;
	const struct firmware *fw;
	char *fw_name;
	int ret = 0;
	u16 signature;

	switch (priv->chip_cut) {
	case 0:
		fw_name = "rtlwifi/rtl8723aufw_A.bin";
		break;
	case 1:
		if (priv->enable_bluetooth)
			fw_name = "rtlwifi/rtl8723aufw_B.bin";
		else
			fw_name = "rtlwifi/rtl8723aufw_B_NoBT.bin";

		break;
	default:
		return -EINVAL;
	}

	dev_info(dev, "%s: Loading firmware %s\n", DRIVER_NAME, fw_name);
	if (request_firmware(&fw, fw_name, &priv->udev->dev)) {
		dev_warn(dev, "request_firmware(%s) failed\n", fw_name);
		ret = -EAGAIN;
		goto exit;
	}
	if (!fw) {
		dev_warn(dev, "Firmware data not available\n");
		ret = -EINVAL;
		goto exit;
	}

	priv->fw_data = kmemdup(fw->data, fw->size, GFP_KERNEL);
	priv->fw_size = fw->size - sizeof(struct rtl8xxxu_firmware_header);

	signature = le16_to_cpu(priv->fw_data->signature);
	switch (signature & 0xfff0) {
	case 0x92c0:
	case 0x88c0:
	case 0x2300:
		break;
	default:
		ret = -EINVAL;
		dev_warn(dev, "%s: Invalid firmware signature: 0x%04x\n",
			 __func__, signature);
	}

	dev_info(dev, "Firmware revision %i.%i (signature 0x%04x)\n",
		 le16_to_cpu(priv->fw_data->major_version),
		 priv->fw_data->minor_version, signature);

exit:
	release_firmware(fw);
	return ret;
}

static void rtl8xxxu_firmware_self_reset(struct rtl8xxxu_priv *priv)
{
	u16 val16;
	int i = 100;

	/* Inform 8051 to perform reset */
	rtl8723au_write8(priv, REG_HMTFR + 3, 0x20);

	for (i = 100; i > 0; i--) {
		val16 = rtl8723au_read16(priv, REG_SYS_FUNC);

		if (!(val16 & SYS_FUNC_CPU_ENABLE)) {
			dev_dbg(&priv->udev->dev,
				"%s: Firmware self reset success!\n", __func__);
			break;
		}
		udelay(50);
	}

	if (!i) {
		/* Force firmware reset */
		val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
		val16 &= ~SYS_FUNC_CPU_ENABLE;
		rtl8723au_write16(priv, REG_SYS_FUNC, val16);
	}
}

static int
rtl8xxxu_init_mac(struct rtl8xxxu_priv *priv, struct rtl8xxxu_reg8val *array)
{
	int i, ret;
	u16 reg;
	u8 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xffff && val == 0xff)
			break;

		ret = rtl8723au_write8(priv, reg, val);
		if (ret != 1) {
			dev_warn(&priv->udev->dev,
				 "Failed to initialize MAC\n");
			return -EAGAIN;
		}
	}

	rtl8723au_write8(priv, REG_MAX_AGGR_NUM, 0x0a);

	return 0;
}

static int rtl8xxxu_init_phy_regs(struct rtl8xxxu_priv *priv,
				  struct rtl8xxxu_reg32val *array)
{
	int i, ret;
	u16 reg;
	u32 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xffff && val == 0xffffffff)
			break;

		ret = rtl8723au_write32(priv, reg, val);
		if (ret != sizeof(val)) {
			dev_warn(&priv->udev->dev,
				 "Failed to initialize PHY\n");
			return -EAGAIN;
		}
		udelay(1);
	}

	return 0;
}

/*
 * Most of this is black magic retrieved from the old rtl8723au driver
 */
static int rtl8xxxu_init_phy_bb(struct rtl8xxxu_priv *priv)
{
	u8 val8, ldoa15, ldov12d, lpldo, ldohci12;
	u32 val32;

	/*
	 * Todo: The vendor driver maintains a table of PHY register
	 *       addresses, which is initialized here. Do we need this?
	 */

	val8 = rtl8723au_read8(priv, REG_AFE_PLL_CTRL);
	udelay(2);
	val8 |= AFE_PLL_320_ENABLE;
	rtl8723au_write8(priv, REG_AFE_PLL_CTRL, val8);
	udelay(2);

	rtl8723au_write8(priv, REG_AFE_PLL_CTRL + 1, 0xff);
	udelay(2);

	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 |= SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB;
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	/* AFE_XTAL_RF_GATE (bit 14) if addressing as 32 bit register */
	val8 = rtl8723au_read8(priv, REG_AFE_XTAL_CTRL + 1);
	val8 &= ~BIT(6);
	rtl8723au_write8(priv, REG_AFE_XTAL_CTRL + 1, val8);

	/* AFE_XTAL_BT_GATE (bit 20) if addressing as 32 bit register */
	val8 = rtl8723au_read8(priv, REG_AFE_XTAL_CTRL + 2);
	val8 &= ~BIT(4);
	rtl8723au_write8(priv, REG_AFE_XTAL_CTRL + 2, val8);

	/* 6. 0x1f[7:0] = 0x07 */
	val8 = RF_ENABLE | RF_RSTB | RF_SDMRSTB;
	rtl8723au_write8(priv, REG_RF_CTRL, val8);

	rtl8xxxu_init_phy_regs(priv, rtl8723a_phy_1t_init_table);

	rtl8xxxu_init_phy_regs(priv, rtl8723a_agc_1t_init_table);
	if (priv->efuse_wifi.efuse.version >= 0x01) {
		val32 = rtl8723au_read32(priv, REG_MAC_PHY_CTRL);

		val8 = priv->efuse_wifi.efuse.xtal_k & 0x3f;
		val32 &= 0xff000fff;
		val32 |= ((val8 | (val8 << 6)) << 12);

		rtl8723au_write32(priv, REG_MAC_PHY_CTRL, val32);
	}

	ldoa15 = LDOA15_ENABLE | LDOA15_OBUF;
	ldov12d = LDOV12D_ENABLE | BIT(2) | (2 << LDOV12D_VADJ_SHIFT);
	ldohci12 = 0x57;
	lpldo = 1;
	val32 = (lpldo << 24) | (ldohci12 << 16) | (ldov12d << 8) | ldoa15;

	rtl8723au_write32(priv, REG_LDOA15_CTRL, val32);

	return 0;
}

static int rtl8xxxu_init_rf_regs(struct rtl8xxxu_priv *priv,
				 struct rtl8xxxu_rfregval *array)
{
	int i, ret;
	u8 reg;
	u32 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xff && val == 0xffffffff)
			break;

		switch (reg) {
		case 0xfe:
			msleep(50);
			continue;
		case 0xfd:
			mdelay(5);
			continue;
		case 0xfc:
			mdelay(1);
			continue;
		case 0xfb:
			udelay(50);
			continue;
		case 0xfa:
			udelay(5);
			continue;
		case 0xf9:
			udelay(1);
			continue;
		}

		reg &= 0x3f;

		ret = rtl8723au_write_rfreg(priv, reg, val);
		if (ret) {
			dev_warn(&priv->udev->dev,
				 "Failed to initialize RF\n");
			return -EAGAIN;
		}
		udelay(1);
	}

	return 0;
}

static int rtl8xxxu_init_phy_rf(struct rtl8xxxu_priv *priv)
{
	u32 val32;
	u16 val16, rfsi_rfenv;

	/* For path B, use XB */
	rfsi_rfenv = rtl8723au_read16(priv, REG_FPGA0_XA_RF_SW_CTRL);
	rfsi_rfenv &= FPGA0_RF_RFENV;

	/*
	 * These two we might be able to optimize into one
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 |= BIT(20);	/* 0x10 << 16 */
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
	udelay(1);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 |= BIT(4);
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
	udelay(1);

	/*
	 * These two we might be able to optimize into one
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	val32 &= ~FPGA0_HSSI_3WIRE_ADDR_LEN;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(1);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	val32 &= ~FPGA0_HSSI_3WIRE_DATA_LEN;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(1);

	rtl8xxxu_init_rf_regs(priv, rtl8723au_radioa_rf6052_1t_init_table);

	/* For path B, use XB */
	val16 = rtl8723au_read16(priv, REG_FPGA0_XA_RF_SW_CTRL);
	val16 &= ~FPGA0_RF_RFENV;
	val16 |= rfsi_rfenv;
	rtl8723au_write16(priv, REG_FPGA0_XA_RF_SW_CTRL, val16);

	return 0;
}

static int rtl8xxxu_llt_write(struct rtl8xxxu_priv *priv, u8 address, u8 data)
{
	int ret = -EBUSY;
	int count = 0;
	u32 value;

	value = LLT_OP_WRITE | address << 8 | data;

	rtl8723au_write32(priv, REG_LLT_INIT, value);

	do {
		value = rtl8723au_read32(priv, REG_LLT_INIT);
		if ((value & LLT_OP_MASK) == LLT_OP_INACTIVE) {
			ret = 0;
			break;
		}
	} while (count++ < 20);

	return ret;
}

static int rtl8xxxu_init_llt_table(struct rtl8xxxu_priv *priv, u8 last_tx_page)
{
	int ret;
	int i;

	for (i = 0; i < last_tx_page; i++) {
		ret = rtl8xxxu_llt_write(priv, i, i + 1);
		if (ret)
			goto exit;
	}

	ret = rtl8xxxu_llt_write(priv, last_tx_page, 0xff);
	if (ret)
		goto exit;

	/* Mark remaining pages as a ring buffer */
	for (i = last_tx_page + 1; i < 0xff; i++) {
		ret = rtl8xxxu_llt_write(priv, i, (i + 1));
		if (ret)
			goto exit;
	}

	/*  Let last entry point to the start entry of ring buffer */
	ret = rtl8xxxu_llt_write(priv, 0xff, last_tx_page + 1);
	if (ret)
		goto exit;

exit:
	return ret;
}

static int rtl8xxxu_init_queue_priority(struct rtl8xxxu_priv *priv)
{
	u16 val16, hi, lo;
	u16 hiq, mgq, bkq, beq, viq, voq;
	int hip, mgp, bkp, bep, vip, vop;
	int ret = 0;

	switch (priv->ep_tx_count) {
	case 1:
		if (priv->ep_tx_high_queue) {
			hi = TRXDMA_QUEUE_HIGH;
		} else if (priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_normal_queue) {
			hi = TRXDMA_QUEUE_NORMAL;
		} else {
			hi = 0;
			ret = -EINVAL;
		}

		hiq = hi;
		mgq = hi;
		bkq = hi;
		beq = hi;
		viq = hi;
		voq = hi;

		hip = 0;
		mgp = 0;
		bkp = 0;
		bep = 0;
		vip = 0;
		vop = 0;
		break;
	case 2:
		if (priv->ep_tx_high_queue && priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_HIGH;
			lo = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_normal_queue && priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_NORMAL;
			lo = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_high_queue && priv->ep_tx_normal_queue) {
			hi = TRXDMA_QUEUE_HIGH;
			lo = TRXDMA_QUEUE_NORMAL;
		} else {
			ret = -EINVAL;
			hi = 0;
			lo = 0;
		}

		hiq = hi;
		mgq = hi;
		bkq = lo;
		beq = lo;
		viq = hi;
		voq = hi;

		hip = 0;
		mgp = 0;
		bkp = 1;
		bep = 1;
		vip = 0;
		vop = 0;
		break;
	case 3:
		beq = TRXDMA_QUEUE_LOW;
		bkq = TRXDMA_QUEUE_LOW;
		viq = TRXDMA_QUEUE_NORMAL;
		voq = TRXDMA_QUEUE_HIGH;
		mgq = TRXDMA_QUEUE_HIGH;
		hiq = TRXDMA_QUEUE_HIGH;

		hip = hiq ^ 3;
		mgp = mgq ^ 3;
		bkp = bkq ^ 3;
		bep = beq ^ 3;
		vip = viq ^ 3;
		vop = viq ^ 3;
		break;
	default:
		ret = -EINVAL;
	}

	/*
	 * None of the vendor drivers are configuring the beacon
	 * queue here .... why?
	 */
	if (!ret) {
		val16 = rtl8723au_read16(priv, REG_TRXDMA_CTRL);
		val16 &= 0x7;
		val16 |= (voq << TRXDMA_CTRL_VOQ_SHIFT) |
			(viq << TRXDMA_CTRL_VIQ_SHIFT) |
			(beq << TRXDMA_CTRL_BEQ_SHIFT) |
			(bkq << TRXDMA_CTRL_BKQ_SHIFT) |
			(mgq << TRXDMA_CTRL_MGQ_SHIFT) |
			(hiq << TRXDMA_CTRL_HIQ_SHIFT);
		rtl8723au_write16(priv, REG_TRXDMA_CTRL, val16);

		priv->pipe_out[TXDESC_QUEUE_VO] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[vop]);
		priv->pipe_out[TXDESC_QUEUE_VI] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[vip]);
		priv->pipe_out[TXDESC_QUEUE_BE] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[bep]);
		priv->pipe_out[TXDESC_QUEUE_BK] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[bkp]);
		priv->pipe_out[TXDESC_QUEUE_BEACON] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[0]);
		priv->pipe_out[TXDESC_QUEUE_MGNT] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[mgp]);
		priv->pipe_out[TXDESC_QUEUE_HIGH] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[hip]);
		priv->pipe_out[TXDESC_QUEUE_CMD] =
			usb_sndbulkpipe(priv->udev, priv->out_ep[0]);
	}

	return ret;
}

static void rtl8xxxu_fill_iqk_matrix_a(struct rtl8xxxu_priv *priv,
				       bool iqk_ok, int result[][8],
				       int candidate, bool tx_only)
{
	u32 oldval, x, tx0_a, reg;
	int y, tx0_c;
	u32 val32;

	if (!iqk_ok)
		return;

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_TX_IQ_IMBALANCE);
	oldval = val32 >> 22;

	x = result[candidate][0];
	if ((x & 0x00000200) != 0)
		x = x | 0xfffffc00;
	tx0_a = (x * oldval) >> 8;

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_TX_IQ_IMBALANCE);
	val32 &= ~0x3ff;
	val32 |= tx0_a;
	rtl8723au_write32(priv, REG_OFDM0_XA_TX_IQ_IMBALANCE, val32);

	val32 = rtl8723au_read32(priv, REG_OFDM0_ENERGY_CCA_THRES);
	val32 &= ~BIT(31);
	if ((x * oldval >> 7) & 0x1)
		val32 |= BIT(31);
	rtl8723au_write32(priv, REG_OFDM0_ENERGY_CCA_THRES, val32);

	y = result[candidate][1];
	if ((y & 0x00000200) != 0)
		y = y | 0xfffffc00;
	tx0_c = (y * oldval) >> 8;

	val32 = rtl8723au_read32(priv, REG_OFDM0_XC_TX_AFE);
	val32 &= ~0xf0000000;
	val32 |= (((tx0_c & 0x3c0) >> 6) << 28);
	rtl8723au_write32(priv, REG_OFDM0_XC_TX_AFE, val32);

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_TX_IQ_IMBALANCE);
	val32 &= ~0x003f0000;
	val32 |= ((tx0_c & 0x3f) << 16);
	rtl8723au_write32(priv, REG_OFDM0_XA_TX_IQ_IMBALANCE, val32);

	val32 = rtl8723au_read32(priv, REG_OFDM0_ENERGY_CCA_THRES);
	val32 &= ~BIT(29);
	if ((y * oldval >> 7) & 0x1)
		val32 |= BIT(29);
	rtl8723au_write32(priv, REG_OFDM0_ENERGY_CCA_THRES, val32);

	if (tx_only) {
		dev_dbg(&priv->udev->dev, "%s: only TX\n", __func__);
		return;
	}

	reg = result[candidate][2];

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_RX_IQ_IMBALANCE);
	val32 &= ~0x3ff;
	val32 |= (reg & 0x3ff);
	rtl8723au_write32(priv, REG_OFDM0_XA_RX_IQ_IMBALANCE, val32);

	reg = result[candidate][3] & 0x3F;

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_RX_IQ_IMBALANCE);
	val32 &= ~0xfc00;
	val32 |= ((reg << 10) & 0xfc00);
	rtl8723au_write32(priv, REG_OFDM0_XA_RX_IQ_IMBALANCE, val32);

	reg = (result[candidate][3] >> 6) & 0xF;

	val32 = rtl8723au_read32(priv, REG_OFDM0_RX_IQ_EXT_ANTA);
	val32 &= ~0xf0000000;
	val32 |= (reg << 28);
	rtl8723au_write32(priv, REG_OFDM0_RX_IQ_EXT_ANTA, val32);
}

#define MAX_TOLERANCE		5

static bool rtl8xxxu_simularity_compare(struct rtl8xxxu_priv *priv,
					int result[][8], int c1, int c2)
{
	u32 i, j, diff, simubitmap, bound = 0;
	int candidate[2] = {-1, -1};	/* for path A and path B */
	bool retval = true, is_2t = false;

	if (is_2t)
		bound = 8;
	else
		bound = 4;

	simubitmap = 0;

	for (i = 0; i < bound; i++) {
		diff = (result[c1][i] > result[c2][i]) ?
			(result[c1][i] - result[c2][i]) :
			(result[c2][i] - result[c1][i]);
		if (diff > MAX_TOLERANCE) {
			if ((i == 2 || i == 6) && !simubitmap) {
				if (result[c1][i] + result[c1][i + 1] == 0)
					candidate[(i / 4)] = c2;
				else if (result[c2][i] + result[c2][i + 1] == 0)
					candidate[(i / 4)] = c1;
				else
					simubitmap = simubitmap | (1 << i);
			} else {
				simubitmap = simubitmap | (1 << i);
			}
		}
	}

	if (simubitmap == 0) {
		for (i = 0; i < (bound / 4); i++) {
			if (candidate[i] >= 0) {
				for (j = i * 4; j < (i + 1) * 4 - 2; j++)
					result[3][j] = result[candidate[i]][j];
				retval = false;
			}
		}
		return retval;
	} else if (!(simubitmap & 0x0f)) {
		/* path A OK */
		for (i = 0; i < 4; i++)
			result[3][i] = result[c1][i];
	} else if (!(simubitmap & 0xf0) && is_2t) {
		/* path B OK */
		for (i = 4; i < 8; i++)
			result[3][i] = result[c1][i];
	}

	return false;
}

static void
rtl8xxxu_save_mac_regs(struct rtl8xxxu_priv *priv, u32 *reg, u32 *backup)
{
	int i;

	for (i = 0; i < (RTL8XXXU_MAC_REGS - 1); i++)
		backup[i] = rtl8723au_read8(priv, reg[i]);

	backup[i] = rtl8723au_read32(priv, reg[i]);
}

static void
rtl8xxxu_restore_mac_regs(struct rtl8xxxu_priv *priv, u32 *reg, u32 *backup)
{
	int i;

	for (i = 0; i < (RTL8XXXU_MAC_REGS - 1); i++)
		rtl8723au_write8(priv, reg[i], backup[i]);

	rtl8723au_write32(priv, reg[i], backup[i]);
}

static void rtl8xxxu_save_regs(struct rtl8xxxu_priv *priv, u32 *regs,
			       u32 *backup, int count)
{
	int i;

	for (i = 0; i < count; i++)
		backup[i] = rtl8723au_read32(priv, regs[i]);
}

static void rtl8xxxu_restore_regs(struct rtl8xxxu_priv *priv, u32 *regs,
				  u32 *backup, int count)
{
	int i;

	for (i = 0; i < count; i++)
		rtl8723au_write32(priv, regs[i], backup[i]);
}


static void rtl8xxxu_path_adda_on(struct rtl8xxxu_priv *priv, u32 *regs,
				  bool path_a_on, bool is_2t)
{
	u32 path_on;
	int i;

	path_on = path_a_on ? 0x04db25a4 : 0x0b1b25a4;
	if (!is_2t) {
		path_on = 0x0bdb25a0;
		rtl8723au_write32(priv, regs[0], 0x0b1b25a0);
	} else {
		rtl8723au_write32(priv, regs[0], path_on);
	}

	for (i = 1 ; i < RTL8XXXU_ADDA_REGS ; i++)
		rtl8723au_write32(priv, regs[i], path_on);
}

static void
rtl8xxxu_mac_calibration(struct rtl8xxxu_priv *priv, u32 *regs, u32 *backup)
{
	int i = 0;

	rtl8723au_write8(priv, regs[i], 0x3f);

	for (i = 1 ; i < (RTL8XXXU_MAC_REGS - 1); i++) {
		rtl8723au_write8(priv, regs[i],
				 (u8)(backup[i] & ~BIT(3)));
	}
	rtl8723au_write8(priv, regs[i], (u8)(backup[i] & ~BIT(5)));
}

static int rtl8xxxu_iqk_path_a(struct rtl8xxxu_priv *priv, bool configpathb)
{
	u32 reg_eac, reg_e94, reg_e9c, reg_ea4;
	int result = 0;

	/* path-A IQK setting */
	rtl8723au_write32(priv, REG_TX_IQK_TONE_A, 0x10008c1f);
	rtl8723au_write32(priv, REG_RX_IQK_TONE_A, 0x10008c1f);
	rtl8723au_write32(priv, REG_TX_IQK_PI_A, 0x82140102);

	rtl8723au_write32(priv, REG_RX_IQK_PI_A, configpathb ? 0x28160202 :
			  /*IS_81xxC_VENDOR_UMC_B_CUT(pHalData->VersionID)?0x28160202: */ 0x28160502);

	/* path-B IQK setting */
	if (configpathb) {
		rtl8723au_write32(priv, REG_TX_IQK_TONE_B, 0x10008c22);
		rtl8723au_write32(priv, REG_RX_IQK_TONE_B, 0x10008c22);
		rtl8723au_write32(priv, REG_TX_IQK_PI_B, 0x82140102);
		rtl8723au_write32(priv, REG_RX_IQK_PI_B, 0x28160202);
	}

	/* LO calibration setting */
	rtl8723au_write32(priv, REG_IQK_AGC_RSP, 0x001028d1);

	/* One shot, path A LOK & IQK */
	rtl8723au_write32(priv, REG_IQK_AGC_PTS, 0xf9000000);
	rtl8723au_write32(priv, REG_IQK_AGC_PTS, 0xf8000000);

	mdelay(1);

	/* Check failed */
	reg_eac = rtl8723au_read32(priv, REG_RX_POWER_AFTER_IQK_A_2);
	reg_e94 = rtl8723au_read32(priv, REG_TX_POWER_BEFORE_IQK_A);
	reg_e9c = rtl8723au_read32(priv, REG_TX_POWER_AFTER_IQK_A);
	reg_ea4 = rtl8723au_read32(priv, REG_RX_POWER_BEFORE_IQK_A_2);

	if (!(reg_eac & BIT(28)) &&
	    ((reg_e94 & 0x03ff0000) != 0x01420000) &&
	    ((reg_e9c & 0x03ff0000) != 0x00420000))
		result |= 0x01;
	else	/* If TX not OK, ignore RX */
		goto out;

	/* If TX is OK, check whether RX is OK */
	if (!(reg_eac & BIT(27)) &&
	    ((reg_ea4 & 0x03ff0000) != 0x01320000) &&
	    ((reg_eac & 0x03ff0000) != 0x00360000))
		result |= 0x02;
	else
		dev_warn(&priv->udev->dev, "%s: Path A RX IQK failed!\n",
			 __func__);
out:
	return result;
}

static void rtl8xxxu_phy_iqcalibrate(struct rtl8xxxu_priv *priv,
				     int result[][8], int t, bool is_2t)
{
	struct device *dev = &priv->udev->dev;
	u32 i, val32;
	int path_a_ok /*, path_b_ok */;
	int retry = 2;

	u32 ADDA_REG[RTL8XXXU_ADDA_REGS] = {
		REG_FPGA0_XCD_SWITCH_CTRL, REG_BLUETOOTH,
		REG_RX_WAIT_CCA, REG_TX_CCK_RFON,
		REG_TX_CCK_BBON, REG_TX_OFDM_RFON,
		REG_TX_OFDM_BBON, REG_TX_TO_RX,
		REG_TX_TO_TX, REG_RX_CCK,
		REG_RX_OFDM, REG_RX_WAIT_RIFS,
		REG_RX_TO_RX, REG_STANDBY,
		REG_SLEEP, REG_PMPD_ANAEN
	};

	u32 IQK_MAC_REG[RTL8XXXU_MAC_REGS] = {
		REG_TXPAUSE, REG_BEACON_CTRL,
		REG_BEACON_CTRL_1, REG_GPIO_MUXCFG
	};

	u32 IQK_BB_REG_92C[RTL8XXXU_BB_REGS] = {
		REG_OFDM0_TRX_PATH_ENABLE, REG_OFDM0_TR_MUX_PAR,
		REG_FPGA0_XCD_RF_SW_CTRL, REG_CONFIG_ANT_A, REG_CONFIG_ANT_B,
		REG_FPGA0_XAB_RF_SW_CTRL, REG_FPGA0_XA_RF_INT_OE,
		REG_FPGA0_XB_RF_INT_OE, REG_FPGA0_RF_MODE
	};

	/*  Note: IQ calibration must be performed after loading  */
	/*		PHY_REG.txt , and radio_a, radio_b.txt	 */

	if (t == 0) {
		/*  Save ADDA parameters, turn Path A ADDA on */
		rtl8xxxu_save_regs(priv, ADDA_REG, priv->adda_backup,
				   RTL8XXXU_ADDA_REGS);
		rtl8xxxu_save_mac_regs(priv, IQK_MAC_REG, priv->mac_backup);
		rtl8xxxu_save_regs(priv, IQK_BB_REG_92C,
				   priv->bb_backup, RTL8XXXU_BB_REGS);
	}

	rtl8xxxu_path_adda_on(priv, ADDA_REG, true, is_2t);

	if (t == 0) {
		val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM1);
		if (val32 & FPGA0_HSSI_PARM1_PI)
			priv->pi_enabled = 1;
	}

	if (!priv->pi_enabled) {
		/*  Switch BB to PI mode to do IQ Calibration. */
		rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM1,
				  0x01000100);
		rtl8723au_write32(priv, REG_FPGA0_XB_HSSI_PARM1,
				  0x01000100);
	}

	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	val32 &= ~FPGA_RF_MODE_CCK;
	rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

	rtl8723au_write32(priv, REG_OFDM0_TRX_PATH_ENABLE, 0x03a05600);
	rtl8723au_write32(priv, REG_OFDM0_TR_MUX_PAR, 0x000800e4);
	rtl8723au_write32(priv, REG_FPGA0_XCD_RF_SW_CTRL, 0x22204000);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XAB_RF_SW_CTRL);
	val32 |= (BIT(10) | BIT(26));
	rtl8723au_write32(priv, REG_FPGA0_XAB_RF_SW_CTRL, val32);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 &= ~BIT(10);
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
	val32 = rtl8723au_read32(priv, REG_FPGA0_XB_RF_INT_OE);
	val32 &= ~BIT(10);
	rtl8723au_write32(priv, REG_FPGA0_XB_RF_INT_OE, val32);

	if (is_2t) {
		rtl8723au_write32(priv, REG_FPGA0_XA_LSSI_PARM, 0x00010000);
		rtl8723au_write32(priv, REG_FPGA0_XB_LSSI_PARM, 0x00010000);
	}

	/* MAC settings */
	rtl8xxxu_mac_calibration(priv, IQK_MAC_REG, priv->mac_backup);

	/* Page B init */
	rtl8723au_write32(priv, REG_CONFIG_ANT_A, 0x00080000);

	if (is_2t)
		rtl8723au_write32(priv, REG_CONFIG_ANT_B, 0x00080000);

	/* IQ calibration setting */
	rtl8723au_write32(priv, REG_FPGA0_IQK, 0x80800000);
	rtl8723au_write32(priv, REG_TX_IQK, 0x01007c00);
	rtl8723au_write32(priv, REG_RX_IQK, 0x01004800);

	for (i = 0; i < retry; i++) {
		path_a_ok = rtl8xxxu_iqk_path_a(priv, is_2t);
		if (path_a_ok == 0x03) {
			val32 = rtl8723au_read32(priv,
						 REG_TX_POWER_BEFORE_IQK_A);
			result[t][0] = (val32 >> 16) & 0x3ff;
			val32 = rtl8723au_read32(priv,
						 REG_TX_POWER_AFTER_IQK_A);
			result[t][1] = (val32 >> 16) & 0x3ff;
			val32 = rtl8723au_read32(priv,
						 REG_RX_POWER_BEFORE_IQK_A_2);
			result[t][2] = (val32 >> 16) & 0x3ff;
			val32 = rtl8723au_read32(priv,
						 REG_RX_POWER_AFTER_IQK_A_2);
			result[t][3] = (val32 >> 16) & 0x3ff;
			break;
		} else if (i == (retry - 1) && path_a_ok == 0x01) {
			/* TX IQK OK */
			dev_dbg(dev, "%s: Path A IQK Only Tx Success!!\n",
				__func__);

			val32 = rtl8723au_read32(priv,
						 REG_TX_POWER_BEFORE_IQK_A);
			result[t][0] = (val32 >> 16) & 0x3ff;
			val32 = rtl8723au_read32(priv,
						 REG_TX_POWER_AFTER_IQK_A);
			result[t][1] = (val32 >> 16) & 0x3ff;
		}
	}

	if (!path_a_ok)
		dev_dbg(dev, "%s: Path A IQK failed!\n", __func__);

#if 0
	if (is_2t) {
		rtl8xxxu_phy_path_a_standby(priv);

		/*  Turn Path B ADDA on */
		rtl8xxxu_phy_path_adda_on(priv, ADDA_REG, false, is_2t);

		for (i = 0; i < retry; i++) {
			path_b_ok = _PHY_PathB_IQK(priv);
			if (path_b_ok == 0x03) {
				val32 = rtl8723au_read32(priv, REG_TX_POWER_BEFORE_IQK_B);
				result[t][4] = (val32 >> 16) & 0x3ff;
				val32 = rtl8723au_read32(priv, REG_TX_POWER_AFTER_IQK_B);
				result[t][5] = (val32 >> 16) & 0x3ff;
				val32 = rtl8723au_read32(priv, REG_RX_POWER_BEFORE_IQK_B_2);
				result[t][6] = (val32 >> 16) & 0x3ff;
				val32 = rtl8723au_read32(priv, REG_RX_POWER_AFTER_IQK_B_2);
				result[t][7] = (val32 >> 16) & 0x3ff;
				break;
			} else if (i == (retry - 1) && path_b_ok == 0x01) {
				/* TX IQK OK */
				val32 = rtl8723au_read32(priv, REG_TX_POWER_BEFORE_IQK_B);
				result[t][4] = (val32 >> 16) & 0x3ff;
				val32 = rtl8723au_read32(priv, REG_TX_POWER_AFTER_IQK_B);
				result[t][5] = (val32 >> 16) & 0x3ff;
			}
		}

		if (!path_b_ok)
			dev_dbg(dev, "%s: Path B IQK failed!\n", __func__);
	}
#endif

	/* Back to BB mode, load original value */
	rtl8723au_write32(priv, REG_FPGA0_IQK, 0);

	if (t) {
		if (!priv->pi_enabled) {
			/*
			 * Switch back BB to SI mode after finishing
			 * IQ Calibration
			 */
			val32 = 0x01000000;
			rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM1, val32);
			rtl8723au_write32(priv, REG_FPGA0_XB_HSSI_PARM1, val32);
		}

		/*  Reload ADDA power saving parameters */
		rtl8xxxu_restore_regs(priv, ADDA_REG, priv->adda_backup,
				      RTL8XXXU_ADDA_REGS);

		/*  Reload MAC parameters */
		rtl8xxxu_restore_mac_regs(priv, IQK_MAC_REG, priv->mac_backup);

		/*  Reload BB parameters */
		rtl8xxxu_restore_regs(priv, IQK_BB_REG_92C,
				      priv->bb_backup, RTL8XXXU_BB_REGS);

		/*  Restore RX initial gain */
		rtl8723au_write32(priv, REG_FPGA0_XA_LSSI_PARM, 0x00032ed3);

		if (is_2t) {
			rtl8723au_write32(priv, REG_FPGA0_XB_LSSI_PARM,
					  0x00032ed3);
		}

		/* load 0xe30 IQC default value */
		rtl8723au_write32(priv, REG_TX_IQK_TONE_A, 0x01008c00);
		rtl8723au_write32(priv, REG_RX_IQK_TONE_A, 0x01008c00);
	}
}

static void rtl8723a_phy_iq_calibrate(struct rtl8xxxu_priv *priv, bool recovery)
{
	struct device *dev = &priv->udev->dev;
	int result[4][8];	/* last is final result */
	int i, candidate;
	bool path_a_ok /*, path_b_ok */;
	u32 reg_e94, reg_e9c, reg_ea4, reg_eac;
	u32 reg_eb4, reg_ebc, reg_ec4, reg_ecc;
	s32 reg_tmp = 0;
	bool simu;
	u32 iqk_bb_reg_92c[RTL8XXXU_BB_REGS] = {
		REG_OFDM0_XA_RX_IQ_IMBALANCE, REG_OFDM0_XB_RX_IQ_IMBALANCE,
		REG_OFDM0_ENERGY_CCA_THRES, REG_OFDM0_AGCR_SSI_TABLE,
		REG_OFDM0_XA_TX_IQ_IMBALANCE, REG_OFDM0_XB_TX_IQ_IMBALANCE,
		REG_OFDM0_XC_TX_AFE, REG_OFDM0_XD_TX_AFE,
		REG_OFDM0_RX_IQ_EXT_ANTA
	};

	if (recovery) {
		rtl8xxxu_restore_regs(priv, iqk_bb_reg_92c,
				      priv->bb_recovery_backup,
				      RTL8XXXU_BB_REGS);
		return;
	}

	memset(result, 0, sizeof(result));
	candidate = -1;

	path_a_ok = false;
#if 0
	path_b_ok = false;
#endif
	rtl8723au_read32(priv, REG_FPGA0_RF_MODE);

	for (i = 0; i < 3; i++) {
		rtl8xxxu_phy_iqcalibrate(priv, result, i, false);

		if (i == 1) {
			simu = rtl8xxxu_simularity_compare(priv, result, 0, 1);
			if (simu) {
				candidate = 0;
				break;
			}
		}

		if (i == 2) {
			simu = rtl8xxxu_simularity_compare(priv, result, 0, 2);
			if (simu) {
				candidate = 0;
				break;
			}

			simu = rtl8xxxu_simularity_compare(priv, result, 1, 2);
			if (simu) {
				candidate = 1;
			} else {
				for (i = 0; i < 8; i++)
					reg_tmp += result[3][i];

				if (reg_tmp)
					candidate = 3;
				else
					candidate = -1;
			}
		}
	}

	for (i = 0; i < 4; i++) {
		reg_e94 = result[i][0];
		reg_e9c = result[i][1];
		reg_ea4 = result[i][2];
		reg_eac = result[i][3];
		reg_eb4 = result[i][4];
		reg_ebc = result[i][5];
		reg_ec4 = result[i][6];
		reg_ecc = result[i][7];
	}

	if (candidate >= 0) {
		reg_e94 = result[candidate][0];
		priv->rege94 =  reg_e94;
		reg_e9c = result[candidate][1];
		priv->rege9c = reg_e9c;
		reg_ea4 = result[candidate][2];
		reg_eac = result[candidate][3];
		reg_eb4 = result[candidate][4];
		priv->regeb4 = reg_eb4;
		reg_ebc = result[candidate][5];
		priv->regebc = reg_ebc;
		reg_ec4 = result[candidate][6];
		reg_ecc = result[candidate][7];
		dev_dbg(dev, "%s: candidate is %x\n", __func__, candidate);
		dev_dbg(dev, "%s: reg_e94 =%x reg_e9C =%x reg_eA4 =%x "
			"reg_eAC =%x reg_eB4 =%x reg_eBC =%x reg_eC4 =%x "
			"reg_eCC =%x\n ", __func__, reg_e94, reg_e9c,
			reg_ea4, reg_eac, reg_eb4, reg_ebc, reg_ec4, reg_ecc);
		path_a_ok = true;
#if 0
		path_b_ok = true;
#endif
	} else {
		reg_e94 = reg_eb4 = priv->rege94 = priv->regeb4 = 0x100;
		reg_e9c = reg_ebc = priv->rege9c = priv->regebc = 0x0;
	}

	if (reg_e94 && candidate >= 0)
		rtl8xxxu_fill_iqk_matrix_a(priv, path_a_ok, result,
					   candidate, (reg_ea4 == 0));

	rtl8xxxu_save_regs(priv, iqk_bb_reg_92c,
			   priv->bb_recovery_backup, RTL8XXXU_BB_REGS);
}

static void rtl8723a_phy_lc_calibrate(struct rtl8xxxu_priv *priv)
{
	u32 val32;
	u32 rf_amode, lstf;

	/* Check continuous TX and Packet TX */
	lstf = rtl8723au_read32(priv, REG_OFDM1_LSTF);

	if (lstf & OFDM_LSTF_MASK) {
		/* Disable all continuous TX */
		val32 = lstf & ~OFDM_LSTF_MASK;
		rtl8723au_write32(priv, REG_OFDM1_LSTF, val32);

		/* Read original RF mode Path A */
		rf_amode = rtl8723au_read_rfreg(priv, RF6052_REG_AC);

#if 0
		/* Path-B */
		if (is_2t)
			rf_bmode = PHY_QueryRFReg(priv, RF_PATH_B, RF_AC,
						  bMask12Bits);
#endif

		/* Set RF mode to standby Path A */
		rtl8723au_write_rfreg(priv, RF6052_REG_AC,
				      (rf_amode & 0xfff) | 0x10000);

#if 0
		/* Path-B */
		if (is_2t)
			PHY_SetRFReg(priv, RF_PATH_B, RF_AC, bMask12Bits,
				     (RF_Bmode & 0x8ffff) | 0x10000);
#endif
	} else {
		/*  Deal with Packet TX case */
		/*  block all queues */
		rtl8723au_write8(priv, REG_TXPAUSE, 0xff);
	}

	/* Start LC calibration */
	val32 = rtl8723au_read_rfreg(priv, RF6052_REG_MODE_AG);
	val32 |= 0x08000;
	rtl8723au_write_rfreg(priv, RF6052_REG_MODE_AG, val32);

	msleep(100);

	/* Restore original parameters */
	if (lstf & OFDM_LSTF_MASK) {
		/* Path-A */
		rtl8723au_write32(priv, REG_OFDM1_LSTF, lstf);
		rtl8723au_write_rfreg(priv, RF6052_REG_AC, rf_amode);

#if 0
		/* Path-B */
		if (is_2t)
			PHY_SetRFReg(priv, RF_PATH_B, RF_AC, bMask12Bits,
				     RF_Bmode);
#endif
	} else /*  Deal with Packet TX case */
		rtl8723au_write8(priv, REG_TXPAUSE, 0x00);
}

static int rtl8xxxu_set_mac(struct rtl8xxxu_priv *priv)
{
	int i;
	u16 reg;

	reg = REG_MACID;

	for (i = 0; i < ETH_ALEN; i++)
		rtl8723au_write8(priv, reg + i, priv->mac_addr[i]);

	return 0;
}

static int rtl8xxxu_set_bssid(struct rtl8xxxu_priv *priv, const u8 *bssid)
{
	int i;
	u16 reg;

	dev_dbg(&priv->udev->dev,
		"%s: (%02x:%02x:%02x:%02x:%02x:%02x)\n", __func__,
		bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

	reg = REG_BSSID;

	for (i = 0; i < ETH_ALEN; i++)
		rtl8723au_write8(priv, reg + i, bssid[i]);

	return 0;
}

void rtl8xxxu_set_ampdu_factor(struct rtl8xxxu_priv *priv, u8 ampdu_factor)
{
	u8 vals[4] = { 0x41, 0xa8, 0x72, 0xb9 };
	u8 max_agg = 0xf;
	int i;

	ampdu_factor = 1 << (ampdu_factor + 2);
	if (ampdu_factor > max_agg)
		ampdu_factor = max_agg;

	for (i = 0; i < 4; i++) {
		if ((vals[i] & 0xf0) > (ampdu_factor << 4))
			vals[i] = (vals[i] & 0x0f) | (ampdu_factor << 4);

		if ((vals[i] & 0x0f) > ampdu_factor)
			vals[i] = (vals[i] & 0xf0) | ampdu_factor;

		rtl8723au_write8(priv, REG_AGGLEN_LMT + i, vals[i]);
	}
}

void rtl8xxxu_set_ampdu_min_space(struct rtl8xxxu_priv *priv, u8 density)
{
	u8 val8;

	val8 = rtl8723au_read8(priv, REG_AMPDU_MIN_SPACE);
	val8 &= 0xf8;
	val8 |= density;
	rtl8723au_write8(priv, REG_AMPDU_MIN_SPACE, val8);
}

static int rtl8xxxu_active_to_emu(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	int count, ret;

	/* Start of rtl8723AU_card_enable_flow */
	/* Act to Cardemu sequence*/
	/* Turn off RF */
	rtl8723au_write8(priv, REG_RF_CTRL, 0);

	/* 0x004E[7] = 0, switch DPDT_SEL_P output from register 0x0065[2] */
	val8 = rtl8723au_read8(priv, REG_LEDCFG2);
	val8 &= ~LEDCFG2_DPDT_SELECT;
	rtl8723au_write8(priv, REG_LEDCFG2, val8);

	/* 0x0005[1] = 1 turn off MAC by HW state machine*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 |= BIT(1);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	for (count = RTL8XXXU_MAX_REG_POLL; count; count--) {
		val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
		if ((val8 & BIT(1)) == 0)
			break;
		udelay(10);
	}

	if (!count) {
		dev_warn(&priv->udev->dev, "%s: Disabling MAC timed out\n",
			 __func__);
		ret = -EBUSY;
		goto exit;
	}

	/* 0x0000[5] = 1 analog Ips to digital, 1:isolation */
	val8 = rtl8723au_read8(priv, REG_SYS_ISO_CTRL);
	val8 |= SYS_ISO_ANALOG_IPS;
	rtl8723au_write8(priv, REG_SYS_ISO_CTRL, val8);

	/* 0x0020[0] = 0 disable LDOA12 MACRO block*/
	val8 = rtl8723au_read8(priv, REG_LDOA15_CTRL);
	val8 &= ~LDOA15_ENABLE;
	rtl8723au_write8(priv, REG_LDOA15_CTRL, val8);

exit:
	return ret;
}

static int rtl8xxxu_active_to_lps(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	u8 val32;
	int count, ret;

	rtl8723au_write8(priv, REG_TXPAUSE, 0xff);

	/*
	 * Poll - wait for RX packet to complete
	 */
	for (count = RTL8XXXU_MAX_REG_POLL; count; count--) {
		val32 = rtl8723au_read8(priv, 0x5f8);
		if (!val32)
			break;
		udelay(10);
	}

	if (!count) {
		dev_warn(&priv->udev->dev,
			 "%s: RX poll timed out (0x05f8)\n", __func__);
		ret = -EBUSY;
		goto exit;
	}

	/* Disable CCK and OFDM, clock gated */
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 &= ~SYS_FUNC_BBRSTB;
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	udelay(2);

	/* Reset baseband */
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 &= ~SYS_FUNC_BB_GLB_RSTN;
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	/* Reset MAC TRX */
	val8 = rtl8723au_read8(priv, REG_CR);
	val8 = CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE;
	rtl8723au_write8(priv, REG_CR, val8);

	/* Reset MAC TRX */
	val8 = rtl8723au_read8(priv, REG_CR + 1);
	val8 &= ~BIT(1); /* CR_SECURITY_ENABLE */
	rtl8723au_write8(priv, REG_CR + 1, val8);

	/* Respond TX OK to scheduler */
	val8 = rtl8723au_read8(priv, REG_DUAL_TSF_RST);
	val8 |= BIT(5);
	rtl8723au_write8(priv, REG_DUAL_TSF_RST, val8);

exit:
	return ret;
}

static void rtl8xxxu_disabled_to_emu(struct rtl8xxxu_priv *priv)
{
	u8 val8;

	/* Clear suspend enable and power down enable*/
	val8 = rtl8723au_read8(priv, 0x05);
	val8 &= ~(BIT(3) | BIT(7));
	rtl8723au_write8(priv, 0x05, val8);

	/* 0x48[16] = 0 to disable GPIO9 as EXT WAKEUP*/
	val8 = rtl8723au_read8(priv, 0x4a);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, 0x4a, val8);

	/* 0x04[12:11] = 11 enable WL suspend*/
	val8 = rtl8723au_read8(priv, 0x05);
	val8 &= ~(BIT(3) | BIT(4));
	rtl8723au_write8(priv, 0x05, val8);
}

static int rtl8xxxu_emu_to_active(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	u32 val32;
	int count, ret = 0;

	/* 0x20[0] = 1 enable LDOA12 MACRO block for all interface*/
	val8 = rtl8723au_read8(priv, REG_LDOA15_CTRL);
	val8 |= LDOA15_ENABLE;
	rtl8723au_write8(priv, REG_LDOA15_CTRL, val8);

	/* 0x67[0] = 0 to disable BT_GPS_SEL pins*/
	val8 = rtl8723au_read8(priv, 0x0067);
	val8 &= ~BIT(4);
	rtl8723au_write8(priv, 0x0067, val8);

	mdelay(1);

	/* 0x00[5] = 0 release analog Ips to digital, 1:isolation */
	val8 = rtl8723au_read8(priv, REG_SYS_ISO_CTRL);
	val8 &= ~BIT(5);
	rtl8723au_write8(priv, REG_SYS_ISO_CTRL, val8);

	/* disable SW LPS 0x04[10]= 0 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~BIT(2);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* wait till 0x04[17] = 1 power ready*/
	for (count = RTL8XXXU_MAX_REG_POLL; count; count--) {
		val32 = rtl8723au_read32(priv, REG_APS_FSMCO);
		if (val32 & BIT(17))
			break;

		udelay(10);
	}

	if (!count) {
		ret = -EBUSY;
		goto exit;
	}

	/* We should be able to optimize the following three entries into one */

	/* release WLON reset 0x04[16]= 1*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 2);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_APS_FSMCO + 2, val8);

	/* disable HWPDN 0x04[15]= 0*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~BIT(7);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* disable WL suspend*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~(BIT(3) | BIT(4));
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* set, then poll until 0 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	for (count = RTL8XXXU_MAX_REG_POLL; count; count--) {
		val32 = rtl8723au_read32(priv, REG_APS_FSMCO);
		if ((val32 & BIT(8)) == 0) {
			ret = 0;
			break;
		}
		udelay(10);
	}

	if (!count) {
		ret = -EBUSY;
		goto exit;
	}

	/* 0x4C[23] = 0x4E[7] = 1, switch DPDT_SEL_P output from WL BB */
	/*
	 * Note: Vendor driver actually clears this bit, despite the
	 * documentation claims it's being set!
	 */
	val8 = rtl8723au_read8(priv, REG_LEDCFG2);
	val8 |= LEDCFG2_DPDT_SELECT;
	val8 &= ~LEDCFG2_DPDT_SELECT;
	rtl8723au_write8(priv, REG_LEDCFG2, val8);

exit:
	return ret;
}

static int rtl8xxxu_emu_to_disabled(struct rtl8xxxu_priv *priv)
{
	u8 val8;

	/* 0x0007[7:0] = 0x20 SOP option to disable BG/MB */
	rtl8723au_write8(priv, REG_APS_FSMCO + 3, 0x20);

	/* 0x04[12:11] = 01 enable WL suspend */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~BIT(4);
	val8 |= BIT(3);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 |= BIT(7);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* 0x48[16] = 1 to enable GPIO9 as EXT wakeup */
	val8 = rtl8723au_read8(priv, REG_GPIO_INTM + 2);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_GPIO_INTM + 2, val8);

	return 0;
}

static int rtl8xxxu_power_on(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	u16 val16;
	u32 val32;
	int ret;

	/*
	 * RSV_CTRL 0x001C[7:0] = 0x00, unlock ISO/CLK/Power control register
	 */
	rtl8723au_write8(priv, REG_RSV_CTRL, 0x0);

	rtl8xxxu_disabled_to_emu(priv);

	ret = rtl8xxxu_emu_to_active(priv);
	if (ret)
		goto exit;

	/*
	 * 0x0004[19] = 1, reset 8051
	 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 2);
	val8 |= BIT(3);
	rtl8723au_write8(priv, REG_APS_FSMCO + 2, val8);

	/*
	 * Enable MAC DMA/WMAC/SCHEDULE/SEC block
	 * Set CR bit10 to enable 32k calibration.
	 */
	val16 = rtl8723au_read16(priv, REG_CR);
	val16 |= (CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE |
		  CR_TXDMA_ENABLE | CR_RXDMA_ENABLE |
		  CR_PROTOCOL_ENABLE | CR_SCHEDULE_ENABLE |
		  CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE |
		  CR_SECURITY_ENABLE | CR_CALTIMER_ENABLE);
	rtl8723au_write16(priv, REG_CR, val16);

	/* For EFuse PG */
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
	val32 &= ~(BIT(28) | BIT(29) | BIT(30));
	val32 |= (0x06 << 28);
	rtl8723au_write32(priv, REG_EFUSE_CTRL, val32);
exit:
	return ret;
}

static void rtl8xxxu_power_off(struct rtl8xxxu_priv *priv)
{
	u8 val8;
	u16 val16;

	rtl8xxxu_active_to_lps(priv);

	/* Turn off RF */
	rtl8723au_write8(priv, REG_RF_CTRL, 0x00);

	/* Reset Firmware if running in RAM */
	if (rtl8723au_read8(priv, REG_MCU_FW_DL) & MCU_FW_RAM_SEL)
		rtl8xxxu_firmware_self_reset(priv);

	/* Reset MCU */
	val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
	val16 &= ~SYS_FUNC_CPU_ENABLE;
	rtl8723au_write16(priv, REG_SYS_FUNC, val16);

	/* Reset MCU ready status */
	rtl8723au_write8(priv, REG_MCU_FW_DL, 0x00);

	rtl8xxxu_active_to_emu(priv);
	rtl8xxxu_emu_to_disabled(priv);

	/* Reset MCU IO Wrapper */
	val8 = rtl8723au_read8(priv, REG_RSV_CTRL + 1);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, REG_RSV_CTRL + 1, val8);

	val8 = rtl8723au_read8(priv, REG_RSV_CTRL + 1);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_RSV_CTRL + 1, val8);

	/* RSV_CTRL 0x1C[7:0] = 0x0e  lock ISO/CLK/Power control register */
	rtl8723au_write8(priv, REG_RSV_CTRL, 0x0e);
}

static int rtl8xxxu_init_device(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct device *dev = &priv->udev->dev;
	bool macpower;
	int ret;
	u8 val8;
	u16 val16;
	u32 val32;

	/* Check if MAC is already powered on */
	val8 = rtl8723au_read8(priv, REG_CR);

	/*
	 * Fix 92DU-VC S3 hang with the reason is that secondary mac is not
	 * initialized. First MAC returns 0xea, second MAC returns 0x00
	 */
	if (val8 == 0xea)
		macpower = false;
	else
		macpower = true;

	ret = rtl8xxxu_power_on(priv);
	if (ret < 0) {
		dev_warn(dev, "%s: Failed power on\n", __func__);
		goto exit;
	}

	dev_dbg(dev, "%s: macpower %i\n", __func__, macpower);
	if (!macpower) {
		ret = rtl8xxxu_init_llt_table(priv, TX_TOTAL_PAGE_NUM);
		if (ret) {
			dev_warn(dev, "%s: LLT table init failed\n", __func__);
			goto exit;
		}
	}

	ret = rtl8xxxu_download_firmware(priv);
	if (ret)
		goto exit;
	ret = rtl8xxxu_start_firmware(priv);
	if (ret)
		goto exit;

	ret = rtl8xxxu_init_mac(priv, rtl8723a_mac_init_table);
	if (ret)
		goto exit;

	ret = rtl8xxxu_init_phy_bb(priv);
	if (ret)
		goto exit;

	ret = rtl8xxxu_init_phy_rf(priv);
	if (ret)
		goto exit;

	/* Reduce 80M spur */
	rtl8723au_write32(priv, REG_AFE_XTAL_CTRL, 0x0381808d);
	rtl8723au_write32(priv, REG_AFE_PLL_CTRL, 0xf0ffff83);
	rtl8723au_write32(priv, REG_AFE_PLL_CTRL, 0xf0ffff82);
	rtl8723au_write32(priv, REG_AFE_PLL_CTRL, 0xf0ffff83);

	/* RFSW Control - clear bit 14 ?? */
	rtl8723au_write32(priv, REG_FPGA0_TXINFO, 0x00000003);
	/* 0x07000760 */
	val32 = 0x07000000 | FPGA0_RF_TRSW | FPGA0_RF_TRSWB |
		FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB | FPGA0_RF_PAPE;
	rtl8723au_write32(priv, REG_FPGA0_XAB_RF_SW_CTRL, val32);
	/* 0x860[6:5]= 00 - why? - this sets antenna B */
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, 0x66F60210);

	priv->rf_mode_ag[0] = rtl8723au_read_rfreg(priv, RF6052_REG_MODE_AG);

	if (!macpower) {
		if (priv->ep_tx_normal_queue)
			val8 = TX_PAGE_NUM_NORM_PQ;
		else
			val8 = 0;

		rtl8723au_write8(priv, REG_RQPN_NPQ, val8);

		val32 = (TX_PAGE_NUM_PUBQ << RQPN_NORM_PQ_SHIFT) | RQPN_LOAD;

		if (priv->ep_tx_high_queue)
			val32 |= (TX_PAGE_NUM_HI_PQ << RQPN_HI_PQ_SHIFT);
		if (priv->ep_tx_low_queue)
			val32 |= (TX_PAGE_NUM_LO_PQ << RQPN_LO_PQ_SHIFT);

		rtl8723au_write32(priv, REG_RQPN, val32);

		/*
		 * Set TX buffer boundary
		 */
		val8 = TX_TOTAL_PAGE_NUM + 1;
		rtl8723au_write8(priv, REG_TXPKTBUF_BCNQ_BDNY, val8);
		rtl8723au_write8(priv, REG_TXPKTBUF_MGQ_BDNY, val8);
		rtl8723au_write8(priv, REG_TXPKTBUF_WMAC_LBK_BF_HD, val8);
		rtl8723au_write8(priv, REG_TRXFF_BNDY, val8);
		rtl8723au_write8(priv, REG_TDECTRL + 1, val8);
	}

	ret = rtl8xxxu_init_queue_priority(priv);
	if (ret)
		goto exit;

	/*
	 * Set RX page boundary
	 */
	rtl8723au_write16(priv, REG_TRXFF_BNDY + 2, 0x27ff);
	/*
	 * Transfer page size is always 128
	 */
	val8 = (PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_RX_SHIFT) |
		(PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_TX_SHIFT);
	rtl8723au_write8(priv, REG_PBP, val8);

	/*
	 * Unit in 8 bytes, not obvious what it is used for
	 */
	rtl8723au_write8(priv, REG_RX_DRVINFO_SZ, 4);

	/*
	 * Enable all interrupts - not obvious USB needs to do this
	 */
	rtl8723au_write32(priv, REG_HISR, 0xffffffff);
	rtl8723au_write32(priv, REG_HIMR, 0xffffffff);

	rtl8xxxu_set_mac(priv);
	rtl8xxxu_set_linktype(priv, NL80211_IFTYPE_STATION);

	/*
	 * Configure initial WMAC settings
	 */
	val32 = RCR_ACCEPT_PHYS_MATCH | RCR_ACCEPT_MCAST | RCR_ACCEPT_BCAST |
		/* RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON | */
		RCR_ACCEPT_MGMT_FRAME | RCR_HTC_LOC_CTRL |
		RCR_APPEND_PHYSTAT | RCR_APPEND_ICV | RCR_APPEND_MIC;
	rtl8723au_write32(priv, REG_RCR, val32);

	/*
	 * Accept all multicast
	 */
	rtl8723au_write32(priv, REG_MAR, 0xffffffff);
	rtl8723au_write32(priv, REG_MAR + 4, 0xffffffff);

	/*
	 * Init adaptive controls
	 */
	val32 = rtl8723au_read32(priv, REG_RESPONSE_RATE_SET);
	val32 &= ~RESPONSE_RATE_BITMAP_ALL;
	val32 |= RESPONSE_RATE_RRSR_CCK_ONLY_1M;
	rtl8723au_write32(priv, REG_RESPONSE_RATE_SET, val32);

	/* CCK = 0x0a, OFDM = 0x10 */
#if 0
	rtl8xxxu_set_spec_sifs(priv, 0x0a, 0x10);
#else
	rtl8xxxu_set_spec_sifs(priv, 0x10, 0x10);
#endif
	rtl8xxxu_set_retry(priv, 0x30, 0x30);
	rtl8xxxu_set_spec_sifs(priv, 0x0a, 0x10);

	/*
	 * Init EDCA
	 */
	rtl8723au_write16(priv, REG_MAC_SPEC_SIFS, 0x100a);

	/* Set CCK SIFS */
	rtl8723au_write16(priv, REG_SIFS_CCK, 0x100a);

	/* Set OFDM SIFS */
	rtl8723au_write16(priv, REG_SIFS_OFDM, 0x100a);

	/* TXOP */
	rtl8723au_write32(priv, REG_EDCA_BE_PARAM, 0x005ea42b);
	rtl8723au_write32(priv, REG_EDCA_BK_PARAM, 0x0000a44f);
	rtl8723au_write32(priv, REG_EDCA_VI_PARAM, 0x005ea324);
	rtl8723au_write32(priv, REG_EDCA_VO_PARAM, 0x002fa226);

	/* Set data auto rate fallback retry count */
	rtl8723au_write32(priv, REG_DARFRC, 0x00000000);
	rtl8723au_write32(priv, REG_DARFRC + 4, 0x10080404);
	rtl8723au_write32(priv, REG_RARFRC, 0x04030201);
	rtl8723au_write32(priv, REG_RARFRC + 4, 0x08070605);

	val8 = rtl8723au_read8(priv, REG_FWHW_TXQ_CTRL);
	val8 |= FWHW_TXQ_CTRL_AMPDU_RETRY;
	rtl8723au_write8(priv, REG_FWHW_TXQ_CTRL, val8);

	/*  Set ACK timeout */
	rtl8723au_write8(priv, REG_ACKTO, 0x40);

	/*
	 * Initialize beacon parameters
	 */
	val16 = BEACON_DISABLE_TSF_UPDATE | (BEACON_DISABLE_TSF_UPDATE << 8);
	rtl8723au_write16(priv, REG_BEACON_CTRL, val16);
	rtl8723au_write16(priv, REG_TBTT_PROHIBIT, 0x6404);
	rtl8723au_write8(priv, REG_DRIVER_EARLY_INT, DRIVER_EARLY_INT_TIME);
	rtl8723au_write8(priv, REG_BEACON_DMA_TIME, BEACON_DMA_ATIME_INT_TIME);
	rtl8723au_write16(priv, REG_BEACON_TCFG, 0x660F);

	/*
	 * Enable CCK and OFDM block
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	val32 |= (FPGA_RF_MODE_CCK | FPGA_RF_MODE_OFDM);
	rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

	/*
	 * Invalidate all CAM entries - bit 30 is undocumented
	 */
	rtl8723au_write32(priv, REG_CAM_CMD, CAM_CMD_POLLING | BIT(30));

	/*
	 * Start out with default power levels for channel 6, 20MHz
	 */
	rtl8723a_set_tx_power(priv, 1, false);

	/* Let the 8051 take control of antenna setting */
	val8 = rtl8723au_read8(priv, REG_LEDCFG2);
	val8 |= LEDCFG2_DPDT_SELECT;
	rtl8723au_write8(priv, REG_LEDCFG2, val8);

	rtl8723au_write8(priv, REG_HWSEQ_CTRL, 0xff);

	/* Disable BAR - not sure if this has any effect on USB */
	rtl8723au_write32(priv, REG_BAR_MODE_CTRL, 0x0201ffff);

#if 0
	if (priv->wifi_spec)
		rtl8723au_write16(priv, REG_FAST_EDCA_CTRL, 0);
#endif
	rtl8723au_write16(priv, REG_FAST_EDCA_CTRL, 0);

#if 0
	/*
	 * From 8192cu driver
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 &= ~BIT(6);
	val32 |= BIT(5);
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
#endif

	/*
	 * Not sure if we should get into this at all
	 */
	if (priv->iqk_initialized) {
		rtl8723a_phy_iq_calibrate(priv, true);
	} else {
		rtl8723a_phy_iq_calibrate(priv, false);
		priv->iqk_initialized = true;
	}

	/*
	 * This should enable thermal meter
	 */
	rtl8723au_write_rfreg(priv, RF6052_REG_T_METER, 0x60);

	rtl8723a_phy_lc_calibrate(priv);

	/* fix USB interface interference issue */
	rtl8723au_write8(priv, 0xfe40, 0xe0);
	rtl8723au_write8(priv, 0xfe41, 0x8d);
	rtl8723au_write8(priv, 0xfe42, 0x80);
	rtl8723au_write32(priv, REG_TXDMA_OFFSET_CHK, 0xfd0320);

	/* Solve too many protocol error on USB bus */
	/* Can't do this for 8188/8192 UMC A cut parts */
	rtl8723au_write8(priv, 0xfe40, 0xe6);
	rtl8723au_write8(priv, 0xfe41, 0x94);
	rtl8723au_write8(priv, 0xfe42, 0x80);

	rtl8723au_write8(priv, 0xfe40, 0xe0);
	rtl8723au_write8(priv, 0xfe41, 0x19);
	rtl8723au_write8(priv, 0xfe42, 0x80);

	rtl8723au_write8(priv, 0xfe40, 0xe5);
	rtl8723au_write8(priv, 0xfe41, 0x91);
	rtl8723au_write8(priv, 0xfe42, 0x80);

	rtl8723au_write8(priv, 0xfe40, 0xe2);
	rtl8723au_write8(priv, 0xfe41, 0x81);
	rtl8723au_write8(priv, 0xfe42, 0x80);

#if 0
	/* Init BT hw config. */
	rtl8723a_init_bt(priv);
#endif

	/*
	 * Not sure if we really need to save these parameters, but the
	 * vendor driver does
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	if (val32 & FPGA0_HSSI_PARM2_CCK_HIGH_PWR)
		priv->path_a_hi_power = 1;

	val32 = rtl8723au_read32(priv, REG_OFDM0_TRX_PATH_ENABLE);
	priv->path_a_rf_paths = val32 & OFDM0_RF_PATH_RX_MASK;

	val32 = rtl8723au_read32(priv, REG_OFDM0_XA_AGC_CORE1);
	priv->path_a_ig_value = val32 & OFDM0_X_AGC_CORE1_IGI_MASK;

	/* Set NAV_UPPER to 30000us */
	val8 = ((30000 + NAV_UPPER_UNIT - 1) / NAV_UPPER_UNIT);
	rtl8723au_write8(priv, REG_NAV_UPPER, val8);

	/*
	 * 2011/03/09 MH debug only, UMC-B cut pass 2500 S5 test,
	 * but we need to fin root cause.
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	if ((val32 & 0xff000000) != 0x83000000) {
		val32 |= FPGA_RF_MODE_CCK;
		rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);
	}

	val32 = rtl8723au_read32(priv, REG_FWHW_TXQ_CTRL);
	val32 |= FWHW_TXQ_CTRL_XMIT_MGMT_ACK;
	/* ack for xmit mgmt frames. */
	rtl8723au_write32(priv, REG_FWHW_TXQ_CTRL, val32);

exit:
	return ret;
}

static void rtl8xxxu_disable_device(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;

	rtl8xxxu_power_off(priv);
}

static void rtl8xxxu_cam_write(struct rtl8xxxu_priv *priv,
			       struct ieee80211_key_conf *key, const u8 *mac)
{
	u32 cmd, val32, addr, ctrl;
	int j, i, tmp_debug;

	tmp_debug = rtl8xxxu_debug;
	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_KEY)
		rtl8xxxu_debug |= RTL8XXXU_DEBUG_REG_WRITE;

	addr = key->keyidx << CAM_CMD_KEY_SHIFT;
	ctrl = (key->cipher & 0x0f) << 2 | key->keyidx | CAM_WRITE_VALID;

	for (j = 5; j >= 0; j--) {
		switch (j) {
		case 0:
			val32 = ctrl | (mac[0] << 16) | (mac[1] << 24);
			break;
		case 1:
			val32 = mac[2] | (mac[3] << 8) |
				(mac[4] << 16) | (mac[5] << 24);
			break;
		default:
			i = (j - 2) << 2;
			val32 = key->key[i] | (key->key[i + 1] << 8) |
				key->key[i + 2] << 16 | key->key[i + 3] << 24;
			break;
		}

		rtl8723au_write32(priv, REG_CAM_WRITE, val32);
		cmd = CAM_CMD_POLLING | CAM_CMD_WRITE | (addr + j);
		rtl8723au_write32(priv, REG_CAM_CMD, cmd);
		udelay(100);
	}

	rtl8xxxu_debug = tmp_debug;
}

static void rtl8xxxu_sw_scan_start(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif, const u8 *mac)
{
#if 0
	struct rtl8xxxu_priv *priv = hw->priv;
	u32 val32;

	val32 = rtl8723au_read32(priv, REG_RCR);
	val32 &= ~(RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON);
	rtl8723au_write32(priv, REG_RCR, val32);

	rtl8723au_write8(priv, REG_BEACON_CTRL, BEACON_ATIM |
			 BEACON_FUNCTION_ENABLE | BEACON_DISABLE_TSF_UPDATE);

	dev_dbg(&priv->udev->dev, "%s\n", __func__);
#endif
}

static void rtl8xxxu_sw_scan_complete(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	u8 val8;

#if 0
	dev_dbg(&priv->udev->dev, "%s\n", __func__);
#endif

	val8 = rtl8723au_read8(priv, REG_BEACON_CTRL);
	val8 &= ~BEACON_DISABLE_TSF_UPDATE;
	rtl8723au_write8(priv, REG_BEACON_CTRL, val8);
}

static void rtl8xxxu_update_rate_mask(struct rtl8xxxu_priv *priv,
				      struct ieee80211_sta *sta)
{
	struct h2c_cmd h2c;
	u32 ramask;

	/* TODO: Set bits 28-31 for rate adaptive id */
	ramask = (sta->supp_rates[0] & 0xfff) |
		sta->ht_cap.mcs.rx_mask[0] << 12 |
		sta->ht_cap.mcs.rx_mask[1] << 20;

	h2c.ramask.cmd = H2C_SET_RATE_MASK;
	h2c.ramask.mask_lo = cpu_to_le16(ramask & 0xffff);
	h2c.ramask.mask_hi = cpu_to_le16(ramask >> 16);

	h2c.ramask.arg = 0x80;
	if (sta->ht_cap.cap &
	    (IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_SGI_20)) {
		h2c.ramask.arg |= 0x20;
		priv->use_shortgi = true;
	} else {
		priv->use_shortgi = false;
	}

	dev_dbg(&priv->udev->dev, "%s: rate mask %08x, arg %02x\n", __func__,
		ramask, h2c.ramask.arg);
	rtl8723a_h2c_cmd(priv, &h2c);
}

static void rtl8xxxu_set_basic_rates(struct rtl8xxxu_priv *priv,
				     struct ieee80211_sta *sta)
{
	u32 rate_cfg, val32;
	u8 rate_idx = 0;

	rate_cfg = sta->supp_rates[0];
	rate_cfg &= 0x15f;
	rate_cfg |= 1;
	val32 = rtl8723au_read32(priv, REG_RESPONSE_RATE_SET);
	val32 &= ~RESPONSE_RATE_BITMAP_ALL;
	val32 |= rate_cfg;
	rtl8723au_write32(priv, REG_RESPONSE_RATE_SET, val32);

	dev_dbg(&priv->udev->dev, "%s: supp_rates %08x rates %08x\n", __func__,
		sta->supp_rates[0], rate_cfg);

	while (rate_cfg) {
		rate_cfg = (rate_cfg >> 1);
		rate_idx++;
	}
	rtl8723au_write8(priv, REG_INIRTS_RATE_SEL, rate_idx);
}

static void
rtl8xxxu_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_bss_conf *bss_conf, u32 changed)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct device *dev = &priv->udev->dev;
	struct ieee80211_sta *sta;
	u32 val32;
#if 0
	u16 val16;
#endif
	u8 val8;

	if (changed & BSS_CHANGED_ASSOC) {
		struct h2c_cmd h2c;

		memset(&h2c, 0, sizeof(struct h2c_cmd));
		rtl8xxxu_set_linktype(priv, vif->type);

		if (bss_conf->assoc) {
			rcu_read_lock();
			sta = ieee80211_find_sta(vif, bss_conf->bssid);
			if (!sta) {
				dev_info(dev, "%s: ASSOC no sta found\n",
					 __func__);
				rcu_read_unlock();
				goto error;
			}

			if (sta->ht_cap.ht_supported)
				dev_info(dev, "%s: HT supported\n", __func__);
			if (sta->vht_cap.vht_supported)
				dev_info(dev, "%s: VHT supported\n", __func__);
			rtl8xxxu_update_rate_mask(priv, sta);
			rcu_read_unlock();

			val32 = rtl8723au_read32(priv, REG_RCR);
			val32 |= RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON;
			rtl8723au_write32(priv, REG_RCR, val32);

			/* Enable RX of data frames */
			rtl8723au_write16(priv, REG_RXFLTMAP2, 0xffff);

			rtl8723au_write8(priv, REG_BCN_MAX_ERR, 0xff);

			/* Stop TX beacon */
			val8 = rtl8723au_read8(priv, REG_FWHW_TXQ_CTRL + 2);
			val8 &= ~BIT(6);
			rtl8723au_write8(priv, REG_FWHW_TXQ_CTRL + 2, val8);

			rtl8723au_write8(priv, REG_TBTT_PROHIBIT + 1, 0x64);
			val8 = rtl8723au_read8(priv, REG_TBTT_PROHIBIT + 2);
			val8 &= ~BIT(0);
			rtl8723au_write8(priv, REG_TBTT_PROHIBIT + 2, val8);

			/* joinbss sequence */
			rtl8723au_write16(priv, REG_BCN_PSR_RPT,
					  0xc000 | bss_conf->aid);

#if 0
			val16 = rtl8723au_read16(priv, REG_CR);
			val16 |= CR_SW_BEACON_ENABLE;
			rtl8723au_read16(priv, REG_CR, val16);

			val8 = rtl8723au_read8(priv, REG_BEACON_CTRL);
			val8 &= ~BEACON_FUNCTION_ENABLE;
			val8 |= BEACON_DISABLE_TSF_UPDATE;
			rtl8723au_write8(priv, REG_BEACON_CTRL, val8);

			val8 = rtl8723au_read8(priv, REG_FWHW_TXQ_CTRL + 2);
			if (val8 & BIT(6))
				recover = true;

			val8 &= ~BIT(6);
			rtl8723au_write8(priv, REG_FWHW_TXQ_CTRL + 2, val8);

			/* build fake beacon */

			val8 = rtl8723au_read8(priv, REG_BEACON_CTRL);
			val8 |= BEACON_FUNCTION_ENABLE;
			val8 &= ~BEACON_DISABLE_TSF_UPDATE;
			rtl8723au_write8(priv, REG_BEACON_CTRL, val8);

			if (recover) {
				val8 = rtl8723au_read8(priv,
						       REG_FWHW_TXQ_CTRL + 2);
				val8 |= BIT(6);
				rtl8723au_write8(priv, REG_FWHW_TXQ_CTRL + 2,
						 val8);
			}
			val16 = rtl8723au_read16(priv, REG_CR);
			val16 &= ~CR_SW_BEACON_ENABLE;
			rtl8723au_read16(priv, REG_CR, val16);
#endif
			h2c.joinbss.data = H2C_JOIN_BSS_CONNECT;
		} else {
			val32 = rtl8723au_read32(priv, REG_RCR);
			val32 &= ~(RCR_CHECK_BSSID_MATCH |
				   RCR_CHECK_BSSID_BEACON);
			rtl8723au_write32(priv, REG_RCR, val32);

			val8 = rtl8723au_read8(priv, REG_BEACON_CTRL);
			val8 |= BEACON_DISABLE_TSF_UPDATE;
			rtl8723au_write8(priv, REG_BEACON_CTRL, val8);

			/* Disable RX of data frames */
			rtl8723au_write16(priv, REG_RXFLTMAP2, 0x0000);
			h2c.joinbss.data = H2C_JOIN_BSS_DISCONNECT;
		}
		h2c.joinbss.cmd = H2C_JOIN_BSS_REPORT;
		rtl8723a_h2c_cmd(priv, &h2c);
	}

	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		dev_info(dev, "Changed ERP_PREAMBLE: Use short preamble %02x\n",
			 bss_conf->use_short_preamble);
		val32 = rtl8723au_read32(priv, REG_RESPONSE_RATE_SET);
		if (bss_conf->use_short_preamble)
			val32 |= RSR_ACK_SHORT_PREAMBLE;
		else
			val32 &= ~RSR_ACK_SHORT_PREAMBLE;
		rtl8723au_write32(priv, REG_RESPONSE_RATE_SET, val32);
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		dev_info(dev, "Changed ERP_SLOT: short_slot_time %i\n",
			 bss_conf->use_short_slot);

		if (bss_conf->use_short_slot)
			val8 = 9;
		else
			val8 = 20;
		rtl8723au_write8(priv, REG_SLOT, val8);
	}

	if (changed & BSS_CHANGED_HT) {
		u8 ampdu_factor, ampdu_density;
		u8 sifs;

		rcu_read_lock();
		sta = ieee80211_find_sta(vif, bss_conf->bssid);
		if (!sta) {
			dev_info(dev, "BSS_CHANGED_HT: No HT sta found!\n");
			rcu_read_unlock();
			goto error;
		}
		if (sta->ht_cap.ht_supported) {
			ampdu_factor = sta->ht_cap.ampdu_factor;
			ampdu_density = sta->ht_cap.ampdu_density;
			sifs = 0x0e;
		} else {
			ampdu_factor = 0;
			ampdu_density = 0;
			sifs = 0x0a;
		}
		rcu_read_unlock();
#if 0
		dev_info(dev,
			 "Changed HT: ampdu_factor %02x, ampdu_density %02x\n",
			 ampdu_factor, ampdu_density);
		rtl8xxxu_set_ampdu_factor(priv, ampdu_factor);
		rtl8xxxu_set_ampdu_min_space(priv, ampdu_density);
#endif

		rtl8723au_write8(priv, REG_SIFS_CCK + 1, sifs);
		rtl8723au_write8(priv, REG_SIFS_OFDM + 1, sifs);
		rtl8723au_write8(priv, REG_SPEC_SIFS + 1, sifs);
		rtl8723au_write8(priv, REG_MAC_SPEC_SIFS + 1, sifs);
		rtl8723au_write8(priv, REG_R2T_SIFS + 1, sifs);
		rtl8723au_write8(priv, REG_T2T_SIFS + 1, sifs);
	}

	if (changed & BSS_CHANGED_BSSID) {
		dev_info(dev, "Changed BSSID!\n");
		rtl8xxxu_set_bssid(priv, bss_conf->bssid);

		rcu_read_lock();
		sta = ieee80211_find_sta(vif, bss_conf->bssid);
		if (!sta) {
			dev_info(dev, "No bssid sta found!\n");
			rcu_read_unlock();
			goto error;
		}
		rcu_read_unlock();
	}

	if (changed & BSS_CHANGED_BASIC_RATES) {
		dev_info(dev, "Changed BASIC_RATES!\n");
		rcu_read_lock();
		sta = ieee80211_find_sta(vif, bss_conf->bssid);
		if (sta)
			rtl8xxxu_set_basic_rates(priv, sta);
		else
			dev_info(dev,
				 "BSS_CHANGED_BASIC_RATES: No sta found!\n");

		rcu_read_unlock();
	}
error:
	return;
}

static u32 rtl8xxxu_80211_to_rtl_queue(u32 queue)
{
	u32 rtlqueue;

	switch (queue) {
	case IEEE80211_AC_VO:
		rtlqueue = TXDESC_QUEUE_VO;
		break;
	case IEEE80211_AC_VI:
		rtlqueue = TXDESC_QUEUE_VI;
		break;
	case IEEE80211_AC_BE:
		rtlqueue = TXDESC_QUEUE_BE;
		break;
	case IEEE80211_AC_BK:
		rtlqueue = TXDESC_QUEUE_BK;
		break;
	default:
		rtlqueue = TXDESC_QUEUE_BE;
	}

	return rtlqueue;
}

static u32 rtl8xxxu_queue_select(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	u32 queue;

	if (ieee80211_is_mgmt(hdr->frame_control))
		queue = TXDESC_QUEUE_MGNT;
	else
		queue = rtl8xxxu_80211_to_rtl_queue(skb_get_queue_mapping(skb));

	return queue;
}

static void rtl8xxxu_calc_tx_desc_csum(struct rtl8xxxu_tx_desc *tx_desc)
{
	u16 *ptr = (u16 *)tx_desc;
	u16 csum = 0;
	int i;

	/*
	 * Clear csum field before calculation, as the csum field is
	 * in the middle of the struct.
	 */
	tx_desc->csum = cpu_to_le16(0);

	for (i = 0; i < (sizeof(struct rtl8xxxu_tx_desc) / sizeof(u16)); i++)
		csum = csum ^ le16_to_cpu(ptr[i]);

	tx_desc->csum |= cpu_to_le16(csum);
}

static void rtl8xxxu_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = (struct sk_buff *)urb->context;
	struct ieee80211_tx_info *tx_info;
	struct ieee80211_hw *hw;

	tx_info = IEEE80211_SKB_CB(skb);
	hw = tx_info->rate_driver_data[0];

	skb_pull(skb, sizeof(struct rtl8xxxu_tx_desc));

	ieee80211_tx_info_clear_status(tx_info);
	tx_info->status.rates[0].idx = -1;
	tx_info->status.rates[0].count = 0;

#if 0
	/*
	 * Until we figure out how to obtain the info,
	 * do not fool the stack.
	 */
	tx_info->flags |= IEEE80211_TX_STAT_ACK;
#endif

	ieee80211_tx_status_irqsafe(hw, skb);

	usb_free_urb(urb);
}

static void rtl8xxxu_tx(struct ieee80211_hw *hw,
			struct ieee80211_tx_control *control,
			struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_rate *tx_rate = ieee80211_get_tx_rate(hw, tx_info);
	struct rtl8xxxu_priv *priv = hw->priv;
	struct rtl8xxxu_tx_desc *tx_desc;
	struct device *dev = &priv->udev->dev;
	struct urb *urb;
	u32 queue, rate;
	u16 pktlen = skb->len;
	u16 seq_number;
	u16 rate_flag = tx_info->control.rates[0].flags;
	int ret;

	if (skb_headroom(skb) < sizeof(struct rtl8xxxu_tx_desc)) {
		dev_warn(dev,
			 "%s: Not enough headroom (%i) for tx descriptor\n",
			 __func__, skb_headroom(skb));
		goto error;
	}

	if (unlikely(skb->len > (65535 - sizeof(struct rtl8xxxu_tx_desc)))) {
		dev_warn(dev, "%s: Trying to send over-sized skb (%i)\n",
			 __func__, skb->len);
		goto error;
	}

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		dev_warn(dev, "%s: Unable to allocate urb\n", __func__);
		goto error;
	}

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_TX)
		dev_info(dev, "%s: TX rate: %d (%d), pkt size %d\n",
			 __func__, tx_rate->bitrate, tx_rate->hw_value, pktlen);

	tx_info->rate_driver_data[0] = hw;

	tx_desc = (struct rtl8xxxu_tx_desc *)
		skb_push(skb, sizeof(struct rtl8xxxu_tx_desc));

	memset(tx_desc, 0, sizeof(struct rtl8xxxu_tx_desc));
	tx_desc->pkt_size = cpu_to_le16(pktlen);
	tx_desc->pkt_offset = sizeof(struct rtl8xxxu_tx_desc);

	tx_desc->txdw0 = TXDESC_OWN | TXDESC_FSG | TXDESC_LSG;
	if (is_multicast_ether_addr(ieee80211_get_DA(hdr)) ||
	    is_broadcast_ether_addr(ieee80211_get_DA(hdr)))
		tx_desc->txdw0 |= TXDESC_BROADMULTICAST;

	queue = rtl8xxxu_queue_select(hw, skb);
	tx_desc->txdw1 = cpu_to_le32(queue << TXDESC_QUEUE_SHIFT);

	if (tx_info->control.hw_key) {
		switch (tx_info->control.hw_key->cipher) {
		case WLAN_CIPHER_SUITE_WEP40:
		case WLAN_CIPHER_SUITE_WEP104:
		case WLAN_CIPHER_SUITE_TKIP:
			tx_desc->txdw1 |= cpu_to_le32(TXDESC_SEC_RC4);
			break;
		case WLAN_CIPHER_SUITE_CCMP:
			tx_desc->txdw1 |= cpu_to_le32(TXDESC_SEC_AES);
			break;
		default:
			break;
		}
	}

	seq_number = IEEE80211_SEQ_TO_SN(le16_to_cpu(hdr->seq_ctrl));
	tx_desc->txdw3 = cpu_to_le32((u32)seq_number << TXDESC_SEQ_SHIFT);

	if (rate_flag & IEEE80211_TX_RC_MCS)
		rate = tx_info->control.rates[0].idx + DESC_RATE_MCS0;
	else
		rate = tx_rate->hw_value;
	tx_desc->txdw5 = cpu_to_le32(rate);

	/*
	 * Black magic!
	 */
#if 0
	if (ieee80211_is_data(hdr->frame_control)) {
		tx_desc->txdw5 = cpu_to_le32(0x0001ff00);

		if ((tx_info->flags & IEEE80211_TX_CTL_AMPDU) &&
		    control->sta->ht_cap.ht_supported &&
		    control && control->sta) {
			u8 ampdu = control->sta->ht_cap.ampdu_density;

			tx_desc->txdw2 |=
				cpu_to_le32(ampdu << TXDESC_AMPDU_DENSITY_SHIFT);
			tx_desc->txdw1 |= cpu_to_le32(TXDESC_AGG_ENABLE);
		} else
			tx_desc->txdw1 |= cpu_to_le32(TXDESC_BK);
	} else
#endif
		tx_desc->txdw1 |= cpu_to_le32(TXDESC_BK);
	if (ieee80211_is_data_qos(hdr->frame_control))
		tx_desc->txdw4 |= cpu_to_le32(TXDESC_QOS);
	if (rate_flag & IEEE80211_TX_RC_USE_SHORT_PREAMBLE)
		tx_desc->txdw4 |= cpu_to_le32(TXDESC_SHORT_PREAMBLE);
	if (rate_flag & IEEE80211_TX_RC_SHORT_GI || priv->use_shortgi)
		tx_desc->txdw5 |= cpu_to_le32(TXDESC_SHORT_GI);
	if (ieee80211_is_mgmt(hdr->frame_control)) {
		tx_desc->txdw5 = cpu_to_le32(tx_rate->hw_value);
		tx_desc->txdw4 |= cpu_to_le32(TXDESC_USE_DRIVER_RATE);
		tx_desc->txdw5 |= cpu_to_le32(6 << TXDESC_RETRY_LIMIT_SHIFT);
		tx_desc->txdw5 |= cpu_to_le32(TXDESC_RETRY_LIMIT_ENABLE);
	}

	if (rate_flag & IEEE80211_TX_RC_USE_RTS_CTS) {
		/* Use RTS rate 24M - does the mac80211 tell us which to use? */
		tx_desc->txdw4 |= cpu_to_le32(DESC_RATE_24M);
		tx_desc->txdw4 |= cpu_to_le32(TXDESC_RTS_ENABLE);
	}

	rtl8xxxu_calc_tx_desc_csum(tx_desc);

	usb_fill_bulk_urb(urb, priv->udev, priv->pipe_out[queue], skb->data,
			  skb->len, rtl8xxxu_tx_complete, skb);

	usb_anchor_urb(urb, &priv->tx_anchor);
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		usb_unanchor_urb(urb);
		goto error;
	}
	return;
error:
	dev_kfree_skb(skb);
}


static void rtl8xxxu_rx_complete(struct urb *urb)
{
	struct rtl8xxxu_rx_urb *rx_urb =
		container_of(urb, struct rtl8xxxu_rx_urb, urb);
	struct ieee80211_hw *hw = rx_urb->hw;
	struct rtl8xxxu_priv *priv = hw->priv;
	struct sk_buff *skb = (struct sk_buff *)urb->context;
	struct rtl8xxxu_rx_desc *rx_desc = (struct rtl8xxxu_rx_desc *)skb->data;
	struct rtl8723au_phy_stats *phy_stats;
	struct ieee80211_rx_status *rx_status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_mgmt *mgmt;
	struct device *dev = &priv->udev->dev;
	__le32 *_rx_desc_le = (__le32 *)skb->data;
	u32 *_rx_desc = (u32 *)skb->data;
	int cnt, len, skb_size, drvinfo_sz, desc_shift, i, ret;

	for (i = 0; i < (sizeof(struct rtl8xxxu_rx_desc) / sizeof(u32)); i++)
		_rx_desc[i] = le32_to_cpu(_rx_desc_le[i]);

	cnt = rx_desc->frag;
	len = rx_desc->pktlen;
	drvinfo_sz = rx_desc->drvinfo_sz * 8;
	desc_shift = rx_desc->shift;
	skb_put(skb, urb->actual_length);

	if (urb->status == 0) {
		skb_pull(skb, sizeof(struct rtl8xxxu_rx_desc));
		phy_stats = (struct rtl8723au_phy_stats *)skb->data;

		skb_pull(skb, drvinfo_sz + desc_shift);

		mgmt = (struct ieee80211_mgmt *)skb->data;

		memset(rx_status, 0, sizeof(struct ieee80211_rx_status));

		/*
		 * Note this is valid for CCK rates only - FIXME
		 */
		if (rx_desc->phy_stats) {
			u8 cck_agc_rpt = phy_stats->cck_agc_rpt_ofdm_cfosho_a;

			switch (cck_agc_rpt & 0xc0) {
			case 0xc0:
				rx_status->signal = -46 - (cck_agc_rpt & 0x3e);
				break;
			case 0x80:
				rx_status->signal = -26 - (cck_agc_rpt & 0x3e);
				break;
			case 0x40:
				rx_status->signal = -12 - (cck_agc_rpt & 0x3e);
				break;
			case 0x00:
				rx_status->signal = 16 - (cck_agc_rpt & 0x3e);
				break;
			}
		}

		rx_status->freq = hw->conf.chandef.chan->center_freq;
		rx_status->band = hw->conf.chandef.chan->band;

		if (!rx_desc->swdec)
			rx_status->flag |= RX_FLAG_DECRYPTED;
		if (rx_desc->crc32)
			rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;
		if (rx_desc->bw)
			rx_status->flag |= RX_FLAG_40MHZ;

		if (rx_desc->rxht) {
			rx_status->flag |= RX_FLAG_HT;
			rx_status->rate_idx = rx_desc->rxmcs - DESC_RATE_MCS0;
		} else {
			rx_status->rate_idx = rx_desc->rxmcs;
		}

		ieee80211_rx_irqsafe(hw, skb);
		skb_size = sizeof(struct rtl8xxxu_rx_desc) +
			RTL_RX_BUFFER_SIZE;
		skb = dev_alloc_skb(skb_size);
		if (!skb) {
			dev_warn(dev, "%s: Unable to allocate skb\n", __func__);
			goto cleanup;
		}

		memset(skb->data, 0, sizeof(struct rtl8xxxu_rx_desc));
		usb_fill_bulk_urb(&rx_urb->urb, priv->udev, priv->pipe_in,
				  skb->data, skb_size, rtl8xxxu_rx_complete,
				  skb);

		usb_anchor_urb(&rx_urb->urb, &priv->rx_anchor);
		ret = usb_submit_urb(&rx_urb->urb, GFP_ATOMIC);
		if (ret) {
			usb_unanchor_urb(&rx_urb->urb);
			goto cleanup;
		}
	} else {
		dev_dbg(dev, "%s: status %i\n",	__func__, urb->status);
		goto cleanup;
	}
	return;

cleanup:
	usb_free_urb(urb);
	dev_kfree_skb(skb);
	return;
}

static int rtl8xxxu_submit_rx_urb(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct sk_buff *skb;
	struct rtl8xxxu_rx_urb *rx_urb;
	int skb_size;
	int ret;

	skb_size = sizeof(struct rtl8xxxu_rx_desc) + RTL_RX_BUFFER_SIZE;
	skb = dev_alloc_skb(skb_size);
	if (!skb)
		return -ENOMEM;

	memset(skb->data, 0, sizeof(struct rtl8xxxu_rx_desc));

	rx_urb = kmalloc(sizeof(struct rtl8xxxu_rx_urb), GFP_ATOMIC);
	if (!rx_urb) {
		dev_kfree_skb(skb);
		return -ENOMEM;
	}
	usb_init_urb(&rx_urb->urb);
	rx_urb->hw = hw;

	usb_fill_bulk_urb(&rx_urb->urb, priv->udev, priv->pipe_in, skb->data,
			  skb_size, rtl8xxxu_rx_complete, skb);
	usb_anchor_urb(&rx_urb->urb, &priv->rx_anchor);
	ret = usb_submit_urb(&rx_urb->urb, GFP_ATOMIC);
	if (ret)
		usb_unanchor_urb(&rx_urb->urb);
	return ret;
}

static void rtl8xxxu_int_complete(struct urb *urb)
{
	struct rtl8xxxu_priv *priv = (struct rtl8xxxu_priv *)urb->context;
	struct device *dev = &priv->udev->dev;
	int i, ret;

	dev_dbg(dev, "%s: status %i\n", __func__, urb->status);
	if (urb->status == 0) {
		for (i = 0; i < USB_INTR_CONTENT_LENGTH; i++) {
			printk("%02x ", priv->int_buf[i]);
			if ((i & 0x0f) == 0x0f)
				printk("\n");
		}

		usb_anchor_urb(urb, &priv->int_anchor);
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			usb_unanchor_urb(urb);
	} else {
		dev_info(dev, "%s: Error %i\n", __func__, urb->status);
	}
}


static int rtl8xxxu_submit_int_urb(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct urb *urb;
	u32 val32;
	int ret;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	usb_fill_int_urb(urb, priv->udev, priv->pipe_interrupt,
			 priv->int_buf, USB_INTR_CONTENT_LENGTH,
			 rtl8xxxu_int_complete, priv, 1);
	usb_anchor_urb(urb, &priv->int_anchor);
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		usb_unanchor_urb(urb);
		goto error;
	}

	val32 = rtl8723au_read32(priv, REG_USB_HIMR);
	val32 |= USB_HIMR_CPWM;
	rtl8723au_write32(priv, REG_USB_HIMR, val32);

error:
	return ret;
}

static int rtl8xxxu_add_interface(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	int ret;
	u8 val8;

	switch (vif->type) {
	case NL80211_IFTYPE_STATION:
		rtl8723a_stop_tx_beacon(priv);

		val8 = rtl8723au_read8(priv, REG_BEACON_CTRL);
		val8 |= BEACON_ATIM | BEACON_FUNCTION_ENABLE |
			BEACON_DISABLE_TSF_UPDATE;
		rtl8723au_write8(priv, REG_BEACON_CTRL, val8);
		ret = 0;
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	rtl8xxxu_set_linktype(priv, vif->type);

	return ret;
}

static void rtl8xxxu_remove_interface(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif)
{
	struct rtl8xxxu_priv *priv = hw->priv;

	dev_dbg(&priv->udev->dev, "%s\n", __func__);
}

static int rtl8xxxu_config(struct ieee80211_hw *hw, u32 changed)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct device *dev = &priv->udev->dev;
	u16 val16;
	int ret = 0, channel;
	bool ht40;

	if (rtl8xxxu_debug & RTL8XXXU_DEBUG_CHANNEL)
		dev_info(dev,
			 "%s: channel: %i (changed %08x chandef.width %02x)\n",
			 __func__, hw->conf.chandef.chan->hw_value,
			 changed, hw->conf.chandef.width);

	if (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS) {
		val16 = ((hw->conf.long_frame_max_tx_count <<
			  RETRY_LIMIT_LONG_SHIFT) & RETRY_LIMIT_LONG_MASK) |
			((hw->conf.short_frame_max_tx_count <<
			  RETRY_LIMIT_SHORT_SHIFT) & RETRY_LIMIT_SHORT_MASK);
		rtl8723au_write16(priv, REG_RETRY_LIMIT, val16);
	}

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		switch (hw->conf.chandef.width) {
		case NL80211_CHAN_WIDTH_20_NOHT:
		case NL80211_CHAN_WIDTH_20:
			ht40 = false;
			break;
		case NL80211_CHAN_WIDTH_40:
			ht40 = true;
			break;
		default:
			ret = -ENOTSUPP;
			goto exit;
		}

		channel = hw->conf.chandef.chan->hw_value;

		rtl8723a_set_tx_power(priv, channel, ht40);

		rtl8723au_config_channel(hw);
	}

exit:
	return ret;
}

static int rtl8xxxu_conf_tx(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif, u16 queue,
			    const struct ieee80211_tx_queue_params *param)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct device *dev = &priv->udev->dev;
	u32 val32;
	u16 cw_min, cw_max, txop;
	u8 aifs, acm_ctrl, acm_bit;

	aifs = param->aifs;
	cw_min = cpu_to_le16(param->cw_min);
	cw_max = cpu_to_le16(param->cw_max);
	txop = cpu_to_le16(param->txop);

	val32 = aifs |
		(cw_min & 0xf) << EDCA_PARAM_ECW_MIN_SHIFT |
		(cw_max & 0xf) << EDCA_PARAM_ECW_MAX_SHIFT |
		txop << EDCA_PARAM_TXOP_SHIFT;

	acm_ctrl = rtl8723au_read8(priv, REG_ACM_HW_CTRL);
	dev_dbg(dev,
		"%s: IEEE80211 queue %02x val %08x, acm %i, acm_ctrl %02x\n",
		__func__, queue, val32, param->acm, acm_ctrl);

	switch (queue) {
	case IEEE80211_AC_VO:
		acm_bit = ACM_HW_CTRL_VO;
		rtl8723au_write32(priv, REG_EDCA_VO_PARAM, val32);
		break;
	case IEEE80211_AC_VI:
		acm_bit = ACM_HW_CTRL_VI;
		rtl8723au_write32(priv, REG_EDCA_VI_PARAM, val32);
		break;
	case IEEE80211_AC_BE:
		acm_bit = ACM_HW_CTRL_BE;
		rtl8723au_write32(priv, REG_EDCA_BE_PARAM, val32);
		break;
	case IEEE80211_AC_BK:
		acm_bit = ACM_HW_CTRL_BK;
		rtl8723au_write32(priv, REG_EDCA_BK_PARAM, val32);
		break;
	default:
		acm_bit = 0;
		break;
	}

	if (param->acm)
		acm_ctrl |= acm_bit;
	else
		acm_ctrl &= ~acm_bit;
	rtl8723au_write8(priv, REG_ACM_HW_CTRL, acm_ctrl);

	return 0;
}

static void rtl8xxxu_configure_filter(struct ieee80211_hw *hw,
				      unsigned int changed_flags,
				      unsigned int *total_flags, u64 multicast)
{
	struct rtl8xxxu_priv *priv = hw->priv;

	dev_dbg(&priv->udev->dev, "%s: changed_flags %08x, total_flags %08x\n",
		__func__, changed_flags, *total_flags);

	*total_flags &= (FIF_ALLMULTI | FIF_CONTROL | FIF_BCN_PRBRESP_PROMISC);
}

static int rtl8xxxu_set_rts_threshold(struct ieee80211_hw *hw, u32 rts)
{
	if (rts > 2347)
		return -EINVAL;

	return 0;
}

static int rtl8xxxu_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			    struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta,
			    struct ieee80211_key_conf *key)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	struct device *dev = &priv->udev->dev;
	u8 mac_addr[ETH_ALEN];
	u8 val8;
	u16 val16;
	u32 val32;
	int retval = -EOPNOTSUPP;

	dev_dbg(dev, "%s: cmd %02x, cipher %08x, index %i\n",
		__func__, cmd, key->cipher, key->keyidx);

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	if (key->keyidx > 3)
		return -EOPNOTSUPP;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:

		break;
	case WLAN_CIPHER_SUITE_CCMP:
		key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
	default:
		return -EOPNOTSUPP;
	}

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
		dev_dbg(dev, "%s: pairwise key\n", __func__);
		ether_addr_copy(mac_addr, sta->addr);
	} else {
		dev_dbg(dev, "%s: group key\n", __func__);
		eth_broadcast_addr(mac_addr);
	}

	val16 = rtl8723au_read16(priv, REG_CR);
	val16 |= CR_SECURITY_ENABLE;
	rtl8723au_write16(priv, REG_CR, val16);

	val8 = SEC_CFG_TX_SEC_ENABLE | SEC_CFG_TXBC_USE_DEFKEY |
		SEC_CFG_RX_SEC_ENABLE | SEC_CFG_RXBC_USE_DEFKEY;
	val8 |= SEC_CFG_TX_USE_DEFKEY | SEC_CFG_RX_USE_DEFKEY;
	rtl8723au_write8(priv, REG_SECURITY_CFG, val8);

	switch (cmd) {
	case SET_KEY:
		/*
		 * This is a bit of a hack - the lower bits of the cipher
		 * suite selector happens to match the cipher index in the
		 * CAM
		 */
		key->hw_key_idx = key->keyidx;
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		rtl8xxxu_cam_write(priv, key, mac_addr);
		retval = 0;
		break;
	case DISABLE_KEY:
		rtl8723au_write32(priv, REG_CAM_WRITE, 0x00000000);
		val32 = CAM_CMD_POLLING | CAM_CMD_WRITE |
			key->keyidx << CAM_CMD_KEY_SHIFT;
		rtl8723au_write32(priv, REG_CAM_CMD, val32);
		retval = 0;
		break;
	default:
		dev_warn(dev, "%s: Unsupported command %02x\n", __func__, cmd);
	}

	return retval;
}

static int rtl8xxxu_start(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;
	int ret, i;

	ret = 0;

	init_usb_anchor(&priv->rx_anchor);
	init_usb_anchor(&priv->tx_anchor);
	init_usb_anchor(&priv->int_anchor);

	rtl8723a_enable_rf(priv);
	ret = rtl8xxxu_submit_int_urb(hw);
	if (ret)
		goto exit;

	for (i = 0; i < 32; i++)
		ret = rtl8xxxu_submit_rx_urb(hw);

exit:
	/*
	 * Disable all data frames
	 */
	rtl8723au_write16(priv, REG_RXFLTMAP2, 0x0000);
	/*
	 * Accept all mgmt frames
	 */
	rtl8723au_write16(priv, REG_RXFLTMAP0, 0xffff);

	rtl8723au_write32(priv, REG_OFDM0_XA_AGC_CORE1, 0x6954341e);

	return ret;
}

static void rtl8xxxu_stop(struct ieee80211_hw *hw)
{
	struct rtl8xxxu_priv *priv = hw->priv;

	rtl8723au_write8(priv, REG_TXPAUSE, 0xff);

	rtl8723au_write16(priv, REG_RXFLTMAP0, 0x0000);
	rtl8723au_write16(priv, REG_RXFLTMAP2, 0x0000);

	usb_kill_anchored_urbs(&priv->rx_anchor);
	usb_kill_anchored_urbs(&priv->tx_anchor);
	usb_kill_anchored_urbs(&priv->int_anchor);

	rtl8723a_disable_rf(priv);

	/*
	 * Disable interrupts
	 */
	rtl8723au_write32(priv, REG_USB_HIMR, 0);
}

static const struct ieee80211_ops rtl8xxxu_ops = {
	.tx = rtl8xxxu_tx,
	.add_interface = rtl8xxxu_add_interface,
	.remove_interface = rtl8xxxu_remove_interface,
	.config = rtl8xxxu_config,
	.conf_tx = rtl8xxxu_conf_tx,
	.bss_info_changed = rtl8xxxu_bss_info_changed,
	.configure_filter = rtl8xxxu_configure_filter,
	.set_rts_threshold = rtl8xxxu_set_rts_threshold,
	.start = rtl8xxxu_start,
	.stop = rtl8xxxu_stop,
	.sw_scan_start = rtl8xxxu_sw_scan_start,
	.sw_scan_complete = rtl8xxxu_sw_scan_complete,
	.set_key = rtl8xxxu_set_key,
};

static int rtl8xxxu_parse_usb(struct rtl8xxxu_priv *priv,
			      struct usb_interface *interface)
{
	struct usb_interface_descriptor *interface_desc;
	struct usb_host_interface *host_interface;
	struct usb_endpoint_descriptor *endpoint;
	struct device *dev = &priv->udev->dev;
	int i, j = 0, endpoints;
	u8 dir, xtype, num;
	int ret = 0;

	host_interface = &interface->altsetting[0];
	interface_desc = &host_interface->desc;
	endpoints = interface_desc->bNumEndpoints;

	for (i = 0; i < endpoints; i++) {
		endpoint = &host_interface->endpoint[i].desc;

		dir = endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK;
		num = usb_endpoint_num(endpoint);
		xtype = usb_endpoint_type(endpoint);
		if (rtl8xxxu_debug & RTL8XXXU_DEBUG_USB)
			dev_dbg(dev,
				"%s: endpoint: dir %02x, # %02x, type %02x\n",
				__func__, dir, num, xtype);
		if (usb_endpoint_dir_in(endpoint) &&
		    usb_endpoint_xfer_bulk(endpoint)) {
			if (rtl8xxxu_debug & RTL8XXXU_DEBUG_USB)
				dev_dbg(dev, "%s: in endpoint num %i\n",
					__func__, num);

			if (priv->pipe_in) {
				dev_warn(dev,
					 "%s: Too many IN pipes\n", __func__);
				ret = -EINVAL;
				goto exit;
			}

			priv->pipe_in =	usb_rcvbulkpipe(priv->udev, num);
		}

		if (usb_endpoint_dir_in(endpoint) &&
		    usb_endpoint_xfer_int(endpoint)) {
			if (rtl8xxxu_debug & RTL8XXXU_DEBUG_USB)
				dev_dbg(dev, "%s: interrupt endpoint num %i\n",
					__func__, num);

			if (priv->pipe_interrupt) {
				dev_warn(dev, "%s: Too many INTERRUPT pipes\n",
					 __func__);
				ret = -EINVAL;
				goto exit;
			}

			priv->pipe_interrupt = usb_rcvintpipe(priv->udev, num);
		}

		if (usb_endpoint_dir_out(endpoint) &&
		    usb_endpoint_xfer_bulk(endpoint)) {
			if (rtl8xxxu_debug & RTL8XXXU_DEBUG_USB)
				dev_dbg(dev, "%s: out endpoint num %i\n",
					__func__, num);
			if (j >= RTL8XXXU_OUT_ENDPOINTS) {
				dev_warn(dev,
					 "%s: Too many OUT pipes\n", __func__);
				ret = -EINVAL;
				goto exit;
			}
			priv->out_ep[j++] = num;
		}
	}
exit:
	return ret;
}

static int rtl8xxxu_probe(struct usb_interface *interface,
			  const struct usb_device_id *id)
{
	struct rtl8xxxu_priv *priv;
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	struct ieee80211_supported_band *sband;
	int ret = 0;

	udev = usb_get_dev(interface_to_usbdev(interface));

	hw = ieee80211_alloc_hw(sizeof(struct rtl8xxxu_priv), &rtl8xxxu_ops);
	if (!hw) {
		ret = -ENOMEM;
		goto exit;
	}

	priv = hw->priv;
	priv->hw = hw;
	priv->udev = udev;
	mutex_init(&priv->usb_buf_mutex);
	mutex_init(&priv->h2c_mutex);

	usb_set_intfdata(interface, hw);

	ret = rtl8xxxu_parse_usb(priv, interface);
	if (ret)
		goto exit;

	rtl8xxxu_8723au_identify_chip(priv);
	rtl8xxxu_read_efuse(priv);
	ether_addr_copy(priv->mac_addr, priv->efuse_wifi.efuse.mac_addr);

	dev_info(&udev->dev, "RTL8723au MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		 priv->efuse_wifi.efuse.mac_addr[0],
		 priv->efuse_wifi.efuse.mac_addr[1],
		 priv->efuse_wifi.efuse.mac_addr[2],
		 priv->efuse_wifi.efuse.mac_addr[3],
		 priv->efuse_wifi.efuse.mac_addr[4],
		 priv->efuse_wifi.efuse.mac_addr[5]);

	rtl8xxxu_load_firmware(priv);

	ret = rtl8xxxu_init_device(hw);

	hw->wiphy->max_scan_ssids = 1;
	hw->wiphy->max_scan_ie_len = IEEE80211_MAX_DATA_LEN;
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->queues = 4;

	sband = &rtl8xxxu_supported_band;
	sband->ht_cap.ht_supported = true;
#if 0
	sband->ht_cap.ampdu_factor = IEEE80211_HT_MAX_AMPDU_8K;
	sband->ht_cap.ampdu_density = IEEE80211_HT_MPDU_DENSITY_16;
#endif
	sband->ht_cap.cap = /* IEEE80211_HT_CAP_SUP_WIDTH_20_40 | */
		IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SGI_40;
	memset(&sband->ht_cap.mcs, 0, sizeof(sband->ht_cap.mcs));
	sband->ht_cap.mcs.rx_mask[0] = 0xff;
	sband->ht_cap.mcs.rx_mask[4] = 0x01;
	if (priv->rf_paths > 1) {
		sband->ht_cap.mcs.rx_mask[1] = 0xff;
		sband->ht_cap.mcs.rx_highest = cpu_to_le16(300);
		sband->ht_cap.cap |= IEEE80211_HT_CAP_SGI_40;
	} else {
		sband->ht_cap.mcs.rx_mask[1] = 0x00;
		sband->ht_cap.mcs.rx_highest = cpu_to_le16(150);
	}
	sband->ht_cap.mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
	hw->wiphy->bands[IEEE80211_BAND_2GHZ] = sband;

	hw->wiphy->max_remain_on_channel_duration = 65535; /* ms */
	hw->wiphy->cipher_suites = rtl8xxxu_cipher_suites;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(rtl8xxxu_cipher_suites);
	hw->wiphy->rts_threshold = 2347;

	SET_IEEE80211_DEV(priv->hw, &interface->dev);
	SET_IEEE80211_PERM_ADDR(hw, priv->mac_addr);

	hw->extra_tx_headroom = sizeof(struct rtl8xxxu_tx_desc);
	hw->flags = IEEE80211_HW_SIGNAL_DBM;
	/*
	 * The firmware can handle rate control, but we need callbacks
	 */
	hw->flags |= IEEE80211_HW_HAS_RATE_CONTROL;

	ret = ieee80211_register_hw(priv->hw);
	if (ret) {
		dev_err(&udev->dev, "%s: Failed to register: %i\n",
			__func__, ret);
		goto exit;
	}

exit:
	if (ret < 0)
		usb_put_dev(udev);
	return ret;
}

static void rtl8xxxu_disconnect(struct usb_interface *interface)
{
	struct rtl8xxxu_priv *priv;
	struct ieee80211_hw *hw;

	hw = usb_get_intfdata(interface);
	priv = hw->priv;

	rtl8xxxu_disable_device(hw);
	usb_set_intfdata(interface, NULL);

	ieee80211_unregister_hw(hw);

	kfree(priv->fw_data);
	mutex_destroy(&priv->usb_buf_mutex);
	mutex_destroy(&priv->h2c_mutex);

	usb_put_dev(priv->udev);
	ieee80211_free_hw(hw);

	wiphy_info(hw->wiphy, "disconnecting\n");
}

static struct usb_driver rtl8xxxu_driver = {
	.name = DRIVER_NAME,
	.probe = rtl8xxxu_probe,
	.disconnect = rtl8xxxu_disconnect,
	.id_table = dev_table,
	.disable_hub_initiated_lpm = 1,
};

static int __init rtl8xxxu_module_init(void)
{
	int res;

	res = usb_register(&rtl8xxxu_driver);
	if (res < 0)
		pr_err(DRIVER_NAME ": usb_register() failed (%i)\n", res);

	return res;
}

static void __exit rtl8xxxu_module_exit(void)
{
	usb_deregister(&rtl8xxxu_driver);
}

module_init(rtl8xxxu_module_init);
module_exit(rtl8xxxu_module_exit);
