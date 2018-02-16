/*
 * matrix-codec.c -- MATRIX microphone array audio driver
 *
 * Copyright 2017 MATRIX Labs
 *
 * Author: Andres Calderon <andres.calderon@admobilize.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include "matrixio-pcm.h"
#include "matrixio-core.h"

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define MATRIXIO_CHANNELS_MAX 8
#define MATRIXIO_RATES SNDRV_PCM_RATE_8000_48000
#define MATRIXIO_FORMATS SNDRV_PCM_FMTBIT_S16_LE
#define MATRIXIO_MICARRAY_BASE 0x2000
#define MATRIXIO_MICARRAY_BUFFER_SIZE (256 * 1 /*MATRIXIO_CHANNELS_MAX*/ * 2)
#define MATRIXIO_FIFO_SIZE (MATRIXIO_MICARRAY_BUFFER_SIZE * 32)

static snd_pcm_uframes_t position = 0;
static struct matrixio_substream *ms;

static uint16_t matrixio_buf[MATRIXIO_CHANNELS_MAX][8192];

static struct snd_pcm_hardware matrixio_pcm_capture_hw = {
    .info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
	    SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_PAUSE,

    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_8000_48000,
    .rate_min = 8000,
    .rate_max = 48000,
    .channels_min = 1,
    .channels_max = 8,
    .buffer_bytes_max = 32768,
    .period_bytes_min = 4096,
    .period_bytes_max = 32768,
    .periods_min = 4,
    .periods_max = 8,

};

static void matrixio_pcm_capture_work(struct work_struct *wk)
{
	int ret;
	uint8_t raw[MATRIXIO_MICARRAY_BUFFER_SIZE];
	struct matrixio_substream *ms;

	ms = container_of(wk, struct matrixio_substream, work);

	mutex_lock(&ms->lock);

	ret = matrixio_read(ms->mio, MATRIXIO_MICARRAY_BASE,
			    MATRIXIO_MICARRAY_BUFFER_SIZE,
			    &matrixio_buf[0][position]);

	//	kfifo_in(&ms->capture_fifo, raw, MATRIXIO_MICARRAY_BUFFER_SIZE);

	position += 256;
	mutex_unlock(&ms->lock);

	snd_pcm_period_elapsed(ms->capture_substream);
}

static irqreturn_t matrixio_pcm_interrupt(int irq, void *irq_data)
{
	static int initialized = 0;
	struct matrixio_substream *ms = irq_data;

	if (ms->capture_substream == 0)
		return IRQ_NONE;

	if (initialized == 0) {
		INIT_WORK(&ms->work, matrixio_pcm_capture_work);
		initialized = 1;
	}

	queue_work(ms->wq, &ms->work);

	return IRQ_HANDLED;
}

static int matrixio_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	snd_soc_set_runtime_hwparams(substream, &matrixio_pcm_capture_hw);

	//	snd_pcm_set_sync(substream);

	if (ms->capture_substream != NULL) {
		return -EBUSY;
	}
	ms->capture_substream = substream;

	kfifo_reset(&ms->capture_fifo);

	position = 0;

	flush_workqueue(ms->wq);

	return 0;
}

static int matrixio_pcm_close(struct snd_pcm_substream *substream)
{
	ms->capture_substream = 0;
	return 0;
}

static int matrixio_pcm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	printk(KERN_INFO "-------------pcm hw params");
	return 0;
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int matrixio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	printk(KERN_INFO "-------------pcm hw free");
	return snd_pcm_lib_free_pages(substream);
}

static int matrixio_pcm_prepare(struct snd_pcm_substream *substream)
{
	printk(KERN_INFO "-------------pcm prepare");
	return 0;
}

static int matrixio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct matrixio_substream *ms = runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static snd_pcm_uframes_t
matrixio_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	printk(KERN_INFO " - pcm pointer %d", position);
	// kfifo_len(&ms->capture_fifo)/2);

	position = position < runtime->buffer_size ? position : 0;
	return position;
	// return kfifo_len(&ms->capture_fifo) / 2;
}

static int matrixio_pcm_copy(struct snd_pcm_substream *substream, int channel,
			     snd_pcm_uframes_t pos, void __user *buf,
			     snd_pcm_uframes_t count)
{
	int ret, len;
	unsigned int copied;
	printk(KERN_INFO "-----------------pcm copy channel=%d pos=%d count=%d",
	       channel, pos, count);

	return copy_to_user(buf, &matrixio_buf[0][pos], count * 2);
}

static struct snd_pcm_ops matrixio_pcm_ops = {
    .open = matrixio_pcm_open,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = matrixio_pcm_hw_params,
    .hw_free = matrixio_pcm_hw_free,
    .prepare = matrixio_pcm_prepare,
    .trigger = matrixio_pcm_trigger,
    .pointer = matrixio_pcm_pointer,
    .mmap = snd_pcm_lib_mmap_iomem,
    .copy = matrixio_pcm_copy,
    .close = matrixio_pcm_close,
};

static int matrixio_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	/*
	struct snd_pcm *pcm = rtd->pcm;

	struct platform_device *pdev = to_platform_device(rtd->platform->dev);

	struct matrixio_substream *ms = dev_get_drvdata(&pdev->dev);

	dev_set_drvdata(rtd->dev, ms);
	// snd_soc_card_set_drvdata(rtd->card, ms);
*/
	return 0;
}

static const struct snd_soc_platform_driver matrixio_soc_platform = {
    .ops = &matrixio_pcm_ops, .pcm_new = matrixio_pcm_new,
};

static int matrixio_pcm_platform_probe(struct platform_device *pdev)
{

	int ret;
	char workqueue_name[12];

	ms = devm_kzalloc(&pdev->dev, sizeof(struct matrixio_substream),
			  GFP_KERNEL);
	if (!ms) {
		dev_err(&pdev->dev, "data allocation");
		return -ENOMEM;
	}

	ms->mio = dev_get_drvdata(pdev->dev.parent);

	ms->capture_substream = 0;

	mutex_init(&ms->lock);
	ms->stamp = 1010101;

	sprintf(workqueue_name, "matrixio_pcm");
	ms->wq = create_singlethread_workqueue(workqueue_name);
	if (!ms->wq) {
		dev_err(&pdev->dev, "cannot create workqueue");
		return -ENOMEM;
	}

	ret = kfifo_alloc(&ms->capture_fifo, MATRIXIO_FIFO_SIZE, GFP_KERNEL);
	if (ret) {
		dev_err(&pdev->dev, "error PCM kfifo allocation");
		return -ENOMEM;
	}

	ms->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);

	ret = devm_request_irq(&pdev->dev, ms->irq, matrixio_pcm_interrupt, 0,
			       dev_name(&pdev->dev), ms);
	if (ret) {
		dev_err(&pdev->dev, "can't request irq %d\n", ms->irq);
		destroy_workqueue(ms->wq);
		return -EBUSY;
	}

	ret =
	    devm_snd_soc_register_platform(&pdev->dev, &matrixio_soc_platform);
	if (ret) {
		dev_err(&pdev->dev,
			"MATRIXIO sound SoC register platform error: %d", ret);
		return ret;
	}

	dev_set_drvdata(&pdev->dev, ms);

	dev_notice(&pdev->dev, "MATRIXIO audio drive loaded (IRQ=%d)", ms->irq);
	return 0;
}

static const struct of_device_id snd_matrixio_pcm_of_match[] = {
    {
	.compatible = "matrixio-pcm",
    },
    {},
};
MODULE_DEVICE_TABLE(of, snd_matrixio_pcm_of_match);

static struct platform_driver matrixio_codec_driver = {
    .driver = {.name = "matrixio-pcm",
	       .owner = THIS_MODULE,
	       .of_match_table = snd_matrixio_pcm_of_match},
    .probe = matrixio_pcm_platform_probe,
};

module_platform_driver(matrixio_codec_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andres Calderon <andres.calderon@admobilize.com>");
MODULE_DESCRIPTION("MATRIXIO PCM module");
