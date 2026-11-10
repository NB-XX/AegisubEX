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
	int active_pixels = 0;
	int mask_w = 0;
	int mask_h = 0;
	std::vector<uint8_t> prob_mask; // binary mask (1 if prob > 0.35)
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

	bool Init() {
		if (available) return true;

		// Find exe directory
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

		// 1. Search for onnxruntime.dll
		std::vector<std::wstring> dll_paths = {
			exe_dir + L"onnxruntime.dll",
			L"onnxruntime.dll",
			exe_dir + L"build-local\\onnxruntime.dll",
			L"build-local/onnxruntime.dll",
			L"D:/Tool/SubtitleOCR_Community_v3.1.2/望言OCR_v3.1.2/onnxruntime.dll"
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

		// Use modern API version (17 or 16)
		ort = api_base->GetApi(17);
		if (!ort) ort = api_base->GetApi(ORT_API_VERSION);
		if (!ort) {
			FreeLibrary(hOrt);
			hOrt = nullptr;
			return false;
		}

		// 2. Search for DBNet model
		std::vector<std::wstring> model_paths = {
			exe_dir + L"models\\ch_PP-OCRv4_det.onnx",
			L"models/ch_PP-OCRv4_det.onnx",
			exe_dir + L"build-local\\models\\ch_PP-OCRv4_det.onnx",
			L"build-local/models/ch_PP-OCRv4_det.onnx",
			L"D:/Tool/SubtitleOCR_Community_v3.1.2/望言OCR_v3.1.2/models/ch_PP-OCRv4_det.onnx"
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

		// 3. Initialize OrtEnv and SessionOptions
		OrtStatus* status = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "aegisub_dbnet", &env);
		if (status) { ort->ReleaseStatus(status); return false; }

		status = ort->CreateSessionOptions(&session_options);
		if (status) { ort->ReleaseStatus(status); return false; }

		ort->SetIntraOpNumThreads(session_options, 2);

		// Try DirectML GPU acceleration if available
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

		// 4. Create Session
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

	DBNetResult Detect(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                   int x0, int y0, int x1, int y1) {
		DBNetResult res;
		if (!available || !ort || !session) return res;

		int crop_w = x1 - x0;
		int crop_h = y1 - y0;
		if (crop_w < 16 || crop_h < 8) return res;

		int target_h = 64;
		int target_w = std::clamp(((crop_w * target_h / crop_h) + 31) / 32 * 32, 64, 960);
		size_t n_pixels = static_cast<size_t>(target_h) * target_w;

		// Prepare planar RGB float input normalized by ImageNet mean & std
		std::vector<float> input_tensor(3 * n_pixels);
		float* r_plane = input_tensor.data();
		float* g_plane = input_tensor.data() + n_pixels;
		float* b_plane = input_tensor.data() + 2 * n_pixels;

		for (int ty = 0; ty < target_h; ++ty) {
			int sy = y0 + (ty * crop_h) / target_h;
			int row = flipped ? frame_h - 1 - sy : sy;
			if (row < 0 || row >= frame_h) continue;

			const uint8_t* row_ptr = frame_data + row * frame_pitch;
			for (int tx = 0; tx < target_w; ++tx) {
				int sx = x0 + (tx * crop_w) / target_w;
				if (sx < 0 || sx >= frame_w) continue;

				const uint8_t* p = row_ptr + sx * 4;
				float b = p[0] / 255.0f;
				float g = p[1] / 255.0f;
				float r = p[2] / 255.0f;

				int idx = ty * target_w + tx;
				r_plane[idx] = (r - 0.485f) / 0.229f;
				g_plane[idx] = (g - 0.456f) / 0.224f;
				b_plane[idx] = (b - 0.406f) / 0.225f;
			}
		}

		// Create OrtValue
		int64_t input_shape[4] = {1, 3, target_h, target_w};
		OrtValue* input_tensor_val = nullptr;
		OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, input_tensor.data(), input_tensor.size() * sizeof(float),
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

			for (size_t i = 0; i < n_pixels; ++i) {
				float p = out_data[i];
				if (p > max_p) max_p = p;
				if (p >= 0.35f) {
					res.prob_mask[i] = 1;
					++active_count;
				}
			}

			res.active_pixels = active_count;
			res.max_prob = max_p;
			res.text_ratio = static_cast<float>(active_count) / static_cast<float>(n_pixels);
			// Threshold: text requires at least 8 active stroke pixels and peak prob >= 0.40
			res.has_text = (active_count >= 8 && max_p >= 0.40f);
		} else if (st) {
			ort->ReleaseStatus(st);
		}

		if (output_tensor_val) ort->ReleaseValue(output_tensor_val);
		return res;
	}

	bool GetTextBoundingBox(const uint8_t* frame_data, int frame_w, int frame_h, int frame_pitch, bool flipped,
	                        int x0, int y0, int x1, int y1,
	                        int& out_x0, int& out_y0, int& out_x1, int& out_y1) {
		if (!available || !ort || !session) return false;

		int crop_w = x1 - x0;
		int crop_h = y1 - y0;
		if (crop_w < 16 || crop_h < 8) return false;

		int target_h = 64;
		int target_w = std::clamp(((crop_w * target_h / crop_h) + 31) / 32 * 32, 64, 960);
		size_t n_pixels = static_cast<size_t>(target_h) * target_w;

		std::vector<float> input_tensor(3 * n_pixels);
		float* r_plane = input_tensor.data();
		float* g_plane = input_tensor.data() + n_pixels;
		float* b_plane = input_tensor.data() + 2 * n_pixels;

		for (int ty = 0; ty < target_h; ++ty) {
			int sy = y0 + (ty * crop_h) / target_h;
			int row = flipped ? frame_h - 1 - sy : sy;
			if (row < 0 || row >= frame_h) continue;

			const uint8_t* row_ptr = frame_data + row * frame_pitch;
			for (int tx = 0; tx < target_w; ++tx) {
				int sx = x0 + (tx * crop_w) / target_w;
				if (sx < 0 || sx >= frame_w) continue;

				const uint8_t* p = row_ptr + sx * 4;
				float b = p[0] / 255.0f;
				float g = p[1] / 255.0f;
				float r = p[2] / 255.0f;

				int idx = ty * target_w + tx;
				r_plane[idx] = (r - 0.485f) / 0.229f;
				g_plane[idx] = (g - 0.456f) / 0.224f;
				b_plane[idx] = (b - 0.406f) / 0.225f;
			}
		}

		int64_t input_shape[4] = {1, 3, target_h, target_w};
		OrtValue* input_tensor_val = nullptr;
		OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, input_tensor.data(), input_tensor.size() * sizeof(float),
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
					if (p >= 0.50f) {
						++active_count;
						if (tx < min_tx) min_tx = tx;
						if (tx > max_tx) max_tx = tx;
						if (ty < min_ty) min_ty = ty;
						if (ty > max_ty) max_ty = ty;
					}
				}
			}

			if (active_count >= 15 && max_p >= 0.55f && max_tx >= min_tx) {
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

				if (out_x1 - out_x0 >= 12 && out_y1 - out_y0 >= 6) {
					found = true;
				}
			}
		} else if (st) {
			ort->ReleaseStatus(st);
		}

		if (output_tensor_val) ort->ReleaseValue(output_tensor_val);
		return found;
	}
};
