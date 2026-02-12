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
#include <util/darray.h>

#define ROI_SIZE 320

static my_filter_data_t *g_filter_data = NULL;

typedef struct {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *param_image_size;
	gs_eparam_t *param_roi_size;
	gs_eparam_t *param_roi_center;
	gs_eparam_t *param_box_color;
	gs_eparam_t *param_box_width;
	gs_technique_t *tech_normal;
	gs_technique_t *tech_roi;

	gs_texture_t *output_texture;
	gs_texture_t *roi_texture;

	uint32_t width;
	uint32_t height;

	int frame_count;
	pthread_mutex_t roi_mutex;
	uint8_t *roi_data_cpu;
	bool roi_ready;
} my_filter_data_t;

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
	data->roi_data_cpu = NULL;
	pthread_mutex_init(&data->roi_mutex, NULL);

	char *effect_path = obs_module_file("center_roi_gpu.effect");
	if (!effect_path) {
		obs_log(LOG_ERROR, "Failed to find center_roi_gpu.effect");
		bfree(data);
		return NULL;
	}

	data->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);

	if (!data->effect) {
		obs_log(LOG_ERROR, "Failed to create effect from center_roi_gpu.effect");
		bfree(data);
		return NULL;
	}

	data->param_image_size = gs_effect_get_param_by_name(data->effect, "image_size");
	data->param_roi_size = gs_effect_get_param_by_name(data->effect, "roi_size");
	data->param_roi_center = gs_effect_get_param_by_name(data->effect, "roi_center");
	data->param_box_color = gs_effect_get_param_by_name(data->effect, "box_color");
	data->param_box_width = gs_effect_get_param_by_name(data->effect, "box_width");

	data->tech_normal = gs_effect_get_technique(data->effect, "NormalRender");
	data->tech_roi = gs_effect_get_technique(data->effect, "ROIRender");

	if (!data->tech_normal || !data->tech_roi) {
		obs_log(LOG_ERROR, "Failed to get techniques from effect");
		gs_effect_destroy(data->effect);
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

	if (filter->output_texture)
		gs_texture_destroy(filter->output_texture);

	if (filter->roi_texture)
		gs_texture_destroy(filter->roi_texture);

	if (filter->roi_data_cpu)
		bfree(filter->roi_data_cpu);

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

static void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	UNUSED_PARAMETER(data);
}

static struct gs_texture *get_nv12_plane(gs_texture_t *tex, uint32_t plane)
{
	if (!tex)
		return NULL;

	enum gs_color_format format = gs_texture_get_color_format(tex);

	if (format == GS_NV12) {
		return gs_texture_get_plane(tex, plane);
	}

	return tex;
}

static void update_textures(my_filter_data_t *filter, uint32_t width, uint32_t height)
{
	if (filter->width != width || filter->height != height ||
	    !filter->output_texture || !filter->roi_texture) {

		if (filter->output_texture)
			gs_texture_destroy(filter->output_texture);

		if (filter->roi_texture)
			gs_texture_destroy(filter->roi_texture);

		filter->output_texture = gs_texture_create(
			width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);

		filter->roi_texture = gs_texture_create(
			ROI_SIZE, ROI_SIZE, GS_RGBA, 1, NULL, GS_DYNAMIC);

		pthread_mutex_lock(&filter->roi_mutex);
		if (filter->roi_data_cpu)
			bfree(filter->roi_data_cpu);
		filter->roi_data_cpu = bzalloc(ROI_SIZE * ROI_SIZE * 4);
		pthread_mutex_unlock(&filter->roi_mutex);

		filter->width = width;
		filter->height = height;

		obs_log(LOG_INFO, "Textures updated: %ux%u", width, height);
	}
}

static void render_with_box(my_filter_data_t *filter, gs_texture_t *tex_y,
			    gs_texture_t *tex_uv)
{
	struct vec4 image_size;
	vec4_set(&image_size, (float)filter->width, (float)filter->height,
		 1.0f / (float)filter->width, 1.0f / (float)filter->height);

	struct vec4 roi_size;
	vec4_set(&roi_size, (float)ROI_SIZE, (float)ROI_SIZE, 0.0f, 0.0f);

	struct vec4 roi_center;
	vec4_set(&roi_center, (float)filter->width / 2.0f,
		 (float)filter->height / 2.0f, 0.0f, 0.0f);

	struct vec4 box_color;
	vec4_set(&box_color, 1.0f, 0.0f, 0.0f, 1.0f);

	gs_effect_set_vec4(filter->param_image_size, &image_size);
	gs_effect_set_vec4(filter->param_roi_size, &roi_size);
	gs_effect_set_vec4(filter->param_roi_center, &roi_center);
	gs_effect_set_vec4(filter->param_box_color, &box_color);
	gs_effect_set_float(filter->param_box_width, 1.0f);

	gs_technique_begin(filter->tech_normal);
	gs_technique_begin_pass(filter->tech_normal, 0);

	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_y"),
			     tex_y);
	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_uv"),
			     tex_uv);

	gs_draw_sprite(tex_y, 0, filter->width, filter->height);

	gs_technique_end_pass(filter->tech_normal);
	gs_technique_end(filter->tech_normal);
}

static void render_roi_only(my_filter_data_t *filter, gs_texture_t *tex_y,
			    gs_texture_t *tex_uv)
{
	struct vec4 image_size;
	vec4_set(&image_size, (float)filter->width, (float)filter->height,
		 1.0f / (float)filter->width, 1.0f / (float)filter->height);

	struct vec4 roi_size;
	vec4_set(&roi_size, (float)ROI_SIZE, (float)ROI_SIZE, 0.0f, 0.0f);

	struct vec4 roi_center;
	vec4_set(&roi_center, (float)filter->width / 2.0f,
		 (float)filter->height / 2.0f, 0.0f, 0.0f);

	gs_effect_set_vec4(filter->param_image_size, &image_size);
	gs_effect_set_vec4(filter->param_roi_size, &roi_size);
	gs_effect_set_vec4(filter->param_roi_center, &roi_center);

	gs_technique_begin(filter->tech_roi);
	gs_technique_begin_pass(filter->tech_roi, 0);

	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_y"),
			     tex_y);
	gs_effect_set_texture(gs_effect_get_param_by_name(filter->effect, "tex_uv"),
			     tex_uv);

	gs_draw_sprite(tex_y, 0, filter->width, filter->height);

	gs_technique_end_pass(filter->tech_roi);
	gs_technique_end(filter->tech_roi);
}

static void copy_roi_to_cpu(my_filter_data_t *filter)
{
	if (!filter->roi_texture || !filter->roi_data_cpu)
		return;

	gs_texture_map(filter->roi_texture, NULL);

	pthread_mutex_lock(&filter->roi_mutex);

	if (gs_texture_get_color_format(filter->roi_texture) == GS_RGBA) {
		gs_texture_get_image(filter->roi_texture, filter->roi_data_cpu,
				     ROI_SIZE * ROI_SIZE * 4, 0);
	}

	filter->roi_ready = true;

	pthread_mutex_unlock(&filter->roi_mutex);
}

static struct gs_texture *filter_video_gpu(void *data, struct gs_texture *tex)
{
	my_filter_data_t *filter = data;

	if (!tex || !filter)
		return tex;

	enum gs_color_format format = gs_texture_get_color_format(tex);
	uint32_t width = gs_texture_get_width(tex);
	uint32_t height = gs_texture_get_height(tex);

	update_textures(filter, width, height);

	gs_texture_t *tex_y = get_nv12_plane(tex, 0);
	gs_texture_t *tex_uv = get_nv12_plane(tex, 1);

	if (!tex_y || !tex_uv) {
		obs_log(LOG_WARNING, "Failed to get NV12 planes");
		return tex;
	}

	gs_set_render_target(filter->output_texture, NULL);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	render_with_box(filter, tex_y, tex_uv);

	gs_set_render_target(filter->roi_texture, NULL);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	render_roi_only(filter, tex_y, tex_uv);

	copy_roi_to_cpu(filter);

	filter->frame_count++;

	if (filter->frame_count % 300 == 0) {
		obs_log(LOG_INFO,
			"[MYFILTER] filter_video_gpu running | %ux%u | format=%d | GPU=true | ROI=%s",
			width, height, format,
			filter->roi_ready ? "ready" : "not ready");
	}

	gs_set_render_target(NULL, NULL);

	return filter->output_texture;
}

bool get_center_roi_gpu_uint8(uint8_t **out_data, int *out_w, int *out_h)
{
	if (!g_filter_data || !g_filter_data->roi_ready) {
		return false;
	}

	pthread_mutex_lock(&g_filter_data->roi_mutex);

	if (g_filter_data->roi_data_cpu) {
		*out_data = g_filter_data->roi_data_cpu;
		*out_w = ROI_SIZE;
		*out_h = ROI_SIZE;
		pthread_mutex_unlock(&g_filter_data->roi_mutex);
		return true;
	}

	pthread_mutex_unlock(&g_filter_data->roi_mutex);
	return false;
}

bool get_center_roi_gpu(float **out_data, int *out_w, int *out_h)
{
	if (!g_filter_data || !g_filter_data->roi_ready) {
		return false;
	}

	pthread_mutex_lock(&g_filter_data->roi_mutex);

	if (g_filter_data->roi_data_cpu) {
		static float *normalized_data = NULL;
		static int normalized_size = 0;

		int required_size = ROI_SIZE * ROI_SIZE * 3;

		if (normalized_size < required_size) {
			if (normalized_data)
				bfree(normalized_data);
			normalized_data = bzalloc(required_size * sizeof(float));
			normalized_size = required_size;
		}

		for (int i = 0; i < ROI_SIZE * ROI_SIZE; i++) {
			uint8_t r = g_filter_data->roi_data_cpu[i * 4 + 0];
			uint8_t g = g_filter_data->roi_data_cpu[i * 4 + 1];
			uint8_t b = g_filter_data->roi_data_cpu[i * 4 + 2];

			normalized_data[i * 3 + 0] = r / 255.0f;
			normalized_data[i * 3 + 1] = g / 255.0f;
			normalized_data[i * 3 + 2] = b / 255.0f;
		}

		*out_data = normalized_data;
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
	.video_render = filter_video_render,
	.filter_video_gpu = filter_video_gpu,
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
