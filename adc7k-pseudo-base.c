/******************************************************************************/
/* adc7k-pseudo-base.c                                                        */
/******************************************************************************/

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "adc7k/adc7k-base.h"

#define verbose(_fmt, _args...) printk(KERN_INFO "[%s] " _fmt, THIS_MODULE->name, ## _args)
#define log(_level, _fmt, _args...) printk(_level "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-pseudo-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)
#define debug(_fmt, _args...) printk(KERN_DEBUG "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-pseudo-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)

struct adc7k_pseudo_channel {
	struct adc7k_channel *adc7k_channel;
	struct cdev cdev;
};

struct adc7k_pseudo_board {
	struct adc7k_board *adc7k_board;
	struct cdev cdev;
	struct adc7k_pseudo_channel *channel[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
};

struct adc7k_pseudo_board_private_data {
	struct adc7k_pseudo_board *board;
	char buff[0x0C00];
	size_t length;
};

static struct adc7k_pseudo_board *adc7k_pseudo_board = NULL;

static struct file_operations adc7k_pseudo_channel_fops = {
	.owner   = THIS_MODULE,
// 	.open    = adc7k_pseudo_channel_open,
// 	.release = adc7k_pseudo_channel_release,
// 	.read    = adc7k_pseudo_channel_read,
// 	.write   = adc7k_pseudo_channel_write,
};

static int adc7k_pseudo_board_open(struct inode *inode, struct file *filp)
{
	size_t i;
	ssize_t res;
	size_t len;

	struct adc7k_pseudo_board *board;
	struct adc7k_pseudo_board_private_data *private_data;

	board = container_of(inode->i_cdev, struct adc7k_pseudo_board, cdev);

	if (!(private_data = kmalloc(sizeof(struct adc7k_pseudo_board_private_data), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory=%lu bytes\n", (unsigned long int)sizeof(struct adc7k_pseudo_board_private_data));
		res = -ENOMEM;
		goto adc7k_pseudo_board_open_error;
	}
	private_data->board = board;

	len = 0;
	len += sprintf(private_data->buff + len, "ADC7K Pseudo\r\n");

	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (board->channel[i]) {
			len += sprintf(private_data->buff + len, "CH%s %s\r\n",
							adc7k_channel_number_to_string(i),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
							dev_name(board->channel[i]->adc7k_channel->class_device)
#else
							board->channel[i]->adc7k_channel->class_device->class_id
#endif
						  );
		}
	}

	private_data->length = len;

	filp->private_data = private_data;

	return 0;

adc7k_pseudo_board_open_error:
	if (private_data) {
		kfree(private_data);
	}
	return res;
}

static int adc7k_pseudo_board_release(struct inode *inode, struct file *filp)
{
	struct adc7k_pseudo_board_private_data *private_data = filp->private_data;

	kfree(private_data);
	return 0;
}

static ssize_t adc7k_pseudo_board_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	size_t len;
	ssize_t res;
	struct adc7k_pseudo_board_private_data *private_data = filp->private_data;

	res = (private_data->length > filp->f_pos)?(private_data->length - filp->f_pos):(0);

	if (res) {
		len = res;
		len = min(count, len);
		if (copy_to_user(buff, private_data->buff + filp->f_pos, len)) {
			res = -EINVAL;
			goto adc7k_pseudo_board_read_end;
		}
		*offp = filp->f_pos + len;
	}

adc7k_pseudo_board_read_end:
	return res;
}

static struct file_operations adc7k_pseudo_board_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_pseudo_board_open,
	.release = adc7k_pseudo_board_release,
	.read    = adc7k_pseudo_board_read,
// 	.write   = adc7k_pseudo_board_write,
};

static int __init adc7k_pseudo_init(void)
{
	size_t i;
	char device_name[ADC7K_DEVICE_NAME_MAX_LENGTH];
	int rc = 0;

	// alloc memory for board data
	if (!(adc7k_pseudo_board = kmalloc(sizeof(struct adc7k_pseudo_board), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory for struct adc7k_pseudo_board\n");
		rc = -1;
		goto adc7k_pseudo_init_error;
	}
	memset(adc7k_pseudo_board, 0, sizeof(struct adc7k_pseudo_board));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "board-pseudo");
#else
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "bp");
#endif
	if (!(adc7k_pseudo_board->adc7k_board =  adc7k_board_register(THIS_MODULE, device_name, &adc7k_pseudo_board->cdev, &adc7k_pseudo_board_fops))) {
		rc = -1;
		goto adc7k_pseudo_init_error;
	}

	// alloc memory for channel data
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (!(adc7k_pseudo_board->channel[i] = kmalloc(sizeof(struct adc7k_pseudo_channel), GFP_KERNEL))) {
			log(KERN_ERR, "can't get memory for struct adc7k_pseudo_channel\n");
			rc = -1;
			goto adc7k_pseudo_init_error;
		}
		memset(adc7k_pseudo_board->channel[i], 0, sizeof(struct adc7k_pseudo_channel));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "board-pseudo-channel-%s", adc7k_channel_number_to_string(i));
#else
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "bpc%s", adc7k_channel_number_to_string(i));
#endif
		if (!(adc7k_pseudo_board->channel[i]->adc7k_channel = adc7k_channel_register(THIS_MODULE, adc7k_pseudo_board->adc7k_board, device_name, &adc7k_pseudo_board->channel[i]->cdev, &adc7k_pseudo_channel_fops))) {
			rc = -1;
			goto adc7k_pseudo_init_error;
		}
	}

	verbose("loaded\n");
	return 0;

adc7k_pseudo_init_error:
	if (adc7k_pseudo_board) {
		if (adc7k_pseudo_board->adc7k_board) {
			for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
				if (adc7k_pseudo_board->channel[i]) {
					if (adc7k_pseudo_board->channel[i]->adc7k_channel) {
						adc7k_channel_unregister(adc7k_pseudo_board->adc7k_board, adc7k_pseudo_board->channel[i]->adc7k_channel);
					}
					kfree(adc7k_pseudo_board->channel[i]);
				}
			}
			adc7k_board_unregister(adc7k_pseudo_board->adc7k_board);
		}
		kfree(adc7k_pseudo_board);
	}
	return rc;
}

static void __exit adc7k_pseudo_exit(void)
{
	size_t i;

	if (adc7k_pseudo_board) {
		if (adc7k_pseudo_board->adc7k_board) {
			for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
				if (adc7k_pseudo_board->channel[i]) {
					if (adc7k_pseudo_board->channel[i]->adc7k_channel) {
						adc7k_channel_unregister(adc7k_pseudo_board->adc7k_board, adc7k_pseudo_board->channel[i]->adc7k_channel);
					}
					kfree(adc7k_pseudo_board->channel[i]);
				}
			}
			adc7k_board_unregister(adc7k_pseudo_board->adc7k_board);
		}
		kfree(adc7k_pseudo_board);
	}
	verbose("stopped\n");
}

module_init(adc7k_pseudo_init);
module_exit(adc7k_pseudo_exit);
