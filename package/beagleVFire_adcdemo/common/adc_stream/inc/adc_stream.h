#ifndef ADC_STREAM_H
#define ADC_STREAM_H

#include <getopt.h>
#include <stddef.h>
#include <stdint.h>

/* read_chunk() return value: capture ended cleanly (stop requested) */
#define ADC_STREAM_STOPPED	1

struct adc_backend;

/**
 * struct adc_backend_ops - hardware specific operations of an ADC back-end
 * @parse_opt:		Handle one back-end specific command line option (see
 *			&adc_backend.extra_optstring). Returns 0 when the option
 *			was handled. Optional.
 * @set_user_config:	Take the user's settings (sample rate) and validate
 *			them. The hardware is not touched. Fills
 *			&adc_backend.target_rate_sps and
 *			&adc_backend.actual_rate_sps. Returns -EINVAL for
 *			unsupported values.
 * @init_sampler:	Map devices and program the SPI/DMA/UIO hardware using
 *			the user config. Fills &adc_backend.spi_clock_hz.
 *			Sampling is not started yet.
 * @start_sampling:	Start sampling. Kept as short and quick as possible.
 * @read_chunk:		Block until the next chunk is complete and copy it to
 *			&adc_backend.chunk. Returns 0 when a chunk is ready,
 *			%ADC_STREAM_STOPPED when the capture ended, -EOVERFLOW
 *			on a sampler overrun and another negative errno on
 *			errors. Must return when adc_stream_stop_requested().
 * @add_timestamp:	Set the timestamp in &adc_backend.chunk. Called after
 *			each successful @read_chunk with the number of samples
 *			read before this chunk. Optional; NULL if the hardware
 *			time-stamps the data itself (RPi4 DMA).
 * @stop_sampling:	Stop sampling. Must be safe to call more than once.
 * @cleanup_sampler:	Release what @init_sampler acquired. Must be safe to
 *			call more than once.
 *
 * TODO: A @reinit_sample_rate callback may be needed if stopping the
 * capture, changing the sample rate and restarting without a full
 * @init_sampler/@cleanup_sampler cycle gets supported. It would re-program
 * only the rate dependent hardware (and refresh the rate fields in
 * &struct adc_backend) so that @start_sampling can stay short. An alternative
 * is to split the rate specific programming out of @init_sampler.
 */
struct adc_backend_ops {
	int (*parse_opt)(struct adc_backend *be, int opt, const char *arg);
	int (*set_user_config)(struct adc_backend *be, uint32_t rate_ksps);
	int (*init_sampler)(struct adc_backend *be);
	int (*start_sampling)(struct adc_backend *be);
	int (*read_chunk)(struct adc_backend *be);
	void (*add_timestamp)(struct adc_backend *be, uint64_t samples_before);
	void (*stop_sampling)(struct adc_backend *be);
	void (*cleanup_sampler)(struct adc_backend *be);
};

/**
 * struct adc_backend - description of an ADC back-end, provided by the product
 * @name:		Name printed in the banner, e.g. "RPi"
 * @version:		Back-end version string
 * @ops:		Hardware specific operations
 * @shm_name:		POSIX shared memory name of the ring-buffer
 * @item_size:		Size of one ring item (chunk) in bytes
 * @num_slots:		Number of ring slots
 * @samples_per_chunk:	Samples in one chunk
 * @default_rate_ksps:	Sample rate used when -r is not given
 * @stop_on_overrun:	Stop streaming on a sampler overrun when non-zero,
 *			otherwise drop the chunk and carry on.
 *			TODO: Decide a real overrun recovery.
 * @extra_optstring:	getopt() string of back-end options, or NULL
 * @extra_long_opts:	NULL terminated array of back-end long options, or NULL
 * @extra_usage:	Usage text of the back-end options, or NULL
 * @target_rate_sps:	Requested sample rate, set by @ops->set_user_config
 * @actual_rate_sps:	Achievable sample rate, set by @ops->set_user_config
 * @spi_clock_hz:	SPI clock, set by @ops->init_sampler
 * @chunk:		Buffer of @item_size bytes owned by the back-end,
 *			filled by @ops->read_chunk
 * @priv:		Back-end private data
 */
struct adc_backend {
	const char *name;
	const char *version;
	const struct adc_backend_ops *ops;

	const char *shm_name;
	size_t item_size;
	uint32_t num_slots;
	uint32_t samples_per_chunk;
	uint32_t default_rate_ksps;
	int stop_on_overrun;

	const char *extra_optstring;
	const struct option *extra_long_opts;
	const char *extra_usage;

	uint32_t target_rate_sps;
	uint32_t actual_rate_sps;
	uint32_t spi_clock_hz;
	void *chunk;
	void *priv;
};

/* The product provides the back-end description */
struct adc_backend *adcdemo_backend(void);

/* True once SIGINT/SIGTERM was received or the user quit */
int adc_stream_stop_requested(void);

#endif
