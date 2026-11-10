// SPDX-License-Identifier: GPL-2.0
/*
 * bvf_seq_check - verify the sample sequence the ADC streamer stores to the
 * mvaring in shared memory.
 *
 * Launches bvf_adc_stream (not the web server), opens the ring it creates and
 * checks that the samples are a counter increasing by one:
 *
 *	(ignored), (ignored), N, N+1, N+2, ...
 *
 * The ADC has 12 meaningful bits in the 16-bit samples, so the counter wraps
 * from 4095 to 0. Only the bits in the sample mask (-m, default 0xfff) are
 * compared.
 *
 * which a test device can generate in place of the real ADC. Every
 * discontinuity is reported with its position (ring slot, sample in the item)
 * so it is possible to tell if samples are lost in the FPGA / DMA path
 * (mid-item or item boundary gaps), or by the ring buffer itself (gaps at
 * the ring wrap-around, or ring "dropped" counter increments).
 *
 * The chunk timestamps are analysed as well: average, minimum and maximum
 * time between consecutive chunks, and the number of times the timestamps
 * indicate that chunks were lost (gap longer than 1.5 x the chunk period).
 * The period is taken from the -r sample rate if given, otherwise it is the
 * median of the measured intervals.
 *
 * Run on the BeagleV Fire:  bvf_seq_check -r 1000 -n 5000
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "adc_common.h"
#include "shmem.h"

#define OPEN_TIMEOUT_S 10
#define LOST_THRESHOLD 1.5

static volatile sig_atomic_t g_stop;

static struct adc_data g_item;

struct stats {
	uint64_t items;
	uint64_t samples;
	uint64_t gaps;		/* discontinuities */
	uint64_t missing;	/* samples skipped by forward jumps */
	uint64_t repeated;	/* samples repeated/backward jumps */
	uint64_t gaps_item_start_slot0;	/* sample 0 of ring slot 0 */
	uint64_t gaps_item_start;	/* sample 0 of other slots */
	uint64_t gaps_mid_item;
	uint64_t eagain;
};

/* Time between consecutive chunks, in microseconds */
struct intervals {
	uint32_t *val;
	size_t count;
	size_t cap;
	bool have_prev;
	uint32_t prev_usecs;
};

struct checker {
	uint32_t mask;
	unsigned int skip;	/* samples still to be ignored */
	bool have_exp;
	uint32_t expected;
	unsigned int max_reports;
	unsigned int reports;
};

static void on_signal(int sig)
{
	g_stop = 1;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Run the ADC streamer and check the samples increase by one.\n\n");
	printf("  -r KSPS   sample rate passed to the streamer\n");
	printf("  -p PATH   streamer program (default bvf_adc_stream)\n");
	printf("  -n ITEMS  stop after ITEMS ring items (default: run until ^C)\n");
	printf("  -m MASK   sample mask, wraps at MASK + 1 (default 0xfff, 12-bit ADC)\n");
	printf("  -s COUNT  samples to ignore at the start (default 2)\n");
	printf("  -l COUNT  max discontinuities to print (default 20)\n");
	printf("  -d US     sleep US after every item read (slow reader test)\n");
	printf("  -h        this help\n");
}

static pid_t start_streamer(const char *path, const char *rate)
{
	pid_t pid = fork();

	if (pid)
		return pid;

	if (rate)
		execlp(path, path, "-c", "-r", rate, (char *)NULL);
	else
		execlp(path, path, "-c", (char *)NULL);
	perror(path);
	_exit(127);
}

static void stop_streamer(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, WNOHANG) == pid)
		return;
	kill(pid, SIGINT);
	waitpid(pid, &status, 0);
}

static int open_ring(pid_t pid, struct shmem_info *shi, struct mvaring **mr)
{
	time_t end = time(NULL) + OPEN_TIMEOUT_S;

	while (time(NULL) < end && !g_stop) {
		if (waitpid(pid, NULL, WNOHANG) == pid) {
			fprintf(stderr, "Streamer exited before ring was ready\n");
			return -ECHILD;
		}
		if (!shmem_open(SHM_NAME, SHM_SIZE, shi, true)) {
			*mr = shi->buff;
			if (ring_is_ok(*mr))
				return 0;
			shmem_close(shi);
		}
		usleep(10000);
	}

	fprintf(stderr, "Timed out waiting for ring '%s'\n", SHM_NAME);
	return -ETIMEDOUT;
}

/* The writer updates the counter, so do not let the compiler cache it */
static uint16_t ring_dropped(const struct mvaring *mr)
{
	return *(const volatile uint16_t *)&mr->dropped;
}

static void report_gap(struct checker *c, struct stats *st,
		       const struct mvaring *mr, unsigned int rindex,
		       unsigned int idx, uint32_t got)
{
	uint32_t range = c->mask + 1;
	uint32_t delta = (got - c->expected) & c->mask;
	bool forward = delta < range / 2;
	unsigned int slot = rindex & (mr->num_slots - 1);
	const char *where;

	if (idx == 0 && slot == 0) {
		where = "item start, ring slot 0 (ring wrap-around)";
		st->gaps_item_start_slot0++;
	} else if (idx == 0) {
		where = "item start";
		st->gaps_item_start++;
	} else {
		where = "mid-item";
		st->gaps_mid_item++;
	}

	st->gaps++;
	if (forward)
		st->missing += delta;
	else
		st->repeated += range - delta;

	if (c->reports++ >= c->max_reports)
		return;
	printf("GAP item %u (slot %u/%u) sample %u: expected %u got %u "
	       "(%s%u) [%s]\n", rindex, slot, mr->num_slots, idx, c->expected,
	       got, forward ? "+" : "-", forward ? delta : range - delta,
	       where);
}

static void add_interval(struct intervals *iv, uint32_t usecs)
{
	if (iv->have_prev) {
		if (iv->count == iv->cap) {
			size_t cap = iv->cap ? iv->cap * 2 : 4096;
			uint32_t *n = realloc(iv->val, cap * sizeof(*n));

			if (!n)
				return;
			iv->val = n;
			iv->cap = cap;
		}
		/* Unsigned subtraction handles the 32 bit wrap */
		iv->val[iv->count++] = usecs - iv->prev_usecs;
	}
	iv->prev_usecs = usecs;
	iv->have_prev = true;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

/* period_us: nominal chunk period from the sample rate, 0 if unknown */
static void print_timestamps(struct intervals *iv, double period_us)
{
	uint64_t sum = 0, lost_events = 0, lost_chunks = 0;
	uint32_t min, max;
	double ref;
	size_t i;

	if (!iv->count) {
		printf("\nTimestamps: not enough chunks to analyse\n");
		return;
	}

	min = max = iv->val[0];
	for (i = 0; i < iv->count; i++) {
		sum += iv->val[i];
		if (iv->val[i] < min)
			min = iv->val[i];
		if (iv->val[i] > max)
			max = iv->val[i];
	}

	if (period_us > 0) {
		ref = period_us;
	} else {
		qsort(iv->val, iv->count, sizeof(*iv->val), cmp_u32);
		ref = iv->val[iv->count / 2];
	}

	for (i = 0; i < iv->count; i++) {
		if (iv->val[i] > ref * LOST_THRESHOLD) {
			lost_events++;
			lost_chunks += (uint64_t)(iv->val[i] / ref + 0.5) - 1;
		}
	}

	printf("\nChunk timestamp intervals (%zu):\n", iv->count);
	printf("  average:  %.2f us\n", (double)sum / iv->count);
	printf("  minimum:  %u us\n", min);
	printf("  maximum:  %u us\n", max);
	printf("  reference period: %.2f us (%s)\n", ref,
	       period_us > 0 ? "from -r" : "median");
	printf("  lost chunks by timestamps: %llu in %llu gaps\n",
	       (unsigned long long)lost_chunks,
	       (unsigned long long)lost_events);
}

static void check_item(struct checker *c, struct stats *st,
		       const struct mvaring *mr, unsigned int rindex)
{
	unsigned int i;

	for (i = 0; i < MAX_SAMPS; i++) {
		uint32_t v = g_item.samples[i] & c->mask;

		st->samples++;
		if (c->skip) {
			c->skip--;
			continue;
		}
		if (c->have_exp && v != c->expected)
			report_gap(c, st, mr, rindex, i, v);
		c->have_exp = true;
		c->expected = (v + 1) & c->mask;
	}
}

static void print_summary(const struct stats *st, const struct mvaring *mr)
{
	printf("\nItems checked:        %llu\n", (unsigned long long)st->items);
	printf("Samples checked:      %llu\n", (unsigned long long)st->samples);
	printf("Discontinuities:      %llu\n", (unsigned long long)st->gaps);
	printf("  at ring wrap (slot 0 start): %llu\n",
	       (unsigned long long)st->gaps_item_start_slot0);
	printf("  at other item starts:        %llu\n",
	       (unsigned long long)st->gaps_item_start);
	printf("  inside an item:              %llu\n",
	       (unsigned long long)st->gaps_mid_item);
	printf("Missing samples:      %llu\n", (unsigned long long)st->missing);
	printf("Repeated/backward:    %llu\n", (unsigned long long)st->repeated);
	printf("Ring dropped counter: %u (items overwritten, reader too slow)\n",
	       ring_dropped(mr));
	printf("ring_read -EAGAIN:    %llu\n", (unsigned long long)st->eagain);
}

int main(int argc, char *argv[])
{
	struct checker chk = { .mask = 0xfff, .skip = 2, .max_reports = 20 };
	const char *streamer = "bvf_adc_stream";
	const char *rate = NULL;
	unsigned long max_items = 0, delay_us = 0;
	struct shmem_info shi = { 0 };
	struct mvaring *mr = NULL;
	struct stats st = { 0 };
	struct intervals iv = { 0 };
	struct sigaction sa = { .sa_handler = on_signal };
	uint16_t last_dropped = 0;
	pid_t pid;
	int opt, ret;

	while ((opt = getopt(argc, argv, "r:p:n:m:s:l:d:h")) != -1) {
		switch (opt) {
		case 'r': rate = optarg; break;
		case 'p': streamer = optarg; break;
		case 'n': max_items = strtoul(optarg, NULL, 0); break;
		case 'm': chk.mask = strtoul(optarg, NULL, 0); break;
		case 's': chk.skip = strtoul(optarg, NULL, 0); break;
		case 'l': chk.max_reports = strtoul(optarg, NULL, 0); break;
		case 'd': delay_us = strtoul(optarg, NULL, 0); break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* Avoid attaching to a stale ring of a previous run */
	shm_unlink(SHM_NAME);

	pid = start_streamer(streamer, rate);
	if (pid < 0) {
		perror("fork");
		return 1;
	}

	ret = open_ring(pid, &shi, &mr);
	if (ret) {
		stop_streamer(pid);
		return 1;
	}
	printf("Ring open: %u slots of %u bytes, %d samples per item\n",
	       mr->num_slots, mr->item_size, MAX_SAMPS);

	while (!g_stop && (!max_items || st.items < max_items)) {
		unsigned int rindex = atomic_load(&mr->rindex);

		ret = ring_read(mr, &g_item, 1);
		if (ret == -EAGAIN) {
			st.eagain++;
			if (waitpid(pid, NULL, WNOHANG) == pid) {
				fprintf(stderr, "Streamer exited\n");
				pid = -1;
				break;
			}
			usleep(200);
			continue;
		}
		if (ret < 0) {
			fprintf(stderr, "ring_read: %d\n", ret);
			break;
		}

		add_interval(&iv, g_item.usecs);
		check_item(&chk, &st, mr, rindex);
		st.items++;
		if (ring_dropped(mr) != last_dropped) {
			last_dropped = ring_dropped(mr);
			printf("Ring dropped counter now %u (item %u)\n",
			       last_dropped, rindex);
		}
		if (delay_us)
			usleep(delay_us);
	}

	if (pid > 0)
		stop_streamer(pid);
	print_summary(&st, mr);
	print_timestamps(&iv, rate ? MAX_SAMPS * 1000.0 / atof(rate) : 0);
	if (chk.reports > chk.max_reports)
		printf("(%u further discontinuities not printed)\n",
		       chk.reports - chk.max_reports);
	shmem_close(&shi);
	free(iv.val);

	return st.gaps ? 2 : 0;
}
