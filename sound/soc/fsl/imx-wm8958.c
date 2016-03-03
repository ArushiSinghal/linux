/*
 * Copyright (C) 2015-2016 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/mfd/wm8994/registers.h>
#include "../fsl/fsl_sai.h"
#include "../codecs/wm8994.h"

#define DAI_NAME_SIZE	32

#define DAI_LINK_NUM (3)
#define HIFI_DAI (0)
#define VOICE_DAI (1)
#define BT_DAI (2)

#define WM8958_MCLK_MAX (2)

#define WM8994_FLL(id) (id == HIFI_DAI ? WM8994_FLL1 : WM8994_FLL2)
#define WM8994_SYSCLK_FLL(id) (id == HIFI_DAI ? WM8994_SYSCLK_FLL1 : WM8994_SYSCLK_FLL2)
#define WM8994_FLL_SRC_MCLK(id) (id == HIFI_DAI ? WM8994_FLL_SRC_MCLK1 : WM8994_FLL_SRC_MCLK2)

struct imx_wm8958_data {
	struct snd_soc_dai_link *dai_link;
	struct snd_soc_card card;
	struct clk *mclk[WM8958_MCLK_MAX];
	u32 mclk_freq[WM8958_MCLK_MAX];
	bool is_hifi_dai_master;
	bool is_stream_in_use[DAI_LINK_NUM][2];
};

static const struct snd_soc_dapm_widget imx_wm8958_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
};

static int imx_wm8958_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct device *dev = card->dev;
	struct imx_wm8958_data *data = snd_soc_card_get_drvdata(card);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	bool hifi_dai_sysclk_dir = SND_SOC_CLOCK_OUT;
	u32 mclk_id, id = codec_dai->id - 1;
	u32 pll_out;
	int ret;

	data->is_stream_in_use[id][tx] = true;

	if (data->mclk_freq[id])
		mclk_id = WM8994_FLL_SRC_MCLK(id);
	else if (id == HIFI_DAI)
		mclk_id = WM8994_FLL_SRC_MCLK2;
	else
		mclk_id = WM8994_FLL_SRC_MCLK1;

	if (id == HIFI_DAI) {
		if (!data->is_hifi_dai_master)
			hifi_dai_sysclk_dir = SND_SOC_CLOCK_IN;

		ret = snd_soc_dai_set_sysclk(cpu_dai,
				0, 0, !hifi_dai_sysclk_dir);

		if (ret) {
			dev_err(dev, "failed to set cpu sysclk: %d\n", ret);
			return ret;
		}

		if (!data->is_hifi_dai_master) {
			ret = snd_soc_dai_set_sysclk(codec_dai, mclk_id,
						data->mclk_freq[mclk_id - 1],
						hifi_dai_sysclk_dir);
			if (ret) {
				dev_err(dev,
					"failed to set codec sysclk: %d\n",
					ret);
				return ret;
			}

			return 0;
		}
	}

	if (params_width(params) == 24)
		pll_out = params_rate(params) * 384;
	else
		pll_out = params_rate(params) * 256;

	ret = snd_soc_dai_set_pll(codec_dai,
				  WM8994_FLL(id),
				  mclk_id,
				  data->mclk_freq[mclk_id - 1],
				  pll_out);
	if (ret) {
		dev_err(dev, "failed to set codec pll: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai,
				     WM8994_SYSCLK_FLL(id),
				     pll_out,
				     SND_SOC_CLOCK_OUT);
	if (ret) {
		dev_err(dev, "failed to set codec sysclk: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx_wm8958_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_card *card = rtd->card;
	struct imx_wm8958_data *data = snd_soc_card_get_drvdata(card);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int id = codec_dai->id - 1;

	data->is_stream_in_use[id][tx] = false;

	if (id == HIFI_DAI && !data->is_hifi_dai_master)
		return 0;

	if (!data->is_stream_in_use[id][!tx]) {
		/*
		 * We should connect AIFxCLK source to FLL after enable FLL,
		 * and disconnect AIF1CLK source to FLL before disable FLL,
		 * otherwise FLL worked abnormal.
		 */
		snd_soc_dai_set_sysclk(codec_dai, WM8994_FLL_SRC_MCLK(id),
				data->mclk_freq[id], SND_SOC_CLOCK_OUT);

		/* Disable FLL1 after all stream finished. */
		snd_soc_dai_set_pll(codec_dai, WM8994_FLL(id), 0, 0, 0);
	}

	return 0;
}

static struct snd_soc_ops imx_hifi_ops = {
	.hw_params = imx_wm8958_hw_params,
	.hw_free   = imx_wm8958_hw_free,
};

static struct snd_soc_ops imx_voice_ops = {
	.hw_params = imx_wm8958_hw_params,
	.hw_free   = imx_wm8958_hw_free,
};

static struct snd_soc_dai_link imx_wm8958_dai_link[] = {
	[HIFI_DAI] =	{
			.name = "HiFi",
			.stream_name = "HiFi",
			.codec_name = "wm8994-codec",
			.codec_dai_name = "wm8994-aif1",
			.ops = &imx_hifi_ops,
			.dai_fmt = SND_SOC_DAIFMT_I2S |
				   SND_SOC_DAIFMT_NB_NF,
	},
	[VOICE_DAI] =   {
			.name = "Voice",
			.stream_name = "Voice",
			.cpu_dai_name = "snd-soc-dummy-dai",
			.codec_name = "wm8994-codec",
			.codec_dai_name = "wm8994-aif2",
			.platform_name = "snd-soc-dummy",
			.ignore_pmdown_time = 1,
			.ops = &imx_voice_ops,
			.dai_fmt = SND_SOC_DAIFMT_I2S |
				   SND_SOC_DAIFMT_NB_NF |
				   SND_SOC_DAIFMT_CBM_CFM,
	},
	[BT_DAI] =	{
			.name = "Bluetooth",
			.stream_name = "Bluetooth",
			.cpu_dai_name = "snd-soc-dummy-dai",
			.codec_name = "wm8994-codec",
			.codec_dai_name = "wm8994-aif3",
			.platform_name = "snd-soc-dummy",
			.ignore_pmdown_time = 1,
	},
};

static int imx_wm8958_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np, *codec_np;
	struct device_node *np = pdev->dev.of_node;
	struct platform_device *cpu_pdev;
	struct i2c_client *codec_dev;
	struct imx_wm8958_data *data;
	char tmp[8];
	int ret, i;

	cpu_np = of_parse_phandle(np, "cpu-dai", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		return -EINVAL;
	}

	codec_np = of_parse_phandle(np, "audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_err(&pdev->dev, "failed to find cpu dai platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev || !codec_dev->dev.driver) {
		dev_err(&pdev->dev, "failed to find codec platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dai_link = imx_wm8958_dai_link;

	/*
	 * AIF1 support codec master and slave mode
	 * AIF2 and AIF3 just support codec master mode
	 */
	if (of_property_read_bool(pdev->dev.of_node, "fsl,hifi-dai-master")) {
		data->is_hifi_dai_master = true;
		data->dai_link[HIFI_DAI].dai_fmt |= SND_SOC_DAIFMT_CBM_CFM;
	} else {
		data->is_hifi_dai_master = false;
		data->dai_link[HIFI_DAI].dai_fmt |= SND_SOC_DAIFMT_CBS_CFS;
	}

	for (i = 0; i < WM8958_MCLK_MAX; i++) {
		sprintf(tmp, "MCLK%d", i + 1);
		data->mclk[i] = devm_clk_get(&codec_dev->dev, tmp);
		if (!IS_ERR(data->mclk[i]))
			data->mclk_freq[i] = clk_get_rate(data->mclk[i]);
	}

	if (!data->mclk_freq[0] && !data->mclk_freq[1]) {
		dev_err(&pdev->dev, "failed to get mclk clock\n");
		ret = -EINVAL;
		goto fail;
	}

	data->dai_link[HIFI_DAI].cpu_dai_name = dev_name(&cpu_pdev->dev);
	data->dai_link[HIFI_DAI].platform_of_node = cpu_np;

	data->card.dev = &pdev->dev;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret)
		goto fail;

	data->card.num_links = DAI_LINK_NUM;
	data->card.dai_link = data->dai_link;
	data->card.dapm_widgets = imx_wm8958_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_wm8958_dapm_widgets);
	data->card.owner = THIS_MODULE;

	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret)
		goto fail;

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto fail;
	}

fail:
	of_node_put(cpu_np);
	of_node_put(codec_np);

	return ret;
}

static const struct of_device_id imx_wm8958_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-wm8958", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_wm8958_dt_ids);

static struct platform_driver imx_wm8958_driver = {
	.driver = {
		.name = "imx-wm8958",
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_wm8958_dt_ids,
	},
	.probe = imx_wm8958_probe,
};
module_platform_driver(imx_wm8958_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Freescale i.MX WM8958 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-wm8958");
