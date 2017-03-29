/******************************************************************************/
/* adc7k-cpci3u-base.c                                                        */
/******************************************************************************/

#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <asm/uaccess.h>

#include "adc7k/adc7k-base.h"

MODULE_AUTHOR("Maksym Tarasevych <mxmtar@gmail.com>");
MODULE_DESCRIPTION("ADC7K CompactPCI-3U board Linux module");
MODULE_LICENSE("GPL");

static int dmastart = 0x10000000;
module_param(dmastart, int, 0);
MODULE_PARM_DESC(dmastart, "ADC7K CompactPCI-3U board start address of external DMA buffer");

static int dmaend = 0x10ffffff;
module_param(dmaend, int, 0);
MODULE_PARM_DESC(dmaend, "ADC7K CompactPCI-3U board end address of external DMA buffer");

#define verbose(_fmt, _args...) printk(KERN_INFO "[%s] " _fmt, THIS_MODULE->name, ## _args)
#define log(_level, _fmt, _args...) printk(_level "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-cpci3u-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)
#define debug(_fmt, _args...) printk(KERN_DEBUG "[%s] %s:%d - %s(): " _fmt, THIS_MODULE->name, "adc7k-cpci3u-base.c", __LINE__, __PRETTY_FUNCTION__, ## _args)

#define ADC7K_CPCI3U_BOARD_REGS_COUNT 32

#define ADC7K_CPCI3U_BOARD_MAX_SAMPLER_LENGTH 0x800000

struct resource *dma_region = NULL;

struct adc7k_cpci3u_channel {
	struct adc7k_channel *adc7k_channel;
	struct cdev cdev;

	spinlock_t lock;

	size_t usage;
	size_t mmap;
	size_t transactions;

	wait_queue_head_t poll_waitq;
	wait_queue_head_t read_waitq;

	size_t sampler_done;

	void *data;
	size_t data_length;

	unsigned long mem_start;
	size_t mem_length;
};

struct adc7k_cpci3u_board {
	struct adc7k_board *adc7k_board;
	struct cdev cdev;
	struct pci_dev *pci_dev;
	struct adc7k_cpci3u_channel *channel[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
	size_t channel_current;

	spinlock_t lock;

	size_t sampling_rate;
	size_t sampler_length;
	size_t sampler_length_max;
	size_t sampler_divider;
	size_t sampler_continuous;

	void *dma_buffer[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
	dma_addr_t dma_address[ADC7K_CHANNEL_PER_BOARD_MAX_COUNT];
	size_t dma_buffer_size;

	wait_queue_head_t dma_waitq;
	size_t dma_run;
	struct timer_list dma_timer;

	unsigned long base_io_addr;
	u8 irq_line;
	u8 irq_pin;

	struct {
		union {
			struct adc7k_cpci3u_reg0 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg0;
		union {
			struct adc7k_cpci3u_reg1 {
				u32 reset_all:1;		// 0
				u32 reset_int:1;		// 1
				u32 reserved0:1;		// 2
				u32 reset_dma:1;		// 3
				u32 ie_sampler:1;		// 4
				u32 ie_dma:1;			// 5
				u32 dma_start:1;		// 6
				u32 dma_chain:1;		// 7
				u32 sampler_start:1;	// 8
				u32 reserved1:3;		// 11 - 9
				u32 en_ad_cnt:1;		// 12
				u32 reserved2:12;		// 24 - 13
				u32 rst_ovf_led:1;		// 25
				u32 sel_mem_rd:2;		// 27, 26
				u32 reserved3:4;		// 31 - 28
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg1;
		union {
			struct adc7k_cpci3u_reg2 {
				u32 div:8;			// 7 - 0
				u32 reserved0:24;		// 31 - 8
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg2;
		union {
			struct adc7k_cpci3u_reg3 {
				u32 dma_done_0:1;		// 0
				u32 dma_done_1:1;		// 1
				u32 overflow:1;		// 2
				u32 reserved0:29;		// 31 - 3
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg3;
		union {
			struct adc7k_cpci3u_reg4 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg4;
		union {
			struct adc7k_cpci3u_reg5 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg5;
		union {
			struct adc7k_cpci3u_reg6 {
				u32 sampler_length:26;// 25 - 0
				u32 reserved0:6;		// 31 - 26
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg6;
		union {
			struct adc7k_cpci3u_reg7 {
				u32 dma_address:32;	// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg7;
		union {
			struct adc7k_cpci3u_reg8 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg8;
		union {
			struct adc7k_cpci3u_reg9 {
				u32 dma_length:32;	// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg9;
		union {
			struct adc7k_cpci3u_reg10 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg10;
		union {
			struct adc7k_cpci3u_reg11 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg11;
		union {
			struct adc7k_cpci3u_reg12 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg12;
		union {
			struct adc7k_cpci3u_reg13 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg13;
		union {
			struct adc7k_cpci3u_reg14 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg14;
		union {
			struct adc7k_cpci3u_reg15 {
				u32 reserved0:28;		// 0 - 27
				u32 adc_sclk:1;		// 28
				u32 adc_sdata:1;		// 29
				u32 adc_cs:1;			// 30
				u32 adc_reset:1;		// 31
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg15;
		union {
			struct adc7k_cpci3u_reg16 {
				u32 reserved0:5;		// 4 - 0
				u32 ddr_reset:1;		// 5
				u32 reserved1:26;		// 31 - 6
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg16;
		union {
			struct adc7k_cpci3u_reg17 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg17;
		union {
			struct adc7k_cpci3u_reg18 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg18;
		union {
			struct adc7k_cpci3u_reg19 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg19;
		union {
			struct adc7k_cpci3u_reg20 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg20;
		union {
			struct adc7k_cpci3u_reg21 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg21;
		union {
			struct adc7k_cpci3u_reg22 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg22;
		union {
			struct adc7k_cpci3u_reg23 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg23;
		union {
			struct adc7k_cpci3u_reg24 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg24;
		union {
			struct adc7k_cpci3u_reg25 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg25;
		union {
			struct adc7k_cpci3u_reg26 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg26;
		union {
			struct adc7k_cpci3u_reg27 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg27;
		union {
			struct adc7k_cpci3u_reg28 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg28;
		union {
			struct adc7k_cpci3u_reg29 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg29;
		union {
			struct adc7k_cpci3u_reg30 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg30;
		union {
			struct adc7k_cpci3u_reg31 {
				u32 reserved0:32;		// 31 - 0
			} __attribute__((packed)) bits;
			u32 full;
		} __attribute__((packed)) reg31;
	} __attribute__((packed)) reg_page;
	size_t reg_page_show;
};

struct adc7k_cpci3u_board_private_data {
	struct adc7k_cpci3u_board *board;
	char buff[0xc000];
	size_t length;
	loff_t f_pos;
};

static inline u32 adc7k_cpci3u_board_read_reg(struct adc7k_cpci3u_board *board, unsigned int index)
{
	u32 *pdata = (u32 *)&board->reg_page;

	pdata += index;

	*pdata = inl(board->base_io_addr + (index * 4));
	rmb();

	return *pdata;
}

static inline void adc7k_cpci3u_board_write_reg(struct adc7k_cpci3u_board *board, unsigned int index, u32 data)
{
	u32 *pdata = (u32 *)&board->reg_page;

	pdata += index;

	*pdata = data;

	outl(*pdata, board->base_io_addr + (index * 4));
	wmb();
}

static inline void adc7k_cpci3u_board_sync_reg(struct adc7k_cpci3u_board *board, unsigned int index)
{
	u32 *pdata = (u32 *)&board->reg_page;

	pdata += index;

	outl(*pdata, board->base_io_addr + (index * 4));
	wmb();
}

static inline void adc7k_cpci3u_board_reset_all(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg1.bits.reset_all = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_all = 1;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_all = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_reset_interrupt(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg1.bits.reset_int = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_int = 1;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_int = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_reset_dma(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg1.bits.reset_dma = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_dma = 1;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.reset_dma = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_sampler_interrupt_enable(struct adc7k_cpci3u_board *board, unsigned int value)
{
	board->reg_page.reg1.bits.ie_sampler = value;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_dma_interrupt_enable(struct adc7k_cpci3u_board *board, unsigned int value)
{
	board->reg_page.reg1.bits.ie_dma = value;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_dma_start(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg1.bits.dma_start = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.dma_start = 1;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.dma_start = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_dma_chain(struct adc7k_cpci3u_board *board, unsigned int value)
{
	board->reg_page.reg1.bits.dma_chain = value;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_sampler_start(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg1.bits.sampler_start = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.sampler_start = 1;
	adc7k_cpci3u_board_sync_reg(board, 1);
	board->reg_page.reg1.bits.sampler_start = 0;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_address_counter(struct adc7k_cpci3u_board *board, unsigned int value)
{
	board->reg_page.reg1.bits.en_ad_cnt = value;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_select_channel(struct adc7k_cpci3u_board *board, unsigned int ch)
{
	board->reg_page.reg1.bits.sel_mem_rd = ch;
	adc7k_cpci3u_board_sync_reg(board, 1);
}

static inline void adc7k_cpci3u_board_set_sampler_divider(struct adc7k_cpci3u_board *board, unsigned int divider)
{
	board->reg_page.reg2.bits.div = divider;
	adc7k_cpci3u_board_sync_reg(board, 2);
}

static inline int adc7k_cpci3u_board_is_interrupt_requested(struct adc7k_cpci3u_board *board)
{
	adc7k_cpci3u_board_read_reg(board, 3);
	return (board->reg_page.reg3.bits.dma_done_0 | board->reg_page.reg3.bits.dma_done_1) ? -1 : 0;
}

static inline void adc7k_cpci3u_board_set_sampler_length(struct adc7k_cpci3u_board *board, u32 length)
{
	board->reg_page.reg6.bits.sampler_length = length * 4;
	adc7k_cpci3u_board_sync_reg(board, 6);
}

static inline void adc7k_cpci3u_board_set_dma_address(struct adc7k_cpci3u_board *board, u32 address)
{
	board->reg_page.reg7.bits.dma_address = address;
	adc7k_cpci3u_board_sync_reg(board, 7);
}

static inline void adc7k_cpci3u_board_set_dma_length(struct adc7k_cpci3u_board *board, size_t length)
{
	board->reg_page.reg9.bits.dma_length = length;
	adc7k_cpci3u_board_sync_reg(board, 9);
}

static inline void adc7k_cpci3u_board_adc_reset(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg15.bits.adc_reset = 1;
	adc7k_cpci3u_board_sync_reg(board, 15);
	board->reg_page.reg15.bits.adc_reset = 0;
	adc7k_cpci3u_board_sync_reg(board, 15);
}

static inline void adc7k_cpci3u_board_adc_write(struct adc7k_cpci3u_board *board, u8 addr, u16 data)
{
	size_t i;

	adc7k_cpci3u_board_read_reg(board, 15);

	board->reg_page.reg15.bits.adc_sclk = 1;
	board->reg_page.reg15.bits.adc_cs = 0;
	adc7k_cpci3u_board_sync_reg(board, 15);

	board->reg_page.reg15.bits.adc_cs = 1;
	adc7k_cpci3u_board_sync_reg(board, 15);

	for (i = 0; i < 8; ++i) {
		board->reg_page.reg15.bits.adc_sclk = 0;
		adc7k_cpci3u_board_sync_reg(board, 15);
		board->reg_page.reg15.bits.adc_sdata = (addr & 0x80) ? 1 : 0;
		adc7k_cpci3u_board_sync_reg(board, 15);
		board->reg_page.reg15.bits.adc_sclk = 1;
		adc7k_cpci3u_board_sync_reg(board, 15);
		addr <<= 1;
	}

	for (i = 0; i < 16; ++i) {
		board->reg_page.reg15.bits.adc_sclk = 0;
		adc7k_cpci3u_board_sync_reg(board, 15);
		board->reg_page.reg15.bits.adc_sdata = (data & 0x8000) ? 1 : 0;
		adc7k_cpci3u_board_sync_reg(board, 15);
		board->reg_page.reg15.bits.adc_sclk = 1;
		adc7k_cpci3u_board_sync_reg(board, 15);
		data <<= 1;
	}

	board->reg_page.reg15.bits.adc_cs = 0;
	adc7k_cpci3u_board_sync_reg(board, 15);
}

static inline void adc7k_cpci3u_board_reset_ddr(struct adc7k_cpci3u_board *board)
{
	board->reg_page.reg16.bits.ddr_reset = 0;
	adc7k_cpci3u_board_sync_reg(board, 16);
	board->reg_page.reg16.bits.ddr_reset = 1;
	adc7k_cpci3u_board_sync_reg(board, 16);
	board->reg_page.reg16.bits.ddr_reset = 0;
	adc7k_cpci3u_board_sync_reg(board, 16);
}

static inline void adc7k_cpci3u_board_init(struct adc7k_cpci3u_board *board)
{
	adc7k_cpci3u_board_write_reg(board, 1, 0);
	adc7k_cpci3u_board_adc_reset(board);
	adc7k_cpci3u_board_reset_all(board);
	adc7k_cpci3u_board_dma_chain(board, 1);
	adc7k_cpci3u_board_set_sampler_length(board, ADC7K_CPCI3U_BOARD_MAX_SAMPLER_LENGTH);
	adc7k_cpci3u_board_set_sampler_divider(board, 0);
	adc7k_cpci3u_board_reset_all(board);
}

static void adc7k_cpci3u_board_dma_timer(unsigned long addr)
{
	size_t i;
	struct adc7k_cpci3u_board *board = (struct adc7k_cpci3u_board *)addr;

	spin_lock(&board->lock);

	adc7k_cpci3u_board_reset_interrupt(board);

	del_timer_sync(&board->dma_timer);

	spin_lock(&board->channel[board->channel_current]->lock);

	++board->channel[board->channel_current]->transactions;
	board->channel[board->channel_current]->data_length = min(board->dma_buffer_size, board->sampler_length * 4);
	if (!board->sampler_continuous) {
		board->channel[board->channel_current]->sampler_done = 1;
	}

	spin_unlock(&board->channel[board->channel_current]->lock);

	++board->channel_current;
	if (board->channel_current < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT) {
		adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
		adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
		adc7k_cpci3u_board_select_channel(board, board->channel_current);
		adc7k_cpci3u_board_reset_all(board);
		adc7k_cpci3u_board_dma_start(board);
		board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
		board->dma_timer.data = (unsigned long)board;
		board->dma_timer.expires = jiffies + HZ;
		add_timer(&board->dma_timer);
	} else {
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			spin_lock(&board->channel[i]->lock);
			wake_up_interruptible(&board->channel[i]->read_waitq);
			wake_up_interruptible(&board->channel[i]->poll_waitq);
			spin_unlock(&board->channel[i]->lock);
		}
		if (board->sampler_continuous) {
			board->channel_current = 0;
			adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
			adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
			adc7k_cpci3u_board_select_channel(board, board->channel_current);
			adc7k_cpci3u_board_reset_all(board);
			adc7k_cpci3u_board_sampler_start(board);
			board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
			board->dma_timer.data = (unsigned long)board;
			board->dma_timer.expires = jiffies + HZ;
			add_timer(&board->dma_timer);
		} else {
			board->dma_run = 0;
			wake_up_interruptible(&board->dma_waitq);
		}
	}

	spin_unlock(&board->lock);
}

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
	len += sprintf(private_data->buff + len, "{\r\n\t\"type\": \"CompactPCI-3U\",");

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
	if (board->reg_page_show) {
		len += sprintf(private_data->buff + len, ",\r\n\t\"registers\": [");
		for (i = 0; i < ADC7K_CPCI3U_BOARD_REGS_COUNT; ++i) {
			len += sprintf(private_data->buff + len, "\r\n\t\t\"%08x\"%s", adc7k_cpci3u_board_read_reg(board, i), (i == ADC7K_CPCI3U_BOARD_REGS_COUNT - 1) ? "" : ",");
		}
		len += sprintf(private_data->buff + len, "\r\n\t]");
	}
	len += sprintf(private_data->buff + len, "\r\n}\r\n");

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
	struct adc7k_cpci3u_board *board = private_data->board;

	if (private_data->f_pos < 0x80000000) {
		res = (private_data->length > private_data->f_pos) ? (private_data->length - private_data->f_pos) : (0);
		if (res) {
			len = res;
			len = min(count, len);
			if (copy_to_user(buff, private_data->buff + private_data->f_pos, len)) {
				res = -EINVAL;
				goto adc7k_cpci3u_board_read_end;
			}
			private_data->f_pos += len;
		}
	} else {
		res = adc7k_cpci3u_board_read_reg(board, private_data->f_pos & 0x1f);
	}

adc7k_cpci3u_board_read_end:
	return res;
}

static ssize_t adc7k_cpci3u_board_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp)
{
	unsigned long flags;
	ssize_t res;
	size_t i;
	size_t len;
	u_int32_t value, value2;
	struct adc7k_cpci3u_board_private_data *private_data = filp->private_data;
	struct adc7k_cpci3u_board *board = private_data->board;

	len = sizeof(private_data->buff) - 1;
	len = min(len, count);

	if (copy_from_user(private_data->buff, buff, len)) {
		res = -EINVAL;
		goto adc7k_cpci3u_board_write_end;
	}
	private_data->buff[len] = '\0';

	spin_lock_irqsave(&board->lock, flags);

	if (sscanf(private_data->buff, "sampler.start(%u)", &value) == 1) {
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (board->channel[i]) {
				board->channel[i]->sampler_done = 0;
				board->channel[i]->data_length = 0;
			}
		}
		board->sampler_continuous = value;
		board->channel_current = 0;
		if (board->dma_run) {
			spin_unlock_irqrestore(&board->lock, flags);
			res = wait_event_interruptible_timeout(board->dma_waitq, board->dma_run == 0, HZ * 2);
			if (res) {
				goto adc7k_cpci3u_board_write_end;
			}
			spin_lock_irqsave(&board->lock, flags);
		}
		adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
		adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
		adc7k_cpci3u_board_select_channel(board, board->channel_current);
		adc7k_cpci3u_board_reset_all(board);
		adc7k_cpci3u_board_sampler_start(board);
		board->dma_run = 1;
		del_timer_sync(&board->dma_timer);
		board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
		board->dma_timer.data = (unsigned long)board;
		board->dma_timer.expires = jiffies + HZ;
		add_timer(&board->dma_timer);
		res = len;
	} else if (strstr(private_data->buff, "sampler.stop()")) {
		board->sampler_continuous = 0;
		res = len;
	} else if (sscanf(private_data->buff, "sampler.length(%u)", &value) == 1) {
		if (value > board->sampler_length_max) {
			value = board->sampler_length_max;
		}
		board->sampler_length = value;
		adc7k_cpci3u_board_set_sampler_length(board, value);
		res = len;
	} else if (sscanf(private_data->buff, "sampler.divider(%u)", &value) == 1) {
		if ((value >= 0) && (value <= 255)) {
			board->sampler_divider = value;
			adc7k_cpci3u_board_set_sampler_divider(board, value);
			res = len;
		} else {
			res = -EPERM;
		}
	} else if (sscanf(private_data->buff, "dma.start(%u)", &value) == 1) {
		if (board->dma_run) {
			spin_unlock_irqrestore(&board->lock, flags);
			res = wait_event_interruptible_timeout(board->dma_waitq, board->dma_run == 0, HZ * 2);
			if (res) {
				goto adc7k_cpci3u_board_write_end;
			}
			spin_lock_irqsave(&board->lock, flags);
		}
		adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
		adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
		adc7k_cpci3u_board_select_channel(board, value);
		adc7k_cpci3u_board_reset_all(board);
		adc7k_cpci3u_board_dma_start(board);
		board->dma_run = 1;
		del_timer_sync(&board->dma_timer);
		board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
		board->dma_timer.data = (unsigned long)board;
		board->dma_timer.expires = jiffies + HZ;
		add_timer(&board->dma_timer);
		res = len;
	} else if (strstr(private_data->buff, "ddr.reset()")) {
		adc7k_cpci3u_board_reset_ddr(board);
		res = len;
	} else if (strstr(private_data->buff, "board.reset()")) {
		adc7k_cpci3u_board_reset_all(board);
		res = len;
	} else if (strstr(private_data->buff, "adc.reset()")) {
		adc7k_cpci3u_board_adc_reset(board);
		res = len;
	} else if (sscanf(private_data->buff, "adc.write(0x%x,0x%x)", &value, &value2) == 2) {
		spin_unlock_irqrestore(&board->lock, flags);
		adc7k_cpci3u_board_adc_write(board, value, value2);
		spin_lock_irqsave(&board->lock, flags);
		res = len;
	} else if (sscanf(private_data->buff, "registers.show(%u)", &value) == 1) {
		board->reg_page_show = value;
		res = len;
	} else if (sscanf(private_data->buff, "register[%u].write(0x%x)", &value, &value2) == 2) {
		adc7k_cpci3u_board_write_reg(board, value, value2);
		res = len;
	} else {
		res = -ENOMSG;
	}

	spin_unlock_irqrestore(&board->lock, flags);

adc7k_cpci3u_board_write_end:
	return res;
}

static loff_t adc7k_cpci3u_board_llseek(struct file *filp, loff_t off, int whence)
{
	loff_t res;
	struct adc7k_cpci3u_board_private_data *private_data = filp->private_data;

	switch (whence) {
		case 0: /* SEEK_SET */
			res = off;
			break;
		case 1: /* SEEK_CUR */
			res = private_data->f_pos + off;
			break;
		case 2: /* SEEK_END */
			res = private_data->length + off;
			break;
		default: /* can't happen */
			res = -EINVAL;
	}
	if (res < 0) {
		res = -EINVAL;
	} else {
		private_data->f_pos = res;
	}

	return res;
}

static struct file_operations adc7k_cpci3u_board_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_cpci3u_board_open,
	.release = adc7k_cpci3u_board_release,
	.read    = adc7k_cpci3u_board_read,
	.write   = adc7k_cpci3u_board_write,
	.llseek   = adc7k_cpci3u_board_llseek,
};

static int adc7k_cpci3u_channel_open(struct inode *inode, struct file *filp)
{
	unsigned long flags;
	struct adc7k_cpci3u_channel *channel = container_of(inode->i_cdev, struct adc7k_cpci3u_channel, cdev);

	filp->private_data = channel;

	spin_lock_irqsave(&channel->lock, flags);
	++channel->usage;
	channel->sampler_done = 0;
	channel->data_length = 0;
	channel->mmap = 0;
	spin_unlock_irqrestore(&channel->lock, flags);

	return 0;
}

static int adc7k_cpci3u_channel_release(struct inode *inode, struct file *filp)
{
	unsigned long flags;
	struct adc7k_cpci3u_channel *channel = filp->private_data;

	spin_lock_irqsave(&channel->lock, flags);
	--channel->usage;
	spin_unlock_irqrestore(&channel->lock, flags);

	return 0;
}

static ssize_t adc7k_cpci3u_channel_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	unsigned long flags;
	size_t len;
	ssize_t res = 0;
	struct adc7k_cpci3u_channel *channel = filp->private_data;

	spin_lock_irqsave(&channel->lock, flags);

	for (;;) {
		// successfull return
		if (channel->data_length || channel->sampler_done) {
			break;
		}

		spin_unlock_irqrestore(&channel->lock, flags);

		if (filp->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}

		// sleeping
		res = wait_event_interruptible(channel->read_waitq, channel->data_length || channel->sampler_done);
		if (res) {
			return res;
		}

		spin_lock_irqsave(&channel->lock, flags);
	}

	if (channel->data_length) {

		res = len = min(count, channel->data_length);

		if (channel->mmap == 0) {
			spin_unlock_irqrestore(&channel->lock, flags);
			if (copy_to_user(buff, channel->data, len)) {
				res = -EINVAL;
				goto adc7k_cpci3u_board_read_end;
			}
			spin_lock_irqsave(&channel->lock, flags);
		}
		channel->data_length = 0;
	} else {
		channel->sampler_done = 0;
		res = 0;
	}

	spin_unlock_irqrestore(&channel->lock, flags);

adc7k_cpci3u_board_read_end:
	return res;
}

static unsigned int adc7k_cpci3u_channel_poll(struct file *filp, struct poll_table_struct *wait_table)
{
	unsigned long flags;
	unsigned int res = 0;
	struct adc7k_cpci3u_channel *channel = filp->private_data;

	poll_wait(filp, &channel->poll_waitq, wait_table);

	spin_lock_irqsave(&channel->lock, flags);

	if (channel->data_length) {
		res |= POLLIN | POLLRDNORM;
	}

	if (channel->sampler_done) {
		res |= POLLHUP;
	}

	spin_unlock_irqrestore(&channel->lock, flags);

	return res;
}

static int adc7k_cpci3u_channel_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long flags;
	struct adc7k_cpci3u_channel *channel = filp->private_data;
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

	spin_lock_irqsave(&channel->lock, flags);
	channel->mmap = 1;
	spin_unlock_irqrestore(&channel->lock, flags);

	return 0;
}

static struct file_operations adc7k_cpci3u_channel_fops = {
	.owner   = THIS_MODULE,
	.open    = adc7k_cpci3u_channel_open,
	.release = adc7k_cpci3u_channel_release,
	.read    = adc7k_cpci3u_channel_read,
	.poll    = adc7k_cpci3u_channel_poll,
	.mmap    = adc7k_cpci3u_channel_mmap,
};

static irqreturn_t adc7k_cpci3u_board_interrupt(int irq, void *data)
{
	size_t i;
	irqreturn_t res = IRQ_NONE;
	struct adc7k_cpci3u_board *board = data;

	spin_lock(&board->lock);
	
	if (adc7k_cpci3u_board_is_interrupt_requested(board)) {

		adc7k_cpci3u_board_reset_interrupt(board);

		del_timer_sync(&board->dma_timer);

		spin_lock(&board->channel[board->channel_current]->lock);

		++board->channel[board->channel_current]->transactions;
		board->channel[board->channel_current]->data_length = min(board->dma_buffer_size, board->sampler_length * 4);
		if (!board->sampler_continuous) {
			board->channel[board->channel_current]->sampler_done = 1;
		}
		wake_up_interruptible(&board->channel[board->channel_current]->read_waitq);
		wake_up_interruptible(&board->channel[board->channel_current]->poll_waitq);

		spin_unlock(&board->channel[board->channel_current]->lock);

		++board->channel_current;
		if (board->channel_current < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT) {
			adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
			adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
			adc7k_cpci3u_board_select_channel(board, board->channel_current);
			adc7k_cpci3u_board_reset_all(board);
			adc7k_cpci3u_board_dma_start(board);
			board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
			board->dma_timer.data = (unsigned long)board;
			board->dma_timer.expires = jiffies + HZ;
			add_timer(&board->dma_timer);
		} else {
			for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
				spin_lock(&board->channel[i]->lock);
				wake_up_interruptible(&board->channel[i]->read_waitq);
				wake_up_interruptible(&board->channel[i]->poll_waitq);
				spin_unlock(&board->channel[i]->lock);
			}
			if (board->sampler_continuous) {
				board->channel_current = 0;
				adc7k_cpci3u_board_set_dma_address(board, board->dma_address[board->channel_current]);
				adc7k_cpci3u_board_set_dma_length(board, min(board->dma_buffer_size / 4, board->sampler_length));
				adc7k_cpci3u_board_select_channel(board, board->channel_current);
				adc7k_cpci3u_board_reset_all(board);
				adc7k_cpci3u_board_sampler_start(board);
				board->dma_timer.function = adc7k_cpci3u_board_dma_timer;
				board->dma_timer.data = (unsigned long)board;
				board->dma_timer.expires = jiffies + HZ;
				add_timer(&board->dma_timer);
			} else {
				board->dma_run = 0;
				wake_up_interruptible(&board->dma_waitq);
			}
		}
		res = IRQ_HANDLED;
	}

	spin_unlock(&board->lock);

	return res;
}

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
	int pci_region_requested = 0;
	int pci_irq_requested = 0;
	int rc = 0;

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "can't enable pci device\n");
		goto adc7k_cpci3u_board_probe_error;
	}
	rc = pci_request_region(pdev, 0, "adc7k-cpci3u");
	if (rc) {
		dev_err(&pdev->dev, "can't request I/O region\n");
		goto adc7k_cpci3u_board_probe_error;
	}
	pci_region_requested = 1;

	// alloc memory for board data
	if (!(board = kmalloc(sizeof(struct adc7k_cpci3u_board), GFP_KERNEL))) {
		log(KERN_ERR, "can't get memory for struct adc7k_cpci3u_board\n");
		rc = -ENOMEM;
		goto adc7k_cpci3u_board_probe_error;
	}
	memset(board, 0, sizeof(struct adc7k_cpci3u_board));

	board->pci_dev = pdev;

	spin_lock_init(&board->lock);
	init_waitqueue_head(&board->dma_waitq);
	init_timer(&board->dma_timer);

	board->sampling_rate = ADC7K_DEFAULT_SAMPLING_RATE;

	// get BAR0 base i/o address
	board->base_io_addr = pci_resource_start(pdev, 0);

	// set interrupt handler
	if (!(rc = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &board->irq_pin))) {
		if (board->irq_pin) {
			if (!(rc = pci_read_config_byte(pdev, PCI_INTERRUPT_LINE, &board->irq_line))) {
				if ((rc = request_irq(pdev->irq, adc7k_cpci3u_board_interrupt, IRQF_SHARED, "adc7k", board))) {
					log(KERN_ERR, "%s: Unable to request IRQ %d (error %d)\n", "adc7k", pdev->irq, rc);
					goto adc7k_cpci3u_board_probe_error;
				} else {
					pci_irq_requested = 1;
				}
			} else {
				dev_err(&pdev->dev, "pci_read_config_byte(pdev, PCI_INTERRUPT_LINE, &board->irq_pin) error=%d\n", rc);
				goto adc7k_cpci3u_board_probe_error;
			}
		} else {
			log(KERN_ERR, "ADC7K CompactPCI-3U board must drive at least one interrupt pin\n");
			rc = -1;
			goto adc7k_cpci3u_board_probe_error;
		}
	} else {
		dev_err(&pdev->dev, "pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &board->irq_pin) error=%d\n", rc);
		goto adc7k_cpci3u_board_probe_error;
	}

	// remap external DMA buffer
	board->dma_buffer_size = (dmaend - dmastart + 1) / ADC7K_CHANNEL_PER_BOARD_MAX_COUNT;
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (!(board->dma_buffer[i] = ioremap(dmastart + i * board->dma_buffer_size, board->dma_buffer_size))) {
			log(KERN_ERR, "%lu: Unable to remap memory 0x%08lx-0x%08lx\n", (unsigned long int)i, (unsigned long int)(dmastart + i * board->dma_buffer_size), (unsigned long int)(dmastart + (i + 1) * board->dma_buffer_size - 1));
			rc = -ENOMEM;
			goto adc7k_cpci3u_board_probe_error;
		}
		board->dma_address[i] = dmastart + i * board->dma_buffer_size;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "board-cpci3u-%02x-%02x-%x", pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
#else
	snprintf(device_name, ADC7K_BOARD_NAME_MAX_LENGTH, "bp%02x%02x%x", pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
#endif
	if (!(board->adc7k_board = adc7k_board_register(THIS_MODULE, device_name, &board->cdev, &adc7k_cpci3u_board_fops))) {
		rc = -1;
		goto adc7k_cpci3u_board_probe_error;
	}

	// alloc memory for channel data
	for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
		if (!(board->channel[i] = kmalloc(sizeof(struct adc7k_cpci3u_channel), GFP_KERNEL))) {
			log(KERN_ERR, "can't get memory for struct adc7k_cpci3u_channel\n");
			rc = -ENOMEM;
			goto adc7k_cpci3u_board_probe_error;
		}
		memset(board->channel[i], 0, sizeof(struct adc7k_cpci3u_channel));

		spin_lock_init(&board->channel[i]->lock);
		init_waitqueue_head(&board->channel[i]->poll_waitq);
		init_waitqueue_head(&board->channel[i]->read_waitq);
		board->channel[i]->data = board->dma_buffer[i];
		board->channel[i]->mem_start = board->dma_address[i];
		board->channel[i]->mem_length = board->dma_buffer_size;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "board-cpci3u-%02x-%02x-%x-channel-%s", pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn), adc7k_channel_number_to_string(i));
#else
		snprintf(device_name, ADC7K_CHANNEL_NAME_MAX_LENGTH, "bpc%02x%02x%x%s", pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn), adc7k_channel_number_to_string(i));
#endif
		if (!(board->channel[i]->adc7k_channel = adc7k_channel_register(THIS_MODULE, board->adc7k_board, device_name, &board->channel[i]->cdev, &adc7k_cpci3u_channel_fops))) {
			rc = -1;
			goto adc7k_cpci3u_board_probe_error;
		}
	}

	// reset board
	adc7k_cpci3u_board_reset_all(board);
	// reset DDR controller
	adc7k_cpci3u_board_reset_ddr(board);
	// init board
	adc7k_cpci3u_board_init(board);
	// set samplers length
	board->sampler_length_max = board->dma_buffer_size / 4;
	if (board->sampler_length_max > ADC7K_CPCI3U_BOARD_MAX_SAMPLER_LENGTH) {
		board->sampler_length_max = ADC7K_CPCI3U_BOARD_MAX_SAMPLER_LENGTH;
	}
	board->sampler_length = board->sampler_length_max;
	adc7k_cpci3u_board_set_sampler_length(board, board->sampler_length);

	pci_set_drvdata(pdev, board);
	return rc;

adc7k_cpci3u_board_probe_error:
	if (board) {
		if (pci_irq_requested) {
			free_irq(pdev->irq, board);
		}
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (board->dma_buffer[i]) {
				iounmap(board->dma_buffer[i]);
			}
		}
		del_timer_sync(&board->dma_timer);
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
	if (pci_region_requested) {
		pci_release_region(pdev, 0);
	}
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
		free_irq(pdev->irq, board);
		for (i = 0; i < ADC7K_CHANNEL_PER_BOARD_MAX_COUNT; ++i) {
			if (board->dma_buffer[i]) {
				iounmap(board->dma_buffer[i]);
			}
		}
		del_timer_sync(&board->dma_timer);
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
	pci_release_region(pdev, 0);
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

	// get external DMA buffer
	if ((dmastart > 0) && (dmaend > 0) && (dmaend > dmastart)) {
		if (!(dma_region = request_mem_region(dmastart, dmaend - dmastart + 1, "adc7k-cpci3u"))) {
			log(KERN_ERR, "Can't request DMA memory region start=0x%08lx end=0x%08lx\n", (unsigned long int)dmastart, (unsigned long int)dmaend);
			rc = -ENOMEM;
			goto adc7k_cpci3u_init_error;
		}
	} else {
		log(KERN_ERR, "Nonexistent external DMA memory dmastart=0x%08lx dmaend=0x%08lx\n", (unsigned long int)dmastart, (unsigned long int)dmaend);
		rc = -ENOMEM;
		goto adc7k_cpci3u_init_error;
	}

	// Register PCI driver
	if ((rc = pci_register_driver(&adc7k_cpci3u_driver)) < 0) {
		log(KERN_ERR, "can't register pci driver\n");
		goto adc7k_cpci3u_init_error;
	}

	verbose("loaded\n");
	return rc;

adc7k_cpci3u_init_error:
	if (dma_region) {
		release_mem_region(dmastart, dmaend - dmastart + 1);
	}
	return rc;
}

static void __exit adc7k_cpci3u_exit(void)
{
	pci_unregister_driver(&adc7k_cpci3u_driver);

	release_mem_region(dmastart, dmaend - dmastart + 1);

	verbose("stopped\n");
}

module_init(adc7k_cpci3u_init);
module_exit(adc7k_cpci3u_exit);
