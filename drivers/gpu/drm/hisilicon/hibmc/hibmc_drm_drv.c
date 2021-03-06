/* Hisilicon Hibmc SoC drm driver
 *
 * Based on the bochs drm driver.
 *
 * Copyright (c) 2016 Huawei Limited.
 *
 * Author:
 *	Rongrong Zou <zourongrong@huawei.com>
 *	Rongrong Zou <zourongrong@gmail.com>
 *	Jianhua Li <lijianhua@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/console.h>
#include <drm/drm_crtc_helper.h>

#include "hibmc_drm_drv.h"
#include "hibmc_drm_regs.h"
#include "hibmc_drm_power.h"

static const struct file_operations hibmc_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.mmap		= hibmc_mmap,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= no_llseek,
};

static int hibmc_enable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct hibmc_drm_device *hidev =
		(struct hibmc_drm_device *)dev->dev_private;

	writel(HIBMC_RAW_INTERRUPT_EN_VBLANK(1),
	       hidev->mmio + HIBMC_RAW_INTERRUPT_EN);

	return 0;
}

static void hibmc_disable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct hibmc_drm_device *hidev =
		(struct hibmc_drm_device *)dev->dev_private;

	writel(HIBMC_RAW_INTERRUPT_EN_VBLANK(0),
	       hidev->mmio + HIBMC_RAW_INTERRUPT_EN);
}

irqreturn_t hibmc_drm_interrupt(int irq, void *arg)
{
	struct drm_device *dev = (struct drm_device *)arg;
	struct hibmc_drm_device *hidev =
		(struct hibmc_drm_device *)dev->dev_private;
	struct drm_crtc *crtc = &hidev->crtc;
	u32 status;

	status = readl(hidev->mmio + HIBMC_RAW_INTERRUPT);

	if (status & HIBMC_RAW_INTERRUPT_VBLANK(1)) {
		writel(HIBMC_RAW_INTERRUPT_VBLANK(1),
		       hidev->mmio + HIBMC_RAW_INTERRUPT);
		drm_crtc_handle_vblank(crtc);
	}

	return IRQ_HANDLED;
}

static struct drm_driver hibmc_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET |
				  DRIVER_ATOMIC | DRIVER_HAVE_IRQ,
	.fops			= &hibmc_fops,
	.name			= "hibmc",
	.date			= "20160828",
	.desc			= "hibmc drm driver",
	.major			= 1,
	.minor			= 0,
	.get_vblank_counter	= drm_vblank_no_hw_counter,
	.enable_vblank		= hibmc_enable_vblank,
	.disable_vblank		= hibmc_disable_vblank,
	.gem_free_object_unlocked = hibmc_gem_free_object,
	.dumb_create            = hibmc_dumb_create,
	.dumb_map_offset        = hibmc_dumb_mmap_offset,
	.dumb_destroy           = drm_gem_dumb_destroy,
	.irq_handler		= hibmc_drm_interrupt,
};

static int hibmc_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	struct hibmc_drm_device *hidev = drm_dev->dev_private;

	drm_kms_helper_poll_disable(drm_dev);
	drm_fb_helper_set_suspend_unlocked(&hidev->fbdev->helper, 1);

	return 0;
}

static int hibmc_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	struct hibmc_drm_device *hidev = drm_dev->dev_private;

	drm_helper_resume_force_mode(drm_dev);
	drm_fb_helper_set_suspend_unlocked(&hidev->fbdev->helper, 0);
	drm_kms_helper_poll_enable(drm_dev);

	return 0;
}

static const struct dev_pm_ops hibmc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(hibmc_pm_suspend,
				hibmc_pm_resume)
};

static int hibmc_kms_init(struct hibmc_drm_device *hidev)
{
	int ret;

	drm_mode_config_init(hidev->dev);
	hidev->mode_config_initialized = true;

	hidev->dev->mode_config.min_width = 0;
	hidev->dev->mode_config.min_height = 0;
	hidev->dev->mode_config.max_width = 1920;
	hidev->dev->mode_config.max_height = 1440;

	hidev->dev->mode_config.fb_base = hidev->fb_base;
	hidev->dev->mode_config.preferred_depth = 24;
	hidev->dev->mode_config.prefer_shadow = 0;

	hidev->dev->mode_config.funcs = (void *)&hibmc_mode_funcs;

	ret = hibmc_plane_init(hidev);
	if (ret) {
		DRM_ERROR("fail to init plane!!!\n");
		return ret;
	}

	ret = hibmc_crtc_init(hidev);
	if (ret) {
		DRM_ERROR("failed to init crtc.\n");
		return ret;
	}

	ret = hibmc_encoder_init(hidev);
	if (ret) {
		DRM_ERROR("failed to init encoder\n");
		return ret;
	}

	ret = hibmc_connector_init(hidev);
	if (ret) {
		DRM_ERROR("failed to init connector\n");
		return ret;
	}

	drm_mode_connector_attach_encoder(&hidev->connector,
					  &hidev->encoder);
	return 0;
}

static void hibmc_kms_fini(struct hibmc_drm_device *hidev)
{
	if (hidev->mode_config_initialized) {
		drm_mode_config_cleanup(hidev->dev);
		hidev->mode_config_initialized = false;
	}
}

static int hibmc_hw_config(struct hibmc_drm_device *hidev)
{
	unsigned int reg;

	/* On hardware reset, power mode 0 is default. */
	hibmc_set_power_mode(hidev, HIBMC_PW_MODE_CTL_MODE_MODE0);

	/* Enable display power gate & LOCALMEM power gate*/
	reg = readl(hidev->mmio + HIBMC_CURRENT_GATE);
	reg &= ~HIBMC_CURR_GATE_DISPLAY_MASK;
	reg &= ~HIBMC_CURR_GATE_LOCALMEM_MASK;
	reg |= HIBMC_CURR_GATE_DISPLAY(ON);
	reg |= HIBMC_CURR_GATE_LOCALMEM(ON);

	hibmc_set_current_gate(hidev, reg);

	/* Reset the memory controller. If the memory controller
	 * is not reset in chip,the system might hang when sw accesses
	 * the memory.The memory should be resetted after
	 * changing the MXCLK.
	 */
	reg = readl(hidev->mmio + HIBMC_MISC_CTRL);
	reg &= ~HIBMC_MSCCTL_LOCALMEM_RESET_MASK;
	reg |= HIBMC_MSCCTL_LOCALMEM_RESET(RESET);
	writel(reg, hidev->mmio + HIBMC_MISC_CTRL);

	reg &= ~HIBMC_MSCCTL_LOCALMEM_RESET_MASK;
	reg |= HIBMC_MSCCTL_LOCALMEM_RESET(NORMAL);

	writel(reg, hidev->mmio + HIBMC_MISC_CTRL);

	/* We can add more initialization as needed. */

	return 0;
}

static int hibmc_hw_map(struct hibmc_drm_device *hidev)
{
	struct drm_device *dev = hidev->dev;
	struct pci_dev *pdev = dev->pdev;
	resource_size_t addr, size, ioaddr, iosize;

	ioaddr = pci_resource_start(pdev, 1);
	iosize = MB(2);

	hidev->mmio = ioremap_nocache(ioaddr, iosize);

	if (!hidev->mmio) {
		DRM_ERROR("Cannot map mmio region\n");
		return -ENOMEM;
	}

	addr = pci_resource_start(pdev, 0);
	size = MB(32);

	hidev->fb_map = ioremap(addr, size);
	if (!hidev->fb_map) {
		DRM_ERROR("Cannot map framebuffer\n");
		return -ENOMEM;
	}
	hidev->fb_base = addr;
	hidev->fb_size = size;

	return 0;
}

static void hibmc_hw_fini(struct hibmc_drm_device *hidev)
{
	if (hidev->mmio)
		iounmap(hidev->mmio);
	if (hidev->fb_map)
		iounmap(hidev->fb_map);
}

static int hibmc_hw_init(struct hibmc_drm_device *hidev)
{
	int ret;

	ret = hibmc_hw_map(hidev);
	if (ret)
		return ret;

	hibmc_hw_config(hidev);

	return 0;
}

static int hibmc_unload(struct drm_device *dev)
{
	struct hibmc_drm_device *hidev = dev->dev_private;

	hibmc_fbdev_fini(hidev);

	if (dev->irq_enabled)
		drm_irq_uninstall(dev);
	if (hidev->msi_enabled)
		pci_disable_msi(dev->pdev);
	drm_vblank_cleanup(dev);

	hibmc_kms_fini(hidev);
	hibmc_mm_fini(hidev);
	hibmc_hw_fini(hidev);
	dev->dev_private = NULL;
	return 0;
}

static int hibmc_load(struct drm_device *dev, unsigned long flags)
{
	struct hibmc_drm_device *hidev;
	int ret;

	hidev = devm_kzalloc(dev->dev, sizeof(*hidev), GFP_KERNEL);
	if (!hidev)
		return -ENOMEM;
	dev->dev_private = hidev;
	hidev->dev = dev;

	ret = hibmc_hw_init(hidev);
	if (ret)
		goto err;

	ret = hibmc_mm_init(hidev);
	if (ret)
		goto err;

	ret = hibmc_kms_init(hidev);
	if (ret)
		goto err;

	ret = drm_vblank_init(dev, dev->mode_config.num_crtc);
	if (ret) {
		DRM_ERROR("failed to initialize vblank.\n");
		goto err;
	}

	hidev->msi_enabled = 0;
	if (pci_enable_msi(dev->pdev)) {
		DRM_ERROR("Enabling MSI failed!\n");
	} else {
		hidev->msi_enabled = 1;
		ret = drm_irq_install(dev, dev->pdev->irq);
		if (ret)
			DRM_ERROR("install irq failed , ret = %d\n", ret);
	}

	/* reset all the states of crtc/plane/encoder/connector */
	drm_mode_config_reset(dev);

	ret = hibmc_fbdev_init(hidev);
	if (ret)
		goto err;

	return 0;

err:
	hibmc_unload(dev);
	DRM_ERROR("failed to initialize drm driver.\n");
	return ret;
}

static int hibmc_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct drm_device *dev;
	int ret;

	dev = drm_dev_alloc(&hibmc_driver, &pdev->dev);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = hibmc_load(dev, 0);
	if (ret)
		goto err_disable;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_unload;

	return 0;

err_unload:
	hibmc_unload(dev);
err_disable:
	pci_disable_device(pdev);
err_free:
	drm_dev_unref(dev);

	return ret;
}

static void hibmc_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);

	drm_dev_unregister(dev);
	hibmc_unload(dev);
	drm_dev_unref(dev);
}

static struct pci_device_id hibmc_pci_table[] = {
	{0x19e5, 0x1711, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0,}
};

static struct pci_driver hibmc_pci_driver = {
	.name =		"hibmc-drm",
	.id_table =	hibmc_pci_table,
	.probe =	hibmc_pci_probe,
	.remove =	hibmc_pci_remove,
	.driver.pm =    &hibmc_pm_ops,
};

static int __init hibmc_init(void)
{
	return pci_register_driver(&hibmc_pci_driver);
}

static void __exit hibmc_exit(void)
{
	return pci_unregister_driver(&hibmc_pci_driver);
}

module_init(hibmc_init);
module_exit(hibmc_exit);

MODULE_DEVICE_TABLE(pci, hibmc_pci_table);
MODULE_AUTHOR("RongrongZou <zourongrong@huawei.com>");
MODULE_DESCRIPTION("DRM Driver for Hisilicon Hibmc");
MODULE_LICENSE("GPL v2");
