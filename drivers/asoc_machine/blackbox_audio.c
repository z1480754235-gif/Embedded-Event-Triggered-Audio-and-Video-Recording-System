#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/of_gpio.h>

#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include "wm8960.h"

struct blackbox_audio {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct mutex stream_lock;
	struct snd_soc_jack hp_jack; /* 保存耳机插入状态 */
	struct snd_soc_jack_pin hp_pins[2]; /* 把插入状态关联到 DAPM 端点 */
	struct snd_soc_jack_gpio hp_gpio; /* 描述检测 GPIO、极性及消抖时间 */
	bool stream_open;

	struct clk *mclk;

	struct device_node *cpu_np;
	struct device_node *codec_np;
};

static void blackbox_put_nodes(struct blackbox_audio *audio)
{
	/* 释放引用 */
	of_node_put(audio->codec_np);
	of_node_put(audio->cpu_np);

	audio->codec_np = NULL;
	audio->cpu_np = NULL;
}

static void blackbox_release_nodes(void *data)
{
	struct blackbox_audio *audio = data;

	blackbox_put_nodes(audio);
}

static void blackbox_release_mclk(void *data)
{
	struct clk *mclk = data;

	clk_put(mclk);
}

static int blackbox_parse_dt(struct device *dev,
							struct blackbox_audio *audio)
{
	struct device_node *np = dev->of_node;

	if (!np) {
		dev_err(dev, "device tree node missing\n");
		return -EINVAL;
	}

	if (!of_property_read_bool(np, "codec-master")) {
		dev_err(dev, "requires codec-master mode\n");
		return -EINVAL;
	}

	audio->cpu_np = of_parse_phandle(np, "cpu-dai", 0);
	if (!audio->cpu_np) {
		dev_err(dev, "cpu-dai missing or invalid\n");
		return -EINVAL;
	}

	audio->codec_np = of_parse_phandle(np, "audio-codec", 0);
	if (!audio->codec_np) {
		dev_err(dev, "audio-codec missing or invalid\n");
		goto err_put_nodes;
	}

	audio->link.name = "Blackbox HiFi";
	audio->link.stream_name = "Blackbox HiFi";

	audio->link.cpu_of_node = audio->cpu_np; /* SAI2的数字音频接口 */
	audio->link.platform_of_node = audio->cpu_np; /* PCM/DMA 平台支持 */

	audio->link.codec_of_node = audio->codec_np;
	audio->link.codec_dai_name = "wm8960-hifi";

	return 0;

err_put_nodes:
	blackbox_put_nodes(audio);
	return -EINVAL;
}

/* 声明板上的音频端点，NULL 表示这个端点不提供额外的电源事件回调 */
static const struct snd_soc_dapm_widget blackbox_widgets[] = {
	SND_SOC_DAPM_MIC("Main MIC", NULL), /* 	板载麦克风输入 */
	SND_SOC_DAPM_MIC("Mic Jack", NULL), /* LINE_IN输入 */
	SND_SOC_DAPM_HP("Headphone Jack", NULL), /* PHONE 耳机输出 */
	SND_SOC_DAPM_SPK("Ext Spk", NULL), /* 扬声器输出 */
};

static int blackbox_get_mclk(struct device *dev, struct blackbox_audio *audio)
{
	int ret;

	audio->mclk = of_clk_get_by_name(audio->codec_np, "mclk");
	if (IS_ERR(audio->mclk)) {
		ret = PTR_ERR(audio->mclk);

		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get codec MCLK: %d\n", ret);

		audio->mclk = NULL;
		return ret;
	}

	ret = devm_add_action(dev, blackbox_release_mclk, audio->mclk);
	if (ret) {
		clk_put(audio->mclk);
		audio->mclk = NULL;
		return ret;
	}

	return 0;
}

/* 配置 SoC 的板级 MCLK 输出控制 */
static int blackbox_configure_gpr(struct device *dev)
{
	struct of_phandle_args args;
	struct regmap *gpr;
	int ret;

	/* 取得gpr节点并解析节点引用后面的参数 */
	ret = of_parse_phandle_with_fixed_args(dev->of_node, "gpr", 3, 0, &args);
	if (ret) {
		dev_err(dev, "failed to parse gpr property: %d\n", ret);
		return ret;
	}

	/* 获取IOMUXC GPR寄存器访问对象 */
	gpr = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(gpr)) {
		ret = PTR_ERR(gpr);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get GPR regmap: %d\n", ret);

		return ret;
	}

	ret = regmap_update_bits(gpr, args.args[0], args.args[1], args.args[2]);
	if (ret)
		dev_err(dev, "failed to configure GPR: %d\n", ret);
	
	return ret;
}

static int blackbox_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_codec *codec = card->rtd[0].codec_dai->codec;
	unsigned int hp_det[2];
	int ret;

	ret = of_property_read_u32_array(card->dev->of_node, "hp-det", hp_det, 2);
	if (ret) {
		dev_err(card->dev, "failed to read hp-det: %d\n", ret);
		return ret;
	}

	if (hp_det[0] != 3 || hp_det[1] > 1) {
		dev_err(card->dev, "requires JD3 detection and polarity 0 or 1\n");
		return -EINVAL;
	}
	/* 将 ADCLRC 引脚切换为 GPIO 功能 */
	ret = snd_soc_update_bits(codec, WM8960_IFACE2, 1 << 6, 1 << 6);
	if (ret < 0) {
		dev_err(card->dev, "failed to configure ADCLRC GPIO: %d\n", ret);
		return ret;
	}

	/* wm8960的 GPIO 功能可以输出耳机插入和拨出状态反馈给AUD_INT，从而产生中断 */
	/* 选择 WM8960 GPIO1 的输出功能 */
	ret = snd_soc_update_bits(codec, WM8960_ADDCTL4, 7 << 4, 3 << 4);
	if (ret < 0) {
		dev_err(card->dev, "failed to select GPIO1 function: %d\n", ret);
		return ret;
	}

	/* 选择 JD3 作为耳机检测输入 */
	ret = snd_soc_update_bits(codec, WM8960_ADDCTL4, 3 << 2, hp_det[0] << 2);
	if (ret < 0) {
		dev_err(card->dev, "failed to select headphone detect input: %d\n", ret);
		return ret;
	}

	/* 配置耳机检测极性 */
	ret = snd_soc_update_bits(codec, WM8960_ADDCTL2, 1 << 5, hp_det[1] << 5);
	if (ret < 0) {
		dev_err(card->dev, "failed to configure headphone polarity: %d\n", ret);
		return ret;
	}

	/* 启用检测消抖所需的慢时钟，沿用 BSP 的时钟选择 */
	ret = snd_soc_update_bits(codec, WM8960_ADDCTL1, 3, 3);
	if (ret < 0) {
		dev_err(card->dev, "failed to configure jack debounce clock: %d\n", ret);
		return ret;
	}

	/* 启用耳机检测与硬件输出切换功能 */
	ret = snd_soc_update_bits(codec, WM8960_ADDCTL2, 1 << 6, 1 << 6);
	if (ret < 0) {
		dev_err(card->dev, "failed to enable headphone detection: %d\n", ret);
		return ret;
	}

	return 0;
}

static int blackbox_hw_params(struct snd_pcm_substream *substream, 
							  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct blackbox_audio *audio;
	struct device *dev = rtd->card->dev;
	unsigned int rate = params_rate(params);
	unsigned int fmt;
	unsigned int target_sysclk;
	unsigned long mclk_rate;
	int ret;

	audio = snd_soc_card_get_drvdata(rtd->card);

	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE || 
		params_channels(params) != 2 || (rate != 16000 && rate != 48000)) {
		dev_err(dev, "requires S16_LE, stereo, 16KHz or 48KHz\n");
		return -EINVAL;
	}

	/* 返回参考时钟频率 */
	mclk_rate = clk_get_rate(audio->mclk);
	if(!mclk_rate) {
		dev_err(dev, "invalid MCLK rate\n");
		return -EINVAL;
	}

	/* 	Codec 提供 BCLK 和帧时钟 */
	fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM;

	ret = snd_soc_dai_set_fmt(rtd->cpu_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set SAI format: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(rtd->codec_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set codec format: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(rtd->cpu_dai, 0, 0, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(dev, "failed to set SAI clock: %d\n", ret);
		return ret;
	}

	target_sysclk = rate * 512;

	/* 告诉 Codec 外部参考时钟mclk_rate */
	ret = snd_soc_dai_set_pll(rtd->codec_dai, WM8960_SYSCLK_AUTO, 0,
							  mclk_rate, target_sysclk);
	if (ret) {
		dev_err(dev, "failed to configure codec clock input: %d\n", ret);
		return ret;
	}

	/* 选择自动时钟策略 让 Codec 自动寻找合适的时钟方案，得到LPCLK=16KHz,BCLK=16000*2*16=512KHz */
	ret = snd_soc_dai_set_sysclk(rtd->codec_dai, WM8960_SYSCLK_AUTO,
								 target_sysclk, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(dev, "failed to set codec SYSCLK: %d\n", ret);
		return ret;
	}

	return 0;
}

static int blackbox_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int fmt;
	int ret;

	/* WM8960 恢复为时钟从机 */
	fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS;

	ret = snd_soc_dai_set_fmt(rtd->codec_dai, fmt);
	if (ret) {
		dev_warn(rtd->card->dev, "failed to restore codec slave mode: %d\n", ret);
	}

	return 0;
}

static int blackbox_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct blackbox_audio *audio;
	int ret;

	audio = snd_soc_card_get_drvdata(rtd->card);

	mutex_lock(&audio->stream_lock);

	if (audio->stream_open) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = clk_prepare_enable(audio->mclk);
	if (ret) {
		dev_err(rtd->card->dev, "failed to enable MCLK: %d\n", ret);
		goto out_unlock;
	}

	audio->stream_open = true;

out_unlock:
	mutex_unlock(&audio->stream_lock);
	return ret;
}

static void blackbox_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct blackbox_audio *audio;

	audio = snd_soc_card_get_drvdata(rtd->card);

	mutex_lock(&audio->stream_lock);

	if (audio->stream_open) {
		clk_disable_unprepare(audio->mclk);
		audio->stream_open = false;
	}

	mutex_unlock(&audio->stream_lock);
}

static const struct snd_soc_ops blackbox_ops = {
	.startup = blackbox_startup,
	.hw_params = blackbox_hw_params,
	.hw_free = blackbox_hw_free,
	.shutdown = blackbox_shutdown,
};

/* 会释放中断、取消延迟工作，并释放 GPIO */
static void blackbox_release_hp_gpio(void *data)
{
	struct blackbox_audio *audio = data;

	snd_soc_jack_free_gpios(&audio->hp_jack, 1, &audio->hp_gpio);
}

/* 耳机检测初始化函数 */
static int blackbox_init_hp_jack(struct device *dev, struct blackbox_audio *audio)
{
	enum of_gpio_flags flags;
	int gpio;
	int ret;

	gpio = of_get_named_gpio_flags(dev->of_node, "hp-det-gpios", 0, &flags);
	if (gpio < 0) {
		if (gpio != -EPROBE_DEFER)
			dev_err(dev, "failed to get headphone GPIO: %d\n", gpio);

		return gpio;
	}	

	/* 描述耳机状态怎么影响DAPM */
	/* 耳机插入，扬声器禁用；耳机拨出，扬声器启用 */
	audio->hp_pins[0].pin = "Headphone Jack";
	audio->hp_pins[0].mask = SND_JACK_HEADPHONE;

	audio->hp_pins[1].pin = "Ext Spk";
	audio->hp_pins[1].mask = SND_JACK_HEADPHONE;
	audio->hp_pins[1].invert = true;

	ret = snd_soc_card_jack_new(&audio->card, "Headphone Jack",
								SND_JACK_HEADPHONE, &audio->hp_jack,
								audio->hp_pins, ARRAY_SIZE(audio->hp_pins));
	if (ret) {
		dev_err(dev, "failed to create headphone jack: %d\n", ret);
		return ret;
	}

	audio->hp_gpio.gpio = gpio;
	audio->hp_gpio.name = "blackbox headphone detect";
	audio->hp_gpio.report = SND_JACK_HEADPHONE;
	audio->hp_gpio.invert = !!(flags & OF_GPIO_ACTIVE_LOW); /* !!转为1或0，结果为0，高电平表示有效 不反转 */
	audio->hp_gpio.debounce_time = 250;						/* 读取到高电平，耳机插入；否则，耳机拨出 */

	/* 申请 GPIO，配置输入，申请上升沿、下降沿中断，安排一次初始状态检测 */
	ret = snd_soc_jack_add_gpios(&audio->hp_jack, 1, &audio->hp_gpio);
	if (ret) {
		dev_err(dev, "failed to register headphone GPIO: %d\n", ret);
		return ret;
	}

	/* 登记自动清理 */
	ret = devm_add_action(dev, blackbox_release_hp_gpio, audio);
	if (ret) {
		snd_soc_jack_free_gpios(&audio->hp_jack, 1, &audio->hp_gpio);
		return ret;
	}

	return 0;
}

static int blackbox_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct blackbox_audio *audio;
	int ret;

	audio = devm_kzalloc(dev, sizeof(struct blackbox_audio), GFP_KERNEL);
	if (!audio) {
		return -ENOMEM;
	}

	mutex_init(&audio->stream_lock);

	ret = blackbox_parse_dt(dev, audio);
	if (ret)
		return ret;

	ret = devm_add_action(dev, blackbox_release_nodes, audio);
	if (ret) {
		blackbox_put_nodes(audio);
		return ret;
	}

	ret = blackbox_get_mclk(dev, audio);
	if (ret) 
		return ret;

	ret = blackbox_configure_gpr(dev);
	if (ret)
		return ret;

	audio->card.dev = dev;
	audio->card.owner = THIS_MODULE;

	audio->card.dai_link = &audio->link;
	audio->card.num_links = 1;

	audio->link.ops = &blackbox_ops;
	audio->card.late_probe = blackbox_late_probe;

	audio->card.dapm_widgets = blackbox_widgets;
	audio->card.num_dapm_widgets = ARRAY_SIZE(blackbox_widgets);

	/* 读取声卡的名称 */
	ret = snd_soc_of_parse_card_name(&audio->card, "model");
	if (ret) {
		dev_err(dev, "failed to parse sound card model: %d\n", ret);
		return ret;
	}

	/* 板上的音频连接 */
	ret = snd_soc_of_parse_audio_routing(&audio->card,
										"audio-routing");
	if (ret) {
		dev_err(dev, "failed to parse audio routing: %d\n", ret);
		return ret;
	}

	snd_soc_card_set_drvdata(&audio->card, audio);
	platform_set_drvdata(pdev, audio);

	ret = devm_snd_soc_register_card(dev, &audio->card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to register sound card: %d\n", ret);
		
		return ret;
	}

	ret = blackbox_init_hp_jack(dev, audio);
	if (ret)
		return ret;
	
	dev_info(dev, "blackbox sound card registered\n");

	return 0;
}

static const struct of_device_id blackbox_audio_of_match[] = {
	{ .compatible = "avblackbox,imx6ull-wm8960" },
	{ },
};
MODULE_DEVICE_TABLE(of, blackbox_audio_of_match);

static struct platform_driver blackbox_audio_driver = {
	.probe = blackbox_audio_probe,
	.driver = {
		.name = "blackbox-audio",
		.of_match_table = blackbox_audio_of_match,
		.pm = &snd_soc_pm_ops,
	},
};

static int __init blackbox_audio_init(void)
{
	return platform_driver_register(&blackbox_audio_driver);
}

static void __exit blackbox_audio_exit(void)
{
	platform_driver_unregister(&blackbox_audio_driver);
}

module_init(blackbox_audio_init);
module_exit(blackbox_audio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("i.MX6ULL AV Blackbox WM8960 machine driver");
