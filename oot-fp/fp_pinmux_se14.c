// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal: un-reserve gpio56-59 and pinmux them to qup16 for FP SPI SE14.
 * Does NOT create geni_spi device (that hung the SoC).
 * Goal: give TZ qsee_spi_open a correct pinmux while L2 votes clocks.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#define DRV "fp_pinmux_se14"

static char *tlmm_label = "f100000.pinctrl";
module_param(tlmm_label, charp, 0644);

static struct gpio_chip *found_chip;
static struct platform_device *g_pdev;
static struct pinctrl *g_pctl;
static struct pinctrl_map *g_maps;
static unsigned int g_maps_n;

static int match_chip(struct gpio_chip *gc, const void *data)
{
	const char *label = data;

	if (gc->label && !strcmp(gc->label, label)) {
		found_chip = gc;
		return 1;
	}
	return 0;
}

static int unreserve(void)
{
	struct gpio_device *gdev;
	unsigned long *mask;
	unsigned int pins[] = { 56, 57, 58, 59 };
	unsigned int i;

	found_chip = NULL;
	gdev = gpio_device_find(tlmm_label, match_chip);
	if (!gdev || !found_chip) {
		if (gdev)
			gpio_device_put(gdev);
		return -ENODEV;
	}
	mask = (unsigned long *)gpiochip_query_valid_mask(found_chip);
	if (!mask) {
		gpio_device_put(gdev);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		set_bit(pins[i], mask);
		pr_info(DRV ": un-reserved gpio%u valid=%d\n",
			pins[i], gpiochip_line_is_valid(found_chip, pins[i]));
	}
	gpio_device_put(gdev);
	return 0;
}

static int apply_pinmux(void)
{
	static const char *const pins[] = {
		"gpio56", "gpio57", "gpio58", "gpio59"
	};
	struct pinctrl_state *st;
	unsigned int i, n = ARRAY_SIZE(pins);
	int ret;

	g_maps = kcalloc(n, sizeof(*g_maps), GFP_KERNEL);
	if (!g_maps)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		g_maps[i].dev_name = DRV;
		g_maps[i].name = "spi";
		g_maps[i].type = PIN_MAP_TYPE_MUX_GROUP;
		g_maps[i].ctrl_dev_name = "f100000.pinctrl";
		g_maps[i].data.mux.group = pins[i];
		g_maps[i].data.mux.function = "qup16";
	}
	g_maps_n = n;

	ret = pinctrl_register_mappings(g_maps, n);
	if (ret) {
		pr_err(DRV ": register_mappings %d\n", ret);
		kfree(g_maps);
		g_maps = NULL;
		return ret;
	}

	g_pdev = platform_device_register_simple(DRV, -1, NULL, 0);
	if (IS_ERR(g_pdev)) {
		ret = PTR_ERR(g_pdev);
		g_pdev = NULL;
		pinctrl_unregister_mappings(g_maps);
		kfree(g_maps);
		g_maps = NULL;
		return ret;
	}

	g_pctl = pinctrl_get(&g_pdev->dev);
	if (IS_ERR(g_pctl)) {
		ret = PTR_ERR(g_pctl);
		pr_err(DRV ": pinctrl_get %d\n", ret);
		g_pctl = NULL;
		return ret;
	}
	st = pinctrl_lookup_state(g_pctl, "spi");
	if (IS_ERR(st)) {
		ret = PTR_ERR(st);
		pr_err(DRV ": lookup spi %d\n", ret);
		return ret;
	}
	ret = pinctrl_select_state(g_pctl, st);
	pr_info(DRV ": pinctrl_select_state(spi/qup16) = %d\n", ret);
	return ret;
}

static int __init fp_pinmux_init(void)
{
	int ret;

	pr_info(DRV ": un-reserve + pinmux gpio56-59 → qup16\n");
	ret = unreserve();
	if (ret) {
		pr_err(DRV ": unreserve %d\n", ret);
		return ret;
	}
	ret = apply_pinmux();
	if (ret)
		pr_err(DRV ": pinmux %d\n", ret);
	else
		pr_info(DRV ": OK — now load L2 clocks + qsee_fingerpr\n");
	/* Keep module loaded to hold pinctrl state */
	return 0;
}

static void __exit fp_pinmux_exit(void)
{
	if (g_pctl) {
		pinctrl_put(g_pctl);
		g_pctl = NULL;
	}
	if (g_pdev) {
		platform_device_unregister(g_pdev);
		g_pdev = NULL;
	}
	if (g_maps) {
		pinctrl_unregister_mappings(g_maps);
		kfree(g_maps);
		g_maps = NULL;
	}
	pr_info(DRV ": exit\n");
}

module_init(fp_pinmux_init);
module_exit(fp_pinmux_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Un-reserve FP SPI pins and pinmux qup16");
MODULE_VERSION("0.1");
