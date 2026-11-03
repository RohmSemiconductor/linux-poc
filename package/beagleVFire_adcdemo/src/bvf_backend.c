/*
 * BeagleV Fire ADC streamer back-end.
 *
 * The FPGA fabric reads the ADC over SPI into a LSRAM ping-pong buffer.
 * This back-end waits for a half to become ready (UIO interrupt), DMAs it
 * to DDR (udmabuf) and hands it as one chunk to the common streamer
 * (common/adc_stream). One ring chunk equals one ping-pong half.
 *
 * Derived from the Raspberry Pi 4 version (Jeremy P Bentham, Apache-2.0).
 */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "adc_stream.h"
#include "../inc/adc_common.h"
#include "../inc/beagle_uio.h"
#include "../inc/bvf_adc_data.h"

#define VERSION "0.30"

#define DEFAULT_RATE_KSPS	250U
#define DEFAULT_TX_WORD		0xd0U

static_assert(MAX_SAMPS == LSRAM_MAX_SAMPLE_COUNT,
	      "ring chunk must match one LSRAM ping-pong half");

static struct adc_data g_chunk;

struct bvf_priv {
	struct bvf_uio dev;
	const struct spi_rate_preset *preset;
	uint32_t tx_word;
	uint32_t half;
	uint32_t usec_start;	/* time of the capture start */
	bool opened;
};

static struct bvf_priv g_priv = { .tx_word = DEFAULT_TX_WORD };

/*
 * The RPi version used the BCM system timer. Here the fabric provides no
 * timestamp. The fabric clocks the samples at a constant rate, so only the
 * capture start is timestamped (monotonic clock); the chunk timestamps are
 * computed from the sample rate and the number of samples read.
 * TODO: Latency between the start command and the first sample is not
 * compensated; a fabric side timestamp register would be more accurate.
 */
static uint32_t usec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint32_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000);
}

static int bvf_parse_opt(struct adc_backend *be, int opt, const char *arg)
{
	struct bvf_priv *p = be->priv;

	if (opt != 't')
		return -EINVAL;

	p->tx_word = strtoul(arg, NULL, 0);

	return 0;
}

static int bvf_set_user_config(struct adc_backend *be, uint32_t rate_ksps)
{
	struct bvf_priv *p = be->priv;

	p->preset = bvf_rate_by_ksps(rate_ksps);
	if (!p->preset || p->tx_word > 0xffffU)
		return -EINVAL;

	/* The fabric uses fixed presets, so the target equals the actual rate */
	be->target_rate_sps = p->preset->sample_rate_sps;
	be->actual_rate_sps = p->preset->sample_rate_sps;
	be->spi_clock_hz = bvf_spi_clock_khz(p->preset) * 1000U;

	return 0;
}

static int bvf_init_sampler(struct adc_backend *be)
{
	struct bvf_priv *p = be->priv;
	int ret;

	ret = bvf_open(&p->dev);
	if (ret)
		return ret;
	p->opened = true;

	ret = bvf_set_rate(&p->dev, p->preset);
	if (ret)
		return ret;

	return bvf_set_tx_word(&p->dev, p->tx_word);
}

static int bvf_start_sampling(struct adc_backend *be)
{
	struct bvf_priv *p = be->priv;
	int ret;

	p->half = 0;
	ret = bvf_start_capture(&p->dev, MAX_SAMPS);
	if (ret)
		return ret;
	p->usec_start = usec_now();

	return 0;
}

/*
 * Process one ping-pong half. Returns 0 on success, positive when the
 * sampler stopped, negative on error (-EOVERFLOW on a sampler overrun).
 */
static int bvf_read_chunk(struct adc_backend *be)
{
	struct bvf_priv *p = be->priv;
	int ret;

	ret = bvf_wait_half(&p->dev, p->half);
	if (ret)
		return ret;

	ret = bvf_read_half(&p->dev, p->half, MAX_SAMPS, g_chunk.samples,
			    MAX_SAMPS);
	if (ret)
		return ret;

	/*
	 * Release the half only after it has been copied out, so the
	 * fabric reports an overrun if the DMA could not keep up.
	 */
	bvf_ack_half(&p->dev, p->half);
	p->half ^= 1U;

	return 0;
}

static void bvf_add_timestamp(struct adc_backend *be, uint64_t samples_before)
{
	struct bvf_priv *p = be->priv;

	/* 32bit counter lasts around 71 minutes until wrapping */
	g_chunk.usecs = p->usec_start +
			(uint32_t)(samples_before * 1000000ULL /
				   be->actual_rate_sps);
}

static void bvf_stop_sampling(struct adc_backend *be)
{
	struct bvf_priv *p = be->priv;

	if (p->opened)
		bvf_stop_capture(&p->dev);
}

static void bvf_cleanup_sampler(struct adc_backend *be)
{
	struct bvf_priv *p = be->priv;

	if (!p->opened)
		return;
	bvf_close(&p->dev);
	p->opened = false;
}

static const struct adc_backend_ops bvf_ops = {
	.parse_opt	= bvf_parse_opt,
	.set_user_config	= bvf_set_user_config,
	.init_sampler		= bvf_init_sampler,
	.start_sampling		= bvf_start_sampling,
	.read_chunk	= bvf_read_chunk,
	.add_timestamp	= bvf_add_timestamp,
	.stop_sampling		= bvf_stop_sampling,
	.cleanup_sampler		= bvf_cleanup_sampler,
};

static const struct option bvf_long_opts[] = {
	{"tx-word", required_argument, NULL, 't'},
	{NULL, 0, NULL, 0},
};

static struct adc_backend g_backend = {
	.name		= "BeagleV Fire",
	.version	= VERSION,
	.ops		= &bvf_ops,
	.shm_name	= SHM_NAME,
	.item_size	= sizeof(struct adc_data),
	.num_slots	= NUM_DATA_CHUNKS,
	.samples_per_chunk = MAX_SAMPS,
	.default_rate_ksps = DEFAULT_RATE_KSPS,
	.stop_on_overrun = 1,
	.extra_optstring = "t:",
	.extra_long_opts = bvf_long_opts,
	.extra_usage	= "  -t  --tx-word          16-bit SPI TX word sent to the ADC, default 0xd0\n"
			  "                         Rates (kHz): 10, 100, 250, 500, 1000\n",
	.chunk		= &g_chunk,
	.priv		= &g_priv,
};

struct adc_backend *adcdemo_backend(void)
{
	return &g_backend;
}
