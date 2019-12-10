/*
 * matrix-codec.c -- MATRIX microphone array audio driver
 *
 * Copyright 2018 MATRIX Labs
 *
 * Author: Andres Calderon <andres.calderon@admobilize.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "fir_coeff.h"
#include "matrixio-core.h"
#include "matrixio-pcm.h"

/* Single global pointer to driver state.  Should use one of the driver private
 * data fields, but the way this is an ASoC driver but not really an ASoC driver
 * makes this hard!  */
static struct matrixio_mic_substream *ms;

static const struct {
	unsigned rate;
	unsigned short decimation; /* sample rate = (PDM clock = 3 MHz) / (decimation+1) */
	unsigned short gain; /* in bits */
} matrixio_params[] = {
	{8000, 374, 1},	 {12000, 249, 2}, {16000, 186, 3},
	{22050, 135, 5}, {24000, 124, 5}, {32000, 92, 6},
	{44100, 67, 7},	 {48000, 61, 7},  {96000, 30, 10}
};

static struct snd_pcm_hardware matrixio_pcm_capture_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP,
	.formats = MATRIXIO_FORMATS,
	.rates = MATRIXIO_RATES,
	.rate_min = matrixio_params[0].rate,
	.rate_max = matrixio_params[ARRAY_SIZE(matrixio_params)-1].rate,
	.channels_min = 1,
	.channels_max = MATRIXIO_CHANNELS_MAX,
	.buffer_bytes_max = MATRIXIO_BUFFER_MAX,
	.period_bytes_min = MATRIXIO_PERIOD_BYTES_PER_CH * 1,
	.period_bytes_max = MATRIXIO_PERIOD_BYTES_PER_CH * MATRIXIO_CHANNELS_MAX,
	.periods_min = MATRIXIO_MIN_PERIODS,
	.periods_max = MATRIXIO_BUFFER_MAX / MATRIXIO_PERIOD_BYTES_PER_CH,
};

/* Threaded portion of interrupt handler */
static irqreturn_t matrixio_pcm_thread(int irq, void *irq_data)
{
	struct matrixio_mic_substream *ms = irq_data;
	struct snd_pcm_substream *substream = ms->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	unsigned long pos;
	uint16_t *buf;
	unsigned i, c;
	int ret;

	ret = matrixio_read(ms->mio, MATRIXIO_MICARRAY_BASE,
			    snd_pcm_lib_period_bytes(substream),
			    ms->frag_buffer);
	/* Clear SPI xfer in progress bit */
	smp_mb__before_atomic();
	clear_bit(1, &ms->flags);
	if (ret) {
		pcm_err(substream->pcm, "matrixio SPI read failed (%d)\n", ret);
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&ms->worker_lock, flags);
	/* Just return if we've stopped the audio process.  This device has no
	 * way to stop the interrupts.	*/
	if (!test_bit(0, &ms->flags)) {
		spin_unlock_irqrestore(&ms->worker_lock, flags);
		return IRQ_HANDLED;
	}
	if (!runtime->dma_area) {
		/* This should not happen */
		pcm_err(substream->pcm, "DMA buffer missing!");
		spin_unlock_irqrestore(&ms->worker_lock, flags);
		return IRQ_HANDLED;
	}
	pos = atomic_read(&ms->position);
	/* Interleave data from fragment into "dma" buffer */
	buf = (uint16_t*)(runtime->dma_area + frames_to_bytes(runtime, pos));
	for (i = 0; i < MATRIXIO_PERIOD_FRAMES; i++)
		for (c = 0; c < runtime->channels; c++)
			*buf++ = ms->frag_buffer[c * MATRIXIO_PERIOD_FRAMES + i];

	pos += MATRIXIO_PERIOD_FRAMES;
	if (pos >= runtime->buffer_size)
		pos -= runtime->buffer_size;
	atomic_set(&ms->position, pos);

	spin_unlock_irqrestore(&ms->worker_lock, flags);

	snd_pcm_period_elapsed(ms->substream);
	return IRQ_HANDLED;
}

static irqreturn_t matrixio_pcm_interrupt(int irq, void *irq_data)
{
	struct matrixio_mic_substream *ms = irq_data;

	if (ms->substream == NULL)
		return IRQ_NONE;

	/* Have we started receive? Device will generate interrupts constantly. */
	if (!test_bit(0, &ms->flags))
		return IRQ_HANDLED;

	if (test_and_set_bit(1, &ms->flags)) {
		/* Buffer was not yet empty */
		pcm_warn(ms->substream->pcm, "Possible overflow, irq thread not keeping up\n");
	}

	return IRQ_WAKE_THREAD;
}

static int matrixio_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &matrixio_pcm_capture_hw);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, MATRIXIO_PERIOD_FRAMES);
	snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, MATRIXIO_PERIOD_FRAMES);

	snd_pcm_set_sync(substream);

	if (ms->substream != NULL)
		return -EBUSY;

	ms->substream = substream;

	atomic_set(&ms->position, 0);

	/* Clear the running flag, so the irq handler will not do anything when it starts */
	clear_bit(0, &ms->flags);
	smp_mb__after_atomic();
	ret = request_threaded_irq(ms->irq, matrixio_pcm_interrupt, matrixio_pcm_thread, 0,
			           "matrixio-mic", ms);
	if (ret < 0)
		goto fail_substream;

	return 0;

 fail_substream:
	ms->substream = NULL;

	return ret;
}

static int matrixio_pcm_close(struct snd_pcm_substream *substream)
{
	clear_bit(0, &ms->flags); /* Should already be clear from trigger stop, but just in case */
	free_irq(ms->irq, ms);
	ms->substream = NULL;

	return 0;
}

static int matrixio_pcm_trigger(struct snd_pcm_substream *substream,
				int cmd)
{
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		set_bit(0, &ms->flags);
		smp_mb__after_atomic();
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		pcm_dbg(substream->pcm, "stopping");
		/* We need the lock here to insure the irq thread is not in
		 * the middle of processing audio data into the dma buffer */
		spin_lock_irqsave(&ms->worker_lock, flags);
		clear_bit(0, &ms->flags);
		spin_unlock_irqrestore(&ms->worker_lock, flags);
		pcm_dbg(substream->pcm, "stopped");
		return 0;
	default:
		return -EINVAL;
	}
}

static int matrixio_pcm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	int i;
	int rate;

	if (snd_pcm_format_width(params_format(hw_params)) != 16)
		return -EINVAL;

	rate = params_rate(hw_params);

	// This regmap write stuff should move to prepare instead of hwparams
	for (i = 0; i < ARRAY_SIZE(matrixio_params); i++) {
		if (rate == matrixio_params[i].rate) {
			regmap_write(ms->mio->regmap, MATRIXIO_CONF_BASE + 0x06,
				     matrixio_params[i].decimation);

			regmap_write(ms->mio->regmap, MATRIXIO_CONF_BASE + 0x07,
				     matrixio_params[i].gain);
			break;
		}
	}
	if (i == ARRAY_SIZE(matrixio_params))
		return -EINVAL;

	// should use ARRAY_SIZE rather than sentinal
	for (i = 0; FIR_Coeff[i].rate_; i++) {
		if (FIR_Coeff[i].rate_ == rate) {
			matrixio_write(ms->mio, MATRIXIO_MICARRAY_BASE,
				       MATRIXIO_FIR_TAP_SIZE,
				       &FIR_Coeff[i].coeff_[0]);
			break;
		}
	}
	if (!FIR_Coeff[i].rate_)
		return -EINVAL;

	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
						params_buffer_bytes(hw_params));
}

static int matrixio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	/* Capture should have been stopped already */
	snd_BUG_ON(test_bit(0, &ms->flags));
	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static int matrixio_pcm_prepare(struct snd_pcm_substream *substream)
{
	if (substream->runtime->period_size != MATRIXIO_PERIOD_FRAMES) {
		pcm_err(substream->pcm, "Need %u frames/period, got %lu\n", MATRIXIO_PERIOD_FRAMES, substream->runtime->period_size);
		return -EINVAL;
	}
	/* We don't need the lock since the irq thread can not be running when prepare is called */
	atomic_set(&ms->position, 0);
	return 0;
}

static snd_pcm_uframes_t
matrixio_pcm_pointer(struct snd_pcm_substream *substream)
{
	return atomic_read(&ms->position);
}

static const struct snd_pcm_ops matrixio_pcm_ops = {
	.open = matrixio_pcm_open,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = matrixio_pcm_hw_params,
	.hw_free = matrixio_pcm_hw_free,
	.prepare = matrixio_pcm_prepare,
	.pointer = matrixio_pcm_pointer,
	.close = matrixio_pcm_close,
	.trigger = matrixio_pcm_trigger,
	.page = snd_pcm_lib_get_vmalloc_page,
};

static int matrixio_pcm_new(struct snd_soc_pcm_runtime *rtd)
{ return 0; }

static const struct snd_soc_component_driver matrixio_soc_platform = {
	.ops = &matrixio_pcm_ops,
	.pcm_new = matrixio_pcm_new,
};

static int matrixio_pcm_platform_probe(struct platform_device *pdev)
{
	int ret;

	/* Yuck, assign to global variable, fix this! */
	ms = devm_kzalloc(&pdev->dev, sizeof(struct matrixio_mic_substream),
			  GFP_KERNEL);
	if (!ms) {
		dev_err(&pdev->dev, "Failed to allocate matrixio substream");
		return -ENOMEM;
	}
	ms->frag_buffer = devm_kmalloc(&pdev->dev, matrixio_pcm_capture_hw.period_bytes_max, GFP_KERNEL);
	if (!ms->frag_buffer) {
		dev_err(&pdev->dev, "Failed to allocate SPI fragment buffer (%zu bytes)", matrixio_pcm_capture_hw.period_bytes_max);
		return -ENOMEM;
	}

	ms->mio = dev_get_drvdata(pdev->dev.parent);
	ms->substream = NULL;
	spin_lock_init(&ms->worker_lock);

	ms->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &matrixio_soc_platform, NULL, 0);
	if (ret) {
		dev_err(&pdev->dev,
			"MATRIXIO sound SoC register platform error: %d", ret);
		return ret;
	}

	dev_set_drvdata(&pdev->dev, ms);

	dev_info(&pdev->dev, "MATRIXIO mic array audio driver loaded (IRQ=%u)", ms->irq);

	return 0;
}

static const struct of_device_id snd_matrixio_pcm_of_match[] = {
	{ .compatible = "matrixio-mic", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_matrixio_pcm_of_match);

static struct platform_driver matrixio_codec_driver = {
	.driver = {
		.name = "matrixio-mic",
		.owner = THIS_MODULE,
		.of_match_table = snd_matrixio_pcm_of_match
	},
	.probe = matrixio_pcm_platform_probe,
};

module_platform_driver(matrixio_codec_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andres Calderon <andres.calderon@admobilize.com>");
MODULE_DESCRIPTION("MATRIXIO MIC array PCM");
