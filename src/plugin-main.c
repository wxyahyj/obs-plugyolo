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
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <plugin-support.h>
#include <util/threading.h>
#include <util/platform.h>

#define ROI_SIZE 320

typedef struct {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *param_image_size;
	gs_eparam_t *param_roi_size;
	gs_eparam_t *param_roi_center;
	gs_eparam_t *param_box_color;
	gs_eparam_t *param_box_width;
	gs_technique_t *tech_normal;

	gs_texture_t *roi_texture;
	uint8_t *roi_buffer;

	uint32_t width;
	uint32_t height;

	int frame_count;
	pthread_mutex_t roi_mutex;
	bool roi_ready;
} my_filter_data_t;

static my_filter_data_t *g_filter_data = NULL;

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "操作显示";
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	my_filter_data_t *data = bzalloc(sizeof(my_filter_data_t));
	data->context = source;
	data->frame_count = 0;
	data->roi_ready = false;
	data->roi_buffer = NULL;
	data->roi_texture = NULL;
	pthread_mutex_init(&data->roi_mutex, NULL);

	char *effect_path = obs_module_file("center_roi_gpu.effect");
	if (!effect_path) {
		obs_log(LOG_ERROR, "Failed to find center_roi_gpu.effect");
		pthread_mutex_destroy(&data->roi_mutex);
		bfree(data);
		return NULL;
	}

	data->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);

	if (!data->effect) {
		obs_log(LOG_ERROR, "Failed to create effect from center_roi_gpu.effect");
		pthread_mutex_destroy(&data->roi_mutex);
		bfree(data);
		return NULL;
	}

	data->param_image_size = gs_effect_get_param_by_name(data->effect, "image_size");
	data->param_roi_size = gs_effect_get_param_by_name(data->effect, "roi_size");
	data->param_roi_center = gs_effect_get_param_by_name(data->effect, "roi_center");
	data->param_box_color = gs_effect_get_param_by_name(data->effect, "box_color");
	data->param_box_width = gs_effect_get_param_by_name(data->effect, "box_width");

	data->tech_normal = gs_effect_get_technique(data->effect, "NormalRender");

	if (!data->tech_normal) {
		obs_log(LOG_ERROR, "Failed to get NormalRender technique from effect");
		gs_effect_destroy(data->effect);
		pthread_mutex_destroy(&data->roi_mutex);
		bfree(data);
		return NULL;
	}

	g_filter_data = data;

	obs_log(LOG_INFO, "Filter created successfully");
	return data;
}

static void filter_destroy(void *data)
{
	my_filter_data_t *filter = data;

	if (!filter)
		return;

	pthread_mutex_destroy(&filter->roi_mutex);

	if (filter->roi_texture)
		gs_texture_destroy(filter->roi_texture);

	if (filter->roi_buffer)
		bfree(filter->roi_buffer);

	if (filter->effect)
		gs_effect_destroy(filter->effect);

	if (g_filter_data == filter)
		g_filter_data = NULL;

	bfree(filter);
	obs_log(LOG_INFO, "Filter destroyed");
}

static void filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
}

static obs_properties_t *filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_properties_create();
}

static struct obs_source_frame *filter_video(void *data, struct obs_source_frame *frame)
{
	UNUSED_PARAMETER(data);
	return frame;
}

static void update_roi_texture(my_filter_data_t *filter, uint32_t width, uint32_t height)
{
	if (filter->width != width || filter->height != height || !filter->roi_texture) {
		if (filter->roi_texture)
			gs_texture_destroy(filter->roi_texture);

		filter->roi_texture = gs_texture_create(ROI_SIZE, ROI_SIZE, GS_RGBA, 1, NULL, GS_DYNAMIC);

		pthread_mutex_lock(&filter->roi_mutex);
		if (filter->roi_buffer)
			bfree(filter->roi_buffer);
		filter->roi_buffer = bzalloc(ROI_SIZE * ROI_SIZE * 4);
		filter->roi_ready = false;
		pthread_mutex_unlock(&filter->roi_mutex);

		filter->width = width;
		filter->height = height;

		obs_log(LOG_INFO, "ROI texture updated: %ux%u -> %ux%u", width, height, ROI_SIZE, ROI_SIZE);
	}
}

static void render_roi_to_texture(my_filter_data_t *filter, gs_texture_t *source_texture)
{
	if (!filter->roi_texture || !source_texture)
		return;

	gs_set_render_target(filter->roi_texture, NULL);
	gs_ortho(0.0f, (float)ROI_SIZE, 0.0f, (float)ROI_SIZE, -100.0f, 100.0f);

	struct vec4 image_size;
	vec4_set(&image_size, (float)filter->width, (float)filter->height,
		 1.0f / (float)filter->width, 1.0f / (float)filter->height);

	struct vec4 roi_size;
	vec4_set(&roi_size, (float)ROI_SIZE, (float)ROI_SIZE, 0.0f, 0.0f);

	struct vec4 roi_center;
	vec4_set(&roi_center, (float)filter->width / 2.0f, (float)filter->height / 2.0f, 0.0f, 0.0f);

	gs_effect_set_vec4(filter->param_image_size, &image_size);
	gs_effect_set_vec4(filter->param_roi_size, &roi_size);
	gs_effect_set_vec4(filter->param_roi_center, &roi_center);

	gs_technique_begin(filter->tech_normal);
	gs_technique_begin_pass(filter->tech_normal, 0);

	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_y"), source_texture);
	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_uv"), source_texture);

	gs_draw_sprite(source_texture, 0, filter->width, filter->height);

	gs_technique_end_pass(filter->tech_normal);
	gs_technique_end(filter->tech_normal);
}

static void copy_roi_to_cpu(my_filter_data_t *filter)
{
	if (!filter->roi_texture || !filter->roi_buffer)
		return;

	uint8_t *data;
	uint32_t linesize;

	if (gs_texture_map(filter->roi_texture, &data, &linesize)) {
		pthread_mutex_lock(&filter->roi_mutex);

		for (int y = 0; y < ROI_SIZE; y++) {
			uint8_t *src = data + y * linesize;
			uint8_t *dst = filter->roi_buffer + y * ROI_SIZE * 4;
			memcpy(dst, src, ROI_SIZE * 4);
		}

		filter->roi_ready = true;

		pthread_mutex_unlock(&filter->roi_mutex);

		gs_texture_unmap(filter->roi_texture);
	}
}

static void video_render(void *data, gs_effect_t *effect)
{
	my_filter_data_t *filter = data;

	if (!filter)
		return;

	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target)
		return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	if (width == 0 || height == 0)
		return;

	update_roi_texture(filter, width, height);

	if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;

	struct vec4 image_size;
	vec4_set(&image_size, (float)width, (float)height,
		 1.0f / (float)width, 1.0f / (float)height);

	struct vec4 roi_size;
	vec4_set(&roi_size, (float)ROI_SIZE, (float)ROI_SIZE, 0.0f, 0.0f);

	struct vec4 roi_center;
	vec4_set(&roi_center, (float)width / 2.0f, (float)height / 2.0f, 0.0f, 0.0f);

	struct vec4 box_color;
	vec4_set(&box_color, 1.0f, 0.0f, 0.0f, 1.0f);

	gs_effect_set_vec4(filter->param_image_size, &image_size);
	gs_effect_set_vec4(filter->param_roi_size, &roi_size);
	gs_effect_set_vec4(filter->param_roi_center, &roi_center);
	gs_effect_set_vec4(filter->param_box_color, &box_color);
	gs_effect_set_float(filter->param_box_width, 1.0f);

	gs_technique_begin(filter->tech_normal);
	gs_technique_begin_pass(filter->tech_normal, 0);

	gs_draw_sprite(NULL, 0, width, height);

	gs_technique_end_pass(filter->tech_normal);
	gs_technique_end(filter->tech_normal);

	obs_source_process_filter_end(filter->context, effect, width, height);

	gs_texture_t *source_texture = obs_filter_get_video_texture(filter->context);
	if (source_texture) {
		render_roi_to_texture(filter, source_texture);
		copy_roi_to_cpu(filter);
	}

	filter->frame_count++;

	if (filter->frame_count % 300 == 0) {
		obs_log(LOG_INFO,
			"[MYFILTER] video_render running | %ux%u | GPU=true | ROI=%s",
			width, height,
			filter->roi_ready ? "ready" : "not ready");
	}
}

bool get_center_roi(uint8_t **out_data, int *out_w, int *out_h)
{
	if (!g_filter_data || !g_filter_data->roi_ready) {
		return false;
	}

	pthread_mutex_lock(&g_filter_data->roi_mutex);

	if (g_filter_data->roi_buffer) {
		*out_data = g_filter_data->roi_buffer;
		*out_w = ROI_SIZE;
		*out_h = ROI_SIZE;
		pthread_mutex_unlock(&g_filter_data->roi_mutex);
		return true;
	}

	pthread_mutex_unlock(&g_filter_data->roi_mutex);
	return false;
}

static struct obs_source_info filter_info = {
	.id = "my_first_obs_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,

	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.update = filter_update,
	.get_properties = filter_properties,
	.filter_video = filter_video,
	.video_render = video_render,
};

bool obs_module_load(void)
{
	obs_register_source(&filter_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
