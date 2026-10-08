#include <errno.h>          /* IWYU pragma: keep */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <iio/iio.h>

#define BLOCKS_PER_STREAM   8
#define SAMPLES_PER_BLOCK   36864

struct lib {
    struct iio_context *context;

    struct iio_device *device;
    unsigned int current_device;

    unsigned int current_channel;
    struct iio_channel *channel;

    struct iio_channels_mask *mask;
    struct iio_buffer *buffer;
    struct iio_stream *stream;

    bool running;
    int bytes_per_sample;
    unsigned short *samples;
    float scale;

    char **sampling_frequencies;
};
static struct lib lib = { .running = false };

int set_device(int index);
int set_channel(int index);

// #define TRACE() printf("%s\n", __PRETTY_FUNCTION__)
#define TRACE()

int connect()
{
    TRACE();

    if (lib.running)
        return 0;

    memset(&lib, 0, sizeof(lib));

    /*
     * create context
     */
    lib.context = iio_create_context(NULL, "local:");
    if (iio_err(lib.context))
        return iio_err(lib.context);

    lib.running = true;

    return 0;
}

void disconnect(void)
{
    TRACE();

    if (!lib.running)
        return;

    if (lib.stream)
        iio_stream_destroy(lib.stream);
    if (lib.channel)
        iio_channel_disable(lib.channel, lib.mask);
    if (lib.mask)
        iio_channels_mask_destroy(lib.mask);

    free(lib.sampling_frequencies);

    iio_context_destroy(lib.context);

    lib.running = false;
    lib.channel = NULL;
}

const char *get_next_device(void)
{
    TRACE();
    struct iio_device *dev;

    if (!lib.running)
        return NULL;

    dev = iio_context_get_device(lib.context, lib.current_device);
    if (!dev) {
        lib.current_device = 0;
        return NULL;
    }

    lib.current_device++;

    return iio_device_get_name(dev);
}

int set_device(int index)
{
    TRACE();
    int ret;

    if (!lib.running)
        return -EINVAL;

    if (lib.device) {
        /* unset channel */
        set_channel(-1);

        iio_channels_mask_destroy(lib.mask);
        lib.mask = NULL;
    }

    /* for only unsetting the device */
    if (index < 0)
        return 0;

    /*
     * get device
     */
    lib.device = iio_context_get_device(lib.context, index);
    if (!lib.device) {
        ret = -ENODEV;
        goto err;
    }

    /*
     * get buffer
     */
    lib.buffer = iio_device_get_buffer(lib.device, 0);
    if (!lib.buffer) {
        ret = -EINVAL;
        goto err;
    }

    /*
     * create channels mask
     */
    lib.mask = iio_create_channels_mask(iio_device_get_channels_count(
                                        lib.device));
    if (!lib.mask) {
        ret = -ENOMEM;
        goto err;
    }

    return 0;

err:
    if (lib.mask)
        iio_channels_mask_destroy(lib.mask);
    lib.device = NULL;
    return ret;
}

const char *get_next_channel(void)
{
    TRACE();
    struct iio_channel *chan;

    if (!lib.running || !lib.device)
        return NULL;

    chan = iio_device_get_channel(lib.device, lib.current_channel);
    if (!chan) {
        lib.current_channel = 0;
        return NULL;
    }

    lib.current_channel++;

    return iio_channel_get_id(chan);
}

int set_channel(int index)
{
    const struct iio_data_format *format;
    TRACE();

    if (!lib.running || !lib.device)
        return -EINVAL;

    if (lib.channel) {
        if (lib.stream)
            iio_stream_destroy(lib.stream);

        iio_channel_disable(lib.channel, lib.mask);
        free(lib.samples);

        free(lib.sampling_frequencies);
        lib.sampling_frequencies = NULL;
    }

    /* for only unsetting the channel */
    if (index < 0)
        return 0;

    /*
     * get channel
     */
    lib.channel = iio_device_get_channel(lib.device, index);
    if (!lib.channel)
        return -ENODEV;

    /*
    * enable channel
    */
    iio_channel_enable(lib.channel, lib.mask);

    format = iio_channel_get_data_format(lib.channel);

    /*
     * create stream
     */
    lib.stream = iio_buffer_create_stream(lib.buffer, BLOCKS_PER_STREAM,
                                          SAMPLES_PER_BLOCK, lib.mask);
    if (iio_err(lib.stream))
        return iio_err(lib.stream);

    /*
     * allocate samples buffer; if the channels are all the same, this could
     * be done in set_device().
     */
    lib.samples = malloc(SAMPLES_PER_BLOCK * sizeof(lib.samples));
    if (!lib.samples)
        return -ENOMEM;

    lib.bytes_per_sample = format->length / 8;
    lib.scale = format->scale;

    return 0;
}

char **get_sampling_frequencies(size_t *count)
{
    TRACE();
    const struct iio_attr *attr;
    char **list;
    int ret;

    if (!lib.channel)
        return NULL;

    free(lib.sampling_frequencies);
    lib.sampling_frequencies = NULL;

    attr = iio_channel_find_attr(lib.channel, "sampling_frequency_available");
    if (!attr)
        return NULL;

    ret = iio_attr_get_available(attr, &list, count);
    if (ret < 0)
        return NULL;

    return list;
}

int get_sampling_frequency(void)
{
    TRACE();
    const struct iio_attr *attr;
    long long val;
    int ret;

    if (!lib.channel)
        return -ENODEV;

    attr = iio_channel_find_attr(lib.channel, "sampling_frequency");
    if (!attr)
        return -EINVAL;

    /*
     * The driver uses int for the sampling frequency, but libiio doesn't have
     * an iio_attr_read_int() function, so this shall do.
     */
    ret = iio_attr_read_longlong(attr, &val);
    if (ret < 0)
        return ret;

    return (int)val;
}

int set_sampling_frequency(int freq)
{
    TRACE();
    const struct iio_attr *attr;
    long long val;
    int ret;

    if (!lib.channel)
        return -ENODEV;

    attr = iio_channel_find_attr(lib.channel, "sampling_frequency");
    if (!attr)
        return -EINVAL;

    val = freq;

    ret = iio_attr_write_longlong(attr, val);
    if (ret < 0)
        return ret;

    return 0;
}

unsigned short *get_block(size_t *count)
{
    TRACE();
    const struct iio_block *block;
    unsigned short *sample;
    char dst[8];
    char *src;

    if (!lib.channel)
        return NULL;

    block = iio_stream_get_next_block(lib.stream);
    src = iio_block_first(block, lib.channel);
    *count = ((char *)iio_block_end(block) - src) / lib.bytes_per_sample;

    for (size_t i = 0; i < *count; i++) {
        sample = &lib.samples[i];

        iio_channel_convert(lib.channel, dst, src);

        switch (lib.bytes_per_sample) {
        case 2:
            *sample = *(unsigned short *)dst;
            break;

            /* TODO: add other sample widths */
        }

        src += lib.bytes_per_sample;
        *sample *= lib.scale;
    }

    return lib.samples;
}
