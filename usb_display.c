#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb/composite.h>

#include "f_display.h"

/* Defines */

#define GS_VERSION_STR			"v0.1"
#define GS_VERSION_NUM			0x0200
#define GS_LONG_NAME			"USB display"
#define GS_VERSION_NAME			GS_LONG_NAME " " GS_VERSION_STR

/*-------------------------------------------------------------------------*/
USB_GADGET_COMPOSITE_OPTIONS();

#define RP_DISP_DRIVER_NAME     "usb_display"
#define RP_DISP_USB_VENDOR_ID   0xFCCF // RP Pseudo vendor id
#define RP_DISP_USB_PRODUCT_ID  0xA001

/* string IDs are assigned dynamically */

#define STRING_DESCRIPTION_IDX		USB_GADGET_FIRST_AVAIL_IDX

static struct usb_string strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "",
	[USB_GADGET_PRODUCT_IDX].s = GS_VERSION_NAME,
	[USB_GADGET_SERIAL_IDX].s = "",
	[STRING_DESCRIPTION_IDX].s = GS_LONG_NAME,
	{  } /* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_device_descriptor device_desc = {
	.bLength =		USB_DT_DEVICE_SIZE,
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =  USB_CLASS_VENDOR_SPEC,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	/* .bMaxPacketSize0 = f(hardware) */
	.idVendor =	cpu_to_le16(RP_DISP_USB_VENDOR_ID),
	.idProduct = cpu_to_le16(RP_DISP_USB_PRODUCT_ID),
	.bcdDevice = cpu_to_le16(GS_VERSION_NUM),
	/* .iManufacturer = DYNAMIC */
	/* .iProduct = DYNAMIC */
	.bNumConfigurations =	1,
};

static struct usb_configuration serial_config_driver = {
	.label = RP_DISP_DRIVER_NAME,
	.bConfigurationValue = 1,
	/* .iConfiguration = DYNAMIC */
	.bmAttributes	= USB_CONFIG_ATT_SELFPOWER,
};

static struct usb_function *func_display;
static struct usb_function_instance *func_inst_display;

static int __init gs_bind(struct usb_composite_dev *cdev)
{
	/* Allocate string descriptor numbers ... note that string
	 * contents can be overridden by the composite_dev glue.
	 */
	int	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0)
		return status;

	device_desc.iManufacturer = strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;
	device_desc.iSerialNumber = strings_dev[USB_GADGET_SERIAL_IDX].id;

    // linux的一个BUG，如果不设置usb_composite_dev的desc的bcdDevice的话，默认会设置成内核版本号，参见composite.c的
	// update_unchanged_dev_desc(&cdev->desc, composite->dev);
    // 如果版本小于1.04协议会不同
    cdev->desc.bcdDevice = cpu_to_le16(GS_VERSION_NUM);

	func_inst_display = usb_get_function_instance("Display");
	if (IS_ERR(func_inst_display))
		return PTR_ERR(func_inst_display);

	func_display = usb_get_function(func_inst_display);
	if (IS_ERR(func_display)) {
		status = PTR_ERR(func_display);
		goto err_put_func_inst_display;
	}

    serial_config_driver.iConfiguration = strings_dev[STRING_DESCRIPTION_IDX].id;
	/* support autoresume for remote wakeup testing */
	serial_config_driver.bmAttributes &= ~USB_CONFIG_ATT_WAKEUP;
	serial_config_driver.descriptors = NULL;
	//if (autoresume) {
	//	serial_config_driver.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	//}

	/* Register primary, then secondary configuration.  Note that
	 * SH3 only allows one config...
	 */
    usb_add_config_only(cdev, &serial_config_driver);
	status = usb_add_function(&serial_config_driver, func_display);
	if (status)
		goto err_put_func_display;

	return 0;

err_put_func_display:
	usb_put_function(func_display);
	func_display = NULL;
err_put_func_inst_display:
	usb_put_function_instance(func_inst_display);
	func_inst_display = NULL;
	return status;
}

static int gs_unbind(struct usb_composite_dev *cdev)
{
	if (!IS_ERR_OR_NULL(func_display))
		usb_put_function(func_display);
	usb_put_function_instance(func_inst_display);
	return 0;
}

static __refdata struct usb_composite_driver usb_display_driver = {
	.name		= RP_DISP_DRIVER_NAME,
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_HIGH,
	.bind		= gs_bind,
	.unbind		= gs_unbind,
};

static int __init init(void)
{
    display_function_init();
    return usb_composite_probe(&usb_display_driver);
}
module_init(init);

static void __exit cleanup(void)
{
	usb_composite_unregister(&usb_display_driver);
    display_function_exit();
}
module_exit(cleanup);

MODULE_DESCRIPTION(GS_VERSION_NAME);
MODULE_AUTHOR("bobo");
MODULE_LICENSE("GPL");
