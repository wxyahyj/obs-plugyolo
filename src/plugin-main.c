/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

/* ------------------------------------------------------------------------- */
/* OBS module boilerplate */

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ------------------------------------------------------------------------- */
/* 滤镜：名字（显示在 OBS UI 里） */

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "操作显示";
}

/* ------------------------------------------------------------------------- */
/* 滤镜：核心视频回调（现在什么都不做，直接返回原帧） */
/* ⚠️ 注意：这是 C 语言，必须写 struct obs_source_frame */

static struct obs_source_frame *filter_video(
	void *data,
	struct obs_source_frame *frame
)
{
	UNUSED_PARAMETER(data);

	static int cnt = 0;
	cnt++;

	/* 每 300 帧打一次日志，避免刷屏 */
	if(cnt % 300 == 0) {
		obs_log(LOG_INFO,
			"[MYFILTER] filter_video running | %ux%u | format=%d",
			frame ? frame->width : 0,
			frame ? frame->height : 0,
			frame ? frame->format : -1);
	}

	return frame;
}

/* ------------------------------------------------------------------------- */
/* 滤镜类型定义（最关键的结构体） */

static struct obs_source_info filter_info = {
	.id = "my_first_obs_filter",
	.type = OBS_SOURCE_TYPE_FILTER,     /* ← 这行决定：这是“滤镜” */
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = filter_get_name,
	.filter_video = filter_video,
};

/* ------------------------------------------------------------------------- */
/* 模块加载 / 卸载 */

bool obs_module_load(void)
{
	obs_register_source(&filter_info);  /* 注册滤镜 */
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
