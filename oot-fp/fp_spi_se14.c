// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bring up FP QUP SPI SE14 (spi@a98000) on Fairphone 5 pmOS.
 *
 * Steps:
 *  1) Un-reserve gpio56-59 (gpio-reserved-ranges blocks msm_pinmux_request)
 *  2) Set DT status=okay on spi@a98000
 *  3) of_platform_device_create → geni_spi probe (pinctrl qup16)
 *  4) Create SPI device and try chip-id read patterns
 *
 * No raw TLMM MMIO. No blocking sysfs bind.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/driver.h>
#include <linux/kprobes.h>
#include <linux/device.h>
#include <linux/string.h>

#define DRV "fp_spi_se14"
#define SE_PATH "/soc@0/geniqup@ac0000/spi@a98000"

static char *se_path = SE_PATH;
module_param(se_path, charp, 0644);

static int vdd_gpio = 60;
module_param(vdd_gpio, int, 0644);
static int reset_gpio = 35;
module_param(reset_gpio, int, 0644);
static char *tlmm_label = "f100000.pinctrl";
module_param(tlmm_label, charp, 0644);

static bool do_chip_id = true;
module_param(do_chip_id, bool, 0644);

typedef int (*of_update_property_t)(struct device_node *np, struct property *newprop);
static of_update_property_t of_update_property_fn;

static struct platform_device *g_spi_pdev;
static struct platform_device *g_gpio_pdev;
static struct gpiod_lookup_table *g_lookup;
static struct gpio_desc *g_vdd, *g_reset;
static struct spi_device *g_spi_dev;
static bool we_created_spi_pdev;

static int resolve_syms(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	unsigned long (*kln)(const char *);

	if (register_kprobe(&kp))
		return -ENOENT;
	kln = (void *)kp.addr;
	unregister_kprobe(&kp);
	of_update_property_fn = (void *)kln("of_update_property");
	return of_update_property_fn ? 0 : -ENOENT;
}

static struct gpio_chip *found_tlmm_chip;

static int match_tlmm_chip(struct gpio_chip *gc, const void *data)
{
	const char *label = data;

	if (gc->label && !strcmp(gc->label, label)) {
		found_tlmm_chip = gc;
		return 1;
	}
	return 0;
}

static int unreserve_fp_spi_pins(void)
{
	struct gpio_device *gdev;
	struct gpio_chip *chip;
	unsigned long *mask;
	unsigned int pins[] = { 56, 57, 58, 59 };
	unsigned int i;

	found_tlmm_chip = NULL;
	gdev = gpio_device_find(tlmm_label, match_tlmm_chip);
	if (!gdev || !found_tlmm_chip) {
		pr_err(DRV ": gpiochip '%s' not found\n", tlmm_label);
		if (gdev)
			gpio_device_put(gdev);
		return -ENODEV;
	}
	chip = found_tlmm_chip;
	mask = (unsigned long *)gpiochip_query_valid_mask(chip);
	if (!mask) {
		pr_warn(DRV ": no valid_mask\n");
		gpio_device_put(gdev);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		if (pins[i] >= chip->ngpio)
			continue;
		set_bit(pins[i], mask);
		pr_info(DRV ": un-reserved gpio%u valid=%d\n",
			pins[i], gpiochip_line_is_valid(chip, pins[i]));
	}
	gpio_device_put(gdev);
	return 0;
}

static int set_status_okay(struct device_node *np)
{
	struct property *newp;
	static const char okay[] = "okay";

	if (of_device_is_available(np)) {
		pr_info(DRV ": already available\n");
		return 0;
	}

	newp = kzalloc(sizeof(*newp), GFP_KERNEL);
	if (!newp)
		return -ENOMEM;
	newp->name = kstrdup("status", GFP_KERNEL);
	newp->length = sizeof(okay);
	newp->value = kmemdup(okay, sizeof(okay), GFP_KERNEL);
	if (!newp->name || !newp->value) {
		kfree(newp->name);
		kfree(newp->value);
		kfree(newp);
		return -ENOMEM;
	}
	return of_update_property_fn(np, newp);
}

static int claim_power_gpios(void)
{
	struct gpiod_lookup_table *t;

	t = kzalloc(struct_size(t, table, 3), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev_id = DRV;
	t->table[0] = GPIO_LOOKUP(tlmm_label, vdd_gpio, "vdd", GPIO_ACTIVE_HIGH);
	t->table[1] = GPIO_LOOKUP(tlmm_label, reset_gpio, "reset", GPIO_ACTIVE_HIGH);
	gpiod_add_lookup_table(t);
	g_lookup = t;

	g_gpio_pdev = platform_device_register_simple(DRV, -1, NULL, 0);
	if (IS_ERR(g_gpio_pdev)) {
		int ret = PTR_ERR(g_gpio_pdev);

		g_gpio_pdev = NULL;
		return ret;
	}

	g_vdd = gpiod_get(&g_gpio_pdev->dev, "vdd", GPIOD_OUT_HIGH);
	if (IS_ERR(g_vdd)) {
		pr_err(DRV ": vdd %ld\n", PTR_ERR(g_vdd));
		g_vdd = NULL;
		return PTR_ERR(g_vdd);
	}
	g_reset = gpiod_get(&g_gpio_pdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(g_reset)) {
		pr_err(DRV ": reset %ld\n", PTR_ERR(g_reset));
		g_reset = NULL;
		return PTR_ERR(g_reset);
	}

	/* Android: reset L → VDD H → reset H L H leave H */
	gpiod_set_value_cansleep(g_reset, 0);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(g_vdd, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(g_reset, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(g_reset, 0);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(g_reset, 1);
	usleep_range(50000, 60000);
	pr_info(DRV ": power/reset OK (reset HIGH)\n");
	return 0;
}

static void release_power_gpios(void)
{
	if (g_reset) {
		gpiod_set_value_cansleep(g_reset, 0);
		gpiod_put(g_reset);
		g_reset = NULL;
	}
	if (g_vdd) {
		gpiod_set_value_cansleep(g_vdd, 0);
		gpiod_put(g_vdd);
		g_vdd = NULL;
	}
	if (g_gpio_pdev) {
		platform_device_unregister(g_gpio_pdev);
		g_gpio_pdev = NULL;
	}
	if (g_lookup) {
		gpiod_remove_lookup_table(g_lookup);
		kfree(g_lookup);
		g_lookup = NULL;
	}
}

static void try_chip_id_reads(struct spi_device *spi)
{
	static const u8 cmds[] = { 0x60, 0x90, 0xa0, 0xf0, 0x00, 0x80, 0xc0, 0x40 };
	u8 cmd, data[4];
	int ret, i;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi_setup(spi);

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		cmd = cmds[i];
		memset(data, 0xff, sizeof(data));
		ret = spi_write_then_read(spi, &cmd, 1, data, 4);
		pr_info(DRV ": wtr cmd=0x%02x ret=%d id=%02x%02x%02x%02x\n",
			cmd, ret, data[0], data[1], data[2], data[3]);
	}
}

static int match_spi_controller(struct device *dev, void *data)
{
	struct spi_controller **out = data;

	if (!dev->class || !dev->class->name)
		return 0;
	if (strcmp(dev->class->name, "spi_master") != 0)
		return 0;
	*out = container_of(dev, struct spi_controller, dev);
	get_device(dev);
	return 1;
}

static int setup_spi_client(struct device *pdev_dev)
{
	struct spi_controller *ctrl = NULL;
	struct spi_board_info info = {
		.modalias = "fp-ft9362",
		.max_speed_hz = 4000000,
		.mode = SPI_MODE_0,
		.chip_select = 0,
	};
	struct spi_device *spi;

	device_for_each_child(pdev_dev, &ctrl, match_spi_controller);
	if (!ctrl) {
		pr_err(DRV ": no spi_master under %s\n", dev_name(pdev_dev));
		return -ENODEV;
	}

	info.bus_num = ctrl->bus_num;
	spi = spi_new_device(ctrl, &info);
	put_device(&ctrl->dev);
	if (IS_ERR(spi)) {
		pr_err(DRV ": spi_new_device %ld\n", PTR_ERR(spi));
		return PTR_ERR(spi);
	}
	g_spi_dev = spi;
	pr_info(DRV ": SPI device %s bus=%d\n", dev_name(&spi->dev), info.bus_num);
	if (do_chip_id)
		try_chip_id_reads(spi);
	return 0;
}

static int __init fp_spi_se14_init(void)
{
	struct device_node *np, *parent = NULL;
	struct platform_device *pdev, *parent_pdev = NULL;
	struct device *parent_dev = NULL;
	int ret, tries;

	pr_info(DRV ": init path=%s\n", se_path);

	ret = resolve_syms();
	if (ret) {
		pr_err(DRV ": kallsyms of_update_property failed\n");
		return ret;
	}

	ret = unreserve_fp_spi_pins();
	if (ret)
		return ret;

	ret = claim_power_gpios();
	if (ret)
		pr_warn(DRV ": power gpios %d (continuing)\n", ret);

	np = of_find_node_by_path(se_path);
	if (!np) {
		release_power_gpios();
		return -ENODEV;
	}

	ret = set_status_okay(np);
	if (ret) {
		pr_err(DRV ": status okay failed %d\n", ret);
		of_node_put(np);
		release_power_gpios();
		return ret;
	}

	of_node_clear_flag(np, OF_POPULATED);
	of_node_clear_flag(np, OF_POPULATED_BUS);

	pdev = of_find_device_by_node(np);
	if (pdev) {
		pr_info(DRV ": device exists %s driver=%s\n",
			dev_name(&pdev->dev),
			pdev->dev.driver ? pdev->dev.driver->name : "none");
		g_spi_pdev = pdev;
		we_created_spi_pdev = false;
		/* keep get_device from of_find_device_by_node */
	} else {
		parent = of_get_parent(np);
		if (parent) {
			parent_pdev = of_find_device_by_node(parent);
			if (parent_pdev)
				parent_dev = &parent_pdev->dev;
		}
		pdev = of_platform_device_create(np, NULL, parent_dev);
		if (parent_pdev)
			put_device(&parent_pdev->dev);
		of_node_put(parent);
		if (IS_ERR_OR_NULL(pdev)) {
			pr_err(DRV ": create failed\n");
			of_node_put(np);
			release_power_gpios();
			return -EIO;
		}
		g_spi_pdev = pdev;
		we_created_spi_pdev = true;
		pr_info(DRV ": created %s\n", dev_name(&pdev->dev));
	}

	/* Wait for geni_spi (or force attach once, non-sysfs) */
	for (tries = 0; tries < 20; tries++) {
		if (g_spi_pdev->dev.driver)
			break;
		if (tries == 5) {
			pr_info(DRV ": device_attach attempt\n");
			ret = device_attach(&g_spi_pdev->dev);
			pr_info(DRV ": device_attach ret=%d\n", ret);
		}
		msleep(100);
	}

	pr_info(DRV ": final driver=%s\n",
		g_spi_pdev->dev.driver ? g_spi_pdev->dev.driver->name : "NONE");

	if (g_spi_pdev->dev.driver) {
		ret = setup_spi_client(&g_spi_pdev->dev);
		if (ret)
			pr_warn(DRV ": SPI client %d\n", ret);
	} else {
		pr_err(DRV ": geni_spi did not bind — see dmesg pinctrl/geni\n");
	}

	of_node_put(np);
	return 0;
}

static void __exit fp_spi_se14_exit(void)
{
	if (g_spi_dev) {
		spi_unregister_device(g_spi_dev);
		g_spi_dev = NULL;
	}
	if (g_spi_pdev) {
		if (we_created_spi_pdev)
			of_platform_device_destroy(&g_spi_pdev->dev, NULL);
		else
			put_device(&g_spi_pdev->dev);
		g_spi_pdev = NULL;
	}
	release_power_gpios();
	pr_info(DRV ": exit\n");
}

module_init(fp_spi_se14_init);
module_exit(fp_spi_se14_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FP5 SE14 SPI bring-up");
MODULE_VERSION("0.2");
