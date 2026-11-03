#ifndef BVF_ADC_DATA_H
#define BVF_ADC_DATA_H

#include <stdint.h>

/*
 * One ring item holds exactly one BeagleV Fire LSRAM ping-pong half.
 * LSRAM_MAX_SAMPLE_COUNT 16-bit samples per half (see beagle_uio.h).
 */
#define MAX_SAMPS	36860
/* ~216 kB per item (with GPIO placeholder), 128 items ~ 28 MB of shm */
#define NUM_DATA_CHUNKS	0x80 /* Ring slots, power of 2 */

/* One ring item. The ring itself does not know this layout. */
struct adc_data {
	uint32_t usecs;
	uint16_t samples[MAX_SAMPS];  /* raw 16-bit ADC words */
	/* Placeholder for future GPIO triggering; currently always zero */
	uint32_t gpio_lev0[MAX_SAMPS];  /* GPIO 0-31 state per sample */
};

#endif
