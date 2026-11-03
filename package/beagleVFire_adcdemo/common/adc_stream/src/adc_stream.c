/*
 * Hardware independent part of the ADC streamer.
 *
 * Parses the command line, sets up the shared memory ring-buffer and moves
 * the chunks produced by a hardware back-end (struct adc_backend_ops) to
 * the ring. The back-end is provided by the product (adcdemo_backend()).
 *
 * Derived from the Raspberry Pi 4 version (Jeremy P Bentham, Apache-2.0).
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adc_stream.h"
#include "adcdemo_helpers.h"
#include "mvaring.h"
#include "shmem.h"

#define MAX_LONG_OPTS	16
#define MAX_OPTSTRING	32

static volatile sig_atomic_t g_stop;
static bool g_halt_when_full;
static uint64_t g_samp_total;
static uint32_t g_overrun_total;
static struct shmem_info g_shm_info;

int adc_stream_stop_requested(void)
{
	return g_stop;
}

static void handle_signal(int sig)
{
	g_stop = 1;
}

static void print_usage(const struct adc_backend *be, const char *prog_name)
{
	printf("Usage: %s [options]\n", prog_name);
	printf("Start reading ADC data (%s)\n\n", be->name);
	printf("Options:\n");
	printf("  -r  --rate-ksps        Sampling frequency in kHz, default %u\n",
	       be->default_rate_ksps);
	if (be->extra_usage)
		printf("%s", be->extra_usage);
	printf("  -c  --create-shm       Create the shared-memory buffer and start reading\n");
	printf("  -h, --help             Show this help message\n\n");
	printf("  -H, --halt-when-full   Debug option; Halt when ring-buffer gets full\n");
	printf("Output data to 'mvaring' type ring buffer in shared memory\n");
	printf("Raw shared memery is in '/dev/shm%s'\n", be->shm_name);
}

static int shm_try_open(const struct adc_backend *be, struct shmem_info *shi,
			struct mvaring **mr)
{
	size_t size = MVARING_BYTES(be->item_size, be->num_slots);
	int ret;

	/* TODO: Add a time-out. */
	do {
		ret = shmem_open(be->shm_name, size, shi, true);
		if (ret && ret != -ENOENT)
			return ret;
		if (ret)
			sleep(0);
	} while (ret);

	*mr = shi->buff;
	while (!ring_is_ok(*mr))
		sleep(0);

	return 0;
}

/* Ring is full in halt mode: leave shm alive until the user quits */
static void wait_for_quit(void)
{
	char cmd[32];

	printf("\nRing buffer full, stopping ADC capture\n");
	printf("Shared memory preserved for consumers to drain buffer.\n");
	printf("Type 'quit' or 'q' and press Enter to exit: ");
	while (fgets(cmd, sizeof(cmd), stdin)) {
		if (!strncmp(cmd, "quit", 4) || cmd[0] == 'q')
			break;
		printf("Type 'quit' or 'q' and press Enter to exit: ");
	}
	g_stop = 1;
}

static void store_chunk(const struct adc_backend *be, struct mvaring *mr)
{
	static unsigned long dropped_chunks;

	if (g_halt_when_full) {
		if (ring_add(mr, be->chunk, be->item_size, true))
			wait_for_quit();
		return;
	}

	if (ring_add(mr, be->chunk, be->item_size, false) == -ENOSPC) {
		dropped_chunks++;
		if ((uint8_t)dropped_chunks == 1)
			printf("ring-full: dropped %lu data-chunks\n",
			       dropped_chunks);
	}
}

static int stream_loop(struct adc_backend *be, struct mvaring *mr)
{
	const struct adc_backend_ops *ops = be->ops;
	int ret = 0;

	while (!g_stop) {
		ret = ops->read_chunk(be);
		if (ret == -EOVERFLOW) {
			/* TODO: Decide how to recover; for now just stop */
			g_overrun_total++;
			if (be->stop_on_overrun)
				break;
			ret = 0;
			continue;
		}
		if (ret)
			break;

		if (ops->add_timestamp)
			ops->add_timestamp(be, g_samp_total);
		g_samp_total += be->samples_per_chunk;
		store_chunk(be, mr);
	}

	/* Stopping by a signal is not an error */
	if (ret == -EINTR && g_stop)
		ret = 0;

	return ret > 0 ? 0 : ret;
}

int main(int argc, char *argv[])
{
	struct adc_backend *be = adcdemo_backend();
	const struct adc_backend_ops *ops = be->ops;
	struct option long_options[MAX_LONG_OPTS] = {
		{"rate-ksps", required_argument, NULL, 'r'},
		{"create-shm", no_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{"halt-when-full", no_argument, NULL, 'H'},
	};
	char optstring[MAX_OPTSTRING] = "chHr:";
	uint32_t rate_ksps = be->default_rate_ksps;
	struct mvaring *mr;
	bool create_shm = false;
	int ret, opt, nopts = 4;

	printf("%s ADC streamer v%s\n", be->name, be->version);

	if (be->extra_optstring)
		strncat(optstring, be->extra_optstring,
			sizeof(optstring) - strlen(optstring) - 1);
	for (; be->extra_long_opts && be->extra_long_opts[nopts - 4].name &&
	       nopts < MAX_LONG_OPTS - 1; nopts++)
		long_options[nopts] = be->extra_long_opts[nopts - 4];

	while ((opt = getopt_long(argc, argv, optstring, long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'c':
			create_shm = true;
			break;
		case 'h':
			print_usage(be, argv[0]);
			return 0;
		case 'H':
			g_halt_when_full = true;
			break;
		case 'r':
			rate_ksps = strtoul(optarg, NULL, 10);
			break;
		default:
			if (opt != '?' && ops->parse_opt &&
			    !ops->parse_opt(be, opt, optarg))
				break;
			fprintf(stderr, "Use -h for help\n");
			return 1;
		}
	}

	if (ops->set_user_config(be, rate_ksps)) {
		fprintf(stderr, "Bad sample-rate or option value\n");
		fprintf(stderr, "Use -h for help\n");
		return 1;
	}

	/*
	 * NOTE! These prints may be parsed by the adc_server.py!
	 * Do not change without a warning!
	 */
	printf("Target sample-rate: %u\n", be->target_rate_sps);
	printf("Actual sample-rate: %u\n", be->actual_rate_sps);

	if (create_shm)
		ret = adcdemo_shm_create(&g_shm_info, &mr, be->shm_name,
					 be->item_size, be->num_slots);
	else
		ret = shm_try_open(be, &g_shm_info, &mr);
	if (ret)
		return 1;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	ret = ops->init_sampler(be);
	if (ret)
		goto out_shm;

	printf("Streaming, SPI SCLK %u Hz\n", be->spi_clock_hz);
	ret = ops->start_sampling(be);
	if (ret)
		goto out_cleanup;

	ret = stream_loop(be, mr);
	ops->stop_sampling(be);

	printf("Closing\n");
	if (g_samp_total)
		printf("Total samples %llu, overruns %u\n",
		       (unsigned long long)g_samp_total, g_overrun_total);

out_cleanup:
	ops->cleanup_sampler(be);
out_shm:
	if (g_shm_info.buff)
		shmem_close(&g_shm_info);

	return ret ? 1 : 0;
}
