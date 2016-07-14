/******************************************************************************/
/* adc7k-cpci3u-base.c                                                        */
/******************************************************************************/

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "adc7k/adc7k-base.h"

#define verbose(_fmt, _args...) printk(KERN_INFO "[%s] " _fmt, THIS_MODULE->name, ## _args)
#define log(_level, _fmt, _args...) printk(_level "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-cpci3u-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)
#define debug(_fmt, _args...) printk(KERN_DEBUG "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-cpci3u-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)

struct adc7k_cpci3u_channel {
	struct adc7k_channel *adc7k_channel;
	struct cdev cdev;
};

struct adc7k_cpci3u_board {
	struct adc7k_board *adc7k_board;
	struct cdev cdev;
	struct adc7k_cpci3u_channel *channel[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
};

struct adc7k_cpci3u_board_private_data {
	struct adc7k_cpci3u_board *board;
	char buff[0x0C00];
	size_t length;
};

static struct file_operations adc7k_cpci3u_channel_fops = {
	.owner   = THIS_MODULE,
// 	.open    = adc7k_cpci3u_channel_open,
// 	.release = adc7k_cpci3u_channel_release,
// 	.read    = adc7k_cpci3u_channel_read,
// 	.write   = adc7k_cpci3u_channel_write,
};

static int adc7k_cpci3u_board_open(struct inode *inode, struct file *filp)
{
	size_t i;
	ssize_t res;
	size_t len;

	struct adc7k_cpci3u_board *board;
	struct adc7k_cpci3u_board_private_data *private_data;

	board = container_of(inode->i_cdev, struct adc7k_cpci3u_board, cdev);

	if (!(private_data = kmalloc(sizeof(struct adc7k_cpci3u_board_private_data), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory=%lu bytes\n", (unsigned long int)sizeof(struct adc7k_cpci3u_board_private_data));
		res = -ENOMEM;
		goto adc7k_cpci3u_board_open_error;
	}
	private_data->board = board;

	len = 0;
	len += sprintf(private_data->buff + len, "ADC7K CompactPCI 3U\r\n");

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

adc7k_cpci3u_board_open_error:
	if (private_data) {
		kfree(private_data);
	}
	return res;
}

static int adc7k_cpci3u_board_release(struct inode *inode, struct file *filp)
{
	struct adc7k_cpci3u_board_private_data *private_data = filp->private_data;

	kfree(private_data);
	return 0;
}

static ssize_t adc7k_cpci3u_board_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	size_t len;
	ssize_t res;
	struct adc7k_cpci3u_board_private_data *private_data = filp->private_data;

	res = (private_data->length > filp->f_pos)?(private_data->length - filp->f_pos):(0);

	if (res) {
		len = res;
		len = min(count, len);
		if (copy_to_user(buff, private_data->buff + filp->f_pos, len)) {
			res = -EINVAL;
			goto adc7k_cpci3u_board_read_end;
		}
		*offp = filp->f_pos + len;
	}

adc7k_cpci3u_board_read_end:
	return res;
}

static struct file_operations adc7k_cpci3u_board_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_cpci3u_board_open,
	.release = adc7k_cpci3u_board_release,
	.read    = adc7k_cpci3u_board_read,
// 	.write   = adc7k_cpci3u_board_write,
};

static struct pci_device_id adc7k_cpci3u_board_id_table[] = {
	{ PCI_DEVICE(0x2004, 0x680c), .driver_data = 1, },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, adc7k_cpci3u_board_id_table);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
static int adc7k_cpci3u_board_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
#else
static int __devinit adc7k_cpci3u_board_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
#endif
{
	size_t i;
	char device_name[ADC7K_DEVICE_NAME_MAX_LENGTH];
	struct adc7k_cpci3u_board *board = NULL;
// 	int pci_region_busy = 0;
	int rc = 0;

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "can't enable pci device\n");
		goto adc7k_cpci3u_board_probe_error;
	}
/*
	rc = pci_request_region(pdev, 0, "adc7k-cpci3u");
	if (rc) {
		dev_err(&pdev->dev, "can't request I/O region\n");
		goto adc7k_cpci3u_board_probe_error;
	}
	pci_region_busy = 1;
*/
	// alloc memory for board data
	if (!(board = kmalloc(sizeof(struct adc7k_cpci3u_board), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory for struct adc7k_cpci3u_board\n");
		rc = -1;
		goto adc7k_cpci3u_board_probe_error;
	}
	memset(board, 0, sizeof(struct adc7k_cpci3u_board));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "board-cpci3u");
#else
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "bp");
#endif
	if (!(board->adc7k_board =  adc7k_board_register(THIS_MODULE, device_name, &board->cdev, &adc7k_cpci3u_board_fops))) {
		rc = -1;
		goto adc7k_cpci3u_board_probe_error;
	}

	// alloc memory for channel data
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (!(board->channel[i] = kmalloc(sizeof(struct adc7k_cpci3u_channel), GFP_KERNEL))) {
			log(KERN_ERR, "can't get memory for struct adc7k_cpci3u_channel\n");
			rc = -1;
			goto adc7k_cpci3u_board_probe_error;
		}
		memset(board->channel[i], 0, sizeof(struct adc7k_cpci3u_channel));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "board-cpci3u-channel-%s", adc7k_channel_number_to_string(i));
#else
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "bpc%s", adc7k_channel_number_to_string(i));
#endif
		if (!(board->channel[i]->adc7k_channel = adc7k_channel_register(THIS_MODULE, board->adc7k_board, device_name, &board->channel[i]->cdev, &adc7k_cpci3u_channel_fops))) {
			rc = -1;
			goto adc7k_cpci3u_board_probe_error;
		}
	}

	pci_set_drvdata(pdev, board);
	verbose("loaded\n");
	return rc;

adc7k_cpci3u_board_probe_error:
	if (board) {
		if (board->adc7k_board) {
			for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
				if (board->channel[i]) {
					if (board->channel[i]->adc7k_channel) {
						adc7k_channel_unregister(board->adc7k_board, board->channel[i]->adc7k_channel);
					}
					kfree(board->channel[i]);
				}
			}
			adc7k_board_unregister(board->adc7k_board);
		}
		kfree(board);
	}
/*
	if (pci_region_busy) {
		pci_release_region(pdev, 0);
	}
*/
	return rc;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
static void adc7k_cpci3u_board_remove(struct pci_dev *pdev)
#else
static void __devexit adc7k_cpci3u_board_remove(struct pci_dev *pdev)
#endif
{
	size_t i;
	struct adc7k_cpci3u_board *board = pci_get_drvdata(pdev);

	if (board) {
		if (board->adc7k_board) {
			for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
				if (board->channel[i]) {
					if (board->channel[i]->adc7k_channel) {
						adc7k_channel_unregister(board->adc7k_board, board->channel[i]->adc7k_channel);
					}
					kfree(board->channel[i]);
				}
			}
			adc7k_board_unregister(board->adc7k_board);
		}
		kfree(board);
	}
/*
	pci_release_region(pdev, 0);
*/
}

static struct pci_driver adc7k_cpci3u_driver = {
	.name = "adc7k-cpci3u",
	.id_table = adc7k_cpci3u_board_id_table,
	.probe = adc7k_cpci3u_board_probe,
	.remove = adc7k_cpci3u_board_remove,
};

static int __init adc7k_cpci3u_init(void)
{
	int rc = 0;

	// Register PCI driver
	if ((rc = pci_register_driver(&adc7k_cpci3u_driver)) < 0) {
		log(KERN_ERR, "can't register pci driver\n");
		goto adc7k_cpci3u_init_end;
	}

	verbose("loaded\n");

adc7k_cpci3u_init_end:
	return rc;
}

static void __exit adc7k_cpci3u_exit(void)
{
	// Unregister PCI driver
	pci_unregister_driver(&adc7k_cpci3u_driver);

	verbose("stopped\n");
}

module_init(adc7k_cpci3u_init);
module_exit(adc7k_cpci3u_exit);
