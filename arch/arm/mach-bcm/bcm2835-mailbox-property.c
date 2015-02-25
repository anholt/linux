/*
 *  Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Defines a module for accessing the property channel of the
 * BCM2835 mailbox.
 *
 * The property interface lets you submit a bus address for a
 * sequence of command packets to the firmware.
 */

#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/mailbox-property.h>

static DEFINE_MUTEX(property_lock);

struct bcm_mbox_property {
	struct device *dev;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct completion c;
};

static struct bcm_mbox_property *mbox_property;

static void response_callback(struct mbox_client *cl, void *msg)
{
	complete(&mbox_property->c);
}

/*
 * Requests the actual property transaction through the BCM2835
 * mailbox driver.
 */
static int
bcm_mbox_property_transaction(dma_addr_t bus_addr)
{
	int ret;

	reinit_completion(&mbox_property->c);
	ret = mbox_send_message(mbox_property->chan, &bus_addr);
	if (ret >= 0) {
		wait_for_completion(&mbox_property->c);
		ret = 0;
	} else {
		dev_err(mbox_property->dev, "mbox_send_message returned %d\n",
			ret);
	}

	return ret;
}

/*
 * Submits a set of concatenated tags to the VPU firmware through the
 * mailbox property interface.
 *
 * The buffer header and the ending tag are added by this function and
 * don't need to be supplied, just the actual tags for your operation.
 * See struct bcm_mbox_property_tag_header for the per-tag structure.
 */
int bcm_mbox_property(void *data, size_t tag_size)
{
	size_t size = tag_size + 12;
	u32 *buf;
	dma_addr_t bus_addr;
	int ret = 0;

	if (!mbox_property)
		return -EPROBE_DEFER;

	/* Packets are processed a dword at a time. */
	if (size & 3)
		return -EINVAL;

	buf = dma_alloc_coherent(NULL, PAGE_ALIGN(size), &bus_addr, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	/* The firmware will error out without parsing in this case. */
	WARN_ON(size >= 1024 * 1024);

	buf[0] = size;
	buf[1] = bcm_mbox_status_request;
	memcpy(&buf[2], data, tag_size);
	buf[size / 4 - 1] = bcm_mbox_property_end;
	wmb();

	mutex_lock(&property_lock);
	ret = bcm_mbox_property_transaction(bus_addr);
	mutex_unlock(&property_lock);

	rmb();
	memcpy(data, &buf[2], tag_size);
	if (ret == 0 && buf[1] != bcm_mbox_status_success) {
		/*
		 * The tag name here might not be the one causing the
		 * error, if there were multiple tags in the request.
		 * But single-tag is the most common, so go with it.
		 */
		dev_err(mbox_property->dev,
			"Request 0x%08x returned status 0x%08x\n",
			buf[2], buf[1]);
		ret = -EINVAL;
	}

	dma_free_coherent(NULL, PAGE_ALIGN(size), buf, bus_addr);

	return ret;
}
EXPORT_SYMBOL_GPL(bcm_mbox_property);


static int bcm2835_mbox_property_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;

	mbox_property = devm_kzalloc(dev, sizeof(*mbox_property), GFP_KERNEL);
	if (!mbox_property)
		return -ENOMEM;

	mbox_property->cl.dev = dev;
	mbox_property->cl.rx_callback = response_callback;
	mbox_property->cl.tx_block = true;

	mbox_property->chan = mbox_request_channel(&mbox_property->cl, 0);
	if (IS_ERR(mbox_property->chan)) {
		dev_err(dev, "Failed to get mbox channel\n");
		ret = PTR_ERR(mbox_property->chan);
		goto fail;
	}

	init_completion(&mbox_property->c);

	platform_set_drvdata(pdev, mbox_property);
	mbox_property->dev = dev;

	return ret;

fail:
	mbox_property = NULL;
	return ret;
}

static int bcm2835_mbox_property_remove(struct platform_device *pdev)
{
	mbox_free_channel(mbox_property->chan);

	return 0;
}

static const struct of_device_id bcm2835_mbox_property_of_match[] = {
	{ .compatible = "brcm,bcm2835-mbox-property", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_mbox_property_of_match);

static struct platform_driver bcm2835_mbox_property_driver = {
	.driver = {
		.name = "bcm2835-mbox-property",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_mbox_property_of_match,
	},
	.probe		= bcm2835_mbox_property_probe,
	.remove		= bcm2835_mbox_property_remove,
};
module_platform_driver(bcm2835_mbox_property_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("BCM2835 mailbox property channel");
MODULE_LICENSE("GPL v2");
