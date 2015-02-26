/*
 *  Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Defines a power domain module for the power control available from
 * the BCM2835 firmware.
 */

#include <dt-bindings/arm/bcm2835_mbox_power.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

struct bcm_mbox_power {
	struct genpd_onecell_data genpd_xlate;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct completion c;
	u32 enabled;
};

static DEFINE_MUTEX(bcm28356_mbox_power_lock);
static struct bcm_mbox_power *mbox_power;

struct bcm_mbox_power_domain {
	u32 bit;
	struct generic_pm_domain base;
};

static void response_callback(struct mbox_client *cl, void *msg)
{
	complete(&mbox_power->c);
}

/*
 * Sends a message to the power channel to set which power domains are
 * enabled.
 *
 * Note that this interface is also available through the property
 * channel, but this is simpler to use.
 */
static int bcm_mbox_set_power(uint32_t power_enables)
{
	int ret;

	reinit_completion(&mbox_power->c);
	ret = mbox_send_message(mbox_power->chan, &power_enables);
	if (ret >= 0) {
		wait_for_completion(&mbox_power->c);
		ret = 0;
	}

	return ret;
}


static int bcm2835_mbox_power_on(struct generic_pm_domain *domain)
{
	struct bcm_mbox_power_domain *bcm_domain =
		container_of(domain, struct bcm_mbox_power_domain, base);
	int ret;

	mutex_lock(&bcm28356_mbox_power_lock);
	mbox_power->enabled |= bcm_domain->bit;
	ret = bcm_mbox_set_power(mbox_power->enabled);
	mutex_unlock(&bcm28356_mbox_power_lock);

	return ret;
}

static int bcm2835_mbox_power_off(struct generic_pm_domain *domain)
{
	struct bcm_mbox_power_domain *bcm_domain =
		container_of(domain, struct bcm_mbox_power_domain, base);
	int ret;

	mutex_lock(&bcm28356_mbox_power_lock);
	mbox_power->enabled &= ~bcm_domain->bit;
	ret = bcm_mbox_set_power(mbox_power->enabled);
	mutex_unlock(&bcm28356_mbox_power_lock);

	return ret;
}

struct bcm_mbox_power_domain bcm2835_mbox_power_domain_sdcard = {
	.bit = (1 << 4),
	.base = {
		.name = "SDCARD",
		.power_off = bcm2835_mbox_power_off,
		.power_on = bcm2835_mbox_power_on,
	}
};

struct bcm_mbox_power_domain bcm2835_mbox_power_domain_usb = {
	.bit = (1 << 7),
	.base = {
		.name = "USB",
		.power_off = bcm2835_mbox_power_off,
		.power_on = bcm2835_mbox_power_on,
	}
};

struct bcm_mbox_power_domain bcm2835_mbox_power_domain_dsi = {
	.bit = (1 << 13),
	.base = {
		.name = "DSI",
		.power_off = bcm2835_mbox_power_off,
		.power_on = bcm2835_mbox_power_on,
	}
};

static struct generic_pm_domain *bcm2835_mbox_power_domains[] = {
	[POWER_DOMAIN_SDCARD] = &bcm2835_mbox_power_domain_sdcard.base,
	[POWER_DOMAIN_USB] = &bcm2835_mbox_power_domain_usb.base,
	[POWER_DOMAIN_DSI] = &bcm2835_mbox_power_domain_dsi.base,
};

static int bcm2835_mbox_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	int i;

	mbox_power = devm_kzalloc(dev, sizeof(*mbox_power), GFP_KERNEL);
	if (!mbox_power)
		return -ENOMEM;

	mbox_power->cl.dev = dev;
	mbox_power->cl.rx_callback = response_callback;
	mbox_power->cl.tx_block = true;

	mbox_power->chan = mbox_request_channel(&mbox_power->cl, 0);
	if (IS_ERR(mbox_power->chan)) {
		ret = PTR_ERR(mbox_power->chan);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get mbox channel: %d\n", ret);
		goto fail;
	}

	init_completion(&mbox_power->c);

	platform_set_drvdata(pdev, mbox_power);

	mbox_power->genpd_xlate.domains =
		bcm2835_mbox_power_domains;
	mbox_power->genpd_xlate.num_domains =
		ARRAY_SIZE(bcm2835_mbox_power_domains);

	for (i = 0; i < ARRAY_SIZE(bcm2835_mbox_power_domains); i++)
		pm_genpd_init(bcm2835_mbox_power_domains[i], NULL, true);

	of_genpd_add_provider_onecell(dev->of_node, &mbox_power->genpd_xlate);

	return ret;
fail:
	mbox_power = NULL;
	return ret;
}

static int bcm2835_mbox_power_remove(struct platform_device *pdev)
{
	bcm_mbox_set_power(0);

	mbox_free_channel(mbox_power->chan);

	return 0;
}

static const struct of_device_id bcm2835_mbox_power_of_match[] = {
	{ .compatible = "brcm,bcm2835-mbox-power", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_mbox_power_of_match);

static struct platform_driver bcm2835_mbox_power_driver = {
	.driver = {
		.name = "bcm2835-mbox-power",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_mbox_power_of_match,
	},
	.probe		= bcm2835_mbox_power_probe,
	.remove		= bcm2835_mbox_power_remove,
};
module_platform_driver(bcm2835_mbox_power_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("BCM2835 mailbox power channel");
MODULE_LICENSE("GPL v2");
