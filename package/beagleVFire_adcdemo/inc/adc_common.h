#ifndef MVA_ADC_COMMON_H
#define MVA_ADC_COMMON_H

#include "mvaring.h"
#include "bvf_adc_data.h"

#define SHM_NAME "/RPI_ADC_BUFF"
#define SHM_SIZE MVARING_BYTES(sizeof(struct adc_data), NUM_DATA_CHUNKS)

#endif
