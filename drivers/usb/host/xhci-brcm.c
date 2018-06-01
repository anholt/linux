/*
 * xhci-brcm.c - xHCI host controller driver for Broadcom STB.
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

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/phy/phy.h>

#include "xhci.h"

static struct hc_driver __read_mostly xhci_brcm_hc_driver;

#define BRCM_DRIVER_DESC "xHCI Broadcom STB driver"
#define BRCM_DRIVER_NAME "xhci-brcm"

static void xhci_brcm_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	/*
	 * As of now platform drivers don't provide MSI support so we ensure
	 * here that the generic code does not try to make a pci_dev from our
	 * dev struct in order to setup MSI
	 */
	xhci->quirks |= XHCI_PLAT;

	/*
	 * The Broadcom XHCI core does not support save/restore state
	 * so we need to reset on resume.
	 */
	xhci->quirks |= XHCI_RESET_ON_RESUME;
}

/* called during probe() after chip reset completes */
static int xhci_brcm_setup(struct usb_hcd *hcd)
{
	return xhci_gen_setup(hcd, xhci_brcm_quirks);
}

static const struct xhci_driver_overrides brcm_overrides __initconst = {

	.reset =	xhci_brcm_setup,
};

static int xhci_brcm_probe(struct platform_device *pdev)
{
	const struct hc_driver	*driver;
	struct xhci_hcd		*xhci;
	struct resource         *res;
	struct usb_hcd		*hcd;
	struct clk              *clk;
	int			ret;
	int			irq;

	if (usb_disabled())
		return -ENODEV;

	driver = &xhci_brcm_hc_driver;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -ENODEV;

	/* Try to set 64-bit DMA first */
	if (WARN_ON(!pdev->dev.dma_mask))
		/* Platform did not initialize dma_mask */
		ret = dma_coerce_mask_and_coherent(&pdev->dev,
						   DMA_BIT_MASK(64));
	else
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	/* If seting 64-bit DMA mask fails, fall back to 32-bit DMA mask */
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	hcd = usb_create_hcd(driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		return -ENOMEM;
		goto disable_runtime;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hcd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		goto put_hcd;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	/*
	 * Not all platforms have a clk so it is not an error if the
	 * clock does not exists.
	 */
	clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(clk)) {
		ret = clk_prepare_enable(clk);
		if (ret)
			goto put_hcd;
	}

	xhci = hcd_to_xhci(hcd);

	device_wakeup_enable(hcd->self.controller);

	xhci->clk = clk;
	xhci->main_hcd = hcd;
	xhci->shared_hcd = usb_create_shared_hcd(driver, &pdev->dev,
			dev_name(&pdev->dev), hcd);
	if (!xhci->shared_hcd) {
		ret = -ENOMEM;
		goto disable_clk;
	}

	/* imod_interval is the interrupt moderation value in nanoseconds. */
	xhci->imod_interval = 40000;
	device_property_read_u32(&pdev->dev, "imod-interval-ns",
				 &xhci->imod_interval);

	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto put_usb3_hcd;

	ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
	if (ret)
		goto dealloc_usb2_hcd;

	device_enable_async_suspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	/*
	 * Prevent runtime pm from being on as default, users should enable
	 * runtime pm using power/control in sysfs.
	 */
	pm_runtime_forbid(&pdev->dev);

	return 0;

dealloc_usb2_hcd:
	usb_remove_hcd(hcd);

put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);

disable_clk:
	if (!IS_ERR(clk))
		clk_disable_unprepare(clk);

put_hcd:
	usb_put_hcd(hcd);

disable_runtime:
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int xhci_brcm_remove(struct platform_device *dev)
{
	struct usb_hcd	*hcd = platform_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);

	xhci->xhc_state |= XHCI_STATE_REMOVING;

	usb_remove_hcd(xhci->shared_hcd);
	usb_remove_hcd(hcd);
	usb_put_hcd(xhci->shared_hcd);

	clk_disable(xhci->clk);
	usb_put_hcd(hcd);

	pm_runtime_set_suspended(&dev->dev);
	pm_runtime_disable(&dev->dev);

	return 0;
}

static int __maybe_unused xhci_brcm_suspend(struct device *dev)
{
	int ret;
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);

	ret = xhci_suspend(xhci, device_may_wakeup(dev));
	clk_disable(xhci->clk);
	return ret;
}

static int __maybe_unused xhci_brcm_resume(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	int err;

	err = clk_enable(xhci->clk);
	if (err)
		return err;
	return xhci_resume(xhci, 0);
}

static int __maybe_unused xhci_brcm_runtime_suspend(struct device *dev)
{
	struct usb_hcd  *hcd = dev_get_drvdata(dev);
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	return xhci_suspend(xhci, true);
}

static int __maybe_unused xhci_brcm_runtime_resume(struct device *dev)
{
	struct usb_hcd  *hcd = dev_get_drvdata(dev);
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	return xhci_resume(xhci, 0);
}

static const struct dev_pm_ops xhci_brcm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(xhci_brcm_suspend, xhci_brcm_resume)

	SET_RUNTIME_PM_OPS(xhci_brcm_runtime_suspend,
			   xhci_brcm_runtime_resume,
			   NULL)
};

#ifdef CONFIG_OF
static const struct of_device_id brcm_xhci_of_match[] = {
	{ .compatible = "brcm,xhci-brcm-v2" },
	{ },
};
MODULE_DEVICE_TABLE(of, brcm_xhci_of_match);
#endif

static struct platform_driver xhci_brcm_driver = {
	.probe	= xhci_brcm_probe,
	.remove	= xhci_brcm_remove,
	.driver	= {
		.name = BRCM_DRIVER_NAME,
		.pm = &xhci_brcm_pm_ops,
		.of_match_table = of_match_ptr(brcm_xhci_of_match),
	},
};

static int __init xhci_brcm_init(void)
{
	pr_info("%s: " BRCM_DRIVER_DESC "\n", BRCM_DRIVER_NAME);
	xhci_init_driver(&xhci_brcm_hc_driver, &brcm_overrides);
	return platform_driver_register(&xhci_brcm_driver);
}
module_init(xhci_brcm_init);

static void __exit xhci_brcm_cleanup(void)
{
	platform_driver_unregister(&xhci_brcm_driver);
}
module_exit(xhci_brcm_cleanup);

MODULE_ALIAS("platform:xhci-brcm");
MODULE_DESCRIPTION(BRCM_DRIVER_DESC);
MODULE_AUTHOR("Al Cooper");
MODULE_LICENSE("GPL");
