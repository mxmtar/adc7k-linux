/******************************************************************************/
/* adc7k-pseudo-base.c                                                        */
/******************************************************************************/

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <asm/uaccess.h>

#include "adc7k/adc7k-base.h"

MODULE_AUTHOR("Maksym Tarasevych <mxmtar@gmail.com>");
MODULE_DESCRIPTION("ADC7K pseudo board Linux module");
MODULE_LICENSE("GPL");

static int boards = 1;
module_param(boards, int, 0);
MODULE_PARM_DESC(boards, "Number of ADC7K Pseudo boards");

static int dmastart = 0x10000000;
module_param(dmastart, int, 0);
MODULE_PARM_DESC(dmastart, "ADC7K Pseudo board start address of external DMA buffer");

static int dmaend = 0x10ffffff;
module_param(dmaend, int, 0);
MODULE_PARM_DESC(dmaend, "ADC7K Pseudo board end address of external DMA buffer");

#define verbose(_fmt, _args...) printk(KERN_INFO "[%s] " _fmt, THIS_MODULE->name, ## _args)
#define log(_level, _fmt, _args...) printk(_level "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-pseudo-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)
#define debug(_fmt, _args...) printk(KERN_DEBUG "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-pseudo-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)

struct adc7k_pseudo_channel {
	struct adc7k_channel *adc7k_channel;
	struct cdev cdev;

	spinlock_t lock;

	size_t usage;
	size_t mmap;
	size_t transactions;

	wait_queue_head_t poll_waitq;
	wait_queue_head_t read_waitq;

	size_t sampler_pos;
	size_t sampler_done;

	void *data;
	size_t data_length;

	unsigned long mem_start;
	size_t mem_length;
};

struct adc7k_pseudo_board {
	struct adc7k_board *adc7k_board;
	struct cdev cdev;
	struct adc7k_pseudo_channel *channel[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];

	spinlock_t lock;

	size_t sampling_rate;
	size_t sampler_length;
	size_t sampler_length_max;
	size_t sampler_divider;
	size_t sampler_continuous;

	struct timer_list sampler_timer;

	void *dma_buffer[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
	dma_addr_t dma_address[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
	size_t dma_buffer_size;

};

struct adc7k_pseudo_board_private_data {
	struct adc7k_pseudo_board *board;
	char buff[0xc000];
	size_t length;
	loff_t f_pos;
};

struct resource *dma_region = NULL;

static struct adc7k_pseudo_board **adc7k_pseudo_board_list = NULL;

static void adc7k_pseudo_board_sampler(unsigned long addr)
{
	size_t i;
	size_t s, e;
	size_t shift;
	u32 *sp;
	struct adc7k_pseudo_board *board = (struct adc7k_pseudo_board *)addr;
	struct adc7k_pseudo_channel *channel;

	spin_lock(&board->lock);

	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if ((channel = board->channel[i])) {
			spin_lock(&channel->lock);
			shift = i * 0x1000;
			sp = channel->data;
			for (s = 0, e = board->sampler_length; s < e; ++s) {
				*sp++ = ((s + shift) & 0x3fff) + ((get_random_int() & 0x3fff) << 14);
			}
			channel->data_length = channel->mem_length;
			channel->sampler_done = 1;
			wake_up_interruptible(&channel->poll_waitq);
			wake_up_interruptible(&channel->read_waitq);
			spin_unlock(&channel->lock);
		}
	}
	spin_unlock(&board->lock);
}

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
	len += sprintf(private_data->buff + len, "{\r\n\t\"type\": \"Pseudo\",");

	len += sprintf(private_data->buff + len, "\r\n\t\"sampler\": {\r\n\t\t\"rate\": %lu,\r\n\t\t\"length\": %lu,\r\n\t\t\"max_length\": %lu,\r\n\t\t\"divider\": %lu\r\n\t},", (long unsigned int)board->sampling_rate, (long unsigned int)board->sampler_length, (long unsigned int)board->sampler_length_max, (long unsigned int)board->sampler_divider);

	len += sprintf(private_data->buff + len, "\r\n\t\"channels\": [");
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (board->channel[i]) {
			len += sprintf(private_data->buff + len, "%s\r\n\t\t{\r\n\t\t\t\"name\": \"%s\",\r\n\t\t\t\"path\": \"%s\",\r\n\t\t\t\"transactions\": %lu,\r\n\t\t\t\"buffer\": {\r\n\t\t\t\t\"address\": %lu,\r\n\t\t\t\t\"size\": %lu\r\n\t\t\t}\r\n\t\t}",
							i ? "," : "",
							adc7k_channel_number_to_string(i),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
							dev_name(board->channel[i]->adc7k_channel->class_device),
#else
							board->channel[i]->adc7k_channel->class_device->class_id,
#endif
				  			(long unsigned int)board->channel[i]->transactions,
							(long unsigned int)board->dma_address[i],
							(long unsigned int)board->dma_buffer_size
							);
		}
	}
	len += sprintf(private_data->buff + len, "\r\n\t]");
	len += sprintf(private_data->buff + len, "\r\n}\r\n");

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

	res = (private_data->length > private_data->f_pos) ? (private_data->length - private_data->f_pos) : (0);

	if (res) {
		len = res;
		len = min(count, len);
		if (copy_to_user(buff, private_data->buff + private_data->f_pos, len)) {
			res = -EINVAL;
			goto adc7k_pseudo_board_read_end;
		}
		private_data->f_pos += len;
	}

adc7k_pseudo_board_read_end:
	return res;
}

static ssize_t adc7k_pseudo_board_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp)
{
	size_t i;
	ssize_t res;
	size_t len;

	u_int32_t value;
	struct adc7k_pseudo_board_private_data *private_data = filp->private_data;
	struct adc7k_pseudo_board *board = private_data->board;

	len = sizeof(private_data->buff) - 1;
	len = min(len, count);

	if (copy_from_user(private_data->buff, buff, len)) {
		res = -EINVAL;
		goto adc7k_cpci3u_board_write_end;
	}
	private_data->buff[len] = '\0';

	spin_lock_bh(&board->lock);

	if (sscanf(private_data->buff, "sampler.start(%u)", &value) == 1) {
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (board->channel[i]) {
				board->channel[i]->sampler_done = 0;
				board->channel[i]->data_length = 0;
			}
		}
		board->sampler_continuous = value;
		del_timer_sync(&board->sampler_timer);
		board->sampler_timer.function = adc7k_pseudo_board_sampler;
		board->sampler_timer.data = (unsigned long)private_data->board;
		board->sampler_timer.expires = jiffies + 1;
		add_timer(&board->sampler_timer);
		res = len;
	} else if (strstr(private_data->buff, "sampler.stop()")) {
		board->sampler_continuous = 0;
		res = len;
	} else if (sscanf(private_data->buff, "sampler.length(%u)", &value) == 1) {
		board->sampler_length = value;
		res = len;
	} else if (sscanf(private_data->buff, "sampler.divider(%u)", &value) == 1) {
		if ((value >= 0) && (value <= 255)) {
			board->sampler_divider = value;
			res = len;
		} else {
			res = -EPERM;
		}
	} else {
		res = -ENOMSG;
	}

	spin_unlock_bh(&board->lock);

adc7k_cpci3u_board_write_end:
	return res;
}

static struct file_operations adc7k_pseudo_board_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_pseudo_board_open,
	.release = adc7k_pseudo_board_release,
	.read    = adc7k_pseudo_board_read,
	.write   = adc7k_pseudo_board_write,
};

static int adc7k_pseudo_channel_open(struct inode *inode, struct file *filp)
{
	struct adc7k_pseudo_channel *channel = container_of(inode->i_cdev, struct adc7k_pseudo_channel, cdev);

	filp->private_data = channel;

	spin_lock_bh(&channel->lock);
	++channel->usage;
	channel->sampler_done = 0;
	channel->data_length = 0;
	channel->mmap = 0;
	spin_unlock_bh(&channel->lock);

	return 0;
}

static int adc7k_pseudo_channel_release(struct inode *inode, struct file *filp)
{
	struct adc7k_pseudo_channel *channel = filp->private_data;

	spin_lock_bh(&channel->lock);
	--channel->usage;
	spin_unlock_bh(&channel->lock);

	return 0;
}

static ssize_t adc7k_pseudo_channel_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	size_t len;
	ssize_t res = 0;
	struct adc7k_pseudo_channel *channel = filp->private_data;

	spin_lock_bh(&channel->lock);

	for (;;) {
		// successfull return
		if (channel->data_length || channel->sampler_done) {
			break;
		}

		spin_unlock_bh(&channel->lock);

		if (filp->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}

		// sleeping
		res = wait_event_interruptible(channel->read_waitq, channel->data_length || channel->sampler_done);
		if (res) {
			return res;
		}

		spin_lock_bh(&channel->lock);
	}

	if (channel->data_length) {

		res = len = min(count, channel->data_length);

		if (channel->mmap == 0) {
			spin_unlock_bh(&channel->lock);
			if (copy_to_user(buff, channel->data, len)) {
				res = -EINVAL;
				goto adc7k_pseudo_board_read_end;
			}
			spin_lock_bh(&channel->lock);
		}
		channel->data_length = 0;
	} else {
		channel->sampler_done = 0;
		res = 0;
	}

	spin_unlock_bh(&channel->lock);

adc7k_pseudo_board_read_end:
	return res;
}

static unsigned int adc7k_pseudo_channel_poll(struct file *filp, struct poll_table_struct *wait_table)
{
	unsigned int res = 0;
	struct adc7k_pseudo_channel *channel = filp->private_data;

	poll_wait(filp, &channel->poll_waitq, wait_table);

	spin_lock_bh(&channel->lock);

	if (channel->data_length) {
		res |= POLLIN | POLLRDNORM;
	}

	if (channel->sampler_done) {
		res |= POLLHUP;
	}

	spin_unlock_bh(&channel->lock);

	return res;
}

static int adc7k_pseudo_channel_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct adc7k_pseudo_channel *channel = filp->private_data;
	unsigned long offsset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long physical = channel->mem_start + offsset;
	unsigned long vsize = vma->vm_end - vma->vm_start;
	unsigned long psize = channel->mem_length - offsset;

	if (vsize > psize) {
		return -EINVAL;
	}
	
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	
	vma->vm_flags |= VM_IO;
	if (io_remap_pfn_range(vma, vma->vm_start, physical >> PAGE_SHIFT, vsize, vma->vm_page_prot)) {
		return -EAGAIN;
	}

	spin_lock_bh(&channel->lock);
	channel->mmap = 1;
	spin_unlock_bh(&channel->lock);

	return 0;
}

static struct file_operations adc7k_pseudo_channel_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_pseudo_channel_open,
	.release = adc7k_pseudo_channel_release,
	.read    = adc7k_pseudo_channel_read,
	.poll    = adc7k_pseudo_channel_poll,
	.mmap    = adc7k_pseudo_channel_mmap,
};

static int __init adc7k_pseudo_init(void)
{
	size_t i, j;
	char device_name[ADC7K_DEVICE_NAME_MAX_LENGTH];
	struct adc7k_pseudo_board *board;
	int rc = 0;

	// get external DMA buffer
	if ((dmastart > 0) && (dmaend > 0) && (dmaend > dmastart)) {
		if (!(dma_region = request_mem_region(dmastart, dmaend - dmastart + 1, "adc7k-pseudo"))) {
			log(KERN_ERR, "Can't request DMA memory region start=0x%08lx end=0x%08lx\n", (unsigned long int)dmastart, (unsigned long int)dmaend);
			rc = -ENOMEM;
			goto adc7k_pseudo_init_error;
		}
	} else {
		log(KERN_ERR, "Nonexistent external DMA memory dmastart=0x%08lx dmaend=0x%08lx\n", (unsigned long int)dmastart, (unsigned long int)dmaend);
		rc = -ENOMEM;
		goto adc7k_pseudo_init_error;
	}

	// alloc memory for board list
	if (!(adc7k_pseudo_board_list = kmalloc(boards * sizeof(struct adc7k_pseudo_board *), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory for adc7k_pseudo_board_list\n");
		rc = -ENOMEM;
		goto adc7k_pseudo_init_error;
	}

	for (j = 0; j < boards; ++j) {
		// alloc memory for board data
		if (!(adc7k_pseudo_board_list[j] = kmalloc(sizeof(struct adc7k_pseudo_board), GFP_KERNEL))) {
			log(KERN_ERR, "can't get memory for struct adc7k_pseudo_board\n");
			rc = -ENOMEM;
			goto adc7k_pseudo_init_error;
		}
		board = adc7k_pseudo_board_list[j];
		memset(board, 0, sizeof(struct adc7k_pseudo_board));

		spin_lock_init(&board->lock);
		init_timer(&board->sampler_timer);
		board->sampling_rate = ADC7K_DEFAULT_SAMPLING_RATE;

		// remap external DMA buffer
		board->dma_buffer_size = (dmaend - dmastart + 1) / (ADC7K_CHANNEL_PER_BOARD_MAX_COUNT * boards);
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (!(board->dma_buffer[i] = ioremap(dmastart + (j * ADC7K_CHANNEL_PER_BOARD_MAX_COUNT + i) * board->dma_buffer_size, board->dma_buffer_size))) {
				log(KERN_ERR, "%lu: Unable to remap memory 0x%08lx-0x%08lx\n", (unsigned long int)i, (unsigned long int)(dmastart + i * board->dma_buffer_size), (unsigned long int)(dmastart + (i + 1) * board->dma_buffer_size - 1));
				rc = -ENOMEM;
				goto adc7k_pseudo_init_error;
			}
			board->dma_address[i] = dmastart + (j * ADC7K_CHANNEL_PER_BOARD_MAX_COUNT + i) * board->dma_buffer_size;
		}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "board-pseudo-%lu", (unsigned long int)j);
#else
		snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "bp%lu", (unsigned long int)j);
#endif
		if (!(board->adc7k_board = adc7k_board_register(THIS_MODULE, device_name, &board->cdev, &adc7k_pseudo_board_fops))) {
			rc = -1;
			goto adc7k_pseudo_init_error;
		}

		// alloc memory for channel data
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (!(board->channel[i] = kmalloc(sizeof(struct adc7k_pseudo_channel), GFP_KERNEL))) {
				log(KERN_ERR, "CH%s: can't get memory for struct adc7k_pseudo_channel\n", adc7k_channel_number_to_string(i));
				rc = -ENOMEM;
				goto adc7k_pseudo_init_error;
			}
			memset(board->channel[i], 0, sizeof(struct adc7k_pseudo_channel));

			spin_lock_init(&board->channel[i]->lock);
			init_waitqueue_head(&board->channel[i]->poll_waitq);
			init_waitqueue_head(&board->channel[i]->read_waitq);
			board->channel[i]->data = board->dma_buffer[i];
			board->channel[i]->mem_start = board->dma_address[i];
			board->channel[i]->mem_length = board->dma_buffer_size;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
			snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "board-pseudo-%lu-channel-%s", (unsigned long int)j, adc7k_channel_number_to_string(i));
#else
			snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "bp%luc%s", (unsigned long int)j, adc7k_channel_number_to_string(i));
#endif
			if (!(board->channel[i]->adc7k_channel = adc7k_channel_register(THIS_MODULE, board->adc7k_board, device_name, &board->channel[i]->cdev, &adc7k_pseudo_channel_fops))) {
				rc = -1;
				goto adc7k_pseudo_init_error;
			}
		}
		// set samplers length
		board->sampler_length_max = board->dma_buffer_size / 4;
		board->sampler_length = board->sampler_length_max;
	}

	verbose("loaded\n");
	return 0;

adc7k_pseudo_init_error:
	if (adc7k_pseudo_board_list) {
		for (j = 0; j < boards; ++j) {
			if ((board = adc7k_pseudo_board_list[j])) {
				for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
					if (board->dma_buffer[i]) {
						iounmap(board->dma_buffer[i]);
					}
				}
				del_timer_sync(&board->sampler_timer);
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
		}
		kfree(adc7k_pseudo_board_list);
	}
	if (dma_region) {
		release_mem_region(dmastart, dmaend - dmastart + 1);
	}
	return rc;
}

static void __exit adc7k_pseudo_exit(void)
{
	struct adc7k_pseudo_board *board;
	size_t i, j;

	if (adc7k_pseudo_board_list) {
		for (j = 0; j < boards; ++j) {
			if ((board = adc7k_pseudo_board_list[j])) {
				for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
					if (board->dma_buffer[i]) {
						iounmap(board->dma_buffer[i]);
					}
				}
				del_timer_sync(&board->sampler_timer);
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
		}
		kfree(adc7k_pseudo_board_list);
	}

	release_mem_region(dmastart, dmaend - dmastart + 1);

	verbose("stopped\n");
}

module_init(adc7k_pseudo_init);
module_exit(adc7k_pseudo_exit);
