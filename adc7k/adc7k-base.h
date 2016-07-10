/******************************************************************************/
/* adc7k-base.h                                                           */
/******************************************************************************/

#ifndef __ADC7K_BASE_H__
#define __ADC7K_BASE_H__

#define ADC7K_DEVICE_MAX_COUNT 256
#define ADC7K_DEVICE_NAME_MAX_LENGTH 256

#define ADC7K_BOARD_MAX_COUNT 32
#define ADC7K_BOARD_NAME_MAX_LENGTH 256

#ifdef __KERNEL__

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/types.h>
#include <linux/wait.h>

struct adc7k_board {
	char name[ADC7K_BOARD_NAME_MAX_LENGTH];
	int devno;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *device;
#else
	struct class_device *device;
#endif
	struct cdev *cdev;
};

struct adc7k_tty_device {
	int tty_minor;
	struct tty_operations *tty_ops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *device;
#else
	struct class_device *device;
#endif
	void *data;
};

struct adc7k_board *adc7k_board_register(struct module *owner, char *name, struct cdev *cdev, struct file_operations *fops);
void adc7k_board_unregister(struct adc7k_board *board);

#endif //__KERNEL__

#endif //__ADC7K_BASE_H__
