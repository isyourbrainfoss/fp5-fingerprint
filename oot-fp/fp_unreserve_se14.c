// SPDX-License-Identifier: GPL-2.0-only
/*
 * Un-reserve TLMM gpio56-59 only (FP SPI SE14 pins).
 * NO pinctrl_select, NO geni_spi, NO of_update — pinmux hang/reboot safe subset.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio/driver.h>
#include <linux/string.h>

#define DRV "fp_unreserve_se14"

static char *tlmm_label = "f100000.pinctrl";
module_param(tlmm_label, charp, 0644);

static struct gpio_chip *found_chip;

static int match_chip(struct gpio_chip *gc, const void *data)
{
	const char *label = data;

	if (gc->label && !strcmp(gc->label, label)) {
		found_chip = gc;
		return 1;
	}
	return 0;
}

static int __init fp_unreserve_init(void)
{
	struct gpio_device *gdev;
	unsigned long *mask;
	unsigned int pins[] = { 56, 57, 58, 59 };
	unsigned int i;

	pr_info(DRV ": un-reserve gpio56-59 only (no pinctrl)\n");
	found_chip = NULL;
	gdev = gpio_device_find(tlmm_label, match_chip);
	if (!gdev || !found_chip) {
		pr_err(DRV ": gpiochip '%s' not found\n", tlmm_label);
		if (gdev)
			gpio_device_put(gdev);
		return -ENODEV;
	}
	mask = (unsigned long *)gpiochip_query_valid_mask(found_chip);
	if (!mask) {
		pr_err(DRV ": no valid_mask\n");
		gpio_device_put(gdev);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		if (pins[i] >= found_chip->ngpio)
			continue;
		set_bit(pins[i], mask);
		pr_info(DRV ": gpio%u valid=%d\n", pins[i],
			gpiochip_line_is_valid(found_chip, pins[i]));
	}
	gpio_device_put(gdev);
	pr_info(DRV ": done — still no pinmux; TA SPI only\n");
	return 0;
}

static void __exit fp_unreserve_exit(void)
{
	pr_info(DRV ": exit (valid bits left set)\n");
}

module_init(fp_unreserve_init);
module_exit(fp_unreserve_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Un-reserve FP SPI TLMM pins only");
MODULE_VERSION("0.1");
