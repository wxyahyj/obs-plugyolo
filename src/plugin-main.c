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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ROI_W 320
#define ROI_H 320

/* ------------------------------------------------------------------------- */
/* OBS module boilerplate */

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ------------------------------------------------------------------------- */
/* 滤镜上下文 */

struct my_filter_data {
    obs_source_t *context;

    uint64_t frame_count;

    uint8_t *roi_rgb; /* ROI 输出缓冲区 */
    int roi_w;
    int roi_h;
};

/* ------------------------------------------------------------------------- */
/* 工具函数 */

static inline int clamp_int(int v, int min_v, int max_v)
{
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

/* NV12 → RGB（BT.601） */
static inline void yuv_to_rgb(uint8_t Y, uint8_t U, uint8_t V,
                              uint8_t *R, uint8_t *G, uint8_t *B)
{
    int C = (int)Y - 16;
    int D = (int)U - 128;
    int E = (int)V - 128;

    int r = (298 * C + 409 * E + 128) >> 8;
    int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    int b = (298 * C + 516 * D + 128) >> 8;

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    *R = (uint8_t)r;
    *G = (uint8_t)g;
    *B = (uint8_t)b;
}

/* ------------------------------------------------------------------------- */
/* 在 NV12 中心画 320×320 框 */

static void draw_center_box_nv12(struct obs_source_frame *frame,
                                 int box_w, int box_h)
{
    if (!frame || frame->format != VIDEO_FORMAT_NV12)
        return;

    int width  = (int)frame->width;
    int height = (int)frame->height;

    uint8_t *Yp  = frame->data[0];
    uint8_t *UVp = frame->data[1];
    int ys = frame->linesize[0];
    int uvs = frame->linesize[1];

    int cx = width / 2;
    int cy = height / 2;

    int left   = cx - box_w / 2;
    int right  = left + box_w - 1;
    int top    = cy - box_h / 2;
    int bottom = top + box_h - 1;

    left   = clamp_int(left,   0, width - 1);
    right  = clamp_int(right,  0, width - 1);
    top    = clamp_int(top,    0, height - 1);
    bottom = clamp_int(bottom, 0, height - 1);

    /* 红色（近似） */
    const uint8_t Yv = 81;
    const uint8_t Uv = 90;
    const uint8_t Vv = 240;

    const int thickness = 2;

    /* 画水平线 */
    for (int t = 0; t < thickness; t++) {
        int y1 = top + t;
        int y2 = bottom - t;

        if (y1 >= 0 && y1 < height) {
            uint8_t *row = Yp + y1 * ys;
            for (int x = left; x <= right; x++) {
                row[x] = Yv;
                uint8_t *uv = UVp + (y1 / 2) * uvs + (x / 2) * 2;
                uv[0] = Uv; uv[1] = Vv;
            }
        }

        if (y2 >= 0 && y2 < height && y2 != y1) {
            uint8_t *row = Yp + y2 * ys;
            for (int x = left; x <= right; x++) {
                row[x] = Yv;
                uint8_t *uv = UVp + (y2 / 2) * uvs + (x / 2) * 2;
                uv[0] = Uv; uv[1] = Vv;
            }
        }
    }

    /* 画竖线 */
    for (int t = 0; t < thickness; t++) {
        int x1 = left + t;
        int x2 = right - t;

        if (x1 >= 0 && x1 < width) {
            for (int y = top; y <= bottom; y++) {
                uint8_t *row = Yp + y * ys;
                row[x1] = Yv;
                uint8_t *uv = UVp + (y / 2) * uvs + (x1 / 2) * 2;
                uv[0] = Uv; uv[1] = Vv;
            }
        }

        if (x2 >= 0 && x2 < width && x2 != x1) {
            for (int y = top; y <= bottom; y++) {
                uint8_t *row = Yp + y * ys;
                row[x2] = Yv;
                uint8_t *uv = UVp + (y / 2) * uvs + (x2 / 2) * 2;
                uv[0] = Uv; uv[1] = Vv;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 提取中心 ROI（NV12 → RGB） */

static bool extract_center_roi_rgb(const struct obs_source_frame *frame,
                                   int roi_w, int roi_h,
                                   uint8_t *out)
{
    if (!frame || !out)
        return false;

    if (frame->format != VIDEO_FORMAT_NV12)
        return false;

    int width  = frame->width;
    int height = frame->height;

    if (roi_w > width || roi_h > height)
        return false;

    const uint8_t *Yp  = frame->data[0];
    const uint8_t *UVp = frame->data[1];
    int ys  = frame->linesize[0];
    int uvs = frame->linesize[1];

    int cx = width / 2;
    int cy = height / 2;

    int left = clamp_int(cx - roi_w / 2, 0, width  - roi_w);
    int top  = clamp_int(cy - roi_h / 2, 0, height - roi_h);

    for (int j = 0; j < roi_h; j++) {
        for (int i = 0; i < roi_w; i++) {

            int x = left + i;
            int y = top  + j;

            uint8_t Y = Yp[y * ys + x];

            const uint8_t *uv = UVp + (y / 2) * uvs + (x / 2) * 2;
            uint8_t U = uv[0];
            uint8_t V = uv[1];

            uint8_t R, G, B;
            yuv_to_rgb(Y, U, V, &R, &G, &B);

            int idx = (j * roi_w + i) * 3;
            out[idx + 0] = R;
            out[idx + 1] = G;
            out[idx + 2] = B;
        }
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* 滤镜：名字 */

static const char *filter_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "操作显示";
}

/* ------------------------------------------------------------------------- */
/* create / destroy */

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(settings);

    struct my_filter_data *f = bzalloc(sizeof(*f));
    f->context = source;
    f->frame_count = 0;

    f->roi_w = ROI_W;
    f->roi_h = ROI_H;
    f->roi_rgb = bmalloc(ROI_W * ROI_H * 3);

    return f;
}

static void filter_destroy(void *data)
{
    struct my_filter_data *f = data;
    if (!f) return;

    if (f->roi_rgb)
        bfree(f->roi_rgb);

    bfree(f);
}

/* ------------------------------------------------------------------------- */
/* filter_video：核心逻辑 */

static struct obs_source_frame *filter_video(void *data,
                                             struct obs_source_frame *frame)
{
    struct my_filter_data *f = data;
    if (!f || !frame)
        return frame;

    f->frame_count++;

    bool roi_ok = false;

    if (frame->format == VIDEO_FORMAT_NV12) {

        /* 阶段 1：画框 */
        draw_center_box_nv12(frame, f->roi_w, f->roi_h);

        /* 阶段 2：提取 ROI */
        if (f->roi_rgb)
            roi_ok = extract_center_roi_rgb(frame, f->roi_w, f->roi_h, f->roi_rgb);
    }

    /* 阶段 3：日志 */
    if (f->frame_count % 300 == 0) {
        obs_log(LOG_INFO,
            "[MYFILTER] frame=%llu | %ux%u | fmt=%d | ROI=%s",
            (unsigned long long)f->frame_count,
            frame->width, frame->height,
            frame->format,
            roi_ok ? "OK" : "FAIL");
    }

    return frame;
}

/* ------------------------------------------------------------------------- */
/* properties / update */

static obs_properties_t *filter_properties(void *data)
{
    UNUSED_PARAMETER(data);
    return obs_properties_create();
}

static void filter_update(void *data, obs_data_t *settings)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(settings);
}

/* ------------------------------------------------------------------------- */
/* 滤镜类型定义 */

static struct obs_source_info filter_info = {
    .id = "my_first_obs_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,

    .get_name = filter_get_name,
    .create = filter_create,
    .destroy = filter_destroy,
    .update = filter_update,
    .get_properties = filter_properties,
    .filter_video = filter_video,
};

/* ------------------------------------------------------------------------- */
/* 模块加载 / 卸载 */

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
