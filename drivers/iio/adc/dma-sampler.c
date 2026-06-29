// SPDX-License-Identifier: GPL-2.0

#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/iio.h>

#define SAMPLER_ADDRESS			0x60000000
#define SAMPLER_SIZE			0x1000

#define SAMPLER_BUFFER_BYTE_COUNT	(SAMPLER_LSRAM_SAMPLE_COUNT * 2)
#define SAMPLER_BUFFER_BYTE_COUNT_HALF	(SAMPLER_BUFFER_BYTE_COUNT / 2)

#define SAMPLER_LSRAM_WORD_COUNT	18432
#define SAMPLER_SAMPLES_PER_LSRAM_WORD  4
#define SAMPLER_LSRAM_SAMPLE_COUNT  	(SAMPLER_LSRAM_WORD_COUNT *	\
					 SAMPLER_SAMPLES_PER_LSRAM_WORD)

#define DMA_CONTROLLER_ADDRESS		0x60010000
#define DMA_CONTROLLER_SIZE		0x1000

#define DMA_SOURCE_ADDRESS		0x80000000
#define DMA_DESTINATION_OFFSET		0x40000000

/*
 * sampler bits and registers
 */
#define SAMPLER_CONTROL_KEY_BITS	((u64)0xadca5a5a << 32)
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

/*
 * dma controller bits and registers
 */
#define DMA_INTR_0_STAT_REG		0x010
#define DMA_INTR_0_MASK_REG		0x014
#define DMA_INTR_0_CLEAR_REG 		0x018

#define DMA_INTR_INVLD_BUFF_DESC	BIT(3)
#define DMA_INTR_RD_TRAN		BIT(2)
#define DMA_INTR_WR_TRAN		BIT(1)
#define DMA_INTR_OPS_COMPL		BIT(0)
#define DMA_INTR_CLEAR_ALL		(DMA_INTR_INVLD_BUFF_DESC |	\
					 DMA_INTR_RD_TRAN |		\
					 DMA_INTR_WR_TRAN |		\
					 DMA_INTR_OPS_COMPL)

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
	int half, irq;

	struct iio_dma_buffer_queue queue;
	struct list_head head;

	void __iomem *sampler_regs, __iomem *dma_regs;
};

#define BD79104_VOLTAGE_CHANNEL(num)					\
	{ 								\
		.type = IIO_VOLTAGE, 					\
		.indexed = 1, 						\
		.channel = (num), 					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.scan_index = (num),					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = 12,					\
			.storagebits = 16,				\
			.shift = 4,					\
			.endianness = IIO_BE,				\
		},							\
	}

static const struct iio_chan_spec dma_sampler_channels[] = {
	BD79104_VOLTAGE_CHANNEL(0),
	BD79104_VOLTAGE_CHANNEL(1),
	BD79104_VOLTAGE_CHANNEL(2),
	BD79104_VOLTAGE_CHANNEL(3),
	BD79104_VOLTAGE_CHANNEL(4),
	BD79104_VOLTAGE_CHANNEL(5),
	BD79104_VOLTAGE_CHANNEL(6),
	BD79104_VOLTAGE_CHANNEL(7),
};

static int bd79104_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*val = 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info dma_sampler_info = {
	.read_raw = bd79104_read_raw,
};

static inline bool dma_sampler_buffer_is_ready(struct dma_sampler_state *st)
{
	u64 status;

	status = ioread64(st->sampler_regs + SAMPLER_STATUS_REG);
	return status & (st->half ?
			 SAMPLER_STATUS_HALF0_READY_BIT :
			 SAMPLER_STATUS_HALF1_READY_BIT);
}

static void dma_sampler_start_transfer(struct dma_sampler_state *st,
				       struct iio_dma_buffer_block *block)
{
	u32 src, dst;

	src = DMA_SOURCE_ADDRESS + st->half * SAMPLER_BUFFER_BYTE_COUNT_HALF;
	dst = block->phys_addr + DMA_DESTINATION_OFFSET;

	/* wait for buffer */
	while (!dma_sampler_buffer_is_ready(st))
		;

	/* clear interrupts */
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_MASK_REG);

	/* setup and start transfer */
	iowrite32(src, st->dma_regs + DMA_DESC_0_SOURCE_ADDR_REG);
	iowrite32(dst, st->dma_regs + DMA_DESC_0_DEST_ADDR_REG);
	iowrite32(SAMPLER_BUFFER_BYTE_COUNT_HALF,
		  st->dma_regs + DMA_DESC_0_BYTE_COUNT_REG);
	iowrite32(DMA_DESC_0_CONFIG, st->dma_regs + DMA_DESC_0_CONFIG_REG);
	mmiowb();

	iowrite32(DMA_START_BIT_0, st->dma_regs + DMA_START_OPERATION_REG);
}

static irqreturn_t dma_sampler_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct dma_sampler_state *st = iio_priv(indio_dev);
	struct iio_dma_buffer_block *block;

	/* clear interrupts */
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_MASK_REG);

	/* release buffer half */
	iowrite64(st->half ? SAMPLER_ACK_HALF1_BIT : SAMPLER_ACK_HALF0_BIT,
		  st->sampler_regs + SAMPLER_ACK_REG);

	st->half = !st->half;

	block = container_of(st->head.next,
			     struct iio_dma_buffer_block, head);

	scoped_guard(spinlock_irqsave, &block->queue->list_lock)
		list_del(&block->head);

	iio_dma_buffer_block_done(block);

	return IRQ_HANDLED;
}

static int dma_sampler_iio_dma_buffer_submit(struct iio_dma_buffer_queue *queue,
					     struct iio_dma_buffer_block *block)
{
	struct dma_sampler_state *st = dev_get_drvdata(queue->dev);

	scoped_guard(spinlock_irqsave, &queue->list_lock)
		list_add_tail(&block->head, &st->head);

	dma_sampler_start_transfer(st, block);

	return 0;
}

static void dma_sampler_iio_dma_buffer_abort(struct iio_dma_buffer_queue *queue)
{
	struct dma_sampler_state *st = dev_get_drvdata(queue->dev);

	iio_dma_buffer_block_list_abort(queue, &st->head);
}

static const struct iio_dma_buffer_ops dma_sampler_iio_dma_buffer_ops = {
	.submit = dma_sampler_iio_dma_buffer_submit,
	.abort = dma_sampler_iio_dma_buffer_abort,
};

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
	.enable = iio_dma_buffer_enable,
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
	u8 num_bytes;
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

	st->sampler_regs = devm_ioremap(dev, SAMPLER_ADDRESS, SAMPLER_SIZE);
	if (!st->sampler_regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map sampler registers\n");

	st->dma_regs = devm_ioremap(dev, DMA_CONTROLLER_ADDRESS,
				    DMA_CONTROLLER_SIZE);
	if (!st->dma_regs)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map dma registers\n");

	/* clear interrupts */
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_CLEAR_REG);
	iowrite32(DMA_INTR_CLEAR_ALL, st->dma_regs + DMA_INTR_0_MASK_REG);
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

	iio_dma_buffer_init(&st->queue, dev, &dma_sampler_iio_dma_buffer_ops);
	INIT_LIST_HEAD(&st->head);

	st->queue.buffer.attrs = NULL;
	st->queue.buffer.access = &dma_sampler_iio_buffer_access_funcs;

	num_bytes = indio_dev->channels[0].scan_type.storagebits / 8;
	st->queue.buffer.length = SAMPLER_BUFFER_BYTE_COUNT_HALF / num_bytes;
	st->queue.buffer.watermark = st->queue.buffer.length;
	st->queue.buffer.direction = IIO_BUFFER_DIRECTION_IN;

	ret = iio_device_attach_buffer(indio_dev, &st->queue.buffer);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to attach buffer to device\n");

	/* set sample count */
	iowrite64(SAMPLER_CONTROL_KEY_BITS | SAMPLER_LSRAM_SAMPLE_COUNT,
		  st->sampler_regs + SAMPLER_CAPTURE_COUNT_REG);

	/* enable continuous capture */
	iowrite64(SAMPLER_CONTROL_KEY_BITS |
		  SAMPLER_CONTROL_CAPTURE_BIT | SAMPLER_CONTROL_CONTINUOUS_BIT,
		  st->sampler_regs + SAMPLER_CONTROL_REG);

-	return devm_iio_device_register(dev, indio_dev);
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
