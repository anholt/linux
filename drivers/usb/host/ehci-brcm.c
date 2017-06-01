/*
 * ehci-brcm.c - EHCI host controller driver for Broadcom STB.
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

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "ehci.h"

#define BRCM_DRIVER_DESC "EHCI Broadcom STB driver"

#define hcd_to_ehci_priv(h) ((struct brcm_priv *)hcd_to_ehci(h)->priv)

struct brcm_priv {
	struct clk *clk;
	struct phy *phy;
};

static const char brcm_hcd_name[] = "ehci-brcm";

static int (*org_hub_control)(struct usb_hcd *hcd,
			u16 typeReq, u16 wValue, u16 wIndex,
			char *buf, u16 wLength);

/* ehci_brcm_wait_for_sof
 * Wait for start of next microframe, then wait extra delay microseconds
 */
static inline void ehci_brcm_wait_for_sof(struct ehci_hcd *ehci, u32 delay)
{
	int frame_idx = ehci_readl(ehci, &ehci->regs->frame_index);

	while (frame_idx == ehci_readl(ehci, &ehci->regs->frame_index))
		;
	udelay(delay);
}

/*
 * ehci_brcm_hub_control
 * Intercept echi-hcd request to complete RESUME and align it to the start
 * of the next microframe.
 * If RESUME is complete too late in the microframe, host controller
 * detects babble on suspended port and resets the port afterwards.
 * This s/w workaround allows to avoid this problem.
 * See SWLINUX-1909 for more details
 */
static int ehci_brcm_hub_control(
	struct usb_hcd	*hcd,
	u16		typeReq,
	u16		wValue,
	u16		wIndex,
	char		*buf,
	u16		wLength)
{
	struct ehci_hcd	*ehci = hcd_to_ehci(hcd);
	int		ports = HCS_N_PORTS(ehci->hcs_params);
	u32 __iomem	*status_reg = &ehci->regs->port_status[
				(wIndex & 0xff) - 1];
	unsigned long flags;
	int retval, irq_disabled = 0;

	/*
	 * RESUME is cleared when GetPortStatus() is called 20ms after start
	 * of RESUME
	 */
	if ((typeReq == GetPortStatus) &&
	    (wIndex && wIndex <= ports) &&
	    ehci->reset_done[wIndex-1] &&
	    time_after_eq(jiffies, ehci->reset_done[wIndex-1]) &&
	    (ehci_readl(ehci, status_reg) & PORT_RESUME)) {

		/*
		 * to make sure we are not interrupted until RESUME bit
		 * is cleared, disable interrupts on current CPU
		 */
		ehci_dbg(ehci, "SOF alignment workaround\n");
		irq_disabled = 1;
		local_irq_save(flags);
		ehci_brcm_wait_for_sof(ehci, 5);
	}
	retval = (*org_hub_control)(hcd, typeReq, wValue, wIndex, buf, wLength);
	if (irq_disabled)
		local_irq_restore(flags);
	return retval;
}

static int ehci_brcm_reset(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	ehci->big_endian_mmio = 1;

	ehci->caps = (struct ehci_caps *) hcd->regs;
	ehci->regs = (struct ehci_regs *) (hcd->regs +
		HC_LENGTH(ehci, ehci_readl(ehci, &ehci->caps->hc_capbase)));

	/* This fixes the lockup during reboot due to prior interrupts */
	ehci_writel(ehci, CMD_RESET, &ehci->regs->command);
	mdelay(10);

	/*
	 * SWLINUX-1705: Avoid OUT packet underflows during high memory
	 *   bus usage
	 * port_status[0x0f] = Broadcom-proprietary USB_EHCI_INSNREG00 @ 0x90
	 */
	ehci_writel(ehci, 0x00800040, &ehci->regs->port_status[0x10]);
	ehci_writel(ehci, 0x00000001, &ehci->regs->port_status[0x12]);

	return ehci_setup(hcd);
}

static struct hc_driver __read_mostly ehci_brcm_hc_driver;

static const struct ehci_driver_overrides brcm_overrides __initconst = {

	.reset =	ehci_brcm_reset,
	.extra_priv_size = sizeof(struct brcm_priv),
};

static int ehci_brcm_probe(struct platform_device *pdev)
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

	/* Hook the hub control routine to work around a bug */
	if (org_hub_control == NULL)
		org_hub_control = ehci_brcm_hc_driver.hub_control;
	ehci_brcm_hc_driver.hub_control = ehci_brcm_hub_control;

	/* initialize hcd */
	hcd = usb_create_hcd(&ehci_brcm_hc_driver,
			&pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "Failed to create hcd\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, hcd);
	priv = hcd_to_ehci_priv(hcd);

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

static int ehci_brcm_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ehci_priv(hcd);

	usb_remove_hcd(hcd);
	phy_exit(priv->phy);
	clk_disable(priv->clk);
	usb_put_hcd(hcd);
	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int ehci_brcm_suspend(struct device *dev)
{
	int ret;
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ehci_priv(hcd);
	bool do_wakeup = device_may_wakeup(dev);

	ret = ehci_suspend(hcd, do_wakeup);
	clk_disable(priv->clk);
	return ret;
}

static int ehci_brcm_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	struct brcm_priv *priv = hcd_to_ehci_priv(hcd);
	int err;

	err = clk_enable(priv->clk);
	if (err)
		return err;
	/*
	 * SWLINUX-1705: Avoid OUT packet underflows during high memory
	 *   bus usage
	 * port_status[0x0f] = Broadcom-proprietary USB_EHCI_INSNREG00
	 * @ 0x90
	 */
	ehci_writel(ehci, 0x00800040, &ehci->regs->port_status[0x10]);
	ehci_writel(ehci, 0x00000001, &ehci->regs->port_status[0x12]);

	ehci_resume(hcd, false);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(ehci_brcm_pm_ops, ehci_brcm_suspend,
		ehci_brcm_resume);

#ifdef CONFIG_OF
static const struct of_device_id brcm_ehci_of_match[] = {
	{ .compatible = "brcm,ehci-brcm", },
	{}
};

MODULE_DEVICE_TABLE(of, brcm_ehci_of_match);
#endif /* CONFIG_OF */

static struct platform_driver ehci_brcm_driver = {
	.probe		= ehci_brcm_probe,
	.remove		= ehci_brcm_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "ehci-brcm",
		.pm	= &ehci_brcm_pm_ops,
		.of_match_table = of_match_ptr(brcm_ehci_of_match),
	}
};

static int __init ehci_brcm_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	pr_info("%s: " BRCM_DRIVER_DESC "\n", brcm_hcd_name);

	ehci_init_driver(&ehci_brcm_hc_driver, &brcm_overrides);
	return platform_driver_register(&ehci_brcm_driver);
}
module_init(ehci_brcm_init);

static void __exit ehci_brcm_cleanup(void)
{
	platform_driver_unregister(&ehci_brcm_driver);
}
module_exit(ehci_brcm_cleanup);

MODULE_ALIAS("platform:ehci-brcm");
MODULE_DESCRIPTION(BRCM_DRIVER_DESC);
MODULE_AUTHOR("Al Cooper");
MODULE_LICENSE("GPL");
