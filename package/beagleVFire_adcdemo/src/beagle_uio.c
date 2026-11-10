#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../inc/beagle_uio.h"

#define SYSFS_PATH_LEN	128
#define ID_STR_LEN	32
#define NUM_UIO_DEVICES	32

static const struct spi_rate_preset rate_presets[] = {
	{ SPI_RATE_10KSPS, 10000U, 300U, 2900U },
	{ SPI_RATE_100KSPS, 100000U, 30U, 290U },
	{ SPI_RATE_250KSPS, 250000U, 12U, 116U },
	{ SPI_RATE_500KSPS, 500000U, 6U, 58U },
	{ SPI_RATE_1000KSPS, 1000000U, 3U, 29U },
};

static void sampler_write(struct bvf_uio *dev, uint32_t offset, uint64_t value)
{
	volatile uint64_t *regs = (volatile uint64_t *)dev->sampler_base;

	regs[offset / sizeof(uint64_t)] = CONTROL_WRITE_KEY | value;
	__sync_synchronize();
}

static uint64_t sampler_read(struct bvf_uio *dev, uint32_t offset)
{
	volatile uint64_t *regs = (volatile uint64_t *)dev->sampler_base;

	return regs[offset / sizeof(uint64_t)];
}

static uint32_t dma_read(struct bvf_uio *dev, uint32_t offset)
{
	volatile uint32_t *regs = (volatile uint32_t *)dev->dma_base;

	return regs[offset / sizeof(uint32_t)];
}

static void dma_write(struct bvf_uio *dev, uint32_t offset, uint32_t value)
{
	volatile uint32_t *regs = (volatile uint32_t *)dev->dma_base;

	regs[offset / sizeof(uint32_t)] = value;
}

static int read_file_value(const char *path, uint64_t *value)
{
	char str[32];
	FILE *fp;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;
	if (!fgets(str, sizeof(str), fp)) {
		fclose(fp);
		return -EIO;
	}
	fclose(fp);
	*value = strtoull(str, NULL, 0);

	return 0;
}

static int find_uio_index(const char *id)
{
	char name[ID_STR_LEN];
	char path[SYSFS_PATH_LEN];
	FILE *fp;
	int i;

	for (i = 0; i < NUM_UIO_DEVICES; i++) {
		snprintf(path, sizeof(path), "/sys/class/uio/uio%d/name", i);
		fp = fopen(path, "r");
		if (!fp)
			break;
		if (fscanf(fp, "%31s", name) != 1)
			name[0] = '\0';
		fclose(fp);
		if (!strncmp(name, id, strlen(id)))
			return i;
	}

	return -1;
}

static int map_uio(const char *name, int *fd, volatile uint8_t **base,
		   uint32_t *size)
{
	char path[SYSFS_PATH_LEN];
	uint64_t map_size;
	int idx;

	idx = find_uio_index(name);
	if (idx < 0) {
		fprintf(stderr, "can't locate uio device for %s\n", name);
		return -ENODEV;
	}

	snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map0/size",
		 idx);
	if (read_file_value(path, &map_size) || !map_size) {
		fprintf(stderr, "bad memory size for uio%d\n", idx);
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "/dev/uio%d", idx);
	*fd = open(path, O_RDWR);
	if (*fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -errno;
	}

	*size = (uint32_t)map_size;
	*base = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
	if (*base == MAP_FAILED) {
		fprintf(stderr, "cannot mmap %s: %s\n", path, strerror(errno));
		close(*fd);
		*fd = -1;
		*base = NULL;
		return -errno;
	}

	return 0;
}

static int map_udmabuf(struct bvf_uio *dev)
{
	char path[SYSFS_PATH_LEN];
	uint64_t val;

	snprintf(path, sizeof(path), "/dev/%s", UDMABUF_DEVICE_NAME);
	dev->udmabuf_fd = open(path, O_RDWR);
	if (dev->udmabuf_fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -errno;
	}

	snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/phys_addr",
		 UDMABUF_DEVICE_NAME);
	if (read_file_value(path, &dev->udmabuf_phys))
		goto out_bad;

	snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/size",
		 UDMABUF_DEVICE_NAME);
	if (read_file_value(path, &val) || !val)
		goto out_bad;
	dev->udmabuf_size = (uint32_t)val;

	dev->udmabuf_base = mmap(NULL, dev->udmabuf_size,
				 PROT_READ | PROT_WRITE, MAP_SHARED,
				 dev->udmabuf_fd, 0);
	if (dev->udmabuf_base == MAP_FAILED) {
		fprintf(stderr, "cannot mmap %s: %s\n", UDMABUF_DEVICE_NAME,
			strerror(errno));
		dev->udmabuf_base = NULL;
		goto out_bad;
	}

	return 0;

out_bad:
	close(dev->udmabuf_fd);
	dev->udmabuf_fd = -1;

	return -EINVAL;
}

int bvf_open(struct bvf_uio *dev)
{
	int ret;

	memset(dev, 0, sizeof(*dev));
	dev->sampler_fd = -1;
	dev->dma_fd = -1;
	dev->udmabuf_fd = -1;

	ret = map_uio(SAMPLER_UIO_DEVICE_NAME, &dev->sampler_fd,
		      &dev->sampler_base, &dev->sampler_size);
	if (ret)
		return ret;

	ret = map_uio(DMA_UIO_DEVICE_NAME, &dev->dma_fd, &dev->dma_base,
		      &dev->dma_size);
	if (ret)
		goto out_close;

	ret = map_udmabuf(dev);
	if (ret)
		goto out_close;

	return 0;

out_close:
	bvf_close(dev);

	return ret;
}

static void unmap_close(int fd, volatile uint8_t *base, uint32_t size)
{
	if (base)
		munmap((void *)base, size);
	if (fd >= 0)
		close(fd);
}

void bvf_close(struct bvf_uio *dev)
{
	unmap_close(dev->udmabuf_fd, dev->udmabuf_base, dev->udmabuf_size);
	unmap_close(dev->dma_fd, dev->dma_base, dev->dma_size);
	unmap_close(dev->sampler_fd, dev->sampler_base, dev->sampler_size);
	dev->udmabuf_fd = dev->dma_fd = dev->sampler_fd = -1;
	dev->udmabuf_base = dev->dma_base = dev->sampler_base = NULL;
}

const struct spi_rate_preset *bvf_rate_by_ksps(uint32_t ksps)
{
	size_t i;

	for (i = 0; i < sizeof(rate_presets) / sizeof(rate_presets[0]) - 1; i++)
		if (rate_presets[i].sample_rate_sps >= ksps * 1000U) {
			printf("jee, %u <= %u\n", rate_presets[i].sample_rate_sps, ksps * 1000U);
			break;
		}

	printf("Using: %u, asked for %u\n", rate_presets[i].sample_rate_sps, ksps * 1000U);

	return &rate_presets[i];
}

const struct spi_rate_preset *bvf_active_rate(struct bvf_uio *dev)
{
	uint32_t sel;
	size_t i;

	sel = (uint32_t)(sampler_read(dev, REG_SPI_RATE_SEL_OFFSET) & 0x7ULL);
	for (i = 0; i < sizeof(rate_presets) / sizeof(rate_presets[0]); i++)
		if (rate_presets[i].selector == sel)
			return &rate_presets[i];

	return NULL;
}

uint32_t bvf_spi_clock_khz(const struct spi_rate_preset *preset)
{
	if (!preset || !preset->sclk_half_period_clks)
		return 0;

	return FABRIC_CLOCK_HZ / (2U * preset->sclk_half_period_clks * 1000U);
}

int bvf_set_rate(struct bvf_uio *dev, const struct spi_rate_preset *preset)
{
	sampler_write(dev, REG_SPI_RATE_SEL_OFFSET, preset->selector);
	(void)sampler_read(dev, REG_SPI_RATE_SEL_OFFSET);

	return 0;
}

int bvf_set_tx_word(struct bvf_uio *dev, uint16_t tx_word)
{
	sampler_write(dev, REG_SPI_TX_WORD_OFFSET, tx_word);
	(void)sampler_read(dev, REG_SPI_TX_WORD_OFFSET);

	return 0;
}

static int uio_enable_irq(int fd)
{
	uint32_t enable = 1U;

	if (write(fd, &enable, sizeof(enable)) != (ssize_t)sizeof(enable))
		return -errno;

	return 0;
}

int bvf_start_capture(struct bvf_uio *dev, uint32_t half_samples)
{
	uint64_t need = STREAM_DATA_OFFSET +
			(uint64_t)half_samples * ADC_SAMPLE_BYTES;

	if (need > dev->udmabuf_size) {
		fprintf(stderr, "%s too small: need %u, have %u bytes\n",
			UDMABUF_DEVICE_NAME, (unsigned int)need,
			dev->udmabuf_size);
		return -ENOMEM;
	}

	sampler_write(dev, REG_CONTROL_OFFSET, 0);
	/*
	 * TODO: The example writes sample_count * 2 here although the unit
	 * of this register is not documented in the example (bytes? or two
	 * ping-pong halves?). Verify against ADC_SAMPLER.md.
	 */
	sampler_write(dev, REG_CAPTURE_COUNT_OFFSET, half_samples * 2U);

	dma_write(dev, DMA_IRQ_CLEAR_OFFSET, 0xFU);
	dma_write(dev, DMA_IRQ_MASK_OFFSET, DMA_IRQ_MASK_ENABLE);
	__sync_synchronize();

	sampler_write(dev, REG_CONTROL_OFFSET,
		      CONTROL_CAPTURE_MASK | CONTROL_CONTINUOUS_MASK);

	return 0;
}

void bvf_stop_capture(struct bvf_uio *dev)
{
	sampler_write(dev, REG_CONTROL_OFFSET, 0);
}

static void print_sampler_state(struct bvf_uio *dev, const char *what,
				uint64_t status)
{
	fprintf(stderr, "%s: status=0x%016llx samples_done=%llu "
		"last_sample=0x%04llx\n", what, (unsigned long long)status,
		(unsigned long long)(sampler_read(dev, REG_SAMPLES_DONE_OFFSET)
				     & 0xffffffffULL),
		(unsigned long long)(sampler_read(dev, REG_LAST_SAMPLE_OFFSET)
				     & 0xffffULL));
}

/* Returns 0 ready, 1 idle, 2 not yet, negative on overrun */
static int check_status(struct bvf_uio *dev, uint32_t half)
{
	uint64_t status = sampler_read(dev, REG_STATUS_OFFSET);
	uint64_t ready = half ? STATUS_HALF1_READY_MASK :
				STATUS_HALF0_READY_MASK;

	if (status & STATUS_OVERRUN_MASK) {
		print_sampler_state(dev, "sampler overrun", status);
		return -EOVERFLOW;
	}
	if (status & ready)
		return 0;
	if (!(status & STATUS_RUNNING_MASK) && (status & STATUS_IDLE_MASK))
		return 1;

	return 2;
}

int bvf_wait_half(struct bvf_uio *dev, uint32_t half)
{
	struct pollfd pfd = { .fd = dev->sampler_fd, .events = POLLIN };
	uint32_t pending;
	int ret;

	for (;;) {
		ret = check_status(dev, half);
		if (ret != 2)
			return ret;

		/* Re-arm the IRQ, then re-check to avoid a lost wakeup */
		ret = uio_enable_irq(dev->sampler_fd);
		if (ret)
			return ret;
		ret = check_status(dev, half);
		if (ret != 2)
			return ret;

		ret = poll(&pfd, 1, CAPTURE_TIMEOUT_MS);
		if (ret < 0)
			return -errno;
		if (!ret) {
			print_sampler_state(dev, "timeout waiting for half",
				sampler_read(dev, REG_STATUS_OFFSET));
			return -ETIMEDOUT;
		}
		if (read(dev->sampler_fd, &pending, sizeof(pending)) !=
		    (ssize_t)sizeof(pending))
			return -errno;
	}
}

static int dma_copy(struct bvf_uio *dev, uint32_t src, uint32_t dst,
		    uint32_t bytes)
{
	struct pollfd pfd = { .fd = dev->dma_fd, .events = POLLIN };
	uint32_t pending;
	uint32_t irq;
	int ret;

	dma_write(dev, DMA_IRQ_CLEAR_OFFSET, 0xFU);
	dma_write(dev, DMA_EXEC_SOURCE_OFFSET, src);
	dma_write(dev, DMA_EXEC_DEST_OFFSET, dst);
	dma_write(dev, DMA_EXEC_BYTES_OFFSET, bytes);
	dma_write(dev, DMA_EXEC_CONFIG_OFFSET, DMA_MEMORY_COPY_CONFIG);
	__sync_synchronize();

	ret = uio_enable_irq(dev->dma_fd);
	if (ret)
		return ret;

	dma_write(dev, DMA_START_OPERATION_OFFSET, DMA_START_OPERATION);

	ret = poll(&pfd, 1, CAPTURE_TIMEOUT_MS);
	if (ret < 0)
		return -errno;
	if (!ret) {
		fprintf(stderr, "dma timeout: irq_status=0x%08" PRIx32 "\n",
			dma_read(dev, DMA_IRQ_STATUS_OFFSET));
		return -ETIMEDOUT;
	}
	if (read(dev->dma_fd, &pending, sizeof(pending)) < 0)
		return -errno;

	irq = dma_read(dev, DMA_IRQ_STATUS_OFFSET);
	if (irq)
		dma_write(dev, DMA_IRQ_CLEAR_OFFSET, irq);
	if (irq & (DMA_IRQ_WRERR_MASK | DMA_IRQ_RDERR_MASK |
		   DMA_IRQ_INVBD_MASK)) {
		fprintf(stderr, "dma error: irq_status=0x%08" PRIx32 "\n", irq);
		return -EIO;
	}

	return 0;
}

int bvf_read_half(struct bvf_uio *dev, uint32_t half, uint32_t half_samples,
		  uint16_t *dst, uint32_t samples)
{
	uint32_t half_bytes = half_samples * ADC_SAMPLE_BYTES;
	uint32_t bytes = samples * ADC_SAMPLE_BYTES;
	volatile uint8_t *data = dev->udmabuf_base + STREAM_DATA_OFFSET;
	int ret;

	ret = dma_copy(dev, LSRAM_DMA_SOURCE_ADDR + half * half_bytes,
		       (uint32_t)(dev->udmabuf_phys + STREAM_DATA_OFFSET),
		       bytes);
	if (ret)
		return ret;

	if (msync((void *)data, bytes, MS_INVALIDATE))
		return -errno;
	memcpy(dst, (const void *)data, bytes);

	return 0;
}

void bvf_ack_half(struct bvf_uio *dev, uint32_t half)
{
	sampler_write(dev, REG_ACK_OFFSET,
		      half ? ACK_HALF1_MASK : ACK_HALF0_MASK);
}
