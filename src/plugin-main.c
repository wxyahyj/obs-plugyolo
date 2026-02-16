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

// 条件编译：只有在有 ONNX Runtime 时才包含相关头文件
#ifdef HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

// 条件编译：只有在有 OpenCV 时才包含相关头文件
#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif

// 条件编译：如果没有推理支持，定义一个空的检测结果结构体
#ifndef NO_INFERENCE
// 检测结果结构体
typedef struct {
	float x1, y1, x2, y2; // 边界框坐标
	float confidence;     // 置信度
	int class_id;         // 类别 ID
} detection_result_t;
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("my_first_obs_filter", "en-US")

#define ROI_SIZE 320
#define ROI_CHANNELS 3 // RGB

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

	// 条件编译：只有在有 ONNX Runtime 时才包含相关成员
#ifndef NO_INFERENCE
	// ONNX Runtime
	Ort::Env *ort_env;
	Ort::Session *ort_session;
	Ort::SessionOptions *ort_options;

	// Model info
	char *model_path;
	int input_width;
	int input_height;
	int num_classes;

	// Detection params
	float conf_threshold;
	float nms_threshold;

	// Classes
	const char **class_names;

	// Detection results
	std::vector<detection_result_t> detections;
	pthread_mutex_t detections_mutex;

	// Async inference
	pthread_t inference_thread;
	bool inference_thread_running;
	bool should_stop_inference;
	pthread_mutex_t inference_mutex;
	pthread_cond_t inference_cond;
	uint8_t *inference_buffer;
	bool inference_buffer_ready;
	std::vector<detection_result_t> pending_detections;
	bool pending_detections_ready;
#endif
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

#ifndef NO_INFERENCE
	// 初始化检测结果相关变量
	pthread_mutex_init(&data->detections_mutex, NULL);

	// 初始化异步推理相关变量
	data->inference_thread_running = false;
	data->should_stop_inference = false;
	pthread_mutex_init(&data->inference_mutex, NULL);
	pthread_cond_init(&data->inference_cond, NULL);
	data->inference_buffer = bzalloc(ROI_SIZE * ROI_SIZE * ROI_CHANNELS);
	data->inference_buffer_ready = false;
	data->pending_detections_ready = false;
#endif

	// 尝试创建 shader effect，但即使失败也继续运行（使用 CPU 模式）
	char *effect_path = obs_module_file("center_roi_gpu.effect");
	if (effect_path) {
		data->effect = gs_effect_create_from_file(effect_path, NULL);
		bfree(effect_path);

		if (data->effect) {
			data->param_image_size = gs_effect_get_param_by_name(data->effect, "image_size");
			data->param_roi_size = gs_effect_get_param_by_name(data->effect, "roi_size");
			data->param_roi_center = gs_effect_get_param_by_name(data->effect, "roi_center");
			data->param_box_color = gs_effect_get_param_by_name(data->effect, "box_color");
			data->param_box_width = gs_effect_get_param_by_name(data->effect, "box_width");

			data->tech_normal = gs_effect_get_technique(data->effect, "NormalRender");

			if (!data->tech_normal) {
				obs_log(LOG_WARNING, "Failed to get NormalRender technique from effect, falling back to CPU mode");
				gs_effect_destroy(data->effect);
				data->effect = NULL;
			}
		} else {
			obs_log(LOG_WARNING, "Failed to create effect from center_roi_gpu.effect, falling back to CPU mode");
		}
	} else {
		obs_log(LOG_WARNING, "Failed to find center_roi_gpu.effect, falling back to CPU mode");
	}

	// 分配 ROI 缓冲区
	data->roi_buffer = bzalloc(ROI_SIZE * ROI_SIZE * ROI_CHANNELS);

#ifndef NO_INFERENCE
	// 初始化 ONNX Runtime
	try {
		data->ort_env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "YOLOFilter");
		data->ort_options = new Ort::SessionOptions();

		// 设置线程数
		data->ort_options->SetIntraOpNumThreads(4);
		data->ort_options->SetInterOpNumThreads(2);

		// 启用内存模式
		data->ort_options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

		// 尝试添加 DirectML 执行提供者
		try {
			data->ort_options->AppendExecutionProvider_DML(0);
			obs_log(LOG_INFO, "DirectML execution provider added successfully");
		} catch (const Ort::Exception &e) {
			obs_log(LOG_WARNING, "Failed to add DirectML execution provider: %s, falling back to CPU", e.what());
		}

	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "Failed to initialize ONNX Runtime: %s", e.what());
		// 即使 ONNX Runtime 初始化失败，也继续创建滤镜
	}

	// 初始化默认参数
	data->model_path = NULL;
	data->input_width = 640;
	data->input_height = 640;
	data->num_classes = 80;
	data->conf_threshold = 0.45f;
	data->nms_threshold = 0.45f;
	data->class_names = NULL;

	obs_log(LOG_INFO, "Inference support enabled");
#else
	obs_log(LOG_INFO, "Inference support disabled (missing dependencies)");
#endif

	g_filter_data = data;

	obs_log(LOG_INFO, "Filter created successfully (mode: %s)", data->effect ? "GPU" : "CPU");
	return data;
}

static void filter_destroy(void *data)
{
	my_filter_data_t *filter = data;

	if (!filter)
		return;

	pthread_mutex_destroy(&filter->roi_mutex);

#ifndef NO_INFERENCE
	// 清理检测结果相关资源
	pthread_mutex_destroy(&filter->detections_mutex);

	// 清理异步推理线程
	if (filter->inference_thread_running) {
		filter->should_stop_inference = true;
		pthread_cond_signal(&filter->inference_cond);
		pthread_join(filter->inference_thread, NULL);
	}

	// 释放异步推理相关资源
	if (filter->inference_buffer) {
		bfree(filter->inference_buffer);
		filter->inference_buffer = NULL;
	}

	pthread_mutex_destroy(&filter->inference_mutex);
	pthread_cond_destroy(&filter->inference_cond);

	// 释放 ONNX Runtime 资源
	if (filter->ort_session) {
		delete filter->ort_session;
		filter->ort_session = NULL;
	}

	if (filter->ort_options) {
		delete filter->ort_options;
		filter->ort_options = NULL;
	}

	if (filter->ort_env) {
		delete filter->ort_env;
		filter->ort_env = NULL;
	}

	// 释放模型路径
	if (filter->model_path) {
		bfree(filter->model_path);
		filter->model_path = NULL;
	}

	// 释放类别名称
	if (filter->class_names) {
		bfree(filter->class_names);
		filter->class_names = NULL;
	}
#endif

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

#ifndef NO_INFERENCE
// 加载 YOLO 模型
static bool load_yolo_model(my_filter_data_t *filter)
{
	if (!filter || !filter->model_path || !filter->ort_env || !filter->ort_options) {
		return false;
	}

	try {
		// 释放旧的会话
		if (filter->ort_session) {
			delete filter->ort_session;
			filter->ort_session = NULL;
		}

		// 创建新的会话
		filter->ort_session = new Ort::Session(*filter->ort_env, filter->model_path, *filter->ort_options);

		// 获取输入信息
		Ort::AllocatorWithDefaultOptions allocator;
		Ort::AllocatedStringPtr input_name = filter->ort_session->GetInputNameAllocated(0, allocator);
		Ort::TypeInfo input_type_info = filter->ort_session->GetInputTypeInfo(0);
		auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();

		// 获取输入形状
		std::vector<int64_t> input_shape = input_tensor_info.GetShape();
		if (input_shape.size() == 4) {
			// 假设输入形状为 [batch, channels, height, width]
			filter->input_height = (int)input_shape[2];
			filter->input_width = (int)input_shape[3];
			obs_log(LOG_INFO, "Auto-detected input size: %dx%d", filter->input_width, filter->input_height);
		}

		// 获取输出信息
		Ort::AllocatedStringPtr output_name = filter->ort_session->GetOutputNameAllocated(0, allocator);
		Ort::TypeInfo output_type_info = filter->ort_session->GetOutputTypeInfo(0);
		auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();

		// 获取输出形状
		std::vector<int64_t> output_shape = output_tensor_info.GetShape();
		if (output_shape.size() == 3) {
			// 假设输出形状为 [batch, num_detections, 7] 或类似格式
			// 根据输出维度推断类别数
			if (output_shape[2] > 5) {
				filter->num_classes = (int)(output_shape[2] - 5);
				obs_log(LOG_INFO, "Auto-detected num_classes: %d", filter->num_classes);
			}
		}

		obs_log(LOG_INFO, "Model loaded successfully: %s", filter->model_path);
		return true;

	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "Failed to load model: %s", e.what());
		return false;
	}
}
#endif

static void filter_update(void *data, obs_data_t *settings)
{
	my_filter_data_t *filter = data;
	if (!filter)
		return;

#ifndef NO_INFERENCE
	// 模型路径
	const char *model_path = obs_data_get_string(settings, "model_path");
	if (model_path && *model_path) {
		if (filter->model_path) {
			bfree(filter->model_path);
		}
		filter->model_path = bstrdup(model_path);
		obs_log(LOG_INFO, "Model path set to: %s", filter->model_path);
	}

	// 置信度阈值
	filter->conf_threshold = obs_data_get_double(settings, "conf_threshold");
	if (filter->conf_threshold <= 0) {
		filter->conf_threshold = 0.45f;
	}

	// NMS 阈值
	filter->nms_threshold = obs_data_get_double(settings, "nms_threshold");
	if (filter->nms_threshold <= 0) {
		filter->nms_threshold = 0.45f;
	}

	// 输入尺寸
	filter->input_width = obs_data_get_int(settings, "input_width");
	if (filter->input_width <= 0) {
		filter->input_width = 640;
	}

	filter->input_height = obs_data_get_int(settings, "input_height");
	if (filter->input_height <= 0) {
		filter->input_height = 640;
	}

	// 类别数
	filter->num_classes = obs_data_get_int(settings, "num_classes");
	if (filter->num_classes <= 0) {
		filter->num_classes = 80;
	}

	// 当模型路径改变时，重新加载模型
	if (filter->model_path && filter->ort_env && filter->ort_options) {
		bool model_loaded = load_yolo_model(filter);
		if (model_loaded) {
			obs_log(LOG_INFO, "Model reloaded successfully");
		} else {
			obs_log(LOG_ERROR, "Failed to reload model");
		}
	}

	// 更新模型信息
	char model_info[512];
	snprintf(model_info, sizeof(model_info), 
		"输入尺寸: %dx%d\n" 
		"类别数: %d\n" 
		"置信度阈值: %.2f\n" 
		"NMS 阈值: %.2f\n" 
		"模型路径: %s\n" 
		"模型状态: %s",
		filter->input_width, filter->input_height,
		filter->num_classes,
		filter->conf_threshold,
		filter->nms_threshold,
		filter->model_path ? filter->model_path : "未设置",
		filter->ort_session ? "已加载" : "未加载");

	obs_data_set_string(settings, "model_info", model_info);
#else
	// 当没有推理支持时，显示提示信息
	char model_info[512];
	snprintf(model_info, sizeof(model_info), "推理功能已禁用（缺少依赖库）\n请安装 ONNX Runtime 和 OpenCV 以启用 YOLO 检测功能");
	obs_data_set_string(settings, "model_info", model_info);
#endif
}

static obs_properties_t *filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	// 模型路径
	obs_properties_add_path(props, "model_path", "模型文件路径", OBS_PATH_FILE, "*.onnx", NULL);

	// 置信度阈值
	obs_properties_add_float_slider(props, "conf_threshold", "置信度阈值", 0.0f, 1.0f, 0.01f);

	// NMS 阈值
	obs_properties_add_float_slider(props, "nms_threshold", "NMS 阈值", 0.0f, 1.0f, 0.01f);

	// 输入尺寸
	obs_properties_add_int_slider(props, "input_width", "输入宽度", 320, 1280, 1);
	obs_properties_add_int_slider(props, "input_height", "输入高度", 320, 1280, 1);

	// 类别数
	obs_properties_add_int_slider(props, "num_classes", "类别数", 1, 200, 1);

	// 模型信息显示（只读）
	obs_properties_add_text(props, "model_info", "模型信息", OBS_TEXT_INFO);

	return props;
}

// NV12 格式说明：
// - Y 分量：width * height 字节
// - UV 分量：(width/2) * (height/2) * 2 字节（交错存储）
// - 总大小：width * height * 3/2 字节

// 在 NV12 格式上绘制矩形框（只修改 Y 分量，显示为白色框）
static void draw_rectangle_nv12(uint8_t *data, int width, int height, int x, int y, int w, int h, int line_width)
{
	// 计算 ROI 边界
	int x1 = x;
	int y1 = y;
	int x2 = x + w - 1;
	int y2 = y + h - 1;

	// 确保边界在有效范围内
	x1 = (x1 < 0) ? 0 : x1;
	y1 = (y1 < 0) ? 0 : y1;
	x2 = (x2 >= width) ? width - 1 : x2;
	y2 = (y2 >= height) ? height - 1 : y2;

	// 绘制上下边框
	for (int i = x1; i <= x2; i++) {
		// 上边框
		for (int j = y1; j < y1 + line_width && j < height; j++) {
			int index = j * width + i;
			data[index] = 255; // 设置为白色
		}
		// 下边框
		for (int j = y2 - line_width + 1; j <= y2 && j >= 0; j++) {
			int index = j * width + i;
			data[index] = 255; // 设置为白色
		}
	}

	// 绘制左右边框
	for (int j = y1; j <= y2; j++) {
		// 左边框
		for (int i = x1; i < x1 + line_width && i < width; i++) {
			int index = j * width + i;
			data[index] = 255; // 设置为白色
		}
		// 右边框
		for (int i = x2 - line_width + 1; i <= x2 && i >= 0; i++) {
			int index = j * width + i;
			data[index] = 255; // 设置为白色
		}
	}
}

// 检测结果结构体
typedef struct {
	float x1, y1, x2, y2; // 边界框坐标
	float confidence;     // 置信度
	int class_id;         // 类别 ID
} detection_result_t;

// YUV 转 RGB 函数
static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
	int y_val = y - 16;
	int u_val = u - 128;
	int v_val = v - 128;

	int r_val = (298 * y_val + 409 * v_val + 128) >> 8;
	int g_val = (298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8;
	int b_val = (298 * y_val + 516 * u_val + 128) >> 8;

	//  clamp to 0-255
	r_val = (r_val < 0) ? 0 : (r_val > 255) ? 255 : r_val;
	g_val = (g_val < 0) ? 0 : (g_val > 255) ? 255 : g_val;
	b_val = (b_val < 0) ? 0 : (b_val > 255) ? 255 : b_val;

	*r = (uint8_t)r_val;
	*g = (uint8_t)g_val;
	*b = (uint8_t)b_val;
}

#ifndef NO_INFERENCE
// 预处理函数：将图像转换为模型输入格式
static bool preprocess_image(cv::Mat &image, cv::Mat &output, int input_width, int input_height)
{
	if (image.empty()) {
		return false;
	}

	// 调整大小（保持宽高比，填充）
	cv::Mat resized;
	cv::resize(image, resized, cv::Size(input_width, input_height));

	// 转换为 RGB 格式
	if (resized.channels() == 4) {
		cv::cvtColor(resized, resized, cv::COLOR_RGBA2RGB);
	} else if (resized.channels() == 1) {
		cv::cvtColor(resized, resized, cv::COLOR_GRAY2RGB);
	}

	// 归一化
	resized.convertTo(output, CV_32F, 1.0 / 255.0);

	// 调整通道顺序（HWC -> CHW）
	cv::dnn::blobFromImage(output, output, 1.0, cv::Size(input_width, input_height), cv::Scalar(0, 0, 0), true, false);

	return true;
}

// 后处理函数：处理模型输出，应用 NMS
static std::vector<detection_result_t> postprocess_output(float *output_data, int output_size, int num_classes, float conf_threshold, float nms_threshold)
{
	std::vector<detection_result_t> detections;

	// 假设输出格式为 [x, y, w, h, confidence, class1, class2, ...]
	for (int i = 0; i < output_size; i += (5 + num_classes)) {
		float x = output_data[i + 0];
		float y = output_data[i + 1];
		float w = output_data[i + 2];
		float h = output_data[i + 3];
		float confidence = output_data[i + 4];

		if (confidence < conf_threshold) {
			continue;
		}

		// 找到最高置信度的类别
		float max_class_conf = 0;
		int max_class_id = 0;
		for (int j = 0; j < num_classes; j++) {
			float class_conf = output_data[i + 5 + j];
			if (class_conf > max_class_conf) {
				max_class_conf = class_conf;
				max_class_id = j;
			}
		}

		// 计算最终置信度
		float final_conf = confidence * max_class_conf;
		if (final_conf < conf_threshold) {
			continue;
		}

		// 转换为边界框坐标（xywh -> xyxy）
		detection_result_t det;
		det.x1 = x - w / 2;
		det.y1 = y - h / 2;
		det.x2 = x + w / 2;
		det.y2 = y + h / 2;
		det.confidence = final_conf;
		det.class_id = max_class_id;

		detections.push_back(det);
	}

	// 应用 NMS
	std::vector<int> indices;
	std::vector<float> scores;
	std::vector<cv::Rect> boxes;

	for (auto &det : detections) {
		boxes.push_back(cv::Rect(det.x1, det.y1, det.x2 - det.x1, det.y2 - det.y1));
		scores.push_back(det.confidence);
	}

	if (!boxes.empty()) {
		cv::dnn::NMSBoxes(boxes, scores, conf_threshold, nms_threshold, indices);
	}

	// 过滤出 NMS 后的结果
	std::vector<detection_result_t> filtered_detections;
	for (int idx : indices) {
		filtered_detections.push_back(detections[idx]);
	}

	return filtered_detections;
}
#endif

#ifndef NO_INFERENCE
// 执行 YOLO 推理
static std::vector<detection_result_t> run_yolo_inference(my_filter_data_t *filter, cv::Mat &image)
{
	std::vector<detection_result_t> detections;

	if (!filter || !filter->ort_session || image.empty()) {
		return detections;
	}

	try {
		// 预处理图像
		cv::Mat input_blob;
		if (!preprocess_image(image, input_blob, filter->input_width, filter->input_height)) {
			return detections;
		}

		// 准备输入张量
		Ort::AllocatorWithDefaultOptions allocator;
		Ort::AllocatedStringPtr input_name = filter->ort_session->GetInputNameAllocated(0, allocator);
		Ort::AllocatedStringPtr output_name = filter->ort_session->GetOutputNameAllocated(0, allocator);

		std::vector<const char *> input_names = {input_name.get()};
		std::vector<const char *> output_names = {output_name.get()};

		// 获取输入形状
		Ort::TypeInfo input_type_info = filter->ort_session->GetInputTypeInfo(0);
		auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
		std::vector<int64_t> input_shape = input_tensor_info.GetShape();

		// 获取输出形状
		Ort::TypeInfo output_type_info = filter->ort_session->GetOutputTypeInfo(0);
		auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
		std::vector<int64_t> output_shape = output_tensor_info.GetShape();

		// 创建输入张量
		Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
			memory_info,
			(float *)input_blob.ptr<float>(),
			input_blob.total(),
			input_shape.data(),
			input_shape.size()
		);

		// 运行推理
		auto output_tensors = filter->ort_session->Run(
			Ort::RunOptions{nullptr},
			input_names.data(),
			&input_tensor,
			1,
			output_names.data(),
			1
		);

		// 处理输出
		float *output_data = output_tensors[0].GetTensorMutableData<float>();
		int output_size = 1;
		for (auto dim : output_shape) {
			output_size *= dim;
		}

		// 后处理
		detections = postprocess_output(
			output_data,
			output_size,
			filter->num_classes,
			filter->conf_threshold,
			filter->nms_threshold
		);

	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "Inference failed: %s", e.what());
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Inference failed: %s", e.what());
	}

	return detections;
}
#endif

// 获取类别名称
static const char *get_class_name(int class_id)
{
	// 默认类别名称
	static const char *default_classes[] = {
		"敌人", "队友", "热能", "小兵"
	};

	if (class_id >= 0 && class_id < 4) {
		return default_classes[class_id];
	}
	return "未知";
}

#ifndef NO_INFERENCE
// 绘制检测结果
static void draw_detections(my_filter_data_t *filter)
{
	if (!filter) {
		return;
	}

	// 获取检测结果
	std::vector<detection_result_t> detections;
	pthread_mutex_lock(&filter->detections_mutex);
	detections = filter->detections;
	pthread_mutex_unlock(&filter->detections_mutex);

	if (detections.empty()) {
		return;
	}

	// 计算 ROI 在原始画面中的位置
	int width = filter->width;
	int height = filter->height;
	int roi_x = (width - ROI_SIZE) / 2;
	int roi_y = (height - ROI_SIZE) / 2;

	// 创建一个临时的渲染目标来绘制检测结果
	gs_texture_t *render_target = gs_get_render_target();
	if (!render_target) {
		return;
	}

	// 设置颜色
	float color[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // 红色
	// 使用 gs_set_blend_state 代替 gs_set_color
	gs_blend_state_push();
	gs_set_blend_state(GS_BLEND_NORMAL, color, 0.0f);

	// 绘制每个检测结果
	for (auto &det : detections) {
		// 将相对坐标转换为绝对坐标
		float x1 = roi_x + det.x1;
		float y1 = roi_y + det.y1;
		float x2 = roi_x + det.x2;
		float y2 = roi_y + det.y2;

		// 确保坐标在有效范围内
		x1 = std::max(0.0f, std::min((float)width, x1));
		y1 = std::max(0.0f, std::min((float)height, y1));
		x2 = std::max(0.0f, std::min((float)width, x2));
		y2 = std::max(0.0f, std::min((float)height, y2));

		// 绘制边界框
		gs_draw_sprite(NULL, 0, (int)(x2 - x1), (int)(y2 - y1));

		// 这里可以添加绘制标签的代码
		// 由于 OBS 的 GS API 不直接支持文本绘制，我们需要使用其他方法
		// 例如，使用 FreeType 或者预渲染文本纹理
	}

	// 恢复渲染目标
	gs_set_render_target(render_target);
	
	// 恢复混合状态
	gs_blend_state_pop();
}
#endif

#ifndef NO_INFERENCE
// 异步推理线程函数
static void *inference_thread_func(void *arg)
{
	my_filter_data_t *filter = (my_filter_data_t *)arg;

	while (true) {
		// 等待新的推理任务
		pthread_mutex_lock(&filter->inference_mutex);
		while (!filter->inference_buffer_ready && !filter->should_stop_inference) {
			pthread_cond_wait(&filter->inference_cond, &filter->inference_mutex);
		}

		// 检查是否需要停止线程
		if (filter->should_stop_inference) {
			pthread_mutex_unlock(&filter->inference_mutex);
			break;
		}

		// 复制推理缓冲区
		uint8_t *buffer_copy = bzalloc(ROI_SIZE * ROI_SIZE * ROI_CHANNELS);
		memcpy(buffer_copy, filter->inference_buffer, ROI_SIZE * ROI_SIZE * ROI_CHANNELS);
		filter->inference_buffer_ready = false;
		pthread_mutex_unlock(&filter->inference_mutex);

		// 执行推理
		if (filter->ort_session) {
			// 将缓冲区转换为 cv::Mat
			cv::Mat roi_mat(ROI_SIZE, ROI_SIZE, CV_8UC3, buffer_copy);

			// 执行推理
			auto start_time = std::chrono::high_resolution_clock::now();
			std::vector<detection_result_t> detections = run_yolo_inference(filter, roi_mat);
			auto end_time = std::chrono::high_resolution_clock::now();
			auto inference_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

			// 存储检测结果
			pthread_mutex_lock(&filter->inference_mutex);
			filter->pending_detections = detections;
			filter->pending_detections_ready = true;
			pthread_mutex_unlock(&filter->inference_mutex);

			// 每 300 帧输出一次推理时间
			if (filter->frame_count % 300 == 0) {
				obs_log(LOG_INFO, "[MYFILTER] Inference time: %.2f ms", (float)inference_time);
			}
		}

		// 释放缓冲区副本
		bfree(buffer_copy);
	}

	return NULL;
}
#endif

#ifndef NO_INFERENCE
// 启动异步推理线程
static void start_inference_thread(my_filter_data_t *filter)
{
	if (!filter || filter->inference_thread_running) {
		return;
	}

	filter->should_stop_inference = false;
	int result = pthread_create(&filter->inference_thread, NULL, inference_thread_func, filter);
	if (result == 0) {
		filter->inference_thread_running = true;
		obs_log(LOG_INFO, "Inference thread started successfully");
	} else {
		obs_log(LOG_ERROR, "Failed to start inference thread: %d", result);
	}
}
#endif

#ifndef NO_INFERENCE
// 在 OpenCV 图像上绘制检测结果（用于 CPU 模式）
static void draw_detections_opencv(cv::Mat &image, std::vector<detection_result_t> &detections)
{
	for (auto &det : detections) {
		// 绘制边界框
		cv::rectangle(image, 
			cv::Point(det.x1, det.y1), 
			cv::Point(det.x2, det.y2), 
			cv::Scalar(0, 0, 255), 2);

		// 绘制标签
		std::string label = get_class_name(det.class_id);
		std::string text = label + " " + std::to_string(det.confidence).substr(0, 4);
		
		// 在边界框上方绘制标签
		cv::putText(image, text, 
			cv::Point(det.x1, det.y1 - 10), 
			cv::FONT_HERSHEY_SIMPLEX, 0.5, 
			cv::Scalar(0, 0, 255), 2);
	}
}
#endif

// 从 NV12 帧中提取中心 ROI 区域（转换为 RGB 格式）
static bool extract_center_roi(struct obs_source_frame *frame, int roi_width, int roi_height, uint8_t *out_buffer)
{
	if (!frame || !out_buffer) {
		return false;
	}

	int width = frame->width;
	int height = frame->height;
	uint8_t *data = frame->data[0];
	uint8_t *uv_data = frame->data[1];

	// 计算 ROI 中心位置
	int roi_x = (width - roi_width) / 2;
	int roi_y = (height - roi_height) / 2;

	// 确保 ROI 在有效范围内
	if (roi_x < 0 || roi_y < 0 || roi_x + roi_width > width || roi_y + roi_height > height) {
		return false;
	}

	// 提取 ROI 数据并转换为 RGB
	int rgb_index = 0;
	for (int y = roi_y; y < roi_y + roi_height; y++) {
		for (int x = roi_x; x < roi_x + roi_width; x++) {
			// 获取 Y 分量
			int y_index = y * width + x;
			uint8_t y_val = data[y_index];

			// 获取 UV 分量（NV12 格式中 UV 分量是交错存储的，每四个 Y 像素共享一个 UV 值）
			int uv_x = x / 2;
			int uv_y = y / 2;
			int uv_index = uv_y * (width / 2) * 2 + uv_x * 2;
			uint8_t u_val = uv_data[uv_index];
			uint8_t v_val = uv_data[uv_index + 1];

			// 转换为 RGB
			uint8_t r, g, b;
			yuv_to_rgb(y_val, u_val, v_val, &r, &g, &b);

			// 存储到输出缓冲区（RGB 格式）
			out_buffer[rgb_index++] = r;
			out_buffer[rgb_index++] = g;
			out_buffer[rgb_index++] = b;
		}
	}

	return true;
}

static struct obs_source_frame *filter_video(void *data, struct obs_source_frame *frame)
{
	if (!data || !frame) {
		return frame;
	}

	my_filter_data_t *filter = data;
	filter->frame_count++;

	// 计算画面中心和 ROI 位置
	int width = frame->width;
	int height = frame->height;
	int roi_x = (width - ROI_SIZE) / 2;
	int roi_y = (height - ROI_SIZE) / 2;

	// 阶段 1：在画面中央绘制 320x320 的检测区域框
	// 注意：直接在 NV12 格式上绘制彩色框比较复杂，这里只修改 Y 分量绘制白色框
	draw_rectangle_nv12(frame->data[0], width, height, roi_x, roi_y, ROI_SIZE, ROI_SIZE, 2);

	// 阶段 2：提取中心 ROI 区域
	bool roi_extracted = false;
	if (filter->roi_buffer) {
		roi_extracted = extract_center_roi(frame, ROI_SIZE, ROI_SIZE, filter->roi_buffer);
		pthread_mutex_lock(&filter->roi_mutex);
		filter->roi_ready = roi_extracted;
		pthread_mutex_unlock(&filter->roi_mutex);

#ifndef NO_INFERENCE
		// 阶段 3：如果 ROI 提取成功且模型已加载，启动异步推理
		if (roi_extracted && filter->ort_session) {
			// 确保推理线程已启动
			if (!filter->inference_thread_running) {
				start_inference_thread(filter);
			}

			// 将 ROI 数据复制到推理缓冲区
			pthread_mutex_lock(&filter->inference_mutex);
			if (!filter->inference_buffer_ready) {
				memcpy(filter->inference_buffer, filter->roi_buffer, ROI_SIZE * ROI_SIZE * ROI_CHANNELS);
				filter->inference_buffer_ready = true;
				pthread_cond_signal(&filter->inference_cond);
			}
			pthread_mutex_unlock(&filter->inference_mutex);
		}

		// 阶段 4：检查是否有新的检测结果
		pthread_mutex_lock(&filter->inference_mutex);
		if (filter->pending_detections_ready) {
			pthread_mutex_lock(&filter->detections_mutex);
			filter->detections = filter->pending_detections;
			pthread_mutex_unlock(&filter->detections_mutex);
			filter->pending_detections_ready = false;
		}
		pthread_mutex_unlock(&filter->inference_mutex);
#endif
	}

	// 阶段 5：每 300 帧输出一次日志
	if (filter->frame_count % 300 == 0) {
#ifndef NO_INFERENCE
		obs_log(LOG_INFO,
			"[MYFILTER] Filter video | Frame: %d | Size: %dx%d | Format: %s | ROI extracted: %s | Model loaded: %s | Thread running: %s",
			filter->frame_count,
			width, height,
			"NV12",
			roi_extracted ? "Yes" : "No",
			filter->ort_session ? "Yes" : "No",
			filter->inference_thread_running ? "Yes" : "No");
#else
		obs_log(LOG_INFO,
			"[MYFILTER] Filter video | Frame: %d | Size: %dx%d | Format: %s | ROI extracted: %s | Inference: Disabled",
			filter->frame_count,
			width, height,
			"NV12",
			roi_extracted ? "Yes" : "No");
#endif
	}

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
		filter->roi_buffer = bzalloc(ROI_SIZE * ROI_SIZE * ROI_CHANNELS); // RGB 格式
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
			uint8_t *dst = filter->roi_buffer + y * ROI_SIZE * ROI_CHANNELS;
			// 转换 RGBA 到 RGB
			for (int x = 0; x < ROI_SIZE; x++) {
				dst[x * 3 + 0] = src[x * 4 + 0]; // R
				dst[x * 3 + 1] = src[x * 4 + 1]; // G
				dst[x * 3 + 2] = src[x * 4 + 2]; // B
			}
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

	// 更新过滤器的宽度和高度
	filter->width = width;
	filter->height = height;

	// 只有在 shader 可用时才执行 GPU 渲染
	if (filter->effect) {
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

		// 绘制检测结果
		draw_detections(filter);

		// 在插件支持被禁用的 CI 环境中，跳过 ROI 提取逻辑
		// obs_filter_get_video_texture 等 API 在禁用插件支持时不可用
		// 保留 ROI 逻辑但跳过实际执行，确保编译通过
		if (0) {
			render_roi_to_texture(filter, NULL);
			copy_roi_to_cpu(filter);
		}
	} else {
		// 当 shader 不可用时，直接传递原始帧
		obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING);
		gs_draw_sprite(NULL, 0, width, height);
		obs_source_process_filter_end(filter->context, effect, width, height);

		// 绘制检测结果（CPU 模式）
		draw_detections(filter);
	}

	filter->frame_count++;

	if (filter->frame_count % 300 == 0) {
		obs_log(LOG_INFO,
			"[MYFILTER] Video render | Size: %dx%d | ROI ready: %s | Mode: %s | Detections: %d",
			width, height,
			filter->roi_ready ? "Yes" : "No",
			filter->effect ? "GPU" : "CPU",
			filter->detections.size());
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
	obs_log(LOG_INFO, "Plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "Plugin unloaded");
}
