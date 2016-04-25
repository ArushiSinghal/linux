/*
 * (C) COPYRIGHT 2016 ARM Limited. All rights reserved.
 * Author: Liviu Dudau <Liviu.Dudau@arm.com>
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * ARM Mali DP500/DP550/DP650 KMS/DRM driver
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_of.h>

#include "malidp_drv.h"
#include "malidp_regs.h"
#include "malidp_hw.h"

#define MALIDP_CONF_VALID_TIMEOUT	250

/*
 * set the "config valid" bit and wait until the hardware
 * acts on it
 */
int malidp_set_and_wait_config_valid(struct drm_device *drm)
{
	struct malidp_drm *malidp = drm->dev_private;
	struct malidp_hw_device *hwdev = malidp->dev;
	int ret;

	hwdev->set_config_valid(hwdev);
	/* don't wait for config_valid flag if we are in config mode */
	if (hwdev->in_config_mode(hwdev))
		return 0;

	ret = wait_event_interruptible_timeout(malidp->wq,
			atomic_read(&malidp->config_valid) == 1,
			msecs_to_jiffies(MALIDP_CONF_VALID_TIMEOUT));

	return (ret > 0) ? 0 : -ETIMEDOUT;
}

static void malidp_output_poll_changed(struct drm_device *drm)
{
	struct malidp_drm *malidp = drm->dev_private;

	if (malidp->fbdev)
		drm_fbdev_cma_hotplug_event(malidp->fbdev);
}

static void malidp_atomic_complete(struct drm_device *drm,
				   struct drm_atomic_state *old_state)
{
	/* ToDo: wait_for_fences(drm, old_state); */

	drm_atomic_helper_commit_modeset_disables(drm, old_state);
	drm_atomic_helper_commit_planes(drm, old_state, false);
	drm_atomic_helper_commit_modeset_enables(drm, old_state);

	drm_atomic_helper_wait_for_vblanks(drm, old_state);

	drm_atomic_helper_cleanup_planes(drm, old_state);
	drm_atomic_state_free(old_state);
}

static void malidp_atomic_work(struct work_struct *work)
{
	struct malidp_drm *malidp = container_of(work, struct malidp_drm,
						 commit.work);

	malidp_atomic_complete(malidp->commit.state->dev,
			       malidp->commit.state);
}

static int malidp_atomic_commit(struct drm_device *drm,
				struct drm_atomic_state *state,
				bool async)
{
	struct malidp_drm *malidp = drm->dev_private;
	int err;

	err = drm_atomic_helper_prepare_planes(drm, state);
	if (err)
		return err;

	if (async && !list_empty(&malidp->commit.work.entry)) {
		/* pending commits found, bail out */
		return -EBUSY;
	}

	mutex_lock(&malidp->commit.lock);
	flush_work(&malidp->commit.work);

	/*
	 * The point of no return awaits here. After this we commit
	 * on software side to handle the new state
	 */
	drm_atomic_helper_swap_state(drm, state);

	malidp->commit.state = state;

	if (async)
		schedule_work(&malidp->commit.work);
	else
		malidp_atomic_complete(drm, state);

	mutex_unlock(&malidp->commit.lock);
	return 0;
}

static const struct drm_mode_config_funcs malidp_mode_config_funcs = {
	.fb_create = drm_fb_cma_create,
	.output_poll_changed = malidp_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = malidp_atomic_commit,
};

static int malidp_enable_vblank(struct drm_device *drm, unsigned int crtc)
{
	return 0;
}

static void malidp_disable_vblank(struct drm_device *drm, unsigned int pipe)
{
}

static int malidp_init(struct drm_device *drm)
{
	int ret;
	struct malidp_drm *malidp = drm->dev_private;
	struct malidp_hw_device *hwdev = malidp->dev;

	drm_mode_config_init(drm);

	drm->mode_config.min_width = hwdev->min_line_size;
	drm->mode_config.min_height = hwdev->min_line_size;
	drm->mode_config.max_width = hwdev->max_line_size;
	drm->mode_config.max_height = hwdev->max_line_size;
	drm->mode_config.funcs = &malidp_mode_config_funcs;

	ret = malidp_de_planes_init(drm);
	if (ret < 0) {
		DRM_ERROR("Failed to initialise planes\n");
		goto plane_init_fail;
	}

	ret = malidp_crtc_init(drm);
	if (ret) {
		DRM_ERROR("Failed to initialise CRTC\n");
		goto crtc_init_fail;
	}

	return 0;

crtc_init_fail:
	malidp_de_planes_destroy(drm);
plane_init_fail:
	drm_mode_config_cleanup(drm);

	return ret;
}

static int malidp_irq_init(struct platform_device *pdev)
{
	int irq_de, irq_se, ret = 0;
	struct drm_device *drm = dev_get_drvdata(&pdev->dev);

	/* fetch the interrupts from DT */
	irq_de = platform_get_irq_byname(pdev, "DE");
	if (irq_de < 0) {
		DRM_ERROR("no 'DE' IRQ specified!\n");
		return irq_de;
	}
	irq_se = platform_get_irq_byname(pdev, "SE");
	if (irq_se < 0) {
		DRM_ERROR("no 'SE' IRQ specified!\n");
		return irq_se;
	}

	ret = malidp_de_irq_init(drm, irq_de);
	if (ret)
		return ret;

	ret = malidp_se_irq_init(drm, irq_se);
	if (ret) {
		malidp_de_irq_cleanup(drm);
		return ret;
	}

	return 0;
}

static void malidp_lastclose(struct drm_device *drm)
{
	struct malidp_drm *malidp = drm->dev_private;

	drm_fbdev_cma_restore_mode(malidp->fbdev);
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_cma_mmap,
};

static struct drm_driver malidp_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
			   DRIVER_PRIME,
	.lastclose = malidp_lastclose,
	.get_vblank_counter = drm_vblank_no_hw_counter,
	.enable_vblank = malidp_enable_vblank,
	.disable_vblank = malidp_disable_vblank,
	.gem_free_object = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.dumb_create = drm_gem_cma_dumb_create,
	.dumb_map_offset = drm_gem_cma_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_get_sg_table = drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
	.fops = &fops,
	.name = "mali-dp",
	.desc = "ARM Mali Display Processor driver",
	.date = "20160106",
	.major = 1,
	.minor = 0,
};

static const struct of_device_id  malidp_drm_of_match[] = {
	{
		.compatible = "arm,mali-dp500",
		.data = &malidp_device[MALIDP_500]
	},
	{
		.compatible = "arm,mali-dp550",
		.data = &malidp_device[MALIDP_550]
	},
	{
		.compatible = "arm,mali-dp650",
		.data = &malidp_device[MALIDP_650]
	},
	{},
};
MODULE_DEVICE_TABLE(of, malidp_drm_of_match);

#define MAX_OUTPUT_CHANNELS	3

static int malidp_bind(struct device *dev)
{
	struct resource *res;
	struct drm_device *drm;
	struct malidp_drm *malidp;
	struct malidp_hw_device *hwdev;
	struct platform_device *pdev = to_platform_device(dev);
	/* number of lines for the R, G and B output */
	u8 output_width[MAX_OUTPUT_CHANNELS];
	int ret = 0, i;
	u32 version, out_depth = 0;

	malidp = devm_kzalloc(dev, sizeof(*malidp), GFP_KERNEL);
	if (!malidp)
		return -ENOMEM;

	hwdev = devm_kzalloc(dev, sizeof(*hwdev), GFP_KERNEL);
	if (!hwdev)
		return -ENOMEM;

	/*
	 * copy the associated data from malidp_drm_of_match to avoid
	 * having to keep a reference to the OF node after binding
	 */
	memcpy(hwdev, of_device_get_match_data(dev), sizeof(*hwdev));
	malidp->dev = hwdev;

	INIT_LIST_HEAD(&malidp->event_list);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hwdev->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(hwdev->regs)) {
		DRM_ERROR("Failed to map control registers area\n");
		return PTR_ERR(hwdev->regs);
	}

	hwdev->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(hwdev->pclk))
		return PTR_ERR(hwdev->pclk);

	hwdev->aclk = devm_clk_get(dev, "aclk");
	if (IS_ERR(hwdev->aclk))
		return PTR_ERR(hwdev->aclk);

	hwdev->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(hwdev->mclk))
		return PTR_ERR(hwdev->mclk);

	hwdev->pxlclk = devm_clk_get(dev, "pxlclk");
	if (IS_ERR(hwdev->pxlclk))
		return PTR_ERR(hwdev->pxlclk);

	/* Get the optional framebuffer memory resource */
	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		return ret;

	drm = drm_dev_alloc(&malidp_driver, dev);
	if (!drm) {
		ret = -ENOMEM;
		goto alloc_fail;
	}

	/* Enable APB clock in order to get access to the registers */
	clk_prepare_enable(hwdev->pclk);
	/*
	 * Enable AXI clock and main clock so that prefetch can start once
	 * the registers are set
	 */
	clk_prepare_enable(hwdev->aclk);
	clk_prepare_enable(hwdev->mclk);

	ret = hwdev->query_hw(hwdev);
	if (ret) {
		DRM_ERROR("Invalid HW configuration\n");
		goto query_hw_fail;
	}

	version = malidp_hw_read(hwdev, hwdev->map.dc_base + MALIDP_DE_CORE_ID);
	DRM_INFO("found ARM Mali-DP%3x version r%dp%d\n", version >> 16,
		 (version >> 12) & 0xf, (version >> 8) & 0xf);

	/* set the number of lines used for output of RGB data */
	ret = of_property_read_u8_array(dev->of_node,
					"arm,malidp-output-port-lines",
					output_width, MAX_OUTPUT_CHANNELS);
	if (ret)
		goto query_hw_fail;

	for (i = 0; i < MAX_OUTPUT_CHANNELS; i++)
		out_depth = (out_depth << 8) | (output_width[i] & 0xf);
	malidp_hw_write(hwdev, out_depth, hwdev->map.out_depth_base);

	drm->dev_private = malidp;
	dev_set_drvdata(dev, drm);
	atomic_set(&malidp->config_valid, 0);
	init_waitqueue_head(&malidp->wq);

	mutex_init(&malidp->commit.lock);
	INIT_WORK(&malidp->commit.work, malidp_atomic_work);

	ret = malidp_init(drm);
	if (ret < 0)
		goto init_fail;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto register_fail;

	/* Set the CRTC's port so that the encoder component can find it */
	malidp->crtc.port = of_graph_get_next_endpoint(dev->of_node, NULL);

	ret = component_bind_all(dev, drm);
	of_node_put(malidp->crtc.port);

	if (ret) {
		DRM_ERROR("Failed to bind all components\n");
		goto bind_fail;
	}

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret < 0) {
		DRM_ERROR("failed to initialise vblank\n");
		goto vblank_fail;
	}
	drm->vblank_disable_allowed = true;

	ret = malidp_irq_init(pdev);
	if (ret < 0)
		goto irq_init_fail;

	drm_mode_config_reset(drm);

	drm_helper_disable_unused_functions(drm);
	malidp->fbdev = drm_fbdev_cma_init(drm, 32, drm->mode_config.num_crtc,
					   drm->mode_config.num_connector);

	if (IS_ERR(malidp->fbdev)) {
		ret = PTR_ERR(malidp->fbdev);
		malidp->fbdev = NULL;
		goto fbdev_fail;
	}

	drm_kms_helper_poll_init(drm);
	return 0;

fbdev_fail:
	malidp_se_irq_cleanup(drm);
	malidp_de_irq_cleanup(drm);
irq_init_fail:
	drm_vblank_cleanup(drm);
vblank_fail:
	component_unbind_all(dev, drm);
bind_fail:
	drm_dev_unregister(drm);
register_fail:
	malidp_de_planes_destroy(drm);
	drm_mode_config_cleanup(drm);
init_fail:
	drm->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
query_hw_fail:
	clk_disable_unprepare(hwdev->mclk);
	clk_disable_unprepare(hwdev->aclk);
	clk_disable_unprepare(hwdev->pclk);
	drm_dev_unref(drm);
alloc_fail:
	of_reserved_mem_device_release(dev);

	return ret;
}

static void malidp_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct malidp_drm *malidp = drm->dev_private;
	struct malidp_hw_device *hwdev = malidp->dev;

	if (malidp->fbdev) {
		drm_fbdev_cma_fini(malidp->fbdev);
		malidp->fbdev = NULL;
	}
	drm_kms_helper_poll_fini(drm);
	malidp_se_irq_cleanup(drm);
	malidp_de_irq_cleanup(drm);
	drm_vblank_cleanup(drm);
	component_unbind_all(dev, drm);
	drm_dev_unregister(drm);
	malidp_de_planes_destroy(drm);
	drm_mode_config_cleanup(drm);
	drm->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
	clk_disable_unprepare(hwdev->mclk);
	clk_disable_unprepare(hwdev->aclk);
	clk_disable_unprepare(hwdev->pclk);
	drm_dev_unref(drm);
	of_reserved_mem_device_release(dev);
}

static const struct component_master_ops malidp_master_ops = {
	.bind = malidp_bind,
	.unbind = malidp_unbind,
};

static int malidp_compare_dev(struct device *dev, void *data)
{
	struct device_node *np = data;

	return dev->of_node == np;
}

static int malidp_platform_probe(struct platform_device *pdev)
{
	struct device_node *port, *ep;
	struct component_match *match = NULL;

	if (!pdev->dev.of_node)
		return -ENODEV;

	/* there is only one output port inside each device, find it */
	ep = of_graph_get_next_endpoint(pdev->dev.of_node, NULL);
	if (!ep)
		return -ENODEV;

	if (!of_device_is_available(ep)) {
		of_node_put(ep);
		return -ENODEV;
	}

	/* add the remote encoder port as component */
	port = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);
	if (!port || !of_device_is_available(port)) {
		of_node_put(port);
		return -EAGAIN;
	}

	component_match_add(&pdev->dev, &match, malidp_compare_dev, port);
	return component_master_add_with_match(&pdev->dev, &malidp_master_ops,
					       match);
}

static int malidp_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &malidp_master_ops);
	return 0;
}

static struct platform_driver malidp_platform_driver = {
	.probe		= malidp_platform_probe,
	.remove		= malidp_platform_remove,
	.driver	= {
		.name = "mali-dp",
		.of_match_table	= malidp_drm_of_match,
	},
};

module_platform_driver(malidp_platform_driver);

MODULE_AUTHOR("Liviu Dudau <Liviu.Dudau@arm.com>");
MODULE_DESCRIPTION("ARM Mali DP DRM driver");
MODULE_LICENSE("GPL v2");
