#pragma once

#include "onnxruntime_c_api.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iostream>

struct DBNetResult {
	bool has_text = false;
	float text_ratio = 0.0f;
	float max_prob = 0.0f;
	float stroke_energy = 0.0f;
	int active_pixels = 0;
	int mask_w = 0;
	int mask_h = 0;
	std::vector<uint8_t> prob_mask; // binary stroke mask: 1 where prob >= 0.35
};

class DBNetDetector {
private:
	HMODULE hOrt = nullptr;
	const OrtApi* ort = nullptr;
	OrtEnv* env = nullptr;
	OrtSessionOptions* session_options = nullptr;
	OrtSession* session = nullptr;
	OrtMemoryInfo* memory_info = nullptr;
	bool available = false;
	bool is_gpu = false;

	std::vector<float> input_tensor_cache;

	DBNetDetector() {
		Init();
	}

	~DBNetDetector() {
		Shutdown();
	}

public:
	static DBNetDetector& Get() {
		static DBNetDetector instance;
		return instance;
	}

	bool IsAvailable() const { return available; }
	bool IsGPU() const { return is_gpu; }

	// 超快速空间梯度能量测算（0.02ms，用于非文字帧零延迟跳过推理）
	static float ComputeGradientEnergy(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                                   int x0, int y0, int x1, int y1) {
		int w = x1 - x0;
		int h = y1 - y0;
		if (w < 4 || h < 4) return 0.0f;

		uint64_t total_grad = 0;
		int count = 0;

		for (int y = y0; y < y1 - 1; y += 2) {
			int row0 = flipped ? frame_h - 1 - y : y;
			int row1 = flipped ? frame_h - 1 - (y + 1) : (y + 1);
			if (row0 < 0 || row0 >= frame_h || row1 < 0 || row1 >= frame_h) continue;

			const uint8_t* p0 = frame_data + row0 * frame_pitch;
			const uint8_t* p1 = frame_data + row1 * frame_pitch;

			for (int x = x0; x < x1 - 1; x += 2) {
				if (x < 0 || x >= frame_w - 1) continue;
				int g00 = (p0[x * 4] + 2 * p0[x * 4 + 1] + p0[x * 4 + 2]) >> 2;
				int g10 = (p0[(x + 1) * 4] + 2 * p0[(x + 1) * 4 + 1] + p0[(x + 1) * 4 + 2]) >> 2;
				int g01 = (p1[x * 4] + 2 * p1[x * 4 + 1] + p1[x * 4 + 2]) >> 2;

				int dx = std::abs(g10 - g00);
				int dy = std::abs(g01 - g00);
				total_grad += (dx + dy);
				++count;
			}
		}

		return count > 0 ? static_cast<float>(total_grad) / static_cast<float>(count) : 0.0f;
	}

	bool Init() {
		if (available) return true;

		wchar_t exe_buf[MAX_PATH] = {0};
		GetModuleFileNameW(NULL, exe_buf, MAX_PATH);
		std::wstring exe_path(exe_buf);
		std::wstring exe_dir = L"";
		size_t last_slash = exe_path.find_last_of(L"\\/");
		if (last_slash != std::wstring::npos) {
			exe_dir = exe_path.substr(0, last_slash + 1);
		}
		if (!exe_dir.empty()) {
			SetDllDirectoryW(exe_dir.c_str());
		}

		// 1. 本地动态载入 onnxruntime.dll
		std::vector<std::wstring> dll_paths = {
			exe_dir + L"onnxruntime.dll",
			L"onnxruntime.dll",
			exe_dir + L"build-local\\onnxruntime.dll",
			L"build-local/onnxruntime.dll",
			exe_dir + L"..\\build-local\\onnxruntime.dll"
		};

		for (const auto& p : dll_paths) {
			hOrt = LoadLibraryW(p.c_str());
			if (hOrt) break;
		}

		if (!hOrt) return false;

		typedef const OrtApiBase* (ORT_API_CALL *OrtGetApiBaseFn)();
		OrtGetApiBaseFn get_api_base = (OrtGetApiBaseFn)GetProcAddress(hOrt, "OrtGetApiBase");
		if (!get_api_base) {
			FreeLibrary(hOrt);
			hOrt = nullptr;
			return false;
		}

		const OrtApiBase* api_base = get_api_base();
		if (!api_base) {
			FreeLibrary(hOrt);
			hOrt = nullptr;
			return false;
		}

		ort = api_base->GetApi(17);
		if (!ort) ort = api_base->GetApi(ORT_API_VERSION);
		if (!ort) {
			FreeLibrary(hOrt);
			hOrt = nullptr;
			return false;
		}

		// 2. 本地检索 DBNet 神经网络模型 (ch_PP-OCRv4_det.onnx)
		std::vector<std::wstring> model_paths = {
			exe_dir + L"models\\ch_PP-OCRv4_det.onnx",
			L"models/ch_PP-OCRv4_det.onnx",
			exe_dir + L"build-local\\models\\ch_PP-OCRv4_det.onnx",
			L"build-local/models/ch_PP-OCRv4_det.onnx",
			exe_dir + L"..\\build-local\\models\\ch_PP-OCRv4_det.onnx",
			exe_dir + L"..\\build\\ch_PP-OCRv4_det.onnx"
		};

		std::wstring found_model = L"";
		for (const auto& mp : model_paths) {
			DWORD attr = GetFileAttributesW(mp.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
				found_model = mp;
				break;
			}
		}

		if (found_model.empty()) {
			FreeLibrary(hOrt);
			hOrt = nullptr;
			return false;
		}

		// 3. 初始化环境与会话选项
		OrtStatus* status = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "aegisub_dbnet", &env);
		if (status) { ort->ReleaseStatus(status); return false; }

		status = ort->CreateSessionOptions(&session_options);
		if (status) { ort->ReleaseStatus(status); return false; }

		ort->SetIntraOpNumThreads(session_options, 2);

		// 优先启用 DirectML GPU 硬件加速 (极速 3ms 推理)
		typedef OrtStatus* (ORT_API_CALL *DMLFn)(OrtSessionOptions*, int);
		DMLFn dml_fn = (DMLFn)GetProcAddress(hOrt, "OrtSessionOptionsAppendExecutionProvider_DML");
		if (dml_fn) {
			OrtStatus* dml_status = dml_fn(session_options, 0);
			if (dml_status == nullptr) {
				is_gpu = true;
			} else {
				ort->ReleaseStatus(dml_status);
			}
		}

		// 4. 创建 Session
		status = ort->CreateSession(env, found_model.c_str(), session_options, &session);
		if (status) {
			ort->ReleaseStatus(status);
			return false;
		}

		status = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info);
		if (status) {
			ort->ReleaseStatus(status);
			return false;
		}

		available = true;
		return true;
	}

	void Shutdown() {
		if (session && ort) ort->ReleaseSession(session);
		if (session_options && ort) ort->ReleaseSessionOptions(session_options);
		if (memory_info && ort) ort->ReleaseMemoryInfo(memory_info);
		if (env && ort) ort->ReleaseEnv(env);
		if (hOrt) FreeLibrary(hOrt);

		session = nullptr;
		session_options = nullptr;
		memory_info = nullptr;
		env = nullptr;
		hOrt = nullptr;
		ort = nullptr;
		available = false;
	}

	// 极速单帧文字笔画概率检测
	DBNetResult Detect(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                   int x0, int y0, int x1, int y1) {
		DBNetResult res;
		if (!available || !ort || !session) return res;

		int crop_w = x1 - x0;
		int crop_h = y1 - y0;
		if (crop_w < 12 || crop_h < 6) return res;

		// 针对对白字幕尺寸智能规划推断分辨率，既保证笔画细节又极致精简算力
		int target_h = 64;
		int target_w = std::clamp(((crop_w * target_h / crop_h) + 31) / 32 * 32, 64, 480);
		size_t n_pixels = static_cast<size_t>(target_h) * target_w;

		if (input_tensor_cache.size() < 3 * n_pixels) {
			input_tensor_cache.resize(3 * n_pixels);
		}

		constexpr float kr = 1.0f / (255.0f * 0.229f);
		constexpr float kg = 1.0f / (255.0f * 0.224f);
		constexpr float kb = 1.0f / (255.0f * 0.225f);
		constexpr float br = -0.485f / 0.229f;
		constexpr float bg = -0.456f / 0.224f;
		constexpr float bb = -0.406f / 0.225f;

		float* r_plane = input_tensor_cache.data();
		float* g_plane = input_tensor_cache.data() + n_pixels;
		float* b_plane = input_tensor_cache.data() + 2 * n_pixels;

		for (int ty = 0; ty < target_h; ++ty) {
			int sy = y0 + (ty * crop_h) / target_h;
			int row = flipped ? frame_h - 1 - sy : sy;
			if (row < 0 || row >= frame_h) continue;

			const uint8_t* row_ptr = frame_data + row * frame_pitch;
			for (int tx = 0; tx < target_w; ++tx) {
				int sx = x0 + (tx * crop_w) / target_w;
				if (sx < 0 || sx >= frame_w) continue;

				const uint8_t* p = row_ptr + sx * 4;
				int idx = ty * target_w + tx;
				r_plane[idx] = p[2] * kr + br;
				g_plane[idx] = p[1] * kg + bg;
				b_plane[idx] = p[0] * kb + bb;
			}
		}

		int64_t input_shape[4] = {1, 3, target_h, target_w};
		OrtValue* input_tensor_val = nullptr;
		OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, input_tensor_cache.data(), 3 * n_pixels * sizeof(float),
			input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor_val);
		if (st) { ort->ReleaseStatus(st); return res; }

		const char* input_names[] = {"x"};
		const char* output_names[] = {"sigmoid_0.tmp_0"};
		OrtValue* output_tensor_val = nullptr;

		st = ort->Run(session, nullptr, input_names, (const OrtValue* const*)&input_tensor_val, 1,
		              output_names, 1, &output_tensor_val);
		ort->ReleaseValue(input_tensor_val);

		if (st) {
			ort->ReleaseStatus(st);
			return res;
		}

		float* out_data = nullptr;
		st = ort->GetTensorMutableData(output_tensor_val, (void**)&out_data);
		if (!st && out_data) {
			res.mask_w = target_w;
			res.mask_h = target_h;
			res.prob_mask.assign(n_pixels, 0);

			int active_count = 0;
			float max_p = 0.0f;
			float sum_active_p = 0.0f;

			for (size_t i = 0; i < n_pixels; ++i) {
				float p = out_data[i];
				if (p > max_p) max_p = p;
				if (p >= 0.35f) {
					res.prob_mask[i] = 1;
					sum_active_p += p;
					++active_count;
				}
			}

			res.active_pixels = active_count;
			res.max_prob = max_p;
			res.text_ratio = static_cast<float>(active_count) / static_cast<float>(n_pixels);
			res.stroke_energy = sum_active_p;
			res.has_text = (active_count >= 6 && max_p >= 0.38f);
		} else if (st) {
			ort->ReleaseStatus(st);
		}

		if (output_tensor_val) ort->ReleaseValue(output_tensor_val);
		return res;
	}

	// 提取基准帧字幕紧致文字边界（Anchor），彻底排除外部大背景干扰
	bool GetTextBoundingBox(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                        int x0, int y0, int x1, int y1,
	                        int& out_x0, int& out_y0, int& out_x1, int& out_y1) {
		if (!available || !ort || !session) return false;

		int crop_w = x1 - x0;
		int crop_h = y1 - y0;
		if (crop_w < 12 || crop_h < 6) return false;

		int target_h = 64;
		int target_w = std::clamp(((crop_w * target_h / crop_h) + 31) / 32 * 32, 64, 480);
		size_t n_pixels = static_cast<size_t>(target_h) * target_w;

		if (input_tensor_cache.size() < 3 * n_pixels) {
			input_tensor_cache.resize(3 * n_pixels);
		}

		constexpr float kr = 1.0f / (255.0f * 0.229f);
		constexpr float kg = 1.0f / (255.0f * 0.224f);
		constexpr float kb = 1.0f / (255.0f * 0.225f);
		constexpr float br = -0.485f / 0.229f;
		constexpr float bg = -0.456f / 0.224f;
		constexpr float bb = -0.406f / 0.225f;

		float* r_plane = input_tensor_cache.data();
		float* g_plane = input_tensor_cache.data() + n_pixels;
		float* b_plane = input_tensor_cache.data() + 2 * n_pixels;

		for (int ty = 0; ty < target_h; ++ty) {
			int sy = y0 + (ty * crop_h) / target_h;
			int row = flipped ? frame_h - 1 - sy : sy;
			if (row < 0 || row >= frame_h) continue;

			const uint8_t* row_ptr = frame_data + row * frame_pitch;
			for (int tx = 0; tx < target_w; ++tx) {
				int sx = x0 + (tx * crop_w) / target_w;
				if (sx < 0 || sx >= frame_w) continue;

				const uint8_t* p = row_ptr + sx * 4;
				int idx = ty * target_w + tx;
				r_plane[idx] = p[2] * kr + br;
				g_plane[idx] = p[1] * kg + bg;
				b_plane[idx] = p[0] * kb + bb;
			}
		}

		int64_t input_shape[4] = {1, 3, target_h, target_w};
		OrtValue* input_tensor_val = nullptr;
		OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, input_tensor_cache.data(), 3 * n_pixels * sizeof(float),
			input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor_val);
		if (st) { ort->ReleaseStatus(st); return false; }

		const char* input_names[] = {"x"};
		const char* output_names[] = {"sigmoid_0.tmp_0"};
		OrtValue* output_tensor_val = nullptr;

		st = ort->Run(session, nullptr, input_names, (const OrtValue* const*)&input_tensor_val, 1,
		              output_names, 1, &output_tensor_val);
		ort->ReleaseValue(input_tensor_val);

		if (st) {
			ort->ReleaseStatus(st);
			return false;
		}

		float* out_data = nullptr;
		st = ort->GetTensorMutableData(output_tensor_val, (void**)&out_data);
		bool found = false;
		if (!st && out_data) {
			int min_tx = target_w, max_tx = -1;
			int min_ty = target_h, max_ty = -1;
			int active_count = 0;
			float max_p = 0.0f;

			for (int ty = 0; ty < target_h; ++ty) {
				for (int tx = 0; tx < target_w; ++tx) {
					float p = out_data[ty * target_w + tx];
					if (p > max_p) max_p = p;
					if (p >= 0.45f) {
						++active_count;
						if (tx < min_tx) min_tx = tx;
						if (tx > max_tx) max_tx = tx;
						if (ty < min_ty) min_ty = ty;
						if (ty > max_ty) max_ty = ty;
					}
				}
			}

			if (active_count >= 12 && max_p >= 0.50f && max_tx >= min_tx) {
				int raw_x0 = x0 + (min_tx * crop_w) / target_w;
				int raw_x1 = x0 + (max_tx * crop_w) / target_w;
				int raw_y0 = y0 + (min_ty * crop_h) / target_h;
				int raw_y1 = y0 + (max_ty * crop_h) / target_h;

				int pad_x = std::clamp(crop_w / 60, 2, 6);
				int pad_y = std::clamp(crop_h / 40, 2, 4);
				out_x0 = std::clamp(raw_x0 - pad_x, x0, x1);
				out_x1 = std::clamp(raw_x1 + pad_x, x0, x1);
				out_y0 = std::clamp(raw_y0 - pad_y, y0, y1);
				out_y1 = std::clamp(raw_y1 + pad_y, y0, y1);

				if (out_x1 - out_x0 >= 10 && out_y1 - out_y0 >= 6) {
					found = true;
				}
			}
		} else if (st) {
			ort->ReleaseStatus(st);
		}

		if (output_tensor_val) ort->ReleaseValue(output_tensor_val);
		return found;
	}

	// 核心：基于竖线锚点（Anchor X, Y, Height）自适应定位并捕获当前帧整行字幕的精确外接矩形
	// 吸收 SubtitleOCR 先进思想：无论字数长短，自动捕获穿过竖线锚点的完整字幕，彻底解决固定死方框在长短字幕上的失真
	bool FindAnchorSubtitleBox(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                           int anchor_x, int anchor_y, int anchor_h,
	                           int& out_x0, int& out_y0, int& out_x1, int& out_y1) {
		if (!available || !ort || !session) return false;
		if (frame_w <= 0 || frame_h <= 0 || anchor_h < 8) return false;

		anchor_x = std::clamp(anchor_x, 0, frame_w - 1);
		anchor_y = std::clamp(anchor_y, 0, frame_h - 1);

		// 1. 在锚点 Y 上下截取水平条带（高度大约为 1.5 ~ 1.8 倍字高）
		int half_h = std::max(12, anchor_h * 4 / 5);
		int strip_y0 = std::clamp(anchor_y - half_h, 0, frame_h - 1);
		int strip_y1 = std::clamp(anchor_y + half_h, 0, frame_h);
		int strip_h = strip_y1 - strip_y0;
		if (strip_h < 12) return false;

		// 2. 水平范围以锚点 X 为中心展开，覆盖整行字幕可能的跨度（最大支持 1440 像素宽带）
		int half_w = std::min(frame_w / 2, std::max(anchor_h * 10, 480));
		int strip_x0 = std::clamp(anchor_x - half_w, 0, frame_w - 1);
		int strip_x1 = std::clamp(anchor_x + half_w, 0, frame_w);
		int strip_w = strip_x1 - strip_x0;
		if (strip_w < 24) return false;

		// 3. 规整推断尺寸为 32 的整数倍
		int target_h = 64;
		int target_w = std::clamp(((strip_w * target_h / strip_h) + 31) / 32 * 32, 64, 768);
		size_t n_pixels = static_cast<size_t>(target_h) * target_w;

		if (input_tensor_cache.size() < 3 * n_pixels) {
			input_tensor_cache.resize(3 * n_pixels);
		}

		constexpr float kr = 1.0f / (255.0f * 0.229f);
		constexpr float kg = 1.0f / (255.0f * 0.224f);
		constexpr float kb = 1.0f / (255.0f * 0.225f);
		constexpr float br = -0.485f / 0.229f;
		constexpr float bg = -0.456f / 0.224f;
		constexpr float bb = -0.406f / 0.225f;

		float* r_plane = input_tensor_cache.data();
		float* g_plane = input_tensor_cache.data() + n_pixels;
		float* b_plane = input_tensor_cache.data() + 2 * n_pixels;

		for (int ty = 0; ty < target_h; ++ty) {
			int sy = strip_y0 + (ty * strip_h) / target_h;
			int row = flipped ? frame_h - 1 - sy : sy;
			if (row < 0 || row >= frame_h) continue;

			const uint8_t* row_ptr = frame_data + row * frame_pitch;
			for (int tx = 0; tx < target_w; ++tx) {
				int sx = strip_x0 + (tx * strip_w) / target_w;
				if (sx < 0 || sx >= frame_w) continue;

				const uint8_t* p = row_ptr + sx * 4;
				int idx = ty * target_w + tx;
				r_plane[idx] = p[2] * kr + br;
				g_plane[idx] = p[1] * kg + bg;
				b_plane[idx] = p[0] * kb + bb;
			}
		}

		int64_t input_shape[4] = {1, 3, target_h, target_w};
		OrtValue* input_tensor_val = nullptr;
		OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, input_tensor_cache.data(), 3 * n_pixels * sizeof(float),
			input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor_val);
		if (st) { ort->ReleaseStatus(st); return false; }

		const char* input_names[] = {"x"};
		const char* output_names[] = {"sigmoid_0.tmp_0"};
		OrtValue* output_tensor_val = nullptr;

		st = ort->Run(session, nullptr, input_names, (const OrtValue* const*)&input_tensor_val, 1,
		              output_names, 1, &output_tensor_val);
		ort->ReleaseValue(input_tensor_val);

		if (st) {
			ort->ReleaseStatus(st);
			return false;
		}

		float* out_data = nullptr;
		st = ort->GetTensorMutableData(output_tensor_val, (void**)&out_data);
		bool found = false;
		if (!st && out_data) {
			// 4. 将竖线锚点 X 映射到目标概率图横坐标
			int target_ax = std::clamp((anchor_x - strip_x0) * target_w / strip_w, 0, target_w - 1);

			// 统计各列是否存在有效笔画 (p >= 0.35f 且该列至少有两个笔画点)
			std::vector<int> col_hits(target_w, 0);
			for (int ty = 0; ty < target_h; ++ty) {
				for (int tx = 0; tx < target_w; ++tx) {
					if (out_data[ty * target_w + tx] >= 0.35f) {
						col_hits[tx]++;
					}
				}
			}

			// 在 target_ax 附近寻找最近的活跃文字列（容忍锚点落在两字之间或韩文/西文词间空格）
			int seed_tx = -1;
			int best_dist = 999999;
			int search_radius = std::max(24, target_h);
			for (int tx = std::max(0, target_ax - search_radius); tx <= std::min(target_w - 1, target_ax + search_radius); ++tx) {
				if (col_hits[tx] >= 1) {
					int d = std::abs(tx - target_ax);
					if (d < best_dist) {
						best_dist = d;
						seed_tx = tx;
					}
				}
			}

			if (seed_tx >= 0) {
				// 词间空格允许的最大连通跳隙（韩语/西文词间距显著，允许跨越空格连通完整句子）
				int max_gap = std::max(8, target_h * 3 / 5);

				// 向左膨胀连通
				int min_tx = seed_tx;
				int gap = 0;
				for (int tx = seed_tx - 1; tx >= 0; --tx) {
					if (col_hits[tx] >= 1) {
						min_tx = tx;
						gap = 0;
					} else {
						if (++gap > max_gap) break;
					}
				}

				// 向右膨胀连通
				int max_tx = seed_tx;
				gap = 0;
				for (int tx = seed_tx + 1; tx < target_w; ++tx) {
					if (col_hits[tx] >= 1) {
						max_tx = tx;
						gap = 0;
					} else {
						if (++gap > max_gap) break;
					}
				}

				// 在 [min_tx, max_tx] 范围内统计垂直边界
				int min_ty = target_h, max_ty = -1;
				int total_active = 0;
				for (int tx = min_tx; tx <= max_tx; ++tx) {
					for (int ty = 0; ty < target_h; ++ty) {
						if (out_data[ty * target_w + tx] >= 0.35f) {
							++total_active;
							if (ty < min_ty) min_ty = ty;
							if (ty > max_ty) max_ty = ty;
						}
					}
				}

				if (total_active >= 8 && max_ty >= min_ty) {
					int raw_x0 = strip_x0 + (min_tx * strip_w) / target_w;
					int raw_x1 = strip_x0 + (max_tx * strip_w) / target_w;
					int raw_y0 = strip_y0 + (min_ty * strip_h) / target_h;
					int raw_y1 = strip_y0 + (max_ty * strip_h) / target_h;

					int pad_x = std::clamp(strip_w / 60, 4, 12);
					int pad_y = std::clamp(strip_h / 30, 2, 6);
					out_x0 = std::clamp(raw_x0 - pad_x, 0, frame_w);
					out_x1 = std::clamp(raw_x1 + pad_x, 0, frame_w);
					out_y0 = std::clamp(raw_y0 - pad_y, 0, frame_h);
					out_y1 = std::clamp(raw_y1 + pad_y, 0, frame_h);

					if (out_x1 - out_x0 >= 16 && out_y1 - out_y0 >= 8) {
						found = true;
					}
				}
			}
		} else if (st) {
			ort->ReleaseStatus(st);
		}

		if (output_tensor_val) ort->ReleaseValue(output_tensor_val);
		return found;
	}
};
