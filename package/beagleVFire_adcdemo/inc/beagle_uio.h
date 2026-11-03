#ifndef BEAGLE_UIO_H
#define BEAGLE_UIO_H

#include <stdint.h>

/*
 * BeagleV Fire FPGA ADC sampler + AXI DMA access over UIO.
 * Register map taken from polarfire-soc-linux-examples
 * fpga-fabric-interfaces/adc-sampler-control/uio-adc-sampler-control.c
 */
#define SAMPLER_UIO_DEVICE_NAME  "fpga_adc_sampler"
#define DMA_UIO_DEVICE_NAME      "dma-controller@60010000"
#define UDMABUF_DEVICE_NAME      "udmabuf-ddr-nc0"

#define REG_CONTROL_OFFSET       0x00
#define REG_STATUS_OFFSET        0x08
#define REG_CAPTURE_COUNT_OFFSET 0x10
#define REG_SAMPLES_DONE_OFFSET  0x18
#define REG_LAST_SAMPLE_OFFSET   0x20
#define REG_ACK_OFFSET           0x28
#define REG_SPI_RATE_SEL_OFFSET  0x30
#define REG_SPI_TX_WORD_OFFSET   0x38

#define CONTROL_CAPTURE_MASK     (1ULL << 1)
#define CONTROL_CONTINUOUS_MASK  (1ULL << 4)
#define CONTROL_WRITE_KEY        (0xADCA5A5AULL << 32)
#define STATUS_RUNNING_MASK      (1ULL << 1)
#define STATUS_IDLE_MASK         (1ULL << 2)
#define STATUS_HALF0_READY_MASK  (1ULL << 5)
#define STATUS_HALF1_READY_MASK  (1ULL << 6)
#define STATUS_OVERRUN_MASK      (1ULL << 7)

#define ACK_HALF0_MASK           (1ULL << 0)
#define ACK_HALF1_MASK           (1ULL << 1)

#define DMA_START_OPERATION_OFFSET 0x04
#define DMA_IRQ_STATUS_OFFSET    0x10
#define DMA_IRQ_MASK_OFFSET      0x14
#define DMA_IRQ_CLEAR_OFFSET     0x18
#define DMA_IRQ_EXT_ADDR_OFFSET  0x1C
#define DMA_EXEC_CONFIG_OFFSET   0x60
#define DMA_EXEC_BYTES_OFFSET    0x64
#define DMA_EXEC_SOURCE_OFFSET   0x68
#define DMA_EXEC_DEST_OFFSET     0x6C

#define DMA_IRQ_MASK_ENABLE      0x0000000FU
#define DMA_IRQ_STATUS_MASK      0x0000000FU
#define DMA_IRQ_WRERR_MASK       0x00000002U
#define DMA_IRQ_RDERR_MASK       0x00000004U
#define DMA_IRQ_INVBD_MASK       0x00000008U
#define DMA_MEMORY_COPY_CONFIG   0x0000F005U
#define DMA_START_OPERATION      0x00000001U

/* LSRAM ping-pong buffer: physical source address and size of one half */
#define LSRAM_DMA_SOURCE_ADDR    0x80000000U
#define LSRAM_MAX_SAMPLE_COUNT   36860U
#define ADC_SAMPLE_BYTES         2U

/* Offset of the DMA destination inside the udmabuf */
#define STREAM_DATA_OFFSET       0x1000U

#define CAPTURE_TIMEOUT_MS       5000
#define FABRIC_CLOCK_HZ          125000000U

enum spi_rate_sel {
	SPI_RATE_10KSPS,
	SPI_RATE_100KSPS,
	SPI_RATE_250KSPS,
	SPI_RATE_500KSPS,
	SPI_RATE_1000KSPS,
};

struct spi_rate_preset {
	uint32_t selector;
	uint32_t sample_rate_sps;
	uint32_t sclk_half_period_clks;
	uint32_t cs_high_period_clks;
};

struct bvf_uio {
	int sampler_fd;
	int dma_fd;
	int udmabuf_fd;
	volatile uint8_t *sampler_base;
	volatile uint8_t *dma_base;
	volatile uint8_t *udmabuf_base;
	uint32_t sampler_size;
	uint32_t dma_size;
	uint32_t udmabuf_size;
	uint64_t udmabuf_phys;
};

int bvf_open(struct bvf_uio *dev);
void bvf_close(struct bvf_uio *dev);

const struct spi_rate_preset *bvf_rate_by_ksps(uint32_t ksps);
const struct spi_rate_preset *bvf_active_rate(struct bvf_uio *dev);
uint32_t bvf_spi_clock_khz(const struct spi_rate_preset *preset);
int bvf_set_rate(struct bvf_uio *dev, const struct spi_rate_preset *preset);
int bvf_set_tx_word(struct bvf_uio *dev, uint16_t tx_word);

int bvf_start_capture(struct bvf_uio *dev, uint32_t half_samples);
void bvf_stop_capture(struct bvf_uio *dev);

/*
 * Wait for the given ping-pong half to become ready.
 * Returns 0 when ready, 1 if the sampler went idle, negative on error.
 */
int bvf_wait_half(struct bvf_uio *dev, uint32_t half);

/*
 * DMA one ping-pong half from LSRAM to the udmabuf and copy it to @dst.
 * @dst must have room for @samples 16-bit words.
 */
int bvf_read_half(struct bvf_uio *dev, uint32_t half, uint32_t half_samples,
		  uint16_t *dst, uint32_t samples);
void bvf_ack_half(struct bvf_uio *dev, uint32_t half);

#endif /* BEAGLE_UIO_H */
