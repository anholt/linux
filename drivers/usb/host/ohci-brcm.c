/*
 * ohci-brcm.c - OHCI host controller driver for Broadcom STB.
 *
 * Copyright (C) 2009 - 2017 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "ohci.h"

#define BRCM_DRIVER_DESC "OHCI Broadcom STB driver"

static const char hcd_name[] = "ohci-brcm";

#define hcd_to_ohci_priv(h) ((struct brcm_priv *)hcd_to_ohci(h)->priv)

struct brcm_priv {
	struct clk *clk;
	struct phy *phy;
};

static struct hc_driver __read_mostly ohci_brcm_hc_driver;

static const struct ohci_driver_overrides brcm_overrides __initconst = {
	.product_desc =	"Broadcom STB OHCI controller",
	.extra_priv_size = sizeof(struct brcm_priv),
};

static int ohci_brcm_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	struct resource *res_mem;
	struct brcm_priv *priv;
	int irq;
	int err;

	if (usb_disabled())
		return -ENODEV;

	err = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq error.\n");
		return -ENODEV;
	}

	/* initialize hcd */
	hcd = usb_create_hcd(&ohci_brcm_hc_driver,
			&pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "Failed to create hcd\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, hcd);
	priv = hcd_to_ohci_priv(hcd);

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "Clock not found in Device Tree\n");
		priv->clk = NULL;
	}
	err = clk_prepare_enable(priv->clk);
	if (err)
		goto err_hcd;

	priv->phy = devm_of_phy_get_by_index(&pdev->dev, pdev->dev.of_node, 0);
	if (IS_ERR(priv->phy)) {
		err = PTR_ERR(priv->phy);
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "USB Phy not found.\n");
		goto err_clk;
	}
	phy_init(priv->phy);

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hcd->regs = devm_ioremap_resource(&pdev->dev, res_mem);
	if (IS_ERR(hcd->regs)) {
		err = PTR_ERR(hcd->regs);
		goto err_phy;
	}
	hcd->rsrc_start = res_mem->start;
	hcd->rsrc_len = resource_size(res_mem);

	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err)
		goto err_phy;

	device_wakeup_enable(hcd->self.controller);

	platform_set_drvdata(pdev, hcd);

	return err;

err_phy:
	phy_exit(priv->phy);
err_clk:
	clk_disable(priv->clk);
err_hcd:
	usb_put_hcd(hcd);

	return err;

}

static int ohci_brcm_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);

	usb_remove_hcd(hcd);
	phy_exit(priv->phy);
	clk_disable(priv->clk);
	usb_put_hcd(hcd);
	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int ohci_brcm_suspend(struct device *dev)
{
	int ret;
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
	bool do_wakeup = device_may_wakeup(dev);

	ret = ohci_suspend(hcd, do_wakeup);
	clk_disable(priv->clk);
	return ret;
}

static int ohci_brcm_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
	int err;

	err = clk_enable(priv->clk);
	if (err)
		return err;
	ohci_resume(hcd, false);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(ohci_brcm_pm_ops, ohci_brcm_suspend,
		ohci_brcm_resume);

#ifdef CONFIG_OF
static const struct of_device_id brcm_ohci_of_match[] = {
	{ .compatible = "brcm,ohci-brcm", },
	{}
};

MODULE_DEVICE_TABLE(of, brcm_ohci_of_match);
#endif /* CONFIG_OF */

static struct platform_driver ohci_brcm_driver = {
	.probe		= ohci_brcm_probe,
	.remove		= ohci_brcm_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "ohci-brcm",
		.pm	= &ohci_brcm_pm_ops,
		.of_match_table = of_match_ptr(brcm_ohci_of_match),
	}
};

static int __init ohci_brcm_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	pr_info("%s: " BRCM_DRIVER_DESC "\n", hcd_name);

	ohci_init_driver(&ohci_brcm_hc_driver, &brcm_overrides);
	return platform_driver_register(&ohci_brcm_driver);
}
module_init(ohci_brcm_init);

static void __exit ohci_brcm_cleanup(void)
{
	platform_driver_unregister(&ohci_brcm_driver);
}
module_exit(ohci_brcm_cleanup);

MODULE_ALIAS("platform:ohci-brcm");
MODULE_DESCRIPTION(BRCM_DRIVER_DESC);
MODULE_AUTHOR("Al Cooper");
MODULE_LICENSE("GPL");
