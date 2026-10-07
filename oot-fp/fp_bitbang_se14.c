// SPDX-License-Identifier: GPL-2.0-only
/*
 * M4ba: REE bitbang SPI on FP SE14 pins (gpio56–59) without pinmux/geni.
 *
 * Safety: un-reserve valid_mask only; request as GPIO (func0). NO pinctrl_select,
 * NO geni_spi, NO of_update — those hang this unit (M4aj/M4al).
 *
 * Load after: L2 VDD on; preferably rmmod qsee so TZ releases SPI bus.
 * Goal: chip-id and/or raw frame bytes for TA image-buffer feed.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define DRV "fp_bitbang_se14"

static char *tlmm_label = "f100000.pinctrl";
module_param(tlmm_label, charp, 0644);

/* Pin offsets within TLMM gpiochip (not global gpio numbers) */
static int pin_cs = 59;
module_param(pin_cs, int, 0644);
static int pin_clk = 58;
module_param(pin_clk, int, 0644);
static int pin_mosi = 56;
module_param(pin_mosi, int, 0644);
static int pin_miso = 57;
module_param(pin_miso, int, 0644);

/* SPI mode 0 (CPOL=0 CPHA=0) matches Android device.spi_mode */
static int half_period_us = 2; /* ~250 kHz bit rate */
module_param(half_period_us, int, 0644);

static int try_permutations = 1;
module_param(try_permutations, int, 0644);
MODULE_PARM_DESC(try_permutations, "try data-pin permutations for chip-id (default 1)");

static struct gpio_chip *found_chip;
static struct gpio_desc *g_cs, *g_clk, *g_mosi, *g_miso;
static struct gpio_device *g_gdev;

static int match_chip(struct gpio_chip *gc, const void *data)
{
	const char *label = data;

	if (gc->label && !strcmp(gc->label, label)) {
		found_chip = gc;
		return 1;
	}
	return 0;
}

static int unreserve_pins(void)
{
	unsigned long *mask;
	unsigned int pins[] = { 56, 57, 58, 59 };
	unsigned int i;

	found_chip = NULL;
	g_gdev = gpio_device_find(tlmm_label, match_chip);
	if (!g_gdev || !found_chip) {
		pr_err(DRV ": gpiochip '%s' not found\n", tlmm_label);
		if (g_gdev)
			gpio_device_put(g_gdev);
		g_gdev = NULL;
		return -ENODEV;
	}
	mask = (unsigned long *)gpiochip_query_valid_mask(found_chip);
	if (!mask) {
		gpio_device_put(g_gdev);
		g_gdev = NULL;
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		if (pins[i] < found_chip->ngpio)
			set_bit(pins[i], mask);
		pr_info(DRV ": unreserve gpio%u valid=%d\n", pins[i],
			gpiochip_line_is_valid(found_chip, pins[i]));
	}
	return 0;
}

static struct gpio_desc *req_pin(unsigned int offset, const char *name,
				 enum gpiod_flags flags)
{
	struct gpio_desc *d;

	if (!found_chip || offset >= found_chip->ngpio)
		return ERR_PTR(-EINVAL);
	d = gpiochip_request_own_desc(found_chip, offset, name,
				      GPIO_LOOKUP_FLAGS_DEFAULT, flags);
	if (IS_ERR(d))
		pr_err(DRV ": request %s offset=%u: %ld\n", name, offset,
		       PTR_ERR(d));
	else
		pr_info(DRV ": claimed %s offset=%u\n", name, offset);
	return d;
}

static void bb_delay(void)
{
	if (half_period_us > 0)
		udelay(half_period_us);
	else
		ndelay(200);
}

/* Mode 0: sample MISO on rising CLK; MOSI set before rising */
static u8 bb_xfer_byte(u8 out)
{
	u8 in = 0;
	int b;

	for (b = 7; b >= 0; b--) {
		gpiod_set_value_cansleep(g_mosi, (out >> b) & 1);
		bb_delay();
		gpiod_set_value_cansleep(g_clk, 1);
		bb_delay();
		if (gpiod_get_value_cansleep(g_miso))
			in |= (u8)(1u << b);
		gpiod_set_value_cansleep(g_clk, 0);
		bb_delay();
	}
	return in;
}

static void bb_cs(bool active)
{
	/* Active low CS */
	gpiod_set_value_cansleep(g_cs, active ? 0 : 1);
	bb_delay();
}

static int bb_write_then_read(const u8 *tx, unsigned int ntx, u8 *rx,
			      unsigned int nrx)
{
	unsigned int i;

	if (!g_cs || !g_clk || !g_mosi || !g_miso)
		return -ENODEV;
	bb_cs(true);
	for (i = 0; i < ntx; i++)
		(void)bb_xfer_byte(tx[i]);
	for (i = 0; i < nrx; i++)
		rx[i] = bb_xfer_byte(0x00);
	bb_cs(false);
	return 0;
}

static bool looks_like_id(const u8 *d, unsigned int n)
{
	unsigned int i, nz = 0;
	u32 v;

	if (n < 2)
		return false;
	for (i = 0; i < n; i++) {
		if (d[i] && d[i] != 0xff)
			nz++;
	}
	if (!nz)
		return false;
	/* Android: id=0x98FF9024 */
	if (n >= 4) {
		v = ((u32)d[0] << 24) | ((u32)d[1] << 16) | ((u32)d[2] << 8) |
		    d[3];
		if ((v & 0xff000000) == 0x98000000)
			return true;
		if ((v & 0x000000ff) == 0x98)
			return true;
		/* also accept non-trivial 4-byte pattern not all same */
		if (d[0] != d[1] || d[1] != d[2] || d[2] != d[3])
			if (nz >= 2)
				return true;
	}
	return nz >= 2;
}

static int try_chip_id_cmds(void)
{
	static const u8 cmds[] = { 0xa0, 0x90, 0xf0, 0x60, 0x80, 0x00, 0x9f,
				   0x9e, 0xae, 0xaf };
	u8 rx[8];
	unsigned int i;
	int hits = 0;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		u8 tx = cmds[i];

		memset(rx, 0, sizeof(rx));
		if (bb_write_then_read(&tx, 1, rx, 4))
			continue;
		pr_info(DRV ": cmd=0x%02x rx=%02x %02x %02x %02x\n", tx, rx[0],
			rx[1], rx[2], rx[3]);
		if (looks_like_id(rx, 4)) {
			pr_info(DRV ": *** CHIP-ID CANDIDATE cmd=0x%02x id=%02x%02x%02x%02x ***\n",
				tx, rx[0], rx[1], rx[2], rx[3]);
			hits++;
		}
	}
	/* duplex: clock out 8 zeros with CS low (some chips auto-stream) */
	memset(rx, 0, sizeof(rx));
	bb_cs(true);
	for (i = 0; i < 8; i++)
		rx[i] = bb_xfer_byte(0x00);
	bb_cs(false);
	pr_info(DRV ": stream8 %*ph\n", 8, rx);
	if (looks_like_id(rx, 4)) {
		pr_info(DRV ": *** CHIP-ID CANDIDATE stream ***\n");
		hits++;
	}
	return hits;
}

static void free_pins(void)
{
	if (g_cs) {
		gpiochip_free_own_desc(g_cs);
		g_cs = NULL;
	}
	if (g_clk) {
		gpiochip_free_own_desc(g_clk);
		g_clk = NULL;
	}
	if (g_mosi) {
		gpiochip_free_own_desc(g_mosi);
		g_mosi = NULL;
	}
	if (g_miso) {
		gpiochip_free_own_desc(g_miso);
		g_miso = NULL;
	}
}

static int claim_map(int cs, int clk, int mosi, int miso)
{
	free_pins();
	g_cs = req_pin(cs, "fp_cs", GPIOD_OUT_HIGH);
	if (IS_ERR(g_cs))
		return PTR_ERR(g_cs);
	g_clk = req_pin(clk, "fp_clk", GPIOD_OUT_LOW);
	if (IS_ERR(g_clk))
		return PTR_ERR(g_clk);
	g_mosi = req_pin(mosi, "fp_mosi", GPIOD_OUT_LOW);
	if (IS_ERR(g_mosi))
		return PTR_ERR(g_mosi);
	g_miso = req_pin(miso, "fp_miso", GPIOD_IN);
	if (IS_ERR(g_miso))
		return PTR_ERR(g_miso);
	pr_info(DRV ": map CS=%d CLK=%d MOSI=%d MISO=%d\n", cs, clk, mosi,
		miso);
	return 0;
}

static int __init fp_bitbang_init(void)
{
	int ret, hits = 0;
	/* data-clk group permutations for 56/57/58 + CS=59 */
	static const int maps[][3] = {
		{ 56, 57, 58 }, /* mosi miso clk */
		{ 57, 56, 58 },
		{ 58, 56, 57 }, /* clk mosi miso */
		{ 56, 58, 57 },
		{ 57, 58, 56 },
		{ 58, 57, 56 },
	};
	unsigned int mi, nmap;

	pr_info(DRV ": init (no pinmux/geni; bitbang only)\n");
	ret = unreserve_pins();
	if (ret)
		return ret;

	nmap = try_permutations ? ARRAY_SIZE(maps) : 1;
	for (mi = 0; mi < nmap; mi++) {
		int mosi = maps[mi][0];
		int miso = maps[mi][1];
		int clk = maps[mi][2];

		/* allow module params to override first map */
		if (mi == 0) {
			mosi = pin_mosi;
			miso = pin_miso;
			clk = pin_clk;
		}
		ret = claim_map(pin_cs, clk, mosi, miso);
		if (ret) {
			pr_err(DRV ": claim_map failed %d\n", ret);
			continue;
		}
		hits = try_chip_id_cmds();
		if (hits > 0) {
			pr_info(DRV ": *** BITBANG SPI ALIVE hits=%d map CS=%d CLK=%d MOSI=%d MISO=%d ***\n",
				hits, pin_cs, clk, mosi, miso);
			/* keep pins claimed for optional follow-up */
			if (g_gdev)
				gpio_device_put(g_gdev);
			g_gdev = NULL;
			return 0;
		}
		pr_info(DRV ": map failed chip-id; try next\n");
	}

	pr_info(DRV ": no chip-id candidate (pins may still be SPI-muxed by TZ, or power off)\n");
	free_pins();
	if (g_gdev) {
		gpio_device_put(g_gdev);
		g_gdev = NULL;
	}
	/* still return 0 so dmesg evidence is kept; module stays loadable */
	return 0;
}

static void __exit fp_bitbang_exit(void)
{
	free_pins();
	if (g_gdev) {
		gpio_device_put(g_gdev);
		g_gdev = NULL;
	}
	pr_info(DRV ": exit\n");
}

module_init(fp_bitbang_init);
module_exit(fp_bitbang_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("REE bitbang SPI for FP SE14 without pinmux hang");
MODULE_VERSION("0.1");
