/******************************************************************************/
/* adc7k-base.h                                                           */
/******************************************************************************/

#ifndef __ADC7K_BASE_H__
#define __ADC7K_BASE_H__

#include <asm/page.h>

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/types.h>

#define ADC7K_DEVICE_MAX_COUNT 256
#define ADC7K_DEVICE_NAME_MAX_LENGTH 256

#define ADC7K_BOARD_MAX_COUNT 8
#define ADC7K_BOARD_NAME_MAX_LENGTH ADC7K_DEVICE_NAME_MAX_LENGTH

#define ADC7K_CHANNEL_PER_BOARD_MAX_COUNT 4
#define ADC7K_CHANNEL_NAME_MAX_LENGTH ADC7K_DEVICE_NAME_MAX_LENGTH

#define ADC7K_DEFAULT_SAMPLING_RATE 50000000

struct adc7k_channel {
	char name[ADC7K_CHANNEL_NAME_MAX_LENGTH];
	dev_t device_number;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *class_device;
#else
	struct class_device *class_device;
#endif
	struct cdev *char_device;
};

struct adc7k_board {
	char name[ADC7K_BOARD_NAME_MAX_LENGTH];
	dev_t device_number;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	struct device *class_device;
#else
	struct class_device *class_device;
#endif
	struct cdev *char_device;

	struct adc7k_channel *channel[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
};

struct adc7k_board *adc7k_board_register(struct module *owner, char *name, struct cdev *cdev, struct file_operations *fops);
void adc7k_board_unregister(struct adc7k_board *board);

struct adc7k_channel *adc7k_channel_register(struct module *owner, struct adc7k_board *board, char *name, struct cdev *cdev, struct file_operations *fops);
void adc7k_channel_unregister(struct adc7k_board *board, struct adc7k_channel *channel);

const char *adc7k_channel_number_to_string(size_t num);

#endif //__ADC7K_BASE_H__
