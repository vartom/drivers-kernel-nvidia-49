/*
 * tegra_machine_driver_mobile.c - Tegra ASoC Machine driver for mobile
 *
 * Copyright (c) 2017-2018 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/platform_data/tegra_asoc_pdata.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <dt-bindings/sound/tas2552.h>
#include "rt5677.h"
#include "tegra_asoc_utils_alt.h"
#include "tegra_asoc_machine_alt.h"
#include "tegra210_xbar_alt.h"
#include "../codecs/rt5677.h"
#include "../codecs/nau8825.h"

#define DRV_NAME "tegra-asoc:"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)
#define GPIO_INT_MIC_EN BIT(2)
#define GPIO_EXT_MIC_EN BIT(3)
#define GPIO_HP_DET     BIT(4)
#define RT5677_MCLK	19200000
#define RT5677_SYSCLK	24576000

#define PARAMS(sformat, channels)		\
	{					\
		.formats = sformat,		\
		.rate_min = 48000,		\
		.rate_max = 48000,		\
		.channels_min = channels,	\
		.channels_max = channels,	\
	}

/* machine structure which holds sound card *
struct tegra_machine {
	struct tegra_asoc_platform_data *pdata;
	struct tegra_asoc_audio_clock_info audio_clock;
	unsigned int num_codec_links;
	int gpio_requested;
	struct snd_soc_card *pcard;
	int rate_via_kcontrol;
	bool is_hs_supported;
	int fmt_via_kcontrol;
	unsigned int bclk_ratio_override;
	struct tegra_machine_soc_data *soc_data;
	struct regulator *digital_reg;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
};*/

/* rt565x specific APIs */
static int tegra_rt5677_event_int_spk(struct snd_soc_dapm_widget *,
		struct snd_kcontrol *, int);
static int tegra_rt5677_event_int_mic(struct snd_soc_dapm_widget *,
		struct snd_kcontrol *, int);
static int tegra_rt5677_event_ext_mic(struct snd_soc_dapm_widget *,
		struct snd_kcontrol *, int);
static int tegra_rt5677_event_hp(struct snd_soc_dapm_widget *,
		struct snd_kcontrol *, int);

/* t210 soc data */
static const struct tegra_machine_soc_data soc_data_tegra210 = {

	.admaif_dai_link_start		= TEGRA210_DAI_LINK_ADMAIF1,
	.admaif_dai_link_end		= TEGRA210_DAI_LINK_ADMAIF10,
#if IS_ENABLED(CONFIG_SND_SOC_TEGRA210_ADSP_ALT)
	.adsp_pcm_dai_link_start	= TEGRA210_DAI_LINK_ADSP_PCM1,
	.adsp_pcm_dai_link_end		= TEGRA210_DAI_LINK_ADSP_PCM2,
	.adsp_compr_dai_link_start	= TEGRA210_DAI_LINK_ADSP_COMPR1,
	.adsp_compr_dai_link_end	= TEGRA210_DAI_LINK_ADSP_COMPR2,
#endif
	.sfc_dai_link			= TEGRA210_DAI_LINK_SFC1_RX,

	.write_idle_bias_off_state	= false,

	.ahub_links			= tegra210_xbar_dai_links,
	.num_ahub_links			= TEGRA210_XBAR_DAI_LINKS,
	.ahub_confs			= tegra210_xbar_codec_conf,
	.num_ahub_confs			= TEGRA210_XBAR_CODEC_CONF,
};

/* t186 soc data */
static const struct tegra_machine_soc_data soc_data_tegra186 = {

	.admaif_dai_link_start		= TEGRA186_DAI_LINK_ADMAIF1,
	.admaif_dai_link_end		= TEGRA186_DAI_LINK_ADMAIF10,
#if IS_ENABLED(CONFIG_SND_SOC_TEGRA210_ADSP_ALT)
	.adsp_pcm_dai_link_start	= TEGRA186_DAI_LINK_ADSP_PCM1,
	.adsp_pcm_dai_link_end		= TEGRA186_DAI_LINK_ADSP_PCM2,
	.adsp_compr_dai_link_start	= TEGRA186_DAI_LINK_ADSP_COMPR1,
	.adsp_compr_dai_link_end	= TEGRA186_DAI_LINK_ADSP_COMPR2,
#endif
	.sfc_dai_link			= TEGRA186_DAI_LINK_SFC1_RX,

	.write_idle_bias_off_state	= true,

	.ahub_links			= tegra186_xbar_dai_links,
	.num_ahub_links			= TEGRA186_XBAR_DAI_LINKS,
	.ahub_confs			= tegra186_xbar_codec_conf,
	.num_ahub_confs			= TEGRA186_XBAR_CODEC_CONF,
};

static const char * const tegra_machine_srate_text[] = {
	"None",
	"8kHz",
	"16kHz",
	"44kHz",
	"48kHz",
	"11kHz",
	"22kHz",
	"24kHz",
	"32kHz",
	"88kHz",
	"96kHz",
	"176kHz",
	"192kHz",
};

static const char * const tegra_machine_format_text[] = {
	"None",
	"16",
	"32",
};

static const struct soc_enum tegra_machine_codec_rate =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tegra_machine_srate_text),
			    tegra_machine_srate_text);

static const struct soc_enum tegra_machine_codec_format =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tegra_machine_format_text),
			    tegra_machine_format_text);

static const int tegra_machine_srate_values[] = {
	0,
	8000,
	16000,
	44100,
	48000,
	11025,
	22050,
	24000,
	32000,
	88200,
	96000,
	176400,
	192000,
};

static int tegra_rt5677_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	if (!(machine->gpio_requested & GPIO_SPKR_EN))
		return 0;
	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				!!SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}
static int tegra_rt5677_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;
	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}
static int tegra_rt5677_event_int_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
		return 0;
	gpio_set_value_cansleep(pdata->gpio_int_mic_en,
				!!SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}
static int tegra_rt5677_event_ext_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	if (!(machine->gpio_requested & GPIO_EXT_MIC_EN))
		return 0;
	gpio_set_value_cansleep(pdata->gpio_ext_mic_en,
				!SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}
static const struct snd_soc_dapm_widget tegra_machine_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_rt5677_event_int_spk),
//	SND_SOC_DAPM_SPK("z Int Spk", tegra_rt5677_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_rt5677_event_hp),
	SND_SOC_DAPM_MIC("Int Mic", tegra_rt5677_event_int_mic),
	SND_SOC_DAPM_MIC("Mic Jack", tegra_rt5677_event_ext_mic),

/*	SND_SOC_DAPM_SPK("d1 Headphone", NULL),
	SND_SOC_DAPM_SPK("d2 Headphone", NULL),
	SND_SOC_DAPM_SPK("d3 Headphone", NULL),

	SND_SOC_DAPM_HP("w Headphone", NULL),
	SND_SOC_DAPM_HP("x Headphone", NULL),
	SND_SOC_DAPM_HP("y Headphone", NULL),
	SND_SOC_DAPM_HP("z Headphone", NULL),
	SND_SOC_DAPM_HP("l Headphone", NULL),
	SND_SOC_DAPM_HP("m Headphone", NULL),
	SND_SOC_DAPM_HP("n Headphone", NULL),
	SND_SOC_DAPM_HP("o Headphone", NULL),
	SND_SOC_DAPM_HP("s Headphone", NULL),
*/
/*	SND_SOC_DAPM_MIC("y Mic", NULL),
	SND_SOC_DAPM_SPK("z OUT", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_MIC("w Mic", NULL),
	SND_SOC_DAPM_MIC("x Mic", NULL),
	SND_SOC_DAPM_MIC("y Mic", NULL),
	SND_SOC_DAPM_MIC("z Mic", NULL),
	SND_SOC_DAPM_MIC("l Mic", NULL),
	SND_SOC_DAPM_MIC("m Mic", NULL),
	SND_SOC_DAPM_MIC("n Mic", NULL),
	SND_SOC_DAPM_MIC("o Mic", NULL),
	SND_SOC_DAPM_MIC("a Mic", NULL),
	SND_SOC_DAPM_MIC("b Mic", NULL),
	SND_SOC_DAPM_MIC("c Mic", NULL),
	SND_SOC_DAPM_MIC("d Mic", NULL),
	SND_SOC_DAPM_MIC("s Mic", NULL),*/
};

static struct snd_soc_pcm_stream tegra_machine_asrc_link_params[] = {
	PARAMS(SNDRV_PCM_FMTBIT_S32_LE, 8),
	PARAMS(SNDRV_PCM_FMTBIT_S16_LE, 2),
	PARAMS(SNDRV_PCM_FMTBIT_S16_LE, 2),
	PARAMS(SNDRV_PCM_FMTBIT_S16_LE, 2),
	PARAMS(SNDRV_PCM_FMTBIT_S16_LE, 2),
	PARAMS(SNDRV_PCM_FMTBIT_S16_LE, 2),
};

static int tegra_machine_codec_get_rate(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = machine->rate_via_kcontrol;

	return 0;
}

static int tegra_machine_codec_put_rate(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	/* set the rate control flag */
	machine->rate_via_kcontrol = ucontrol->value.integer.value[0];

	return 0;
}

static int tegra_machine_codec_get_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = machine->fmt_via_kcontrol;

	return 0;
}

static int tegra_machine_codec_put_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	/* set the format control flag */
	machine->fmt_via_kcontrol = ucontrol->value.integer.value[0];

	return 0;
}

static int tegra_machine_set_params(struct snd_soc_card *card,
				    struct tegra_machine *machine,
				    unsigned int rate,
				    unsigned int channels,
				    u64 formats)
{
	unsigned int mask = (1 << channels) - 1;
	struct snd_soc_pcm_runtime *rtd;
	int idx = 0, err = 0;
	u64 format_k;

	int num_of_dai_links = machine->soc_data->num_ahub_links +
			       machine->num_codec_links;


	format_k = (machine->fmt_via_kcontrol == 2) ?
			(1ULL << SNDRV_PCM_FORMAT_S32_LE) : formats;

	/* update dai link hw_params */
	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (rtd->dai_link->params) {
			struct snd_soc_pcm_stream *dai_params;

			dai_params =
			  (struct snd_soc_pcm_stream *)
			  rtd->dai_link->params;

			dai_params->rate_min = rate;
			dai_params->channels_min = channels;
			dai_params->formats = format_k;

			if ((idx >= machine->soc_data->num_ahub_links)
				&& (idx < num_of_dai_links)) {
				unsigned int fmt;

				/* TODO: why below overrite is needed */
				dai_params->formats = formats;

				fmt = rtd->dai_link->dai_fmt;
				fmt &= SND_SOC_DAIFMT_FORMAT_MASK;

				/* set TDM slot mask */
				if (fmt == SND_SOC_DAIFMT_DSP_A ||
				    fmt == SND_SOC_DAIFMT_DSP_B) {
					err = snd_soc_dai_set_tdm_slot(
							rtd->cpu_dai, mask,
							mask, 0, 0);
					if (err < 0) {
						dev_err(card->dev,
						"%s cpu DAI slot mask not set\n",
						rtd->cpu_dai->name);
						return err;
					}
				}
			}
		}
		idx++;
	}
	return 0;
}

static int tegra_machine_dai_init(struct snd_soc_pcm_runtime *runtime,
				  unsigned int rate, unsigned int channels,
				  u64 formats)
{
	struct snd_soc_card *card = runtime->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_stream *dai_params;
	unsigned int aud_mclk, srate;
	int err;

	struct snd_soc_pcm_runtime *rtd;

	srate = (machine->rate_via_kcontrol) ?
			tegra_machine_srate_values[machine->rate_via_kcontrol] :
			rate;

	err = tegra_alt_asoc_utils_set_rate(&machine->audio_clock, srate, 0, 0);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	aud_mclk = machine->audio_clock.set_aud_mclk_rate;

	pr_info("pll_a_out0 = %u Hz, aud_mclk = %u Hz, sample rate = %u Hz\n",
		 machine->audio_clock.set_pll_out_rate, aud_mclk, srate);


	err = tegra_machine_set_params(card, machine, rate, channels, formats);
	if (err < 0)
		return err;

	rtd = snd_soc_get_pcm_runtime(card, "rt5677-playback");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
		dai_params->formats = (machine->fmt_via_kcontrol == 2) ?
			(1ULL << SNDRV_PCM_FORMAT_S32_LE) : formats;


/*		err = snd_soc_dai_set_pll(rtd->codec_dai, 0,
				RT5677_PLL1_S_MCLK, RT5677_MCLK, RT5677_SYSCLK);
		if (err < 0) {
			dev_err(card->dev, "codec_dai pll not set\n");
			return err;
		}
*/
		err = snd_soc_dai_set_sysclk(rtd->codec_dai,
		RT5677_SCLK_S_MCLK, aud_mclk, SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
/*		rt5677_sel_asrc_clk_src(rtd->codec_dai->codec,
			RT5677_DA_STEREO_FILTER | RT5677_AD_STEREO1_FILTER |
			RT5677_AD_STEREO2_FILTER | RT5677_I2S1_SOURCE,
			RT5677_CLK_SEL_I2S1_ASRC);
		rt5677_sel_asrc_clk_src(rtd->codec_dai->codec,
			RT5677_AD_STEREO3_FILTER | RT5677_DA_MONO3_L_FILTER |
			RT5677_DA_MONO3_R_FILTER,
			RT5677_CLK_SEL_SYS);
*/
		/* update link_param to update hw_param for DAPM 
		dai_params->rate_min = srate;
		dai_params->channels_min = channels;
		dai_params->formats = formats;*/
	}

/*	rtd = snd_soc_get_pcm_runtime(card, "spdif-dit-0");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
	}

	rtd = snd_soc_get_pcm_runtime(card, "spdif-dit-1");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
	}

	* set clk rate for i2s3 dai link*
	rtd = snd_soc_get_pcm_runtime(card, "spdif-dit-2");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
	}

	rtd = snd_soc_get_pcm_runtime(card, "spdif-dit-3");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
	}

	rtd = snd_soc_get_pcm_runtime(card, "spdif-dit-5");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		* update link_param to update hw_param for DAPM *
		dai_params->rate_min = srate;
		dai_params->channels_min = channels;
		dai_params->formats = formats;
	}
*/
	rtd = snd_soc_get_pcm_runtime(card, "nau8825");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		dai_params->rate_min = srate;
		dai_params->channels_min = channels;
		dai_params->formats = formats;
		dev_info(card->dev, "snd_soc_get_pcm_runtime nau8825 set\n");
	}

	rtd = snd_soc_get_pcm_runtime(card, "max98357a");
	if (rtd) {
		dai_params =
		(struct snd_soc_pcm_stream *)rtd->dai_link->params;

		/* update link_param to update hw_param for DAPM */
		dai_params->rate_min = srate;
		dai_params->channels_min = channels;
		dai_params->formats = formats;
		dev_info(card->dev, "snd_soc_get_pcm_runtime max98357a set\n");
	}

	return 0;
}

static int tegra_machine_pcm_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	int err;

	err = tegra_machine_dai_init(rtd, params_rate(params),
				     params_channels(params),
				     1ULL << params_format(params));
	if (err < 0) {
		dev_err(card->dev, "Failed dai init\n");
		return err;
	}

	return 0;
}

static int tegra_machine_pcm_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_alt_asoc_utils_clk_enable(&machine->audio_clock);

	return 0;
}

static void tegra_machine_pcm_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_alt_asoc_utils_clk_disable(&machine->audio_clock);
}

static int tegra_machine_suspend_pre(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;

	/* DAPM dai link stream work for non pcm links */
	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (rtd->dai_link->params)
			INIT_DELAYED_WORK(&rtd->delayed_work, NULL);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_SND_SOC_TEGRA210_ADSP_ALT)
static int tegra_machine_compr_startup(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	tegra_alt_asoc_utils_clk_enable(&machine->audio_clock);

	return 0;
}

static void tegra_machine_compr_shutdown(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);

	tegra_alt_asoc_utils_clk_disable(&machine->audio_clock);
}

static int tegra_machine_compr_set_params(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_codec codec_params;
	int err;

	if (platform->driver->compr_ops &&
		platform->driver->compr_ops->get_params) {
		err = platform->driver->compr_ops->get_params(cstream,
			&codec_params);
		if (err < 0) {
			dev_err(card->dev, "Failed to get compr params\n");
			return err;
		}
	} else {
		dev_err(card->dev, "compr ops not set\n");
		return -EINVAL;
	}

	err = tegra_machine_dai_init(rtd, codec_params.sample_rate,
				     codec_params.ch_out,
				     SNDRV_PCM_FMTBIT_S16_LE);
	if (err < 0) {
		dev_err(card->dev, "Failed dai init\n");
		return err;
	}

	return 0;
}
#endif

static int tegra_machine_nau8825_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_codec *codec = rtd->codec;
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	struct snd_soc_jack *jack;
	int err;

	dev_info(card->dev, "This is tegra_machine_nau8825_init \n");

	snd_soc_codec_set_sysclk(codec, NAU8825_CLK_MCLK, 0, 0, SND_SOC_CLOCK_IN);

/*	jack = devm_kzalloc(card->dev, sizeof(struct snd_soc_jack), GFP_KERNEL);
	if (!jack)
		return -ENOMEM;
*/
	/* Enable Headset and 4 Buttons Jack detection */
	err = snd_soc_card_jack_new(card, "Headset Jack",
	                            SND_JACK_HEADPHONE | SND_JACK_MICROPHONE |
	                            SND_JACK_BTN_0 | SND_JACK_BTN_1 |
	                            SND_JACK_BTN_2 | SND_JACK_BTN_3,
	                            &machine->jack, NULL, 0);
	if (err) {
		dev_err(card->dev, "New Headset Jack failed! (%d)\n", err);
		return err;
	}

	jack = &machine->jack;

/*	err = tegra_machine_add_codec_jack_control(card, rtd, jack);
	if (err) {
		dev_err(card->dev, "Failed to add jack control: %d\n", err);
		return err;
	}
*/

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_MEDIA);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
	err = nau8825_enable_jack_detect(codec, jack);
	if (err) {
		dev_err(card->dev, "Failed to set jack for RT5677: %d\n", err);
		return err;
	}

//	snd_soc_dapm_sync(&card->dapm);

	return 0;
}

static int tegra_machine_rt5677_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
//	struct snd_soc_jack *jack;
//	int err;

	dev_info(card->dev, "This is tegra_machine_rt5677_init 1\n");

//	dev_info(card->dev, "This is tegra_machine_rt5677_init\n");

/*	jack = devm_kzalloc(card->dev, sizeof(struct snd_soc_jack), GFP_KERNEL);
	if (!jack)
		return -ENOMEM;

	err = snd_soc_card_jack_new(card, "Headset Jack", SND_JACK_HEADSET,
				    jack, NULL, 0);
	if (err) {
		dev_err(card->dev, "Headset Jack creation failed %d\n", err);
		return err;
	}

	err = tegra_machine_add_codec_jack_control(card, rtd, jack);
	if (err) {
		dev_err(card->dev, "Failed to add jack control: %d\n", err);
		return err;
	}

	err = nau8825_enable_jack_detect(rtd->codec, jack);
	if (err) {
		dev_err(card->dev, "Failed to set jack for RT5677: %d\n", err);
		return err;
	}
*/
	/* single button supporting play/pause */
//	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_MEDIA);

	/* multiple buttons supporting play/pause and volume up/down */
/*	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_MEDIA);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);

	snd_soc_dapm_sync(&card->dapm);
*/
	return 0;
}

static int codec_init(struct tegra_machine *machine)
{
	struct snd_soc_dai_link *dai_links = machine->asoc->dai_links;
	unsigned int num_links = machine->asoc->num_links, i;

	if (!dai_links || !num_links)
		return -EINVAL;

	for (i = 0; i < num_links; i++) {
		if (!dai_links[i].name)
			continue;

		if (strstr(dai_links[i].name, "rt5677-playback") ||
		    strstr(dai_links[i].name, "rt5677-codec-sysclk-bclk1"))
			dai_links[i].init = tegra_machine_rt5677_init;
		if (strstr(dai_links[i].name, "nau8825"))
			dai_links[i].init = tegra_machine_nau8825_init;
	}

	return 0;
}

static struct snd_soc_ops tegra_machine_pcm_ops = {
	.hw_params	= tegra_machine_pcm_hw_params,
	.startup	= tegra_machine_pcm_startup,
	.shutdown	= tegra_machine_pcm_shutdown,
};

#if IS_ENABLED(CONFIG_SND_SOC_TEGRA210_ADSP_ALT)
static struct snd_soc_compr_ops tegra_machine_compr_ops = {
	.set_params	= tegra_machine_compr_set_params,
	.startup	= tegra_machine_compr_startup,
	.shutdown	= tegra_machine_compr_shutdown,
};
#endif

static void set_dai_ops(struct tegra_machine *machine)
{
	int i;

	/* set ADMAIF dai_ops */
	for (i = machine->soc_data->admaif_dai_link_start;
	     i <= machine->soc_data->admaif_dai_link_end; i++)
		machine->asoc->dai_links[i].ops = &tegra_machine_pcm_ops;
#if IS_ENABLED(CONFIG_SND_SOC_TEGRA210_ADSP_ALT)
	/* set ADSP PCM/COMPR */
	for (i = machine->soc_data->adsp_pcm_dai_link_start;
	     i <= machine->soc_data->adsp_pcm_dai_link_end; i++)
		machine->asoc->dai_links[i].ops = &tegra_machine_pcm_ops;
	/* set ADSP COMPR */
	for (i = machine->soc_data->adsp_compr_dai_link_start;
	     i <= machine->soc_data->adsp_compr_dai_link_end; i++)
		machine->asoc->dai_links[i].compr_ops =
			&tegra_machine_compr_ops;
#endif
#if IS_ENABLED(CONFIG_SND_SOC_TEGRA186_ASRC_ALT)
	if (!(of_machine_is_compatible("nvidia,tegra210")  ||
		of_machine_is_compatible("nvidia,tegra210b01"))) {
		/* set ASRC params. The default is 2 channels */
		for (i = 0; i < 6; i++) {
			int tx = TEGRA186_DAI_LINK_ASRC1_TX1 + i;
			int rx = TEGRA186_DAI_LINK_ASRC1_RX1 + i;

			machine->asoc->dai_links[tx].params =
				&tegra_machine_asrc_link_params[i];
			machine->asoc->dai_links[rx].params =
				&tegra_machine_asrc_link_params[i];
		}
	}
#endif
}

static int add_dai_links(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_machine *machine = snd_soc_card_get_drvdata(card);
	int ret;

	machine->asoc = devm_kzalloc(&pdev->dev, sizeof(*machine->asoc),
				     GFP_KERNEL);
	if (!machine->asoc)
		return -ENOMEM;

	ret = tegra_asoc_populate_dai_links(pdev);
	if (ret < 0)
		return ret;

	ret = tegra_asoc_populate_codec_confs(pdev);
	if (ret < 0)
		return ret;

	ret = codec_init(machine);
	if (ret < 0)
		return ret;

	set_dai_ops(machine);

	return 0;
}

static const struct snd_kcontrol_new tegra_machine_controls[] = {
	SOC_ENUM_EXT("codec-x rate", tegra_machine_codec_rate,
		tegra_machine_codec_get_rate, tegra_machine_codec_put_rate),
	SOC_ENUM_EXT("codec-x format", tegra_machine_codec_format,
		tegra_machine_codec_get_format, tegra_machine_codec_put_format),
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static struct snd_soc_card snd_soc_tegra_card = {
	.owner = THIS_MODULE,
	.controls = tegra_machine_controls,
	.num_controls = ARRAY_SIZE(tegra_machine_controls),
	.dapm_widgets = tegra_machine_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra_machine_dapm_widgets),
	.suspend_pre = tegra_machine_suspend_pre,
	.fully_routed = true,
};

/* structure to match device tree node */
static const struct of_device_id tegra_machine_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-t186ref-mobile-rt5677",
		.data = &soc_data_tegra186 },
	{ .compatible = "nvidia,tegra-audio-t210ref-mobile-rt5677",
		.data = &soc_data_tegra210 },
	{ .compatible = "nvidia,tegra-audio-mystique",
		.data = &soc_data_tegra186 },
	{},
};

/*static int tegra_machine_set_mclk(struct tegra_machine *machine, struct device *dev)
{
	int err;

	err = tegra_alt_asoc_utils_set_rate(&machine->audio_clock,
			48000, RT5677_SYSCLK, RT5677_SYSCLK);
	if (err < 0) {
		dev_err(dev, "Can't configure clocks\n");
		return err;
	}

	return 0;
}*/

static int tegra_machine_driver_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct snd_soc_card *card = &snd_soc_tegra_card;
	struct tegra_machine *machine;
	struct tegra_asoc_platform_data *pdata = NULL;
	int ret = 0;
	const struct of_device_id *match;

	card->dev = &pdev->dev;
	/* parse card name first to log errors with proper device name */
	ret = snd_soc_of_parse_card_name(card, "nvidia,model");
	if (ret)
		return ret;

	match = of_match_device(tegra_machine_of_match, &pdev->dev);
	if (!match) {
		dev_err(&pdev->dev, "Error: No device match found\n");
		return -ENODEV;
	}

	if (!np) {
		dev_err(&pdev->dev, "No DT node for tegra machine driver");
		return -ENODEV;
	}

	machine = devm_kzalloc(&pdev->dev, sizeof(*machine), GFP_KERNEL);
	if (!machine)
		return -ENOMEM;

	machine->soc_data = (struct tegra_machine_soc_data *)match->data;
	if (!machine->soc_data)
		return -EINVAL;

	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	if (machine->soc_data->write_cdev1_state)
		machine->audio_clock.clk_cdev1_state = 0;

	if (machine->soc_data->write_idle_bias_off_state)
		card->dapm.idle_bias_off = true;

	ret = snd_soc_of_parse_audio_routing(card,
				"nvidia,audio-routing");
	if (ret)
		return ret;

	memset(&machine->audio_clock, 0, sizeof(machine->audio_clock));
	if (of_property_read_u32(np, "mclk-fs",
				 &machine->audio_clock.mclk_scale) < 0)
		dev_dbg(&pdev->dev, "Missing property mclk-fs\n");

	tegra_machine_dma_set_mask(pdev);

	ret = add_dai_links(pdev);
	if (ret < 0)
		goto cleanup_asoc;

	pdata = devm_kzalloc(&pdev->dev,
				sizeof(struct tegra_asoc_platform_data),	
				GFP_KERNEL);	
	if (!pdata) {	
		dev_err(&pdev->dev,	
			"Can't allocate tegra_asoc_platform_data struct\n");	
		ret = -ENOMEM;	
		goto cleanup_asoc;
	}

	pdata->gpio_spkr_en = of_get_named_gpio(np,
		"nvidia,spkr-en-gpios", 0);
	if (pdata->gpio_spkr_en == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (gpio_is_valid(pdata->gpio_spkr_en)) {
		ret = devm_gpio_request_one(&pdev->dev,
			pdata->gpio_spkr_en, GPIOF_OUT_INIT_LOW,
			"speaker_en");
		if (ret) {
			dev_err(card->dev, "cannot get speaker_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_SPKR_EN;
	}
	pdata->gpio_int_mic_en = of_get_named_gpio(np,
		"nvidia,dmic-clk-en-gpios", 0);
	if (pdata->gpio_int_mic_en == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (gpio_is_valid(pdata->gpio_int_mic_en)) {
		ret = devm_gpio_request_one(&pdev->dev,
			pdata->gpio_int_mic_en, GPIOF_OUT_INIT_LOW,
			"dmic_clk_en");
		if (ret) {
			dev_err(card->dev, "cannot get dmic_clk_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_INT_MIC_EN;
	}
	pdata->gpio_codec1 = pdata->gpio_codec2 = pdata->gpio_codec3 =
	pdata->gpio_hp_mute = pdata->gpio_ext_mic_en = -1;
 	machine->pdata = pdata;	
//	machine->pcard = card;
	ret = tegra_alt_asoc_utils_init(&machine->audio_clock,
					&pdev->dev,
					card);
	if (ret < 0)
		goto cleanup_asoc;

/*	ret = tegra_machine_set_mclk(machine, &pdev->dev);
	if (ret)
		goto cleanup_asoc;
*/
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto cleanup_asoc;
	}

	tegra_machine_add_i2s_codec_controls(card,
					machine->soc_data->num_ahub_links +
					machine->num_codec_links);

	return 0;
cleanup_asoc:
	release_asoc_phandles(machine);
	return ret;
}

static int tegra_machine_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);

	return 0;
}

#if CONFIG_PM
static void tegra_asoc_machine_resume(struct device *dev)
{
	WARN_ON(snd_soc_resume(dev));
}
#else
#define tegra_asoc_machine_resume NULL
#endif

static const struct dev_pm_ops tegra_asoc_machine_pm_ops = {
	.prepare = snd_soc_suspend,
	.complete = tegra_asoc_machine_resume,
	.poweroff = snd_soc_poweroff,
};

static struct platform_driver tegra_asoc_machine_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &tegra_asoc_machine_pm_ops,
		.of_match_table = tegra_machine_of_match,
	},
	.probe = tegra_machine_driver_probe,
	.remove = tegra_machine_driver_remove,
};
module_platform_driver(tegra_asoc_machine_driver);

MODULE_AUTHOR("Mohan Kumar <mkumard@nvidia.com>, Sameer Pujar <spujar@nvidia.com>");
MODULE_DESCRIPTION("Tegra ASoC machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, tegra_machine_of_match);
