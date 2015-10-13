/**
 * Copyright (C) 2012-2013 Analog Devices, Inc.
 *
 * Licensed under the GPL-2.
 *
 **/

#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include <iio.h>

#include <pulse/simple.h>

#define SAMPLES_COUNT 16384

/* Min and max are used for automatic gain control and DC offset control */
static int min = 0xfffffff;
static int max = -0xfffffff;

static const pa_sample_spec sample_spec = {
	.format = PA_SAMPLE_S16LE,
	.rate = 48000,
	.channels = 1,
};


static size_t bytes_used(const struct iio_buffer *buf)
{
	return iio_buffer_end(buf) - iio_buffer_start(buf);
}


static int demodulate(struct iio_buffer *buf, pa_simple *pa)
{
	int new_min, new_max;
	long i[3], q[3], di, dq;
	long long sample = 0;
	unsigned int j;
	unsigned int sub = 4;
	unsigned int x = 0;
	unsigned int n = 0;
	short *sample_buffer, *buffer = iio_buffer_start(buf);
	size_t num_bytes = bytes_used(buf);
	int ret;

	new_min = 0xfffffff;
	new_max = -0xfffffff;

	sample_buffer = malloc(num_bytes / 64);

	i[2] = buffer[0];
	q[2] = buffer[1];
	i[1] = buffer[2];
	q[1] = buffer[3];

	x = 0;
	for (j = 2; j < num_bytes / 2; j += 2 * sub) {

		/* FM demodulation implemented as described in
		 * http://www.embedded.com/design/embedded/4212086/DSP-Tricks--Frequency-demodulation-algorithms-
		 */
		i[0] = buffer[j];
		q[0] = buffer[j + 1];

		di = i[0] - i[2];
		dq = q[0] - q[2];

		sample += (i[1] * dq - q[1] * di);

		i[2] = i[1];
		q[2] = q[1];
		i[1] = i[0];
		q[1] = q[0];

		x += sub;
		if (x == 32) {
			x = 0;
			sample /= (32 / sub);

			if (sample < new_min)
				new_min = sample;
			if (sample > new_max)
				new_max = sample;

			if (min >= max)
				continue;

			sample -= (max - min) / 2;
			sample = sample * 0x1fff / (max - min);
			if (sample > 0x1fff)
				sample = 0x1fff;
			else if(sample < -0x1fff)
				sample = -0x1fff;

			sample_buffer[n] = sample;
			n++;
			sample = 0;
		}
	}

	min = new_min;
	max = new_max;

	if (n == 0)
		return 0;

	ret = pa_simple_write(pa, sample_buffer, 2 * n, NULL);
	free(sample_buffer);

	if (ret < 0) {
		perror("Failed to write samples to PulseAudio");
		return -1;
	}

	return 0;
}

static int app_running = 1;

static void terminate(int signal)
{
	app_running = 0;
}

static void setup_sigterm_handler(void)
{
	struct sigaction action = {
		.sa_handler = terminate,
	};

	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGPIPE, &action, NULL);
}

/**
 * Usage: `iio_fm_radio [frequency]`
 */
int main(int argc, char *argv[])
{
	struct iio_context *ctx;
	struct iio_device *dev, *phy;
	struct iio_channel *chn;
	struct iio_buffer *buf;
	pa_simple *pa;

	setup_sigterm_handler();

	ctx = iio_create_default_context();
	if (!ctx)
		return EXIT_FAILURE;

	dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
	phy = iio_context_find_device(ctx, "ad9361-phy");
	if (!dev || !phy) {
		fprintf(stderr, "Failed to find 'cf-ad9361-lpc' device\n");
		goto err_context_destroy;
	}

	/* Select I and Q data of the first channel */
	iio_channel_enable(iio_device_find_channel(dev, "voltage0", false));
	iio_channel_enable(iio_device_find_channel(dev, "voltage1", false));

	chn = iio_device_find_channel(dev, "voltage2", false);
	if (chn)
		iio_channel_disable(chn);

	chn = iio_device_find_channel(dev, "voltage3", false);
	if (chn)
		iio_channel_disable(chn);

	chn = iio_device_find_channel(phy, "voltage0", false);

	/* 32x oversampling for 48kHz audio */
	iio_channel_attr_write_longlong(chn, "sampling_frequency", 1536000);

	/* Set bandwidth to 300 kHz */
	iio_channel_attr_write_longlong(chn, "rf_baudwidth", 30000);

	if (argc > 1) {
		float freq;
		freq = atof(argv[1]);
		if (freq < 1000)
			freq *= 1000000;
		chn = iio_device_find_channel(phy, "altvoltage0", true);
		iio_channel_attr_write_longlong(chn,
				"frequency", (long long) freq);
	}

	buf = iio_device_create_buffer(dev, SAMPLES_COUNT, false);
	if (!buf) {
		perror("Unable to open device");
		goto err_context_destroy;
	}

	pa = pa_simple_new(NULL, "IIO FM Radio", PA_STREAM_PLAYBACK,
			NULL, "playback", &sample_spec, NULL, NULL, NULL);
	if (!pa) {
		fprintf(stderr, "Unable to connect to PulseAudio\n");
		goto err_buffer_destroy;
	}

	fprintf(stderr, "Starting FM modulation\n");

	while (app_running) {
		iio_buffer_refill(buf);
		if (demodulate(buf, pa))
			break;
	}

	fprintf(stderr, "Stopping FM modulation\n");

	pa_simple_free(pa);
	iio_buffer_destroy(buf);
	iio_context_destroy(ctx);
	return EXIT_SUCCESS;

err_buffer_destroy:
	iio_buffer_destroy(buf);
err_context_destroy:
	iio_context_destroy(ctx);
	return EXIT_FAILURE;
}
