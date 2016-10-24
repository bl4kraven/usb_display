#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/vmalloc.h>
#include <linux/usb/composite.h>
#include <linux/fb.h>

#include "debug.h"
#include "f_display.h"
#include "protocol.h"

#define RP_DISP_DEFAULT_HEIGHT      480
#define RP_DISP_DEFAULT_WIDTH       800
#define RP_DISP_DEFAULT_PIXEL_BITS  16

// 但版本大于1.04时，新加了cmd=5协议，在原有的bitblt基础上加了压缩算法，以128字节为一个段，消耗一字节做重复计数
#define __BUFFER_SIZE  (RP_DISP_DEFAULT_HEIGHT*RP_DISP_DEFAULT_WIDTH*RP_DISP_DEFAULT_PIXEL_BITS/8)
#define BUFFER_SIZE (__BUFFER_SIZE+(((__BUFFER_SIZE>>1) + 0x7f)>>7))

#define BUFFER_COUNT 2
#define USB_BULK_MAX_PACKET 512

// USB TOUCH包大小用于interrupt endpoint
#define USB_TOUCH_PACKET_SIZE sizeof(rpusbdisp_status_normal_packet_t)

struct display_buffer
{
    volatile int cmd;
    volatile int count;
    unsigned char head[16];
    unsigned char buffer[BUFFER_SIZE];
};

struct f_display
{
	struct usb_function	function;
	struct usb_ep		*in_ep;
	struct usb_ep		*out_ep;

    // bind framebuffer
    struct fb_info *fb;

    // circular buffer
    volatile struct display_buffer *buffer_head;
    volatile struct display_buffer *buffer_tail;
    struct display_buffer *buffers;

    // for debug
    volatile int irq_count;
};

static void display_do_tasklet(unsigned long);
DECLARE_TASKLET(display_tasklet, display_do_tasklet, 0);

static inline struct f_display *func_to_display(struct usb_function *f)
{
	return container_of(f, struct f_display, function);
}

// 增加循环Buffer指针
static inline void circular_buffer_incr(struct f_display *display, volatile struct display_buffer **p)
{
	if (*p == (display->buffers + BUFFER_COUNT - 1))
		*p = display->buffers;
	else
		(*p)++;
}

/*-------------------------------------------------------------------------*/
static struct usb_interface_descriptor display_intf = {
	.bLength =		sizeof display_intf,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	/* .iInterface = DYNAMIC */
};

/* full speed support: */
static struct usb_endpoint_descriptor fs_display_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize	=  USB_TOUCH_PACKET_SIZE,
	.bInterval	= 4, 
};

static struct usb_endpoint_descriptor fs_display_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_descriptor_header *fs_display_descs[] = {
	(struct usb_descriptor_header *) &display_intf,
	(struct usb_descriptor_header *) &fs_display_sink_desc,
	(struct usb_descriptor_header *) &fs_display_source_desc,
	NULL,
};

/* high speed support: */
static struct usb_endpoint_descriptor hs_display_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize	= USB_TOUCH_PACKET_SIZE,
	.bInterval	= 4, 
};

static struct usb_endpoint_descriptor hs_display_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(USB_BULK_MAX_PACKET),
};

static struct usb_descriptor_header *hs_display_descs[] = {
	(struct usb_descriptor_header *) &display_intf,
	(struct usb_descriptor_header *) &hs_display_source_desc,
	(struct usb_descriptor_header *) &hs_display_sink_desc,
	NULL,
};

/* function-specific strings: */
static struct usb_string strings_display[] = {
	[0].s = "usb display",
	{}			/* end of list */
};

static struct usb_gadget_strings stringtab_display = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_display,
};

static struct usb_gadget_strings *display_strings[] = {
	&stringtab_display,
	NULL,
};

/*-------------------------------------------------------------------------*/
static void free_ep_req(struct usb_ep *ep, struct usb_request *req)
{
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

static void disable_ep(struct usb_composite_dev *cdev, struct usb_ep *ep)
{
	int	value;
	if (ep->driver_data) {
		value = usb_ep_disable(ep);
		if (value < 0)
			DBG_DEV(cdev, "disable %s --> %d\n", ep->name, value);
		ep->driver_data = NULL;
	}
}

static int display_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct f_display *display = func_to_display(f);
	int ret;

	/* allocate interface ID(s) */
	int id = usb_interface_id(c, f);
	if (id < 0)
		return id;
	display_intf.bInterfaceNumber = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_display[0].id = id;
	display_intf.iInterface = id;

	/* allocate endpoints */
	display->in_ep = usb_ep_autoconfig(cdev->gadget, &fs_display_source_desc);
	if (!display->in_ep) {
autoconf_fail:
		ERR_DEV(cdev, "%s: can't autoconfigure on %s\n", f->name, cdev->gadget->name);
		return -ENODEV;
	}
	display->in_ep->driver_data = cdev;	/* claim */

	display->out_ep = usb_ep_autoconfig(cdev->gadget, &fs_display_sink_desc);
	if (!display->out_ep)
		goto autoconf_fail;
	display->out_ep->driver_data = cdev;	/* claim */

	/* support high speed hardware */
	hs_display_source_desc.bEndpointAddress = fs_display_source_desc.bEndpointAddress;
	hs_display_sink_desc.bEndpointAddress = fs_display_sink_desc.bEndpointAddress;

	ret = usb_assign_descriptors(f, fs_display_descs, hs_display_descs, NULL);
	if (ret)
		return ret;

	DBG_DEV(cdev, "%s speed %s: IN/%s, OUT/%s\n",
	     (gadget_is_dualspeed(c->cdev->gadget) ? "high" : "full"),
			f->name, display->in_ep->name, display->out_ep->name);
	return 0;
}

static void display_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_display	*display = ep->driver_data;
	struct usb_composite_dev *cdev = display->function.config->cdev;
	int	status = req->status;
	switch (status) {
	case 0:/* normal completion? */
		if (ep == display->out_ep) {
            //DBG_DEV(cdev, "receive data %d irq:%ld\n", req->actual, in_interrupt());
            unsigned char cmd = ((unsigned char *)req->buf)[0];
            if ((cmd&RPUSBDISP_CMD_MASK) == RPUSBDISP_DISPCMD_BITBLT ||
                (cmd&RPUSBDISP_CMD_MASK) == RPUSBDISP_DISPCMD_BITBLT_RLE)
            {
                // bitblt直接颜色数组
                // bitblt_rel用压缩算法，原理是分成最大128字节的段，段头一个字节表示长度和是否是相同颜色
                if (cmd & RPUSBDISP_CMD_FLAG_START)
                {
                    //DUMP_MSG(cdev, "recv bitblt", req->buf, req->actual);
                    rpusbdisp_disp_bitblt_packet_t *p = (rpusbdisp_disp_bitblt_packet_t *)req->buf;
                    //p->x = le16_to_cpu(p->x);
                    //p->y = le16_to_cpu(p->y);
                    //p->width = le16_to_cpu(p->width);
                    //p->height = le16_to_cpu(p->height);
                    //DBG_DEV(cdev, "bitblt x:%d y:%d width:%d height:%d\n", p->x, p->y, p->width, p->height);
                    unsigned char *data = (unsigned char *)(p+1);
                    display->buffer_head->cmd = cmd & RPUSBDISP_CMD_MASK;
                    display->buffer_head->count = req->actual - sizeof(rpusbdisp_disp_bitblt_packet_t);
                    memcpy((unsigned char *)(display->buffer_head->head), p, sizeof(rpusbdisp_disp_bitblt_packet_t));
                    memcpy((unsigned char *)(display->buffer_head->buffer), data, display->buffer_head->count);
                    //DBG_DEV(cdev, "bitblt cmd:%d count:%d headsize:%d\n", display->buffer_head->cmd, display->buffer_head->count, sizeof(rpusbdisp_disp_bitblt_packet_t));
                }
                else
                {
                    rpusbdisp_disp_packet_header_t *p = (rpusbdisp_disp_packet_header_t *)req->buf;
                    unsigned char *data = (unsigned char *)(p+1);
                    int count = req->actual - sizeof(rpusbdisp_disp_packet_header_t);
                    if (count+display->buffer_head->count <= BUFFER_SIZE)
                    {
                        memcpy((unsigned char *)(display->buffer_head->buffer+display->buffer_head->count), data, count);
                        display->buffer_head->count += count;
                        if (req->actual != USB_BULK_MAX_PACKET)
                        {
                            // packet end
                            //DBG_DEV(cdev, "recv sub bitblt cmd:%d count:%d\n", p->cmd_flag, display->buffer_head->count);
                            circular_buffer_incr(display, &display->buffer_head);
                            //DBG_DEV(cdev, "bitblt buffer %p %p\n", display->buffer_head, display->buffer_tail);
                            display_tasklet.data = (unsigned long)(display);
                            display->irq_count++;
                            tasklet_schedule(&display_tasklet);
                        }
                    }
                    else
                    {
                        ERR_DEV(cdev, "too big!!\n");
                    }
                }
            }
            else
            {
                ERR_DEV(cdev, "other cmd type!!\n");
            }

            status = usb_ep_queue(display->out_ep, req, GFP_ATOMIC);
            if (status == 0)
            	return;
		}

		/* "should never get here" */
		/* FALLTHROUGH */
	default:
		DBG_DEV(cdev, "%s display complete --> %d, %d/%d\n", ep->name, status, req->actual, req->length);
		/* FALLTHROUGH */

	/* NOTE:  since this driver doesn't maintain an explicit record
	 * of requests it submitted (just maintains qlen count), we
	 * rely on the hardware driver to clean up on disconnect or
	 * endpoint disable.
	 */
	case -ECONNABORTED:		/* hardware forced ep reset */
	case -ECONNRESET:		/* request dequeued */
	case -ESHUTDOWN:		/* disconnect from host */
        DBG_DEV(cdev, "disconnect free usb_request\n");
		free_ep_req(ep, req);
		return;
	}
}

static void disable_display(struct f_display *display)
{
	struct usb_composite_dev	*cdev;
	cdev = display->function.config->cdev;
	disable_ep(cdev, display->in_ep);
	disable_ep(cdev, display->out_ep);
	DBG_DEV(cdev, "%s disabled\n", display->function.name);
}

static int enable_display(struct usb_composite_dev *cdev, struct f_display *display)
{
	int	result = 0;
	struct usb_ep *ep;
	struct usb_request *req;

	/* one endpoint writes data back IN to the host */
	ep = display->in_ep;
	result = config_ep_by_speed(cdev->gadget, &(display->function), ep);
	if (result)
		return result;
	result = usb_ep_enable(ep);
	if (result < 0)
		return result;
	ep->driver_data = display;

	/* one endpoint just reads OUT packets */
	ep = display->out_ep;
	result = config_ep_by_speed(cdev->gadget, &(display->function), ep);
	if (result)
		goto fail0;

	result = usb_ep_enable(ep);
	if (result < 0) {
fail0:
		ep = display->in_ep;
		usb_ep_disable(ep);
		ep->driver_data = NULL;
		return result;
	}
	ep->driver_data = display;

    req = usb_ep_alloc_request(display->out_ep, GFP_KERNEL);
    if (req) {
        req->length = USB_BULK_MAX_PACKET;
        req->buf = kmalloc(req->length, GFP_KERNEL);
        if (!req->buf) {
            usb_ep_free_request(ep, req);
            req = NULL;
        }
    }

    if (req)
    {
        req->complete = display_complete;
        result = usb_ep_queue(display->out_ep, req, GFP_KERNEL);
        if (result)
            DBG_DEV(cdev, "%s queue req --> %d\n", ep->name, result);
    }

	DBG_DEV(cdev, "%s enabled\n", display->function.name);
	return result;
}

static int display_set_alt(struct usb_function *f,
		unsigned intf, unsigned alt)
{
	struct f_display *display = func_to_display(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	/* we know alt is zero */
	if (display->in_ep->driver_data)
		disable_display(display);
	return enable_display(cdev, display);
}

static void display_disable(struct usb_function *f)
{
	struct f_display*display = func_to_display(f);
	disable_display(display);
}

static void display_do_tasklet(unsigned long data)
{
    struct f_display *display = (struct f_display *)data;
    struct usb_composite_dev *cdev = display->function.config->cdev;
    //DBG_DEV(cdev, "in tasklet irq count:%d\n", display->irq_count);
    display->irq_count = 0;

    if (display->fb && (display->buffer_tail != display->buffer_head))
    {
        volatile struct display_buffer *cur_buffer = display->buffer_tail;
        if (cur_buffer->cmd == RPUSBDISP_DISPCMD_BITBLT ||
            cur_buffer->cmd == RPUSBDISP_DISPCMD_BITBLT_RLE)
        {
            rpusbdisp_disp_bitblt_packet_t *p = (rpusbdisp_disp_bitblt_packet_t *)cur_buffer->head;
            unsigned int start = (p->x*display->fb->var.bits_per_pixel + p->y*display->fb->fix.line_length*8)/8;
            unsigned char __iomem *dst = (unsigned char *)(display->fb->screen_base + start);
            if (cur_buffer->cmd == RPUSBDISP_DISPCMD_BITBLT)
            {
                if (start+cur_buffer->count <= display->fb->fix.smem_len)
                {
                    fb_memcpy_tofb(dst, (const char *)cur_buffer->buffer, cur_buffer->count);
                }
                else
                {
                    ERR_DEV(cdev, "fb copy to large!\n");
                }
            }
            else
            {
                unsigned char * const data_origin = (unsigned char *)cur_buffer->buffer;
                unsigned char *data = data_origin;
                unsigned char section_head;
                int copy_len = 0;
                int cur_len = 0;
                int i;
                while ((data-data_origin) < cur_buffer->count)
                {
                    section_head = data[0];
                    data++;
                    cur_len = ((section_head&RPUSBDISP_RLE_BLOCKFLAG_SIZE_BIT)+1)*RP_DISP_DEFAULT_PIXEL_BITS/8;
                    if (section_head & RPUSBDISP_RLE_BLOCKFLAG_COMMON_BIT)
                    {
#if RP_DISP_DEFAULT_PIXEL_BITS != 16
#error "not support now"
#endif
                        for (i=0; i<(cur_len/2); i++, dst+=2)
                        {
                            *(unsigned short *)dst = *(unsigned short *)data;
                        }
                        data+=2;
                    }
                    else
                    {
                        fb_memcpy_tofb(dst, data, cur_len);
                        dst += cur_len;
                        data += cur_len;
                    }

                    copy_len += cur_len;
                }

                //DBG_DEV(cdev, "fb copy:%d\n", copy_len);
            }

            //DBG_DEV(cdev, "fbinfo type:%d visual:%d bpp:%d %d %d %d\n", 
            //        display->fb->fix.type,
            //        display->fb->fix.visual,
            //        display->fb->var.bits_per_pixel,
            //        display->fb->var.red.offset,
            //        display->fb->var.green.offset,
            //        display->fb->var.blue.offset
            //        );
        }

        circular_buffer_incr(display, &display->buffer_tail);
    }
    else
    {
        ERR_DEV(cdev, "no fb:%p or empty buffer!\n", display->fb);
    }
}

/*-------------------------------------------------------------------------*/
static void display_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_display *display = func_to_display(f);

    tasklet_disable(&display_tasklet);

	usb_free_all_descriptors(f);
    vfree(display->buffers);

    if (display->fb)
    {
        if (display->fb->fbops->fb_release)
            display->fb->fbops->fb_release(display->fb, 0);
        module_put(display->fb->fbops->owner);
    }

	kfree(display);
    DBG("display_unbind\n");
}

int __init add_display_function(struct usb_configuration *c)
{
    int ret;
    struct fb_info *fb0 = NULL;
	struct f_display *display = kzalloc(sizeof(struct f_display), GFP_KERNEL);
	if (!display)
		return -ENOMEM;

	display->function.name = "display";
	display->function.bind = display_bind;
	display->function.set_alt = display_set_alt;
	display->function.disable = display_disable;
	display->function.strings = display_strings;
	//display->function.free_func = display_free_func;
    display->function.unbind = display_unbind;
    display->buffers = vmalloc_32(sizeof(struct display_buffer)*BUFFER_COUNT);
    if (!display->buffers)
    {
        ret = -ENOMEM;
        goto VMALLOC;
    }
	memset(display->buffers, 0, sizeof(struct display_buffer)*BUFFER_COUNT); 
    display->buffer_head = display->buffers;
    display->buffer_tail = display->buffers;

    fb0 = registered_fb[0];
    if (fb0)
    {
        if (fb0->fbops->owner && !try_module_get(fb0->fbops->owner))
        {
            ERR("get framebuffer module error\n");
            ret = -ENODEV;
            goto VMALLOC;
        }

        if (fb0->fbops->fb_open)
        {
            mutex_lock(&fb0->lock);
            if (fb0->fbops->fb_open(fb0, 0))
            {
                ERR("fb0 open fail\n");
                mutex_unlock(&fb0->lock);
                module_put(fb0->fbops->owner);
                ret = -EBUSY;
                goto VMALLOC;
            }
            mutex_unlock(&fb0->lock);
        }
        display->fb = fb0;
    }
    else
    {
        ERR("no fb0\n");
        ret = -ENODEV;
        goto VMALLOC;
    }

	ret = usb_add_function(c, &display->function);
	if (ret)
    {
        goto VMALLOC;
    }

    return ret;
VMALLOC:
    kfree(display);
	return ret;
}
