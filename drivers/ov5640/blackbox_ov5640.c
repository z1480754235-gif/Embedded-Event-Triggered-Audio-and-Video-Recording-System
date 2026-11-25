#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-async.h>
#include <linux/v4l2-mediabus.h>

#include "blackbox_ov5640_regs.h"

struct blackbox_ov5640 {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct v4l2_subdev subdev;
	struct mutex lock;
	const struct blackbox_ov5640_mode *mode;
	bool streaming;
	bool powered;
};

static int blackbox_ov5640_read_reg(struct blackbox_ov5640 *sensor, 
									unsigned short reg, unsigned char *value)
{
	struct i2c_client *client = sensor->client;
	unsigned char addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(addr),
			.buf = addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;
	
	return ret == ARRAY_SIZE(msg) ? 0 : -EIO;
}

static int blackbox_ov5640_write_reg(struct blackbox_ov5640 *sensor,
									 unsigned short reg, unsigned char value)
{
	struct i2c_client *client = sensor->client;
	unsigned char buf[3] = { reg >> 8, reg & 0xff, value };
	struct i2c_msg msg[1] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(buf),
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if(ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msg) ? 0 : -EIO;
}

/* 读改写函数 */
static int blackbox_ov5640_update_bits(struct blackbox_ov5640 *sensor,
			unsigned short reg, unsigned char mask, unsigned char value)
{
	unsigned char old_value;
	unsigned char new_value;
	int ret;

	ret = blackbox_ov5640_read_reg(sensor, reg, &old_value);
	if (ret)
		return ret;

	new_value = old_value;
	new_value &= ~mask;
	new_value |= value & mask;
	/* new_value = (old_value & ~mask) | (value & mask); */

	if (new_value == old_value)
		return 0;

	return blackbox_ov5640_write_reg(sensor, reg, new_value);
}

static int blackbox_ov5640_get_gpios(struct blackbox_ov5640 *sensor)
{
	struct device *dev = &sensor->client->dev;
	int ret;

	sensor->pwdn_gpio = devm_gpiod_get(dev, "pwn", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio)) {
		ret = PTR_ERR(sensor->pwdn_gpio);
		dev_err(dev, "failed to get PWDN GPIO: %d\n", ret);
		return ret;
	}

	sensor->reset_gpio = devm_gpiod_get(dev, "rst", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio)) {
		ret = PTR_ERR(sensor->reset_gpio);
		dev_err(dev, "failed to get RESET GPIO: %d\n", ret);
		return ret;
	}

	return 0;
}

/* 解除复位+低功耗 */
static void blackbox_ov5640_power_on(struct blackbox_ov5640 *sensor)
{
	if (sensor->powered)
		return;
	
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);
	msleep(5);

	gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
	msleep(5);

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	msleep(20);

	sensor->powered = true;
}

/* 保持复位，并进入掉电模式 */
static void blackbox_ov5640_power_off(struct blackbox_ov5640 *sensor)
{
	if (!sensor->powered)
		return;
	
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);

	sensor->streaming = false;
	sensor->powered = false;
}

/* 回调找到整个 sensor 结构体 */
static struct blackbox_ov5640 *blackbox_ov5640_from_subdev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct blackbox_ov5640, subdev);
}

/* 表加载函数 */
static int blackbox_ov5640_write_table(struct blackbox_ov5640 *sensor,
		const struct blackbox_ov5640_reg *table, unsigned int count)
{
	struct device *dev = &sensor->client->dev;
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		if (table[i].mask) {
			ret = blackbox_ov5640_update_bits(sensor, table[i].reg, 
							table[i].mask, table[i].value);
		} else {
			ret = blackbox_ov5640_write_reg(sensor, table[i].reg, table[i].value);
		}

		if (ret) {
			dev_err(dev, "table entry %u, register 0x%04x failed: %d\n",
					i, table[i].reg, ret);
			return ret;
		}

		if (table[i].delay_ms)
			msleep(table[i].delay_ms);
	}

	return 0;
}

/* 软件复位函数 */
static int blackbox_ov5640_soft_reset(struct blackbox_ov5640 *sensor)
{
	int ret;

	ret = blackbox_ov5640_write_reg(sensor, 0x3103, 0x11);
	if (ret)
		return ret;

	ret = blackbox_ov5640_write_reg(sensor, 0x3008, 0x82);
	if (ret)
		return ret;
	
	msleep(10);

	return 0;
}

/* 模式初始化函数 */
static int blackbox_ov5640_init_mode(struct blackbox_ov5640 *sensor)
{
	int ret;

	ret = blackbox_ov5640_soft_reset(sensor);
	if (ret)
		return ret;

	ret = blackbox_ov5640_write_table(sensor, blackbox_ov5640_global_regs,
									  ARRAY_SIZE(blackbox_ov5640_global_regs));
	if (ret)
		return ret;
	
	ret = blackbox_ov5640_write_table(sensor, blackbox_ov5640_vga_regs,
									  ARRAY_SIZE(blackbox_ov5640_vga_regs));
	if (ret)
		return ret;
	
	if (sensor->mode != &blackbox_ov5640_modes[0]) {
		ret = blackbox_ov5640_write_table(sensor, sensor->mode->regs,
										  sensor->mode->num_regs);
		if (ret)
			return ret;
	}
	
	/* 将数字输出引脚的电气驱动能力设置为 2 倍 */
	ret = blackbox_ov5640_update_bits(sensor, 0x302c, 0xc0, 0x40);
	if (ret)
		return ret;

	msleep(300);

	/* 初始化完成，等待应用开始采集 软件待机 */
	return blackbox_ov5640_update_bits(sensor, 0x3008, 1 << 6, 1 << 6);
}

/* 电源回调函数 */
static int blackbox_ov5640_set_power(struct v4l2_subdev *sd, int on)
{
	struct blackbox_ov5640 *sensor;
	int ret = 0;

	sensor = blackbox_ov5640_from_subdev(sd);
	on = !!on;

	mutex_lock(&sensor->lock);

	if (sensor->powered == on)
		goto out_unlock;
	
	if (!on) {
		blackbox_ov5640_power_off(sensor);
		goto out_unlock;
	}

	blackbox_ov5640_power_on(sensor);
	
	ret = blackbox_ov5640_init_mode(sensor);
	if (ret) {
		dev_err(&sensor->client->dev, "failed to restore camera mode: %d\n", ret);
		blackbox_ov5640_power_off(sensor);
		goto out_unlock;
	}

	sensor->streaming = false;

out_unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

/* 芯片检查函数 */
static int blackbox_ov5640_check_id(struct blackbox_ov5640 *sensor)
{
	struct device *dev = &sensor->client->dev;
	unsigned char high;
	unsigned char low;
	unsigned short chip_id;
	int ret;

	ret = blackbox_ov5640_read_reg(sensor, 0x300A, &high);
	if (ret) {
		dev_err(dev, "failed to read chip ID high byte: %d\n", ret);
		return ret;
	}

	ret = blackbox_ov5640_read_reg(sensor, 0x300B, &low);
	if (ret) {
		dev_err(dev, "failed to read chip ID low byte: %d\n", ret);
		return ret;
	}

	chip_id = ((unsigned short)high << 8) | low;
	if (chip_id != 0x5640) {
		dev_err(dev, "unexpected chip ID: 0x%04x\n", chip_id);
		return -ENODEV;
	}

	dev_info(dev, "ov5640 detected, chip ID: 0x%04x\n", chip_id);

	return 0;
}

/* 回读检测关键寄存器 */
static int blackbox_ov5640_check_mode(struct blackbox_ov5640 *sensor)
{
	struct device *dev = &sensor->client->dev;
	const struct blackbox_ov5640_mode *mode = sensor->mode;
	struct blackbox_ov5640_expected {
		unsigned short reg;
		unsigned char value;
	};
	unsigned char value;
	unsigned int i;
	int ret;

	const struct blackbox_ov5640_expected expected[] = {
		{0x3808, (mode->width >> 8) & 0xff},
		{0x3809, mode->width & 0xff},
		{0x380a, (mode->height >> 8) & 0xff},
		{0x380b, mode->height & 0xff},
		{0x4300, 0x30},
	};
	
	for(i = 0; i < ARRAY_SIZE(expected); i++)
	{
		ret = blackbox_ov5640_read_reg(sensor, expected[i].reg, &value);
		if (ret) {
			dev_err(dev, "failed to read register 0x%04x: %d\n", expected[i].reg, ret);
			return ret;
		}

		if (value != expected[i].value) {
			dev_err(dev, "register 0x%04x: expected 0x%02x, got 0x%02x\n",
					expected[i].reg, expected[i].value, value);
			return -EIO;
		}
	}

	dev_info(dev, "%ux%u size and YUYV registers verified\n",
		 mode->width, mode->height);

	return 0;
}

/* 模式查找函数 */
static const struct blackbox_ov5640_mode *blackbox_ov5640_find_mode(unsigned int width,
											unsigned int height)
{
	const struct blackbox_ov5640_mode *mode;
	unsigned int i;
	
	for (i = 0; i < ARRAY_SIZE(blackbox_ov5640_modes); i++) {
		mode = &blackbox_ov5640_modes[i];

		if (width == mode->width && height == mode->height)
			return mode;
	}

	return &blackbox_ov5640_modes[0];
}

/* 格式接口函数 */
static void blackbox_ov5640_fill_format(const struct blackbox_ov5640_mode *mode,
										struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_YUYV8_2X8; /* 使用 8 位总线，平均每像素传输两个 8 位数据 */
	fmt->field = V4L2_FIELD_NONE; /* 逐行图像 */
	fmt->colorspace = V4L2_COLORSPACE_JPEG; /* 不表示输出 JPEG 文件 */
}

/* 枚举支持的格式 */
static int blackbox_ov5640_enum_fmt(struct v4l2_subdev *sd,
									unsigned int index, unsigned int *code)
{
	if (index != 0)
		return -EINVAL;

	*code = MEDIA_BUS_FMT_YUYV8_2X8;

	return 0;
}

/* 找到请求能匹配的模式 */
static int blackbox_ov5640_try_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *fmt)
{
	const struct blackbox_ov5640_mode *mode;

	mode = blackbox_ov5640_find_mode(fmt->width, fmt->height);
	blackbox_ov5640_fill_format(mode, fmt);

	return 0;
}

/* 查询传感器子设备当前选择的格式 */
static int blackbox_ov5640_get_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *fmt)
{
	struct blackbox_ov5640 *sensor;

	sensor = blackbox_ov5640_from_subdev(sd);

	mutex_lock(&sensor->lock);
	blackbox_ov5640_fill_format(sensor->mode, fmt);
	mutex_unlock(&sensor->lock);

	return 0;
}

/* 接受 YUYV 请求，选择应用图像模式 */
static int blackbox_ov5640_set_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *fmt)
{
	struct blackbox_ov5640 *sensor;
	const struct blackbox_ov5640_mode *mode;
	const struct blackbox_ov5640_mode *oldmode;
	int restore_ret;
	int ret = 0;

	if (fmt->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;

	sensor = blackbox_ov5640_from_subdev(sd);
	mode = blackbox_ov5640_find_mode(fmt->width, fmt->height);

	mutex_lock(&sensor->lock);

	/* 采集中不允许重新配置模式 */
	if (sensor->streaming) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (mode == sensor->mode)
		goto fill_format;
	
	oldmode = sensor->mode;
	sensor->mode = mode;

	/* 未上电时只记录选择，下次上电再应用 */
	if (!sensor->powered)
		goto fill_format;

	ret = blackbox_ov5640_init_mode(sensor);
	if (ret) {
		dev_err(&sensor->client->dev, "failed to set mode: %d\n", ret);
		sensor->mode = oldmode;
		restore_ret = blackbox_ov5640_init_mode(sensor);
		if (restore_ret) {
			dev_err(&sensor->client->dev,
				"failed to restore previous mode: %d\n", restore_ret);
			blackbox_ov5640_power_off(sensor);
		}

		goto out_unlock;
	}

fill_format:
	blackbox_ov5640_fill_format(sensor->mode, fmt);
out_unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

/* 查询当前采集参数 */
static int blackbox_ov5640_get_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *parm)
{
	struct blackbox_ov5640 *sensor;
	struct v4l2_captureparm *capture;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sensor = blackbox_ov5640_from_subdev(sd);

	capture = &parm->parm.capture;
	memset(capture, 0, sizeof(struct v4l2_captureparm));

	mutex_lock(&sensor->lock);

	capture->capability = V4L2_CAP_TIMEPERFRAME;
	capture->timeperframe.numerator = 1;
	capture->timeperframe.denominator = sensor->mode->fps;

	mutex_unlock(&sensor->lock);

	return 0;
}

static int blackbox_ov5640_set_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *parm)
{
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (parm->parm.capture.capturemode != 0)
		return -EINVAL;

	/* 当前只支持固定模式，返回实际采用的帧参数 */
	return blackbox_ov5640_get_parm(sd, parm);
}

/* 流控制函数 */
static int blackbox_ov5640_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct blackbox_ov5640 *sensor;
	int ret = 0;

	sensor = blackbox_ov5640_from_subdev(sd);
	enable = !!enable;

	mutex_lock(&sensor->lock);

	/* 摄像头现在能不能启动 */
	/* enable = 1：应用要求开始出图，但摄像头还没准备好 */
	/* enable = 0：应用要求停止出图，摄像头已经关闭，目标已经满足 */
	if (!sensor->powered) {
		ret = enable ? -EPIPE : 0;
		goto out_unlock;
	}

	/* 为真当前状态已经等于请求的状态 */
	if (sensor->streaming == enable)
		goto out_unlock;
	
	/* 0为退出软件待机 */
	ret = blackbox_ov5640_update_bits(sensor, 0x3008, 1 << 6, enable ? 0 : 1 << 6);
	if (ret) {
		dev_err(&sensor->client->dev, "failed to %s stream: %d\n",
			    enable ? "start" : "stop", ret);
		goto out_unlock;
	}

	sensor->streaming = enable;
	
out_unlock:
	mutex_unlock(&sensor->lock);
	return ret;
}

/* 尺寸枚举函数 */
static int blackbox_ov5640_enum_frame_size(struct v4l2_subdev *sd, 
	struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_frame_size_enum *fse)
{
	const struct blackbox_ov5640_mode *mode;

	if (fse->pad != 0)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(blackbox_ov5640_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;

	mode = &blackbox_ov5640_modes[fse->index];

	fse->min_width = mode->width;
	fse->max_width = mode->width;
	fse->min_height = mode->height;
	fse->max_height = mode->height;

	return 0;
}

/* 帧间隔枚举函数 */
static int blackbox_ov5640_enum_frame_interval(struct v4l2_subdev *sd,
	struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_frame_interval_enum *fie)
{
	const struct blackbox_ov5640_mode *mode;
	unsigned int i;

	if (fie->pad != 0 || fie->index != 0)
		return -EINVAL;

	if (fie->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;
	
	/* 当前每种分辨率只提供一个帧间隔，因此只接受索引 0 */
	for (i = 0; i < ARRAY_SIZE(blackbox_ov5640_modes); i++) {
		mode = &blackbox_ov5640_modes[i];

		if (fie->width != mode->width || fie->height != mode->height)
			continue;

		fie->interval.numerator = 1;
		fie->interval.denominator = mode->fps;

		return 0;
	}

	return -EINVAL;
}

static const struct v4l2_subdev_core_ops blackbox_ov5640_core_ops = {
	.s_power = blackbox_ov5640_set_power,
};

static const struct v4l2_subdev_video_ops blackbox_ov5640_video_ops = {
	.enum_mbus_fmt = blackbox_ov5640_enum_fmt,
	.try_mbus_fmt = blackbox_ov5640_try_fmt,
	.g_mbus_fmt = blackbox_ov5640_get_fmt,
	.s_mbus_fmt = blackbox_ov5640_set_fmt,
	.g_parm = blackbox_ov5640_get_parm,
	.s_parm = blackbox_ov5640_set_parm,
	.s_stream = blackbox_ov5640_set_stream,
};

static const struct v4l2_subdev_pad_ops blackbox_ov5640_pad_ops = {
	.enum_frame_size = blackbox_ov5640_enum_frame_size,
	.enum_frame_interval = blackbox_ov5640_enum_frame_interval,
};

static const struct v4l2_subdev_ops blackbox_ov5640_subdev_ops = {
	.core = &blackbox_ov5640_core_ops,
	.video = &blackbox_ov5640_video_ops,
	.pad = &blackbox_ov5640_pad_ops,
};

static int blackbox_ov5640_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct blackbox_ov5640 *sensor;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "adapter does not support I2C tranfers\n");
		return -EOPNOTSUPP;
	}

	sensor = devm_kzalloc(dev, sizeof(struct blackbox_ov5640), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	mutex_init(&sensor->lock);

	sensor->client = client;
	sensor->mode = &blackbox_ov5640_modes[0];

	/* 复位+低功耗 */
	ret = blackbox_ov5640_get_gpios(sensor);
	if (ret) 
		return ret;

	blackbox_ov5640_power_on(sensor);

	ret = blackbox_ov5640_check_id(sensor);
	if (ret)
		goto err_power_off;

	ret = blackbox_ov5640_init_mode(sensor);
	if (ret) {
		dev_err(dev, "failed to initialize VGA mode: %d\n", ret);
		goto err_power_off;
	}

	ret = blackbox_ov5640_check_mode(sensor);
	if (ret)
		goto err_power_off;

	blackbox_ov5640_power_off(sensor);

	/* 执行后，clientdata 保存的是 subdev 的地址，不再是 sensor 的地址 */
	/* i2c_set_clientdata(client, &sensor->subdev) */
	v4l2_i2c_subdev_init(&sensor->subdev, client, &blackbox_ov5640_subdev_ops);

	ret = v4l2_async_register_subdev(&sensor->subdev);
	if (ret) {
		dev_err(dev, "failed to register sensor subdevice: %d\n", ret);
		goto err_power_off;
	}
	
	dev_info(dev, "ov5640 subdevice registered\n");

	return 0;

err_power_off:
	blackbox_ov5640_power_off(sensor);
	return ret;
}

static int blackbox_ov5640_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct blackbox_ov5640 *sensor;

	sensor = blackbox_ov5640_from_subdev(sd);

	v4l2_async_unregister_subdev(sd);

	mutex_lock(&sensor->lock);
	blackbox_ov5640_power_off(sensor);
	mutex_unlock(&sensor->lock);

	dev_info(&client->dev, "ov5640 subdevice removed\n");

	return 0;
}

static const struct i2c_device_id blackbox_ov5640_id[] = {
	{ "blackbox-ov5640", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, blackbox_ov5640_id);

static const struct of_device_id blackbox_ov5640_of_match[] = {
	{ .compatible = "avblackbox,gravityov5640" },
	{ },
};
MODULE_DEVICE_TABLE(of, blackbox_ov5640_of_match);

static struct i2c_driver blackbox_ov5640_driver = {
	.probe = blackbox_ov5640_probe,
	.remove = blackbox_ov5640_remove,
	.id_table = blackbox_ov5640_id,
	.driver = {
		.name = "blackbox-ov5640",
		.of_match_table = blackbox_ov5640_of_match,
	},
};

static int __init blackbox_ov5640_init(void)
{
	return i2c_add_driver(&blackbox_ov5640_driver);
}

static void __exit blackbox_ov5640_exit(void)
{
	i2c_del_driver(&blackbox_ov5640_driver);
}

module_init(blackbox_ov5640_init);
module_exit(blackbox_ov5640_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("Blackbox ov5640 sensor driver");
