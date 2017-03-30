/******************************************************************************/
/* adc7k-base.c                                                               */
/******************************************************************************/

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

// #include <asm/atomic.h>
// #if defined(CONFIG_COMPAT) && defined(HAVE_COMPAT_IOCTL) && (HAVE_COMPAT_IOCTL == 1)
// #include <asm/compat.h>
// #endif
// #include <asm/io.h>
#include <asm/uaccess.h>

#include "adc7k/adc7k-base.h"
#include "adc7k/version.h"

MODULE_AUTHOR("Maksym Tarasevych <mxmtar@gmail.com>");
MODULE_DESCRIPTION("ADC7K Linux base module");
MODULE_LICENSE("GPL");

static int major = 0;
module_param(major, int, 0);
MODULE_PARM_DESC(major, "Major number for ADC7K subsystem device");

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
	char buff[0xc000];
	size_t length;
	loff_t f_pos;
};

static struct cdev adc7k_subsystem_cdev;
static int adc7k_subsystem_cdev_minor_alloc[ADC7K_DEVICE_MAX_COUNT];

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
	private_data->f_pos = 0;

	mutex_lock(&adc7k_board_list_lock);
	len = 0;
	len += sprintf(private_data->buff + len, "{\r\n");
	len += sprintf(private_data->buff + len, "\r\n\t\"boards\": [");
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; i++) {
		if (adc7k_board_list[i]) {
			len += sprintf(private_data->buff + len, "%s\r\n\t\t{\r\n\t\t\t\"driver\": \"%s\",\r\n\t\t\t\"path\": \"%s\"\r\n\t\t}",
							i ? "," : "",
							adc7k_board_list[i]->char_device->owner->name,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
							dev_name(adc7k_board_list[i]->class_device)
#else
							adc7k_board_list[i]->class_device->class_id
#endif
							);
		}
	}
	len += sprintf(private_data->buff + len, "\r\n\t]");
	len += sprintf(private_data->buff + len, "\r\n}\r\n");
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

	res = (private_data->length > private_data->f_pos) ? (private_data->length - private_data->f_pos) : (0);

	if (res) {
		len = res;
		len = min(count, len);
		if (copy_to_user(buff, private_data->buff + private_data->f_pos, len)) {
			res = -EINVAL;
			goto adc7k_subsystem_read_end;
		}
		private_data->f_pos += len;
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
	char board_name[ADC7K_BOARD_NAME_MAX_LENGTH];
	int rc;
	int minor = -1;
	struct adc7k_board *board;

	if (!(board = kmalloc(sizeof(struct adc7k_board), GFP_KERNEL))) {
		log(KERN_ERR, "\"%s\" - can't get memory for struct adc7k_board\n", name);
		goto adc7k_board_register_error;
	}
	memset(board, 0, sizeof(struct adc7k_board));

	mutex_lock(&adc7k_board_list_lock);
	// check for name is not used
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, name))) {
			mutex_unlock(&adc7k_board_list_lock);
			log(KERN_ERR, "\"%s\" already registered\n", name);
			goto adc7k_board_register_error;
		}
	}
	// get free minor number
	for (i = 0; i < ADC7K_DEVICE_MAX_COUNT; ++i) {
		if (!adc7k_subsystem_cdev_minor_alloc[i]) {
			adc7k_subsystem_cdev_minor_alloc[i] = 1;
			minor = i;
			break;
		}
	}
	if (minor >= 0) {
		// get free board list slot
		for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
			if (!adc7k_board_list[i]) {
				adc7k_board_list[i] = board;
				board->device_number = MKDEV(major, minor);
				snprintf(board->name, ADC7K_BOARD_NAME_MAX_LENGTH, "%s", name);
				break;
			}
		}
	}
	mutex_unlock(&adc7k_board_list_lock);

	if (minor < 0) {
		log(KERN_ERR, "\"%s\" - can't get free minor number\n", name);
		goto adc7k_board_register_error;
	}
	if (i == ADC7K_BOARD_MAX_COUNT) {
		log(KERN_ERR, "\"%s\" - can't get free board list slot\n", name);
		goto adc7k_board_register_error;
	}

	// Add char device to system
	cdev_init(cdev, fops);
	cdev->owner = owner;
	cdev->ops = fops;
	board->char_device = cdev;
	if ((rc = cdev_add(cdev, board->device_number, 1)) < 0) {
		log(KERN_ERR, "\"%s\" - cdev_add() error=%d\n", name, rc);
		goto adc7k_board_register_error;
	}
	snprintf(board_name, ADC7K_BOARD_NAME_MAX_LENGTH, "adc7k!%s", name);
	if (!(board->class_device = CLASS_DEV_CREATE(adc7k_class, board->device_number, NULL, board_name))) {
		log(KERN_ERR, "\"%s\" - class_dev_create() error\n", name);
		goto adc7k_board_register_error;
	}

	verbose("\"%s\" registered\n", name);
	return board;

adc7k_board_register_error:
	mutex_lock(&adc7k_board_list_lock);
	if (minor >= 0) {
		adc7k_subsystem_cdev_minor_alloc[minor] = 0;
	}
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, name))) {
			adc7k_board_list[i] = NULL;
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);
	if (board) {
		kfree(board);
	}
	return NULL;
}
EXPORT_SYMBOL(adc7k_board_register);

void adc7k_board_unregister(struct adc7k_board *board)
{
	size_t i;

	CLASS_DEV_DESTROY(adc7k_class, board->device_number);
	cdev_del(board->char_device);

	verbose("\"%s\" unregistered\n", board->name);

	mutex_lock(&adc7k_board_list_lock);
	adc7k_subsystem_cdev_minor_alloc[MINOR(board->device_number)] = 0;
	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		if ((adc7k_board_list[i]) && (!strcmp(adc7k_board_list[i]->name, board->name))) {
			kfree(adc7k_board_list[i]);
			adc7k_board_list[i] = NULL;
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);
}
EXPORT_SYMBOL(adc7k_board_unregister);

struct adc7k_channel *adc7k_channel_register(struct module *owner, struct adc7k_board *board, char *name, struct cdev *cdev, struct file_operations *fops)
{
	size_t i;
	char channel_name[ADC7K_CHANNEL_NAME_MAX_LENGTH];
	int rc;
	int minor = -1;
	struct adc7k_channel *channel;

	if (!(channel = kmalloc(sizeof(struct adc7k_channel), GFP_KERNEL))) {
		log(KERN_ERR, "\"%s\" - can't get memory for struct adc7k_channel\n", name);
		goto adc7k_channel_register_error;
	}
	memset(channel, 0, sizeof(struct adc7k_channel));

	mutex_lock(&adc7k_board_list_lock);
	// check for name is not used
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if ((board->channel[i]) && (!strcmp(board->channel[i]->name, name))) {
			mutex_unlock(&adc7k_board_list_lock);
			log(KERN_ERR, "\"%s\" already registered\n", name);
			goto adc7k_channel_register_error;
		}
	}
	// get free minor number
	for (i = 0; i < ADC7K_DEVICE_MAX_COUNT; ++i) {
		if (!adc7k_subsystem_cdev_minor_alloc[i]) {
			adc7k_subsystem_cdev_minor_alloc[i] = 1;
			minor = i;
			break;
		}
	}
	if (minor >= 0) {
		// get free channel list slot
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (!board->channel[i]) {
				board->channel[i] = channel;
				channel->device_number = MKDEV(major, minor);
				snprintf(channel->name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "%s", name);
				break;
			}
		}
	}
	mutex_unlock(&adc7k_board_list_lock);

	if (minor < 0) {
		log(KERN_ERR, "\"%s\" - can't get free minor number\n", name);
		goto adc7k_channel_register_error;
	}
	if (i == ADC7K_BOARD_MAX_COUNT) {
		log(KERN_ERR, "\"%s\" - can't get free board list slot\n", name);
		goto adc7k_channel_register_error;
	}

	// Add char device to system
	cdev_init(cdev, fops);
	cdev->owner = owner;
	cdev->ops = fops;
	channel->char_device = cdev;
	if ((rc = cdev_add(cdev, channel->device_number, 1)) < 0) {
		log(KERN_ERR, "\"%s\" - cdev_add() error=%d\n", name, rc);
		goto adc7k_channel_register_error;
	}
	snprintf(channel_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "adc7k!%s", name);
	if (!(channel->class_device = CLASS_DEV_CREATE(adc7k_class, channel->device_number, NULL, channel_name))) {
		log(KERN_ERR, "\"%s\" - class_dev_create() error\n", name);
		goto adc7k_channel_register_error;
	}

	verbose("\"%s\" registered\n", name);
	return channel;

adc7k_channel_register_error:
	mutex_lock(&adc7k_board_list_lock);
	if (minor >= 0) {
		adc7k_subsystem_cdev_minor_alloc[minor] = 0;
	}
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if ((board->channel[i]) && (!strcmp(board->channel[i]->name, name))) {
			board->channel[i] = NULL;
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);
	if (channel) {
		kfree(channel);
	}
	return NULL;
}
EXPORT_SYMBOL(adc7k_channel_register);

void adc7k_channel_unregister(struct adc7k_board *board, struct adc7k_channel *channel)
{
	size_t i;

	CLASS_DEV_DESTROY(adc7k_class, channel->device_number);
	cdev_del(channel->char_device);

	verbose("\"%s\" unregistered\n", channel->name);

	mutex_lock(&adc7k_board_list_lock);
	adc7k_subsystem_cdev_minor_alloc[MINOR(channel->device_number)] = 0;
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if ((board->channel[i]) && (!strcmp(board->channel[i]->name, channel->name))) {
			kfree(board->channel[i]);
			board->channel[i] = NULL;
			break;
		}
	}
	mutex_unlock(&adc7k_board_list_lock);
}
EXPORT_SYMBOL(adc7k_channel_unregister);

const char *adc7k_channel_number_to_string(size_t num)
{
	switch (num) {
		case 0: return "12";
		case 1: return "34";
		case 2: return "56";
		case 3: return "78";
		default: return "xx";
	}
}
EXPORT_SYMBOL(adc7k_channel_number_to_string);

static int __init adc7k_init(void)
{
	size_t i;
	dev_t device_number;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *class_device = NULL;
#else
	struct class_device *class_device = NULL;
#endif
	int major_reg = 0;
	int rc = -1;

	verbose("loading version \"%s\"\n", ADC7K_LINUX_VERSION);

	for (i = 0; i < ADC7K_DEVICE_MAX_COUNT; ++i) {
		adc7k_subsystem_cdev_minor_alloc[i] = 0;
	}

	for (i = 0; i < ADC7K_BOARD_MAX_COUNT; ++i) {
		adc7k_board_list[i] = NULL;
	}

	// Registering adc7k device class
	if (!(adc7k_class = class_create(THIS_MODULE, "adc7k"))) {
		log(KERN_ERR, "class_create() error\n");
		goto adc7k_init_error;
	}
	// Register char device region
	if (major) {
		device_number = MKDEV(major, 0);
		rc = register_chrdev_region(device_number, ADC7K_DEVICE_MAX_COUNT, "adc7k");
	} else {
		rc = alloc_chrdev_region(&device_number, 0, ADC7K_DEVICE_MAX_COUNT, "adc7k");
		if (rc >= 0) {
			major = MAJOR(device_number);
		}
	}
	if (rc < 0) {
		log(KERN_ERR, "register chrdev region error=%d\n", rc);
		goto adc7k_init_error;
	}
	major_reg = 1;

	// Add subsystem device
	cdev_init(&adc7k_subsystem_cdev, &adc7k_subsystem_fops);
	adc7k_subsystem_cdev.owner = THIS_MODULE;
	adc7k_subsystem_cdev.ops = &adc7k_subsystem_fops;
	device_number = MKDEV(major, ADC7K_DEVICE_MAX_COUNT - 1);
	if ((rc = cdev_add(&adc7k_subsystem_cdev, device_number, 1)) < 0) {
		log(KERN_ERR, "\"subsystem\" - cdev_add() error=%d\n", rc);
		goto adc7k_init_error;
	}
	adc7k_subsystem_cdev_minor_alloc[ADC7K_DEVICE_MAX_COUNT - 1] = 1;
	if (!(class_device = CLASS_DEV_CREATE(adc7k_class, device_number, NULL, "adc7k!subsystem"))) {
		log(KERN_ERR, "\"subsystem\" - class_dev_create() error\n");
		goto adc7k_init_error;
	}

	return 0;

adc7k_init_error:
	if (class_device) {
		CLASS_DEV_DESTROY(adc7k_class, MKDEV(major, ADC7K_DEVICE_MAX_COUNT - 1));
	}
	if (major_reg) {
		unregister_chrdev_region(MKDEV(major, 0), ADC7K_DEVICE_MAX_COUNT);
	}
	if (adc7k_class) {
		class_destroy(adc7k_class);
	}
	return rc;
}

static void __exit adc7k_exit(void)
{
	// Destroy subsystem device
	CLASS_DEV_DESTROY(adc7k_class, MKDEV(major, ADC7K_DEVICE_MAX_COUNT - 1));
	cdev_del(&adc7k_subsystem_cdev);
	// Unregister char device region
	unregister_chrdev_region(MKDEV(major, 0), ADC7K_DEVICE_MAX_COUNT);
	// Destroy adc7k device class
	class_destroy(adc7k_class);

	verbose("stopped\n");
}

module_init(adc7k_init);
module_exit(adc7k_exit);
