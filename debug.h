#ifndef __DEBUG_H__
#define __DEBUG_H__

#undef DBG
#define DBG(x...) printk(KERN_ERR x)
//#define DBG(x...) printk(KERN_DEBUG x)

#undef DBG_DEV
#define DBG_DEV(cdev, x...) dev_err(&cdev->gadget->dev, x)
//#define DBG_DEV(cdev, x...) dev_dbg(&cdev->gadget->dev, x)

#undef ERR
#define ERR(x...) printk(KERN_ERR x)

#undef ERR_DEV
#define ERR_DEV(cdev, x...) dev_err(&cdev->gadget->dev, x)

#define DUMP_MSG(cdev, /* const char * */ label,			\
		   /* const u8 * */ buf, /* unsigned */ length) do {	\
	if (length <= 512) {						\
		DBG_DEV(cdev, "%s, length %u:\n", label, length);		\
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET,	\
			       16, 1, buf, length, 0);			\
	}								\
} while (0)

#endif
