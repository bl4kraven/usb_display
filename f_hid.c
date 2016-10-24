/*
 * f_hid.c -- USB HID function driver
 *
 * Copyright (C) 2010 Fabien Chouteau <fabien.chouteau@barco.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/sched.h>
#include <linux/usb/composite.h>

#include "pixcir_i2c_ts.h"
#include "f_hid.h"
#include "debug.h"


/*-------------------------------------------------------------------------*/
/*                            HID gadget struct                            */
struct f_hidg {
	/* configuration */
	unsigned char			bInterfaceSubClass;
	unsigned char			bInterfaceProtocol;
	unsigned short			report_desc_length;
	char				    *report_desc;
	unsigned short			report_length;

	/* send report */
	struct usb_function		func;
	struct usb_ep			*in_ep;
};

static inline struct f_hidg *func_to_hidg(struct usb_function *f)
{
	return container_of(f, struct f_hidg, func);
}

/*-------------------------------------------------------------------------*/
/*                           Static descriptors                            */

static struct usb_interface_descriptor hidg_interface_desc = {
	.bLength		= sizeof hidg_interface_desc,
	.bDescriptorType	= USB_DT_INTERFACE,
	/* .bInterfaceNumber	= DYNAMIC */
	.bAlternateSetting	= 0,
	.bNumEndpoints		= 1,
	.bInterfaceClass	= USB_CLASS_HID,
	/* .bInterfaceSubClass	= DYNAMIC */
	/* .bInterfaceProtocol	= DYNAMIC */
	/* .iInterface		= DYNAMIC */
};


static struct hid_descriptor hidg_desc = {
	.bLength			= sizeof hidg_desc,
	.bDescriptorType		= HID_DT_HID,
	.bcdHID				= 0x0111,
	.bCountryCode			= 0x00,
	.bNumDescriptors		= 0x1,
	/*.desc[0].bDescriptorType	= DYNAMIC */
	/*.desc[0].wDescriptorLenght	= DYNAMIC */
};

/* High-Speed Support */
static struct usb_endpoint_descriptor hidg_hs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 4, /* FIXME: Add this field in the
				      * HID gadget configuration?
				      * (struct hidg_func_descriptor)
				      */
};

static struct usb_descriptor_header *hidg_hs_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_hs_in_ep_desc,
	NULL,
};

/* Full-Speed Support */
static struct usb_endpoint_descriptor hidg_fs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 20, /* FIXME: Add this field in the
				       * HID gadget configuration?
				       * (struct hidg_func_descriptor)
				       */
};

static struct usb_descriptor_header *hidg_fs_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_fs_in_ep_desc,
	NULL,
};

/*-------------------------------------------------------------------------*/
/*                                usb_function                             */
static int hidg_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_hidg			*hidg = func_to_hidg(f);
	struct usb_composite_dev	*cdev = f->config->cdev;
	struct usb_request		*req  = cdev->req;
	int status = 0;
	__u16 value, length;

	value	= __le16_to_cpu(ctrl->wValue);
	length	= __le16_to_cpu(ctrl->wLength);

	DBG_DEV(cdev, "hid_setup crtl_request : bRequestType:0x%x bRequest:0x%x "
		"Value:0x%x\n", ctrl->bRequestType, ctrl->bRequest, value);

	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {
	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_REPORT):
		DBG_DEV(cdev, "get_report\n");

		/* send an empty report */
		length = min_t(unsigned, length, hidg->report_length);
		memset(req->buf, 0x0, length);

		goto respond;
		break;

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_PROTOCOL):
		DBG_DEV(cdev, "get_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_REPORT):
		DBG_DEV(cdev, "set_report | wLenght=%d\n", ctrl->wLength);
		goto stall;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_PROTOCOL):
		DBG_DEV(cdev, "set_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) << 8
		  | USB_REQ_GET_DESCRIPTOR):
		switch (value >> 8) {
		case HID_DT_HID:
			DBG_DEV(cdev, "USB_REQ_GET_DESCRIPTOR: HID\n");
			length = min_t(unsigned short, length,
						   hidg_desc.bLength);
			memcpy(req->buf, &hidg_desc, length);
			goto respond;
			break;
		case HID_DT_REPORT:
			DBG_DEV(cdev, "USB_REQ_GET_DESCRIPTOR: REPORT\n");
			length = min_t(unsigned short, length,
						   hidg->report_desc_length);
			memcpy(req->buf, hidg->report_desc, length);
			goto respond;
			break;

		default:
			DBG_DEV(cdev, "Unknown descriptor request 0x%x\n",
				 value >> 8);
			goto stall;
			break;
		}
		break;

	default:
		DBG_DEV(cdev, "Unknown request 0x%x\n",
			 ctrl->bRequest);
		goto stall;
		break;
	}

stall:
	return -EOPNOTSUPP;

respond:
	req->zero = 0;
	req->length = length;
	status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (status < 0)
		ERROR(cdev, "usb_ep_queue error on ep0 %d\n", value);
	return status;
}

static void hidg_disable(struct usb_function *f)
{
	struct f_hidg *hidg = func_to_hidg(f);
	usb_ep_disable(hidg->in_ep);
	hidg->in_ep->driver_data = NULL;
}

static int hidg_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_composite_dev		*cdev = f->config->cdev;
	struct f_hidg				*hidg = func_to_hidg(f);
	int status = 0;

	DBG_DEV(cdev, "hidg_set_alt intf:%d alt:%d\n", intf, alt);

	if (hidg->in_ep != NULL) {
		/* restart endpoint */
		if (hidg->in_ep->driver_data != NULL)
			usb_ep_disable(hidg->in_ep);

		status = config_ep_by_speed(f->config->cdev->gadget, f,
					    hidg->in_ep);
		if (status) {
			ERROR(cdev, "config_ep_by_speed FAILED!\n");
			goto fail;
		}
		status = usb_ep_enable(hidg->in_ep);
		if (status < 0) {
			ERROR(cdev, "Enable IN endpoint FAILED!\n");
			goto fail;
		}
		hidg->in_ep->driver_data = hidg;
	}

fail:
	return status;
}

static int __init hidg_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_ep		*ep;
	struct f_hidg		*hidg = func_to_hidg(f);
	int			status;

	/* allocate instance-specific interface IDs, and patch descriptors */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	hidg_interface_desc.bInterfaceNumber = status;

	/* allocate instance-specific endpoints */
	status = -ENODEV;
	ep = usb_ep_autoconfig(c->cdev->gadget, &hidg_fs_in_ep_desc);
	if (!ep)
		goto fail;
	ep->driver_data = c->cdev;	/* claim */
	hidg->in_ep = ep;

	/* set descriptor dynamic values */
	hidg_interface_desc.bInterfaceSubClass = hidg->bInterfaceSubClass;
	hidg_interface_desc.bInterfaceProtocol = hidg->bInterfaceProtocol;

	hidg_hs_in_ep_desc.bEndpointAddress = hidg_fs_in_ep_desc.bEndpointAddress;
	hidg_hs_in_ep_desc.wMaxPacketSize = cpu_to_le16(hidg->report_length);
	hidg_fs_in_ep_desc.wMaxPacketSize = cpu_to_le16(hidg->report_length);

	hidg_desc.desc[0].bDescriptorType = HID_DT_REPORT;
	hidg_desc.desc[0].wDescriptorLength = cpu_to_le16(hidg->report_desc_length);

	status = usb_assign_descriptors(f,  hidg_fs_descriptors, hidg_hs_descriptors, NULL);
	//status = usb_assign_descriptors(f,  hidg_fs_descriptors, NULL, NULL);
	if (status)
		goto fail;

	return 0;

fail:
	ERROR(f->config->cdev, "hidg_bind FAILED\n");
	usb_free_all_descriptors(f);
	return status;
}

static void hidg_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_hidg *hidg = func_to_hidg(f);

    pixcir_exit();
	/* disable/free request and end point */
	//usb_ep_disable(hidg->in_ep);
	usb_free_all_descriptors(f);

	kfree(hidg->report_desc);
	kfree(hidg);
    DBG("hidg_unbind\n");
}

static void f_touch_req_complete(struct usb_ep *ep, struct usb_request *req)
{
	//struct f_hidg *hidg = (struct f_hidg *)ep->driver_data;
    if (req->status != 0) 
    {
        ERR("touch request fail!\n");
    }
    else
    {
        DBG("touch request OK\n");
    }

    // just release it 
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

static void touch_callback(int touch, int x, int y, void *data)
{
	struct f_hidg *hidg = (struct f_hidg *)data;
    char mouse_data[5] = {touch, x, x>>8, y, y>>8};
	struct usb_request *req = usb_ep_alloc_request(hidg->in_ep, GFP_ATOMIC);
	if (req) {
		req->length = sizeof(mouse_data);
		req->buf = kmalloc(req->length, GFP_ATOMIC);
		if (req->buf)
        {
            int result;
            req->status   = 0;
            req->zero     = 0;
            req->complete = f_touch_req_complete;
            memcpy(req->buf, mouse_data, sizeof(mouse_data));
            result = usb_ep_queue(hidg->in_ep, req, GFP_ATOMIC);
            if (result)
                DBG("%s queue req fail --> %d\n", hidg->in_ep->name, result);
		}
        else
        {
			usb_ep_free_request(hidg->in_ep, req);
			req = NULL;
        }
	}

    DBG("touch:%d x:%d y:%d\n", touch, x, y);
}
/*-------------------------------------------------------------------------*/
/*                                 Strings                                 */

#define CT_FUNC_HID_IDX	0

static struct usb_string ct_func_string_defs[] = {
	[CT_FUNC_HID_IDX].s	= "HID Interface",
	{},			/* end of list */
};

static struct usb_gadget_strings ct_func_string_table = {
	.language	= 0x0409,	/* en-US */
	.strings	= ct_func_string_defs,
};

static struct usb_gadget_strings *ct_func_strings[] = {
	&ct_func_string_table,
	NULL,
};

/*-------------------------------------------------------------------------*/
/*                             usb_configuration                           */
static struct hidg_func_descriptor __initdata s_fdesc = {
	.subclass = 0,
	.protocol = USB_INTERFACE_PROTOCOL_MOUSE,
    // 中断数据长度(按键(1)，X(2), Y(2))
	.report_length = 5,
	.report_desc_length = 64,
	.report_desc = {
        0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
        0x09, 0x02,                    // USAGE (Mouse)
        0xa1, 0x01,                    // COLLECTION (Application)
        0x09, 0x01,                    //   USAGE (Pointer)
        0xa1, 0x00,                    //   COLLECTION (Physical)
        0x05, 0x09,                    //     USAGE_PAGE (Button)
        0x19, 0x01,                    //     USAGE_MINIMUM (Button 1)
        0x29, 0x03,                    //     USAGE_MAXIMUM (Button 3)
        0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
        0x25, 0x01,                    //     LOGICAL_MAXIMUM (1)
        0x95, 0x03,                    //     REPORT_COUNT (3)
        0x75, 0x01,                    //     REPORT_SIZE (1)
        0x81, 0x02,                    //     INPUT (Data,Var,Abs)
        0x95, 0x01,                    //     REPORT_COUNT (1)
        0x75, 0x05,                    //     REPORT_SIZE (5)
        0x81, 0x03,                    //     INPUT (Cnst,Var,Abs)
        0x05, 0x01,                    //     USAGE_PAGE (Generic Desktop)
        0x09, 0x30,                    //     USAGE (X)
        0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
        0x26, 0x1f, 0x03,              //     LOGICAL_MAXIMUM (799)
        0x75, 0x10,                    //     REPORT_SIZE (16)
        0x95, 0x01,                    //     REPORT_COUNT (1)
        0x81, 0x02,                    //     INPUT (Data,Var,Abs)
        0x05, 0x01,                    //     USAGE_PAGE (Generic Desktop)
        0x09, 0x31,                    //     USAGE (Y)
        0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
        0x26, 0xdf, 0x01,              //     LOGICAL_MAXIMUM (479)
        0x75, 0x10,                    //     REPORT_SIZE (16)
        0x95, 0x01,                    //     REPORT_COUNT (1)
        0x81, 0x02,                    //     INPUT (Data,Var,Abs)
        0xc0,                          //   END_COLLECTION
        0xc0                           // END_COLLECTION
    },
};

int __init add_hid_function(struct usb_configuration *c)
{
	struct f_hidg *hidg;
	int status;

	/* maybe allocate device-global string IDs, and patch descriptors */
	if (ct_func_string_defs[CT_FUNC_HID_IDX].id == 0) {
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ct_func_string_defs[CT_FUNC_HID_IDX].id = status;
		hidg_interface_desc.iInterface = status;
	}

	/* allocate and initialize one new instance */
	hidg = kzalloc(sizeof *hidg, GFP_KERNEL);
	if (!hidg)
		return -ENOMEM;

	hidg->bInterfaceSubClass = s_fdesc.subclass;
	hidg->bInterfaceProtocol = s_fdesc.protocol;
	hidg->report_length = s_fdesc.report_length;
	hidg->report_desc_length = s_fdesc.report_desc_length;
	hidg->report_desc = kmemdup(s_fdesc.report_desc, s_fdesc.report_desc_length, GFP_KERNEL);
	if (!hidg->report_desc) {
		kfree(hidg);
		return -ENOMEM;
	}

	hidg->func.name    = "hid";
	hidg->func.strings = ct_func_strings;
	hidg->func.bind    = hidg_bind;
	hidg->func.unbind  = hidg_unbind;
	hidg->func.set_alt = hidg_set_alt;
	hidg->func.disable = hidg_disable;
	hidg->func.setup   = hidg_setup;

    status = pixcir_init(touch_callback, hidg);
    if (status)
    {
        kfree(hidg);
        return status;
    }

	status = usb_add_function(c, &hidg->func);
	if (status)
        kfree(hidg);

    return status;
}
