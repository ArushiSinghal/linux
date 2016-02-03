/*
 * f_rndis.c -- RNDIS link function driver
 *
 * Copyright (C) 2003-2005,2008 David Brownell
 * Copyright (C) 2003-2004 Robert Schwebel, Benedikt Spranger
 * Copyright (C) 2008 Nokia Corporation
 * Copyright (C) 2009 Samsung Electronics
 *                    Author: Michal Nazarewicz (mina86@mina86.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* #define VERBOSE_DEBUG */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/etherdevice.h>

#include <linux/atomic.h>

#include "u_ether.h"
#include "u_ether_configfs.h"
#include "u_rndis.h"
#include "rndis.h"
#include "configfs.h"

/*
 * This function is an RNDIS Ethernet port -- a Microsoft protocol that's
 * been promoted instead of the standard CDC Ethernet.  The published RNDIS
 * spec is ambiguous, incomplete, and needlessly complex.  Variants such as
 * ActiveSync have even worse status in terms of specification.
 *
 * In short:  it's a protocol controlled by (and for) Microsoft, not for an
 * Open ecosystem or markets.  Linux supports it *only* because Microsoft
 * doesn't support the CDC Ethernet standard.
 *
 * The RNDIS data transfer model is complex, with multiple Ethernet packets
 * per USB message, and out of band data.  The control model is built around
 * what's essentially an "RNDIS RPC" protocol.  It's all wrapped in a CDC ACM
 * (modem, not Ethernet) veneer, with those ACM descriptors being entirely
 * useless (they're ignored).  RNDIS expects to be the only function in its
 * configuration, so it's no real help if you need composite devices; and
 * it expects to be the first configuration too.
 *
 * There is a single technical advantage of RNDIS over CDC Ethernet, if you
 * discount the fluff that its RPC can be made to deliver: it doesn't need
 * a NOP altsetting for the data interface.  That lets it work on some of the
 * "so smart it's stupid" hardware which takes over configuration changes
 * from the software, and adds restrictions like "no altsettings".
 *
 * Unfortunately MSFT's RNDIS drivers are buggy.  They hang or oops, and
 * have all sorts of contrary-to-specification oddities that can prevent
 * them from working sanely.  Since bugfixes (or accurate specs, letting
 * Linux work around those bugs) are unlikely to ever come from MSFT, you
 * may want to avoid using RNDIS on purely operational grounds.
 *
 * Omissions from the RNDIS 1.0 specification include:
 *
 *   - Power management ... references data that's scattered around lots
 *     of other documentation, which is incorrect/incomplete there too.
 *
 *   - There are various undocumented protocol requirements, like the need
 *     to send garbage in some control-OUT messages.
 *
 *   - MS-Windows drivers sometimes emit undocumented requests.
 */

struct f_rndis {
	struct gether			port;
	u8				ctrl_id, data_id;
	u8				ethaddr[ETH_ALEN];
	u32				vendorID;
	const char			*manufacturer;
	struct rndis_params		*params;

	struct usb_ep			*notify;
	struct usb_request		*notify_req;
	atomic_t			notify_count;
};

static inline struct f_rndis *func_to_rndis(struct usb_function *f)
{
	return container_of(f, struct f_rndis, port.func);
}

/* peak (theoretical) bulk transfer rate in bits-per-second */
static unsigned int bitrate(struct usb_gadget *g)
{
	if (gadget_is_superspeed(g) && g->speed == USB_SPEED_SUPER)
		return 13 * 1024 * 8 * 1000 * 8;
	else if (gadget_is_dualspeed(g) && g->speed == USB_SPEED_HIGH)
		return 13 * 512 * 8 * 1000 * 8;
	else
		return 19 * 64 * 1 * 1000 * 8;
}

/*-------------------------------------------------------------------------*/

/*
 */

#define RNDIS_STATUS_INTERVAL_MS	32
#define STATUS_BYTECOUNT		8	/* 8 bytes data */


/* interface descriptor: */

static struct usb_interface_descriptor rndis_control_intf = {
	.bLength =		sizeof rndis_control_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	/* status endpoint is optional; this could be patched later */
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_COMM,
	.bInterfaceSubClass =   USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol =   USB_CDC_ACM_PROTO_VENDOR,
	/* .iInterface = DYNAMIC */
};

static struct usb_cdc_header_desc header_desc = {
	.bLength =		sizeof header_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_HEADER_TYPE,

	.bcdCDC =		cpu_to_le16(0x0110),
};

static struct usb_cdc_call_mgmt_descriptor call_mgmt_descriptor = {
	.bLength =		sizeof call_mgmt_descriptor,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_CALL_MANAGEMENT_TYPE,

	.bmCapabilities =	0x00,
	.bDataInterface =	0x01,
};

static struct usb_cdc_acm_descriptor rndis_acm_descriptor = {
	.bLength =		sizeof rndis_acm_descriptor,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_ACM_TYPE,

	.bmCapabilities =	0x00,
};

static struct usb_cdc_union_desc rndis_union_desc = {
	.bLength =		sizeof(rndis_union_desc),
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_UNION_TYPE,
	/* .bMasterInterface0 =	DYNAMIC */
	/* .bSlaveInterface0 =	DYNAMIC */
};

/* the data interface has two bulk endpoints */

static struct usb_interface_descriptor rndis_data_intf = {
	.bLength =		sizeof rndis_data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	/* .iInterface = DYNAMIC */
};


static struct usb_interface_assoc_descriptor
rndis_iad_descriptor = {
	.bLength =		sizeof rndis_iad_descriptor,
	.bDescriptorType =	USB_DT_INTERFACE_ASSOCIATION,

	.bFirstInterface =	0, /* XXX, hardcoded */
	.bInterfaceCount = 	2,	// control + data
	.bFunctionClass =	USB_CLASS_COMM,
	.bFunctionSubClass =	USB_CDC_SUBCLASS_ETHERNET,
	.bFunctionProtocol =	USB_CDC_PROTO_NONE,
	/* .iFunction = DYNAMIC */
};

/* full speed support: */

static struct usb_endpoint_descriptor fs_notify_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(STATUS_BYTECOUNT),
	.bInterval =		RNDIS_STATUS_INTERVAL_MS,
};

static struct usb_endpoint_descriptor fs_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

/* high speed support: */

static struct usb_endpoint_descriptor hs_notify_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(STATUS_BYTECOUNT),
	.bInterval =		USB_MS_TO_HS_INTERVAL(RNDIS_STATUS_INTERVAL_MS)
};

static struct usb_endpoint_descriptor hs_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

/* super speed support: */

static struct usb_endpoint_descriptor ss_notify_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(STATUS_BYTECOUNT),
	.bInterval =		USB_MS_TO_HS_INTERVAL(RNDIS_STATUS_INTERVAL_MS)
};

static struct usb_ss_ep_comp_descriptor ss_intr_comp_desc = {
	.bLength =		sizeof ss_intr_comp_desc,
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 3 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
	.wBytesPerInterval =	cpu_to_le16(STATUS_BYTECOUNT),
};

static struct usb_endpoint_descriptor ss_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor ss_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ss_bulk_comp_desc = {
	.bLength =		sizeof ss_bulk_comp_desc,
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 2 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
};

USB_COMPOSITE_ENDPOINT(ep_notify, &fs_notify_desc, &hs_notify_desc,
		&ss_notify_desc, &ss_intr_comp_desc);
USB_COMPOSITE_ENDPOINT(ep_in, &fs_in_desc, &hs_in_desc,
		&ss_in_desc, &ss_bulk_comp_desc);
USB_COMPOSITE_ENDPOINT(ep_out, &fs_out_desc, &hs_out_desc,
		&ss_out_desc, &ss_bulk_comp_desc);

USB_COMPOSITE_ALTSETTING(intf0alt0, &rndis_control_intf, &ep_notify);
USB_COMPOSITE_ALTSETTING(intf1alt0, &rndis_data_intf, &ep_in, &ep_out);

USB_COMPOSITE_INTERFACE(intf0, &intf0alt0);
USB_COMPOSITE_INTERFACE(intf1, &intf1alt0);

USB_COMPOSITE_DESCRIPTORS(rndis_descs, &intf0, &intf1);

/* string descriptors: */

static struct usb_string rndis_string_defs[] = {
	[0].s = "RNDIS Communications Control",
	[1].s = "RNDIS Ethernet Data",
	[2].s = "RNDIS",
	{  } /* end of list */
};

static struct usb_gadget_strings rndis_string_table = {
	.language =		0x0409,	/* en-us */
	.strings =		rndis_string_defs,
};

static struct usb_gadget_strings *rndis_strings[] = {
	&rndis_string_table,
	NULL,
};

/*-------------------------------------------------------------------------*/

static struct sk_buff *rndis_add_header(struct gether *port,
					struct sk_buff *skb)
{
	struct sk_buff *skb2;

	skb2 = skb_realloc_headroom(skb, sizeof(struct rndis_packet_msg_type));
	rndis_add_hdr(skb2);

	dev_kfree_skb(skb);
	return skb2;
}

static void rndis_response_available(void *_rndis)
{
	struct f_rndis			*rndis = _rndis;
	struct usb_request		*req = rndis->notify_req;
	struct usb_composite_dev	*cdev = rndis->port.func.config->cdev;
	__le32				*data = req->buf;
	int				status;

	if (atomic_inc_return(&rndis->notify_count) != 1)
		return;

	/* Send RNDIS RESPONSE_AVAILABLE notification; a
	 * USB_CDC_NOTIFY_RESPONSE_AVAILABLE "should" work too
	 *
	 * This is the only notification defined by RNDIS.
	 */
	data[0] = cpu_to_le32(1);
	data[1] = cpu_to_le32(0);

	status = usb_ep_queue(rndis->notify, req, GFP_ATOMIC);
	if (status) {
		atomic_dec(&rndis->notify_count);
		DBG(cdev, "notify/0 --> %d\n", status);
	}
}

static void rndis_response_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rndis			*rndis = req->context;
	struct usb_composite_dev	*cdev = rndis->port.func.config->cdev;
	int				status = req->status;

	/* after TX:
	 *  - USB_CDC_GET_ENCAPSULATED_RESPONSE (ep0/control)
	 *  - RNDIS_RESPONSE_AVAILABLE (status/irq)
	 */
	switch (status) {
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* connection gone */
		atomic_set(&rndis->notify_count, 0);
		break;
	default:
		DBG(cdev, "RNDIS %s response error %d, %d/%d\n",
			ep->name, status,
			req->actual, req->length);
		/* FALLTHROUGH */
	case 0:
		if (ep != rndis->notify)
			break;

		/* handle multiple pending RNDIS_RESPONSE_AVAILABLE
		 * notifications by resending until we're done
		 */
		if (atomic_dec_and_test(&rndis->notify_count))
			break;
		status = usb_ep_queue(rndis->notify, req, GFP_ATOMIC);
		if (status) {
			atomic_dec(&rndis->notify_count);
			DBG(cdev, "notify/1 --> %d\n", status);
		}
		break;
	}
}

static void rndis_command_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rndis			*rndis = req->context;
	int				status;

	/* received RNDIS command from USB_CDC_SEND_ENCAPSULATED_COMMAND */
//	spin_lock(&dev->lock);
	status = rndis_msg_parser(rndis->params, (u8 *) req->buf);
	if (status < 0)
		pr_err("RNDIS command error %d, %d/%d\n",
			status, req->actual, req->length);
//	spin_unlock(&dev->lock);
}

static int
rndis_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_rndis		*rndis = func_to_rndis(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request	*req = cdev->req;
	int			value = -EOPNOTSUPP;
	u16			w_index = le16_to_cpu(ctrl->wIndex);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u16			w_length = le16_to_cpu(ctrl->wLength);

	/* composite driver infrastructure handles everything except
	 * CDC class messages; interface activation uses set_alt().
	 */
	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {

	/* RNDIS uses the CDC command encapsulation mechanism to implement
	 * an RPC scheme, with much getting/setting of attributes by OID.
	 */
	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_SEND_ENCAPSULATED_COMMAND:
		if (w_value || w_index != rndis->ctrl_id)
			goto invalid;
		/* read the request; process it later */
		value = w_length;
		req->complete = rndis_command_complete;
		req->context = rndis;
		/* later, rndis_response_available() sends a notification */
		break;

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_GET_ENCAPSULATED_RESPONSE:
		if (w_value || w_index != rndis->ctrl_id)
			goto invalid;
		else {
			u8 *buf;
			u32 n;

			/* return the result */
			buf = rndis_get_next_response(rndis->params, &n);
			if (buf) {
				memcpy(req->buf, buf, n);
				req->complete = rndis_response_complete;
				req->context = rndis;
				rndis_free_response(rndis->params, buf);
				value = n;
			}
			/* else stalls ... spec says to avoid that */
		}
		break;

	default:
invalid:
		VDBG(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (value >= 0) {
		DBG(cdev, "rndis req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
		req->zero = (value < w_length);
		req->length = value;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0)
			ERROR(cdev, "rndis response on err %d\n", value);
	}

	/* device either stalls (value < 0) or reports success */
	return value;
}


static int rndis_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_rndis		*rndis = func_to_rndis(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	/* we know alt == 0 */

	if (intf == 0) {
		VDBG(cdev, "reset rndis control %d\n", intf);

		rndis->notify = usb_function_get_ep(f, intf, 0);
		if (!rndis->notify)
			return -ENODEV;

		/* allocate notification request and buffer */
		rndis->notify_req = usb_ep_alloc_request(rndis->notify,
				GFP_KERNEL);
		if (!rndis->notify_req)
			return -ENOMEM;
		rndis->notify_req->buf = kmalloc(STATUS_BYTECOUNT, GFP_KERNEL);
		if (!rndis->notify_req->buf) {
			usb_ep_free_request(rndis->notify, rndis->notify_req);
			return -ENOMEM;
		}
		rndis->notify_req->length = STATUS_BYTECOUNT;
		rndis->notify_req->context = rndis;
		rndis->notify_req->complete = rndis_response_complete;
	} else if (intf == 1) {
		struct net_device	*net;

		rndis->port.in_ep = usb_function_get_ep(f, intf, 0);
		if (!rndis->port.in_ep)
			return -ENODEV;
		rndis->port.out_ep = usb_function_get_ep(f, intf, 1);
		if (!rndis->port.out_ep)
			return -ENODEV;

		/* Avoid ZLPs; they can be troublesome. */
		rndis->port.is_zlp_ok = false;

		/* RNDIS should be in the "RNDIS uninitialized" state,
		 * either never activated or after rndis_uninit().
		 *
		 * We don't want data to flow here until a nonzero packet
		 * filter is set, at which point it enters "RNDIS data
		 * initialized" state ... but we do want the endpoints
		 * to be activated.  It's a strange little state.
		 *
		 * REVISIT the RNDIS gadget code has done this wrong for a
		 * very long time.  We need another call to the link layer
		 * code -- gether_updown(...bool) maybe -- to do it right.
		 */
		rndis->port.cdc_filter = 0;

		DBG(cdev, "RNDIS RX/TX early activation ... \n");
		net = gether_connect(&rndis->port);
		if (IS_ERR(net))
			return PTR_ERR(net);

		rndis_set_param_dev(rndis->params, net,
				&rndis->port.cdc_filter);
	}

	return 0;
}

static void rndis_clear_alt(struct usb_function *f,
		unsigned intf, unsigned alt)
{
	struct f_rndis		*rndis = func_to_rndis(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	if (intf == 0) {
		kfree(rndis->notify_req->buf);
		usb_ep_free_request(rndis->notify, rndis->notify_req);
	} else if (intf == 1) {
		DBG(cdev, "rndis deactivated\n");
		rndis_uninit(rndis->params);
		gether_disconnect(&rndis->port);
	}
}

/*-------------------------------------------------------------------------*/

/*
 * This isn't quite the same mechanism as CDC Ethernet, since the
 * notification scheme passes less data, but the same set of link
 * states must be tested.  A key difference is that altsettings are
 * not used to tell whether the link should send packets or not.
 */

static void rndis_open(struct gether *geth)
{
	struct f_rndis		*rndis = func_to_rndis(&geth->func);
	struct usb_composite_dev *cdev = geth->func.config->cdev;

	DBG(cdev, "%s\n", __func__);

	rndis_set_param_medium(rndis->params, RNDIS_MEDIUM_802_3,
				bitrate(cdev->gadget) / 100);
	rndis_signal_connect(rndis->params);
}

static void rndis_close(struct gether *geth)
{
	struct f_rndis		*rndis = func_to_rndis(&geth->func);

	DBG(geth->func.config->cdev, "%s\n", __func__);

	rndis_set_param_medium(rndis->params, RNDIS_MEDIUM_802_3, 0);
	rndis_signal_disconnect(rndis->params);
}

/*-------------------------------------------------------------------------*/

/* Some controllers can't support RNDIS ... */
static inline bool can_support_rndis(struct usb_configuration *c)
{
	/* everything else is *presumably* fine */
	return true;
}

/* ethernet function driver setup/binding */

static int rndis_prep_descs(struct usb_function *f)
{
	struct f_rndis		*rndis = func_to_rndis(f);
	struct f_rndis_opts	*rndis_opts;
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_string       *us;
	int			status;

	rndis_opts = container_of(f->fi, struct f_rndis_opts, func_inst);

	if (!can_support_rndis(f->config))
		return -EINVAL;

	if (cdev->use_os_string) {
		f->os_desc_table = kzalloc(sizeof(*f->os_desc_table),
					   GFP_KERNEL);
		if (!f->os_desc_table)
			return -ENOMEM;
		f->os_desc_n = 1;
		f->os_desc_table[0].os_desc = &rndis_opts->rndis_os_desc;
	}

	us = usb_gstrings_attach(cdev, rndis_strings,
				 ARRAY_SIZE(rndis_string_defs));
	if (IS_ERR(us)) {
		status = PTR_ERR(us);
		goto fail;
	}
	rndis_control_intf.iInterface = us[0].id;
	rndis_data_intf.iInterface = us[1].id;
	rndis_iad_descriptor.iFunction = us[2].id;

	return usb_function_set_descs(f, &rndis_descs);

fail:
	kfree(f->os_desc_table);
	f->os_desc_n = 0;

	if (rndis->notify_req) {
		kfree(rndis->notify_req->buf);
		usb_ep_free_request(rndis->notify, rndis->notify_req);
	}

	ERROR(cdev, "%s: can't bind, err %d\n", f->name, status);

	return status;
}

static int rndis_prep_vendor_descs(struct usb_function *f)
{
	struct f_rndis		*rndis = func_to_rndis(f);
	struct f_rndis_opts	*rndis_opts;
	struct usb_composite_dev *cdev = f->config->cdev;
	int			status, intf0_id, intf1_id;

	rndis_opts = container_of(f->fi, struct f_rndis_opts, func_inst);

	/*
	 * in drivers/usb/gadget/configfs.c:configfs_composite_bind()
	 * configurations are bound in sequence with list_for_each_entry,
	 * in each configuration its functions are bound in sequence
	 * with list_for_each_entry, so we assume no race condition
	 * with regard to rndis_opts->bound access
	 */
	if (!rndis_opts->bound) {
		gether_set_gadget(rndis_opts->net, cdev->gadget);
		status = gether_register_netdev(rndis_opts->net);
		if (status)
			return status;
		rndis_opts->bound = true;
	}

	intf0_id = usb_get_interface_id(f, 0);
	intf1_id = usb_get_interface_id(f, 1);

	rndis->ctrl_id = intf0_id;
	rndis->data_id = intf1_id;

	rndis_iad_descriptor.bFirstInterface = intf0_id;

	rndis_union_desc.bMasterInterface0 = intf0_id;
	rndis_union_desc.bSlaveInterface0 = intf1_id;

	if (cdev->use_os_string)
		f->os_desc_table[0].if_id =
			rndis_iad_descriptor.bFirstInterface;

	usb_function_add_vendor_desc(f,
			(struct usb_descriptor_header *)&rndis_iad_descriptor);

	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&header_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&call_mgmt_descriptor);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&rndis_acm_descriptor);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&rndis_union_desc);

	rndis->port.open = rndis_open;
	rndis->port.close = rndis_close;

	rndis_set_param_medium(rndis->params, RNDIS_MEDIUM_802_3, 0);
	rndis_set_host_mac(rndis->params, rndis->ethaddr);

	if (rndis->manufacturer && rndis->vendorID &&
			rndis_set_param_vendor(rndis->params, rndis->vendorID,
					       rndis->manufacturer)) {
		return -EINVAL;
	}

	/* NOTE:  all that is done without knowing or caring about
	 * the network link ... which is unavailable to this code
	 * until we're activated via set_alt().
	 */

	return 0;
}

void rndis_borrow_net(struct usb_function_instance *f, struct net_device *net)
{
	struct f_rndis_opts *opts;

	opts = container_of(f, struct f_rndis_opts, func_inst);
	if (opts->bound)
		gether_cleanup(netdev_priv(opts->net));
	else
		free_netdev(opts->net);
	opts->borrowed_net = opts->bound = true;
	opts->net = net;
}
EXPORT_SYMBOL_GPL(rndis_borrow_net);

static inline struct f_rndis_opts *to_f_rndis_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_rndis_opts,
			    func_inst.group);
}

/* f_rndis_item_ops */
USB_ETHERNET_CONFIGFS_ITEM(rndis);

/* f_rndis_opts_dev_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_DEV_ADDR(rndis);

/* f_rndis_opts_host_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_HOST_ADDR(rndis);

/* f_rndis_opts_qmult */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_QMULT(rndis);

/* f_rndis_opts_ifname */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_IFNAME(rndis);

static struct configfs_attribute *rndis_attrs[] = {
	&rndis_opts_attr_dev_addr,
	&rndis_opts_attr_host_addr,
	&rndis_opts_attr_qmult,
	&rndis_opts_attr_ifname,
	NULL,
};

static struct config_item_type rndis_func_type = {
	.ct_item_ops	= &rndis_item_ops,
	.ct_attrs	= rndis_attrs,
	.ct_owner	= THIS_MODULE,
};

static void rndis_free_inst(struct usb_function_instance *f)
{
	struct f_rndis_opts *opts;

	opts = container_of(f, struct f_rndis_opts, func_inst);
	if (!opts->borrowed_net) {
		if (opts->bound)
			gether_cleanup(netdev_priv(opts->net));
		else
			free_netdev(opts->net);
	}

	kfree(opts->rndis_os_desc.group.default_groups); /* single VLA chunk */
	kfree(opts);
}

static struct usb_function_instance *rndis_alloc_inst(void)
{
	struct f_rndis_opts *opts;
	struct usb_os_desc *descs[1];
	char *names[1];

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);
	opts->rndis_os_desc.ext_compat_id = opts->rndis_ext_compat_id;

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = rndis_free_inst;
	opts->net = gether_setup_default();
	if (IS_ERR(opts->net)) {
		struct net_device *net = opts->net;
		kfree(opts);
		return ERR_CAST(net);
	}
	INIT_LIST_HEAD(&opts->rndis_os_desc.ext_prop);

	descs[0] = &opts->rndis_os_desc;
	names[0] = "rndis";
	usb_os_desc_prepare_interf_dir(&opts->func_inst.group, 1, descs,
				       names, THIS_MODULE);
	config_group_init_type_name(&opts->func_inst.group, "",
				    &rndis_func_type);

	return &opts->func_inst;
}

static void rndis_free(struct usb_function *f)
{
	struct f_rndis *rndis;
	struct f_rndis_opts *opts;

	rndis = func_to_rndis(f);
	rndis_deregister(rndis->params);
	opts = container_of(f->fi, struct f_rndis_opts, func_inst);
	kfree(rndis);
	mutex_lock(&opts->lock);
	opts->refcnt--;
	mutex_unlock(&opts->lock);
}

static void rndis_unbind(struct usb_configuration *c, struct usb_function *f)
{
	kfree(f->os_desc_table);
	f->os_desc_n = 0;
}

static struct usb_function *rndis_alloc(struct usb_function_instance *fi)
{
	struct f_rndis	*rndis;
	struct f_rndis_opts *opts;
	struct rndis_params *params;

	/* allocate and initialize one new instance */
	rndis = kzalloc(sizeof(*rndis), GFP_KERNEL);
	if (!rndis)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_rndis_opts, func_inst);
	mutex_lock(&opts->lock);
	opts->refcnt++;

	gether_get_host_addr_u8(opts->net, rndis->ethaddr);
	rndis->vendorID = opts->vendor_id;
	rndis->manufacturer = opts->manufacturer;

	rndis->port.ioport = netdev_priv(opts->net);
	mutex_unlock(&opts->lock);
	/* RNDIS activates when the host changes this filter */
	rndis->port.cdc_filter = 0;

	/* RNDIS has special (and complex) framing */
	rndis->port.header_len = sizeof(struct rndis_packet_msg_type);
	rndis->port.wrap = rndis_add_header;
	rndis->port.unwrap = rndis_rm_hdr;

	rndis->port.func.name = "rndis";
	/* descriptors are per-instance copies */
	rndis->port.func.prep_descs = rndis_prep_descs;
	rndis->port.func.prep_vendor_descs = rndis_prep_vendor_descs;
	rndis->port.func.unbind = rndis_unbind;
	rndis->port.func.set_alt = rndis_set_alt;
	rndis->port.func.clear_alt = rndis_clear_alt;
	rndis->port.func.setup = rndis_setup;
	rndis->port.func.free_func = rndis_free;

	params = rndis_register(rndis_response_available, rndis);
	if (IS_ERR(params)) {
		kfree(rndis);
		return ERR_CAST(params);
	}
	rndis->params = params;

	return &rndis->port.func;
}

DECLARE_USB_FUNCTION_INIT(rndis, rndis_alloc_inst, rndis_alloc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Brownell");
