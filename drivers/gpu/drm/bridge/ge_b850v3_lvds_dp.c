/*
 * Driver for GE B850v3 DP display bridge

 * Copyright (c) 2016, Collabora Ltd.
 * Copyright (c) 2016, General Electric Company

 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.

 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.

 * This driver creates a drm_bridge and a drm_connector for the LVDS to DP++
 * display bridge of the GE B850v3. There are two physical bridges on the video
 * signal pipeline: a STDP4028(LVDS to DP) and a STDP2690(DP to DP++). However
 * the physical bridges are automatically configured by the input video signal,
 * and the driver has no access to the video processing pipeline. The driver is
 * only needed to read EDID from the STDP2690 and to handle HPD events from the
 * STDP4028. The driver communicates with both bridges over i2c. The video
 * signal pipeline is as follows:
 *
 *   Host -> LVDS|--(STDP4028)--|DP -> DP|--(STDP2690)--|DP++ -> Video output
 *
 */

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drmP.h>

/*
 * 220Mhz is a limitation of the host, as the bridge is capable of up to
 * 330Mhz. See section 9.2.1.2.4 of the i.MX 6Dual/6Quad Applications
 * Processor Reference Manual for more information about the 220Mhz limit.
 * The imx-ldb driver will warn about clocks over 170Mhz, but it seem to work
 * fine.
 */
#define MAX_PIXEL_CLOCK 220000

#define EDID_EXT_BLOCK_CNT 0x7E

#define STDP4028_IRQ_OUT_CONF_REG 0x02
#define STDP4028_DPTX_IRQ_EN_REG 0x3C
#define STDP4028_DPTX_IRQ_STS_REG 0x3D
#define STDP4028_DPTX_STS_REG 0x3E

#define STDP4028_DPTX_DP_IRQ_EN 0x1000

#define STDP4028_DPTX_HOTPLUG_IRQ_EN 0x0400
#define STDP4028_DPTX_LINK_CH_IRQ_EN 0x2000
#define STDP4028_DPTX_IRQ_CONFIG \
		(STDP4028_DPTX_LINK_CH_IRQ_EN | STDP4028_DPTX_HOTPLUG_IRQ_EN)

#define STDP4028_DPTX_HOTPLUG_STS 0x0200
#define STDP4028_DPTX_LINK_STS 0x1000
#define STDP4028_CON_STATE_CONNECTED \
		(STDP4028_DPTX_HOTPLUG_STS | STDP4028_DPTX_LINK_STS)

#define STDP4028_DPTX_HOTPLUG_CH_STS 0x0400
#define STDP4028_DPTX_LINK_CH_STS 0x2000
#define STDP4028_DPTX_IRQ_CLEAR \
		(STDP4028_DPTX_LINK_CH_STS | STDP4028_DPTX_HOTPLUG_CH_STS)

struct ge_b850v3_lvds_dp {
	struct drm_connector connector;
	struct drm_bridge bridge;
	struct i2c_client *ge_b850v3_lvds_dp_i2c;
	struct i2c_client *edid_i2c;
	struct edid *edid;
	struct mutex lock;
};

static inline struct ge_b850v3_lvds_dp *
		bridge_to_ge_b850v3_lvds_dp(struct drm_bridge *bridge)
{
	return container_of(bridge, struct ge_b850v3_lvds_dp, bridge);
}

static inline struct ge_b850v3_lvds_dp *
		connector_to_ge_b850v3_lvds_dp(struct drm_connector *connector)
{
	return container_of(connector, struct ge_b850v3_lvds_dp, connector);
}

u8 *stdp2690_get_edid(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	unsigned char start = 0x00;
	unsigned int total_size;
	u8 *block = kmalloc(EDID_LENGTH, GFP_KERNEL);

	struct i2c_msg msgs[] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &start,
		}, {
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= EDID_LENGTH,
			.buf	= block,
		}
	};

	if (!block)
		return NULL;

	if (i2c_transfer(adapter, msgs, 2) != 2) {
		DRM_ERROR("Unable to read EDID.\n");
		goto err;
	}

	if (!drm_edid_block_valid(block, 0, false, NULL)) {
		DRM_ERROR("Invalid EDID block\n");
		goto err;
	}

	total_size = (block[EDID_EXT_BLOCK_CNT] + 1) * EDID_LENGTH;
	if (total_size > EDID_LENGTH) {
		kfree(block);
		block = kmalloc(total_size, GFP_KERNEL);
		if (!block)
			return NULL;

		/* Yes, read the entire buffer, and do not skip the first
		 * EDID_LENGTH bytes.
		 */
		start = 0x00;
		msgs[1].len = total_size;
		msgs[1].buf = block;

		if (i2c_transfer(adapter, msgs, 2) != 2) {
			DRM_ERROR("Unable to read EDID extension blocks.\n");
			goto err;
		}
	}

	return block;

err:
	kfree(block);
	return NULL;
}

static int ge_b850v3_lvds_dp_get_modes(struct drm_connector *connector)
{
	struct ge_b850v3_lvds_dp *ptn_bridge;
	struct i2c_client *client;
	int num_modes = 0;

	ptn_bridge = connector_to_ge_b850v3_lvds_dp(connector);
	client = ptn_bridge->edid_i2c;

	mutex_lock(&ptn_bridge->lock);

	kfree(ptn_bridge->edid);
	ptn_bridge->edid = (struct edid *) stdp2690_get_edid(client);

	if (ptn_bridge->edid) {
		drm_mode_connector_update_edid_property(connector,
				ptn_bridge->edid);
		num_modes = drm_add_edid_modes(connector, ptn_bridge->edid);
	}

	mutex_unlock(&ptn_bridge->lock);

	return num_modes;
}


static enum drm_mode_status ge_b850v3_lvds_dp_mode_valid(
		struct drm_connector *connector, struct drm_display_mode *mode)
{
	if (mode->clock > MAX_PIXEL_CLOCK) {
		DRM_INFO("The pixel clock for the mode %s is too high, and not supported.",
				mode->name);
		return MODE_CLOCK_HIGH;
	}

	return MODE_OK;
}

static const struct
drm_connector_helper_funcs ge_b850v3_lvds_dp_connector_helper_funcs = {
	.get_modes = ge_b850v3_lvds_dp_get_modes,
	.mode_valid = ge_b850v3_lvds_dp_mode_valid,
};

static enum drm_connector_status ge_b850v3_lvds_dp_detect(
		struct drm_connector *connector, bool force)
{
	struct ge_b850v3_lvds_dp *ptn_bridge =
			connector_to_ge_b850v3_lvds_dp(connector);
	struct i2c_client *ge_b850v3_lvds_dp_i2c =
			ptn_bridge->ge_b850v3_lvds_dp_i2c;
	s32 link_state;

	link_state = i2c_smbus_read_word_data(ge_b850v3_lvds_dp_i2c,
			STDP4028_DPTX_STS_REG);

	if (link_state == STDP4028_CON_STATE_CONNECTED)
		return connector_status_connected;

	if (link_state == 0)
		return connector_status_disconnected;

	return connector_status_unknown;
}

static const struct drm_connector_funcs ge_b850v3_lvds_dp_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = ge_b850v3_lvds_dp_detect,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static irqreturn_t ge_b850v3_lvds_dp_irq_handler(int irq, void *dev_id)
{
	struct ge_b850v3_lvds_dp *ptn_bridge = dev_id;
	struct i2c_client *ge_b850v3_lvds_dp_i2c
			= ptn_bridge->ge_b850v3_lvds_dp_i2c;

	mutex_lock(&ptn_bridge->lock);

	i2c_smbus_write_word_data(ge_b850v3_lvds_dp_i2c,
			STDP4028_DPTX_IRQ_STS_REG, STDP4028_DPTX_IRQ_CLEAR);

	mutex_unlock(&ptn_bridge->lock);

	if (ptn_bridge->connector.dev)
		drm_kms_helper_hotplug_event(ptn_bridge->connector.dev);

	return IRQ_HANDLED;
}

static int ge_b850v3_lvds_dp_attach(struct drm_bridge *bridge)
{
	struct ge_b850v3_lvds_dp *ptn_bridge
			= bridge_to_ge_b850v3_lvds_dp(bridge);
	struct drm_connector *connector = &ptn_bridge->connector;
	struct i2c_client *ge_b850v3_lvds_dp_i2c
			= ptn_bridge->ge_b850v3_lvds_dp_i2c;
	int ret;

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found");
		return -ENODEV;
	}

	connector->polled = DRM_CONNECTOR_POLL_HPD;

	drm_connector_helper_add(connector,
			&ge_b850v3_lvds_dp_connector_helper_funcs);

	ret = drm_connector_init(bridge->dev, connector,
			&ge_b850v3_lvds_dp_connector_funcs,
			DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		DRM_ERROR("Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_register(connector);
	ret = drm_mode_connector_attach_encoder(connector, bridge->encoder);
	if (ret)
		return ret;

	drm_bridge_enable(bridge);
	if (ge_b850v3_lvds_dp_i2c->irq) {
		drm_helper_hpd_irq_event(connector->dev);

		ret = devm_request_threaded_irq(&ge_b850v3_lvds_dp_i2c->dev,
				ge_b850v3_lvds_dp_i2c->irq, NULL,
				ge_b850v3_lvds_dp_irq_handler,
				IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
				"ge-b850v3-lvds-dp", ptn_bridge);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct drm_bridge_funcs ge_b850v3_lvds_dp_funcs = {
	.attach = ge_b850v3_lvds_dp_attach,
};

static int ge_b850v3_lvds_dp_probe(struct i2c_client *ge_b850v3_lvds_dp_i2c,
				const struct i2c_device_id *id)
{
	struct device *dev = &ge_b850v3_lvds_dp_i2c->dev;
	struct ge_b850v3_lvds_dp *ptn_bridge;
	int ret;
	u32 edid_i2c_reg;

	ptn_bridge = devm_kzalloc(dev, sizeof(*ptn_bridge), GFP_KERNEL);
	if (!ptn_bridge)
		return -ENOMEM;

	mutex_init(&ptn_bridge->lock);

	ptn_bridge->ge_b850v3_lvds_dp_i2c = ge_b850v3_lvds_dp_i2c;
	ptn_bridge->bridge.driver_private = ptn_bridge;
	i2c_set_clientdata(ge_b850v3_lvds_dp_i2c, ptn_bridge);

	ret = of_property_read_u32(dev->of_node, "edid-reg", &edid_i2c_reg);
	if (ret) {
		dev_err(dev, "edid-reg not specified, aborting...\n");
		return -ENODEV;
	}

	ptn_bridge->edid_i2c = devm_kzalloc(dev,
			sizeof(struct i2c_client), GFP_KERNEL);

	if (!ptn_bridge->edid_i2c)
		return -ENOMEM;

	memcpy(ptn_bridge->edid_i2c, ge_b850v3_lvds_dp_i2c,
			sizeof(struct i2c_client));

	ptn_bridge->edid_i2c->addr = (unsigned short) edid_i2c_reg;

	/*
	 * Configures the bridge to re-enable interrupts after each ack. As
	 * this is the first communication with the chip, fail on error.
	 */
	ret = i2c_smbus_write_word_data(ge_b850v3_lvds_dp_i2c,
			STDP4028_IRQ_OUT_CONF_REG, STDP4028_DPTX_DP_IRQ_EN);
	if (ret) {
		dev_err(dev, "i2c communication failed, aborting...\n");
		return ret;
	}

	i2c_smbus_write_word_data(ge_b850v3_lvds_dp_i2c,
			STDP4028_DPTX_IRQ_EN_REG, STDP4028_DPTX_IRQ_CONFIG);

	/* Clear pending interrupts since power up. */
	i2c_smbus_write_word_data(ge_b850v3_lvds_dp_i2c,
			STDP4028_DPTX_IRQ_STS_REG, STDP4028_DPTX_IRQ_CLEAR);

	ptn_bridge->bridge.funcs = &ge_b850v3_lvds_dp_funcs;
	ptn_bridge->bridge.of_node = dev->of_node;
	ret = drm_bridge_add(&ptn_bridge->bridge);
	if (ret) {
		DRM_ERROR("Failed to add bridge\n");
		return ret;
	}

	return 0;
}

static int ge_b850v3_lvds_dp_remove(struct i2c_client *ge_b850v3_lvds_dp_i2c)
{
	struct ge_b850v3_lvds_dp *ptn_bridge =
		i2c_get_clientdata(ge_b850v3_lvds_dp_i2c);

	drm_bridge_remove(&ptn_bridge->bridge);

	kfree(ptn_bridge->edid);

	return 0;
}

static const struct i2c_device_id ge_b850v3_lvds_dp_i2c_table[] = {
	{"b850v3-lvds-dp", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ge_b850v3_lvds_dp_i2c_table);

static const struct of_device_id ge_b850v3_lvds_dp_match[] = {
	{ .compatible = "ge,b850v3-lvds-dp" },
	{},
};
MODULE_DEVICE_TABLE(of, ge_b850v3_lvds_dp_match);

static struct i2c_driver ge_b850v3_lvds_dp_driver = {
	.id_table	= ge_b850v3_lvds_dp_i2c_table,
	.probe		= ge_b850v3_lvds_dp_probe,
	.remove		= ge_b850v3_lvds_dp_remove,
	.driver		= {
		.name		= "b850v3-lvds-dp",
		.of_match_table = ge_b850v3_lvds_dp_match,
	},
};
module_i2c_driver(ge_b850v3_lvds_dp_driver);

MODULE_AUTHOR("Peter Senna Tschudin <peter.senna@collabora.com>");
MODULE_AUTHOR("Martyn Welch <martyn.welch@collabora.co.uk>");
MODULE_DESCRIPTION("GE LVDS to DP++ display bridge)");
MODULE_LICENSE("GPL v2");
