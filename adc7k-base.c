/******************************************************************************/
/* adc7k-base.c                                                               */
/******************************************************************************/

#include <linux/kobject.h>
#include <linux/fs.h>
#include <linux/cdev.h>
// #include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
// #include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/io.h>
#if defined(CONFIG_COMPAT) && defined(HAVE_COMPAT_IOCTL) && (HAVE_COMPAT_IOCTL == 1)
#include <asm/compat.h>
#endif

#include "adc7k/adc7k-base.h"
#include "adc7k/version.h"

MODULE_AUTHOR("Maksym Tarasevych <mxmtar@gmail.com>");
MODULE_DESCRIPTION("ADC7K Linux base module");
MODULE_LICENSE("GPL");

static int adc7k_subsystem_major = 0;
module_param(adc7k_subsystem_major, int, 0);
MODULE_PARM_DESC(adc7k_subsystem_major, "Major number for ADC7K subsystem device");

EXPORT_SYMBOL(adc7k_board_register);
EXPORT_SYMBOL(adc7k_board_unregister);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
	#define CLASS_DEV_CREATE(_class, _devt, _device, _name) device_create(_class, _device, _devt, NULL, "%s", _name)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	#define CLASS_DEV_CREATE(_class, _devt, _device, _name) device_create(_class, _device, _devt, _name)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	#define CLASS_DEV_CREATE(_class, _devt, _device, _name) class_device_create(_class, NULL, _devt, _device, _name)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
	#define CLASS_DEV_CREATE(_class, _devt, _device, _name) class_device_create(_class, _devt, _device, _name)
#else
	#define CLASS_DEV_CREATE(_class, _devt, _device, _name) class_simple_device_add(_class, _devt, _device, _name)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	#define CLASS_DEV_DESTROY(_class, _devt) device_destroy(_class, _devt)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
	#define CLASS_DEV_DESTROY(_class, _devt) class_device_destroy(_class, _devt)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,9)
	#define CLASS_DEV_DESTROY(_class, _devt) class_simple_device_remove(_devt)
#else
	#define CLASS_DEV_DESTROY(_class, _devt) class_simple_device_remove(_class, _devt)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
	static struct class *adc7k_class = NULL;
#else
	static struct class_simple *adc7k_class = NULL;
	#define class_create(_a, _b) class_simple_create(_a, _b)
	#define class_destroy(_a) class_simple_destroy(_a)
#endif

#define verbose(_fmt, _args...) printk(KERN_INFO "[%s] " _fmt, THIS_MODULE->name, ## _args)
#define log(_level, _fmt, _args...) printk(_level "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)
#define debug(_fmt, _args...) printk(KERN_DEBUG "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)

struct adc7k_subsystem_private_data {
	char buff[0x8000];
	size_t length;
};

static struct cdev adc7k_subsystem_cdev;

static struct adc7k_board *adc7k_board_list[ADC7K_BOARD_MAX_COUNT];
static DEFINE_MUTEX(adc7k_board_list_lock);


static int adc7k_subsystem_open(struct inode *inode, struct file *filp)
{
	ssize_t res;
	size_t i;
	size_t len;

	struct adc7k_subsystem_private_data *private_data;

	if (!(private_data = kmalloc(sizeof(struct adc7k_subsystem_private_data), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory=%lu bytes\n", (unsigned long int)sizeof(struct adc7k_subsystem_private_data));
		res = -ENOMEM;
		goto adc7k_subsystem_open_error;
	}
// 	memset(private_data, 0, sizeof(struct subsystem_private_data));

	mutex_lock(&adc7k_board_list_lock);
	len = 0;
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; i++) {
		if (adc7k_board_list[i]) {
			len += sprintf(private_data->buff+len, "%s %s\r\n", adc7k_board_list[i]->cdev->owner->name,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
							dev_name(adc7k_board_list[i]->device)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
							dev_name(adc7k_board_list[i]->device)
#else
							adc7k_board_list[i]->device->class_id
#endif
						  );
		}
	}
	mutex_unlock(&adc7k_board_list_lock);

	private_data->length = len;

	filp->private_data = private_data;

	return 0;

adc7k_subsystem_open_error:
	if (private_data) {
		kfree(private_data);
	}
	return res;
}

static int adc7k_subsystem_release(struct inode *inode, struct file *filp)
{
	struct subsystem_private_data *private_data = filp->private_data;

	kfree(private_data);
	return 0;
}

static ssize_t adc7k_subsystem_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	size_t len;
	ssize_t res;
	struct adc7k_subsystem_private_data *private_data = filp->private_data;

	res = (private_data->length > filp->f_pos)?(private_data->length - filp->f_pos):(0);

	if (res) {
		len = res;
		len = min(count, len);
		if (copy_to_user(buff, private_data->buff + filp->f_pos, len)) {
			res = -EINVAL;
			goto adc7k_subsystem_read_end;
		}
		*offp = filp->f_pos + len;
	}

adc7k_subsystem_read_end:
	return res;
}

static struct file_operations adc7k_subsystem_fops = {
	.owner		= THIS_MODULE,
	.open		= adc7k_subsystem_open,
	.release	= adc7k_subsystem_release,
	.read		= adc7k_subsystem_read,
};

struct adc7k_board *adc7k_board_register(struct module *owner, char *name, struct cdev *cdev, struct file_operations *fops)
{
	size_t i;
	char brdname[ADC7K_BOARD_NAME_MAX_LENGTH];
	int rc;
	int devno = -1;
	struct adc7k_board *brd;

	if (!(brd = kmalloc(sizeof(struct adc7k_board), GFP_KERNEL))) {
		log(KERN_ERR, "\"%s\" - can't get memory for struct adc7k_board\n", name);
		goto adc7k_board_register_error;
	}

	mutex_lock(&adc7k_board_list_lock);
	// check for name is not used
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, name))) {
			mutex_unlock(&adc7k_board_list_lock);
			log(KERN_ERR, "\"%s\" already registered\n", name);
			goto adc7k_board_register_error;
		}
	}
	// get free slot
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if (!adc7k_board_list[i]) {
			devno = MKDEV(adc7k_subsystem_major, i);
			adc7k_board_list[i] = brd;
			brd->devno = devno;
			snprintf(brd->name, ADC7K_BOARD_NAME_MAX_LENGTH, "%s", name);
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);

	if (devno < 0) {
		log(KERN_ERR, "\"%s\" - can't get free slot\n", name);
		goto adc7k_board_register_error;
	}

	// Add char device to system
	cdev_init(cdev, fops);
	cdev->owner = owner;
	cdev->ops = fops;
	brd->cdev = cdev;
	if ((rc = cdev_add(cdev, devno, 1)) < 0) {
		log(KERN_ERR, "\"%s\" - cdev_add() error=%d\n", name, rc);
		goto adc7k_board_register_error;
	}
	snprintf(brdname, ADC7K_BOARD_NAME_MAX_LENGTH, "adc7k!%s", name);
	if (!(brd->device = CLASS_DEV_CREATE(adc7k_class, devno, NULL, brdname))) {
		log(KERN_ERR, "\"%s\" - class_dev_create() error\n", name);
		goto adc7k_board_register_error;
	}

	verbose("\"%s\" registered\n", name);
	return brd;

adc7k_board_register_error:
	if (devno >= 0) {
		mutex_lock(&adc7k_board_list_lock);
		for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
			if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, name))) {
				adc7k_board_list[i] = NULL;
				break;
			}
		}
		mutex_unlock(&adc7k_board_list_lock);
	}
	if (brd) {
		kfree(brd);
	}
	return NULL;
}

void adc7k_board_unregister(struct adc7k_board *brd)
{
	size_t i;

	CLASS_DEV_DESTROY(adc7k_class, brd->devno);
	cdev_del(brd->cdev);

	verbose("\"%s\" unregistered\n", brd->name);

	mutex_lock(&adc7k_board_list_lock);

	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, brd->name))) {
			kfree(adc7k_board_list[i]);
			adc7k_board_list[i] = NULL;
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);
}

static int __init adc7k_init(void)
{
	size_t i;
	dev_t devno;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *device = NULL;
#else
	struct class_device *device = NULL;
#endif
	int adc7k_subsystem_major_reg = 0;
	int rc = -1;

	verbose("loading version \"%s\"...\n", ADC7K_LINUX_VERSION);

	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		adc7k_board_list[i] = NULL;
	}

	// Registering adc7k device class
	if (!(adc7k_class = class_create(THIS_MODULE, "adc7k"))) {
		log(KERN_ERR, "class_create() error\n");
		goto adc7k_init_error;
	}
	// Register char device region
	if (adc7k_subsystem_major) {
		devno = MKDEV(adc7k_subsystem_major, 0);
		rc = register_chrdev_region(devno, ADC7K_DEVICE_MAX_COUNT, "adc7k");
	} else {
		rc = alloc_chrdev_region(&devno, 0, ADC7K_DEVICE_MAX_COUNT, "adc7k");
		if(rc >= 0) adc7k_subsystem_major = MAJOR(devno);
	}
	if (rc < 0) {
		log(KERN_ERR, "register chrdev region error=%d\n", rc);
		goto adc7k_init_error;
	}
	debug("adc7k subsystem major=%d\n", adc7k_subsystem_major);
	adc7k_subsystem_major_reg = 1;

	// Add subsystem device
	cdev_init(&adc7k_subsystem_cdev, &adc7k_subsystem_fops);
	adc7k_subsystem_cdev.owner = THIS_MODULE;
	adc7k_subsystem_cdev.ops = &adc7k_subsystem_fops;
	devno = MKDEV(adc7k_subsystem_major, ADC7K_DEVICE_MAX_COUNT - 1);
	if ((rc = cdev_add(&adc7k_subsystem_cdev, devno, 1)) < 0) {
		log(KERN_ERR, "\"subsystem\" - cdev_add() error=%d\n", rc);
		goto adc7k_init_error;
	}
	if (!(device = CLASS_DEV_CREATE(adc7k_class, devno, NULL, "adc7k!subsystem"))) {
		log(KERN_ERR, "\"subsystem\" - class_dev_create() error\n");
		goto adc7k_init_error;
	}

	verbose("loaded successfull\n");
	return 0;

adc7k_init_error:
	if (device) {
		CLASS_DEV_DESTROY(adc7k_class, MKDEV(adc7k_subsystem_major, ADC7K_DEVICE_MAX_COUNT - 1));
	}
	if (adc7k_subsystem_major_reg) {
		unregister_chrdev_region(MKDEV(adc7k_subsystem_major, 0), ADC7K_DEVICE_MAX_COUNT);
	}
	if (adc7k_class) {
		class_destroy(adc7k_class);
	}
	return rc;
}

static void __exit adc7k_exit(void)
{
	// Destroy subsystem device
	CLASS_DEV_DESTROY(adc7k_class, MKDEV(adc7k_subsystem_major, ADC7K_DEVICE_MAX_COUNT - 1));
	cdev_del(&adc7k_subsystem_cdev);
	// Unregister char device region
	unregister_chrdev_region(MKDEV(adc7k_subsystem_major, 0), ADC7K_DEVICE_MAX_COUNT);
	// Destroy adc7k device class
	class_destroy(adc7k_class);

	verbose("stopped\n");
}

module_init(adc7k_init);
module_exit(adc7k_exit);
