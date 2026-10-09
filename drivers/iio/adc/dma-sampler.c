// SPDX-License-Identifier: GPL-2.0

#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/iio.h>

#define SAMPLER_MAX_SAMPLE_COUNT	36864
#define SAMPLER_SAMPLES_PER_WORD	4
#define SAMPLER_BYTES_PER_WORD		8

#define SAMPLER_BUFFER_SAMPLE_COUNT	SAMPLER_MAX_SAMPLE_COUNT
#define SAMPLER_BUFFER_BYTE_COUNT	(SAMPLER_BUFFER_SAMPLE_COUNT / 	\
					 SAMPLER_SAMPLES_PER_WORD * 	\
					 SAMPLER_BYTES_PER_WORD)

#define SAMPLER_ADDRESS			0x60000000
#define SAMPLER_SIZE			0x1000

#define DMA_CONTROLLER_ADDRESS		0x60010000
#define DMA_CONTROLLER_SIZE		0x1000

#define DMA_SOURCE_ADDRESS		0x80000000
#define DMA_DESTINATION_OFFSET		0x40000000

/*
 * Sampler bits and registers.
 */
#define SAMPLER_CONTROL_KEY		0xadca5a5a
#define SAMPLER_CONTROL_CAPTURE_BIT	BIT(1)
#define SAMPLER_CONTROL_CONTINUOUS_BIT	BIT(4)

#define SAMPLER_STATUS_HALF0_READY_BIT	BIT(5)
#define SAMPLER_STATUS_HALF1_READY_BIT	BIT(6)

#define SAMPLER_ACK_HALF0_BIT		BIT(0)
#define SAMPLER_ACK_HALF1_BIT		BIT(1)

#define SAMPLER_CONTROL_REG		0x00
#define SAMPLER_STATUS_REG		0x08
#define SAMPLER_CAPTURE_COUNT_REG	0x10
#define SAMPLER_ACK_REG			0x28
#define SAMPLER_SPI_RATE_SEL_REG	0x30
#define SAMPLER_SPI_TX_WORD_REG		0x38

/*
 * DMA controller bits and registers.
 */
#define DMA_INTR_0_STAT_REG		0x010
#define DMA_INTR_0_MASK_REG		0x014
#define DMA_INTR_0_CLEAR_REG 		0x018

#define DMA_INTR_INVLD_BUFF_DESC_BIT	BIT(3)
#define DMA_INTR_RD_TRAN_BIT		BIT(2)
#define DMA_INTR_WR_TRAN_BIT		BIT(1)
#define DMA_INTR_OPS_COMPL_BIT		BIT(0)
#define DMA_INTR_MASK_ALL		(DMA_INTR_INVLD_BUFF_DESC_BIT |	\
					 DMA_INTR_RD_TRAN_BIT |		\
					 DMA_INTR_WR_TRAN_BIT |		\
					 DMA_INTR_OPS_COMPL_BIT)

#define DMA_DESC_0_CONFIG_REG		0x060
#define DMA_DESC_0_BYTE_COUNT_REG	0x064
#define DMA_DESC_0_SOURCE_ADDR_REG	0x068
#define DMA_DESC_0_DEST_ADDR_REG	0x06c
#define DMA_DESC_0_NEXT_DEST_ADDR_REG	0x070

#define DMA_DESC_0_CONFIG		(DMA_DESCRIPTOR_VALID |		\
					 DMA_DEST_DATA_READY |		\
					 DMA_SOURCE_DATA_VALID |	\
					 DMA_INTR_ON_PROCESS |		\
					 DMA_DESTINATION_OPR_01 |	\
					 DMA_SOURCE_OPR_01)

#define DMA_DESCRIPTOR_VALID		BIT(15)
#define DMA_DEST_DATA_READY		BIT(14)
#define DMA_SOURCE_DATA_VALID		BIT(13)
#define DMA_INTR_ON_PROCESS		BIT(12)
#define DMA_DESTINATION_OPR_01		BIT(2)
#define DMA_SOURCE_OPR_01		BIT(0)

#define DMA_START_OPERATION_REG		0x004
#define DMA_START_BIT_0			BIT(0)

struct dma_sampler_state {
	bool running;
	int half, irq, sampling_freq;

	struct iio_dma_buffer_queue queue;
	struct list_head head;

	void __iomem *sampler_regs, __iomem *dma_regs;
};

#define DMA_SAMPLER_VOLTAGE_CHANNEL(num)			\
	{ 							\
		.type = IIO_VOLTAGE, 				\
		.indexed = 1, 					\
		.channel = (num), 				\
		.info_mask_shared_by_type =			\
			BIT(IIO_CHAN_INFO_SCALE) |		\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.info_mask_shared_by_type_available =		\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.scan_index = (num),				\
		.scan_type = {					\
			.sign = 'u',				\
			.realbits = 12,				\
			.storagebits = 16,			\
			.shift = 4,				\
			.endianness = IIO_BE,			\
		},						\
	}

static const struct iio_chan_spec dma_sampler_channels[] = {
	DMA_SAMPLER_VOLTAGE_CHANNEL(0),
	DMA_SAMPLER_VOLTAGE_CHANNEL(1),
	DMA_SAMPLER_VOLTAGE_CHANNEL(2),
	DMA_SAMPLER_VOLTAGE_CHANNEL(3),
	DMA_SAMPLER_VOLTAGE_CHANNEL(4),
	DMA_SAMPLER_VOLTAGE_CHANNEL(5),
	DMA_SAMPLER_VOLTAGE_CHANNEL(6),
	DMA_SAMPLER_VOLTAGE_CHANNEL(7),
};

/*
 * Only one channel may be enabled at a time: the FPGA sampler only has a
 * single SPI_TX_WORD register used to select which ADC channel is sampled,
 * so simultaneous multi-channel capture is not supported by the hardware.
 */
static const unsigned long dma_sampler_available_scan_masks[] = {
	BIT(0), BIT(1), BIT(2), BIT(3),
	BIT(4), BIT(5), BIT(6), BIT(7),
	0
};

#define DMA_SAMPLER_DEFAULT_SAMPLING_FREQ 1000000

static const int dma_sampler_sampling_freq_avail[] = {
	10000, 100000, 250000, 500000, 1000000
};

static void dma_sampler_fpga_write(struct dma_sampler_state *st,
				   unsigned int reg, unsigned int val,
				   bool flush)
{
	/*
	 * Although the FPGA sampler registers are 64 bits wide, inputs are
	 * 32 bits at most. The upper 32 bits are reserved for a key, for some
	 * registers, and otherwise they are ignored. Thus we can include the
	 * key unconditionally.
	 */
	iowrite64(val | ((u64)SAMPLER_CONTROL_KEY << 32),
		  st->sampler_regs + reg);

	if (flush)
		(void)ioread64(st->sampler_regs + reg);
}

static int dma_sampler_set_sampling_freq(struct dma_sampler_state *st,
					 int freq)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(dma_sampler_sampling_freq_avail); i++) {
		if (freq == dma_sampler_sampling_freq_avail[i])
			goto valid;
	}

	return -EINVAL;

valid:
	dma_sampler_fpga_write(st, SAMPLER_SPI_RATE_SEL_REG, i, true);

	st->sampling_freq = freq;

	return 0;
}

static int dma_sampler_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct dma_sampler_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*val = 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = st->sampling_freq;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int dma_sampler_read_avail(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  const int **vals, int *type, int *length,
				  long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = dma_sampler_sampling_freq_avail;
		*length = ARRAY_SIZE(dma_sampler_sampling_freq_avail);
		*type = IIO_VAL_INT;

		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static int dma_sampler_write_raw(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 int val, int val2, long mask)
{
	struct dma_sampler_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return dma_sampler_set_sampling_freq(st, val);

	default:
		return -EINVAL;
	}
}

static const struct iio_info dma_sampler_info = {
	.read_raw = dma_sampler_read_raw,
	.read_avail = dma_sampler_read_avail,
	.write_raw = dma_sampler_write_raw,
};

static irqreturn_t dma_sampler_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct dma_sampler_state *st = iio_priv(indio_dev);
	struct iio_dma_buffer_block *block;

	iowrite32(DMA_INTR_MASK_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(DMA_INTR_MASK_ALL, st->dma_regs + DMA_INTR_0_MASK_REG);

	scoped_guard(spinlock_irqsave, &st->queue.list_lock) {
		block = list_first_entry_or_null(&st->head,
						 struct iio_dma_buffer_block,
						 head);
		if (block)
			list_del(&block->head);
	}

	if (block)
		iio_dma_buffer_block_done(block);

	return IRQ_HANDLED;
}

static inline bool dma_sampler_buffer_is_ready(struct dma_sampler_state *st)
{
	u64 status;

	status = ioread64(st->sampler_regs + SAMPLER_STATUS_REG);

	return status & (st->half ?
			 SAMPLER_STATUS_HALF1_READY_BIT :
			 SAMPLER_STATUS_HALF0_READY_BIT);
}

static int dma_sampler_iio_dma_buffer_submit(struct iio_dma_buffer_queue *queue,
					     struct iio_dma_buffer_block *block)
{
	struct dma_sampler_state *st = dev_get_drvdata(queue->dev);
	unsigned int ack;
	u32 src, dst;

	if (!st->running) {
		dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG, 0, false);
		dma_sampler_fpga_write(st, SAMPLER_CAPTURE_COUNT_REG,
				       SAMPLER_BUFFER_SAMPLE_COUNT * 2, false);

		iowrite32(DMA_INTR_MASK_ALL,
			  st->dma_regs + DMA_INTR_0_CLEAR_REG);
		iowrite32(DMA_INTR_MASK_ALL,
			  st->dma_regs + DMA_INTR_0_MASK_REG);
		mmiowb();

		dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG,
				       SAMPLER_CONTROL_CAPTURE_BIT |
				       SAMPLER_CONTROL_CONTINUOUS_BIT, true);

		dma_sampler_fpga_write(st, SAMPLER_ACK_REG,
				       SAMPLER_ACK_HALF0_BIT, true);
		dma_sampler_fpga_write(st, SAMPLER_ACK_REG,
				       SAMPLER_ACK_HALF1_BIT, true);

		st->running = true;
	}

	/* Release other buffer half. */
	ack = st->half ? SAMPLER_ACK_HALF0_BIT : SAMPLER_ACK_HALF1_BIT;
	dma_sampler_fpga_write(st, SAMPLER_ACK_REG, ack, true);

	/* Wait for buffer. */
	while (!dma_sampler_buffer_is_ready(st))
		;

	src = DMA_SOURCE_ADDRESS + st->half * SAMPLER_BUFFER_BYTE_COUNT;
	dst = block->phys_addr + DMA_DESTINATION_OFFSET;

	scoped_guard(spinlock_irqsave, &queue->list_lock)
		list_add_tail(&block->head, &st->head);

	/* Setup and start transfer. */
	iowrite32(DMA_INTR_MASK_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(src, st->dma_regs + DMA_DESC_0_SOURCE_ADDR_REG);
	iowrite32(dst, st->dma_regs + DMA_DESC_0_DEST_ADDR_REG);
	iowrite32(SAMPLER_BUFFER_BYTE_COUNT,
		  st->dma_regs + DMA_DESC_0_BYTE_COUNT_REG);
	iowrite32(DMA_DESC_0_CONFIG, st->dma_regs + DMA_DESC_0_CONFIG_REG);
	mmiowb();

	iowrite32(DMA_START_BIT_0, st->dma_regs + DMA_START_OPERATION_REG);

	st->half = !st->half;

	return 0;
}

static void dma_sampler_iio_dma_buffer_abort(struct iio_dma_buffer_queue *queue)
{
	struct dma_sampler_state *st = dev_get_drvdata(queue->dev);

	iio_dma_buffer_block_list_abort(queue, &st->head);

	if (st->running) {
		dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG, 0, true);

		st->running = false;
	}
}

static const struct iio_dma_buffer_ops dma_sampler_iio_dma_buffer_ops = {
	.submit = dma_sampler_iio_dma_buffer_submit,
	.abort = dma_sampler_iio_dma_buffer_abort,
};

static int dma_sampler_iio_buffer_enable(struct iio_buffer *buffer,
					 struct iio_dev *indio_dev)
{
	struct dma_sampler_state *st = iio_priv(indio_dev);
	unsigned int chan;

	chan = find_first_bit(indio_dev->active_scan_mask,
			      indio_dev->masklength);

	dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG, 0, false);
	mmiowb();

	dma_sampler_fpga_write(st, SAMPLER_SPI_TX_WORD_REG, chan, false);

	return iio_dma_buffer_enable(buffer, indio_dev);
}

static void dma_sampler_iio_buffer_access_release(struct iio_buffer *buf)
{
	struct iio_dma_buffer_queue *queue =
		container_of(buf, struct iio_dma_buffer_queue, buffer);

	iio_dma_buffer_release(queue);
}

static const struct iio_buffer_access_funcs
				dma_sampler_iio_buffer_access_funcs = {
	.read = iio_dma_buffer_read,
	.write = iio_dma_buffer_write,
	.set_bytes_per_datum = iio_dma_buffer_set_bytes_per_datum,
	.set_length = iio_dma_buffer_set_length,
	.request_update = iio_dma_buffer_request_update,
	.enable = dma_sampler_iio_buffer_enable,
	.disable = iio_dma_buffer_disable,
	.data_available = iio_dma_buffer_usage,
	.space_available = iio_dma_buffer_usage,
	.release = dma_sampler_iio_buffer_access_release,

	.enqueue_dmabuf = iio_dma_buffer_enqueue_dmabuf,
	.attach_dmabuf = iio_dma_buffer_attach_dmabuf,
	.detach_dmabuf = iio_dma_buffer_detach_dmabuf,

	.lock_queue = iio_dma_buffer_lock_queue,
	.unlock_queue = iio_dma_buffer_unlock_queue,

	.modes = INDIO_BUFFER_HARDWARE,
	.flags = INDIO_BUFFER_FLAG_FIXED_WATERMARK,
};

static int dma_sampler_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct dma_sampler_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	dev_set_drvdata(dev, st);

	indio_dev->name = "dma-sampler";
	indio_dev->modes = INDIO_BUFFER_HARDWARE;
	indio_dev->info = &dma_sampler_info;
	indio_dev->channels = dma_sampler_channels;
	indio_dev->num_channels = ARRAY_SIZE(dma_sampler_channels);
	indio_dev->available_scan_masks = dma_sampler_available_scan_masks;

	st->sampler_regs = devm_ioremap(dev, SAMPLER_ADDRESS, SAMPLER_SIZE);
	if (!st->sampler_regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map sampler registers\n");

	st->dma_regs = devm_ioremap(dev, DMA_CONTROLLER_ADDRESS,
				    DMA_CONTROLLER_SIZE);
	if (!st->dma_regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map dma registers\n");

	iio_dma_buffer_init(&st->queue, dev, &dma_sampler_iio_dma_buffer_ops);
	INIT_LIST_HEAD(&st->head);

	st->queue.buffer.attrs = NULL;
	st->queue.buffer.access = &dma_sampler_iio_buffer_access_funcs;

	st->queue.buffer.length = SAMPLER_BUFFER_BYTE_COUNT;
	st->queue.buffer.watermark = st->queue.buffer.length;
	st->queue.buffer.direction = IIO_BUFFER_DIRECTION_IN;

	ret = iio_device_attach_buffer(indio_dev, &st->queue.buffer);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to attach buffer to device\n");

	dma_sampler_fpga_write(st, SAMPLER_CONTROL_REG, 0, false);
	dma_sampler_set_sampling_freq(st, DMA_SAMPLER_DEFAULT_SAMPLING_FREQ);

	iowrite32(DMA_INTR_MASK_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(DMA_INTR_MASK_ALL, st->dma_regs + DMA_INTR_0_MASK_REG);
	mmiowb();

	st->irq = platform_get_irq(pdev, 0);
	if (st->irq < 0)
		return dev_err_probe(dev, st->irq, "failed to get irq\n");

	ret = devm_request_irq(dev, st->irq, dma_sampler_irq_handler, 0,
			       dev_name(dev), indio_dev);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to request irq: irq %d\n",
				     st->irq);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id dma_sampler_of_match[] = {
	{ .compatible = "rohm,dma-sampler" },
	{ }
};
MODULE_DEVICE_TABLE(of, dma_sampler_of_match);

static const struct platform_device_id dma_sampler_devices_ids[] = {
	{ .name = "dma-sampler" },
	{ }
};
MODULE_DEVICE_TABLE(platform, dma_sampler_devices_ids);

static struct platform_driver dma_sampler_driver = {
	.driver = {
		.name = "dma-sampler",
		.of_match_table = dma_sampler_of_match,
	},
	.probe = dma_sampler_probe,
	.id_table = dma_sampler_devices_ids,
};
module_platform_driver(dma_sampler_driver);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_DMA_BUFFER);
