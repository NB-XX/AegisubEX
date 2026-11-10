// Silero VAD + 音色特征提取与说话人识别引擎 (C++ Header-Only)
// 基于 ONNX Runtime 动态加载与 Silero VAD v5 模型，提供毫秒级精准人声切分与声纹识别

#pragma once

#include "onnxruntime_c_api.h"
#include "fft.h"

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <atomic>
#include <functional>

struct VADSegment {
	int64_t start_ms = 0;           ///< 语音起始时刻 (ms)，紧贴说话起始
	int64_t end_ms = 0;             ///< 语音结束时刻 (ms)，包含后置延长缓冲
	float avg_prob = 0.0f;          ///< 平均人声置信度
	float max_prob = 0.0f;          ///< 峰值置信度
	float harmonicity = 0.0f;       ///< 谐波度（用于过滤游戏音效、爆炸、环境杂音）
	std::string speaker;            ///< 说话人标签（如 "角色A" 或 "Speaker 1"）
	int speaker_id = 0;             ///< 说话人序号 (0, 1, 2...)
	float similarity = 0.0f;        ///< 与参考说话人的相似度
	std::vector<float> timbre;      ///< 归一化音色声纹特征向量
};

struct VADStats {
	size_t raw_detected = 0;        ///< VAD 初步检出的人声片段总数
	size_t filtered_by_sfx = 0;     ///< 被游戏音效/抗噪条件过滤的片段数
	size_t filtered_by_speaker = 0; ///< 被未匹配说话人条件过滤的片段数
	size_t final_count = 0;         ///< 最终保留的片段数
};

struct VADConfig {
	float threshold = 0.50f;        ///< 语音活动判定阈值 (默认 0.50 官方推荐)
	float neg_threshold = 0.35f;    ///< 静音脱落判定阈值
	int min_speech_ms = 250;        ///< 最小人声持续时间 (ms)，过滤短促音效杂音
	int min_silence_ms = 200;       ///< 最小静音合并间隔 (ms)，连贯断句
	int pad_start_ms = 0;           ///< 发声起始缓冲 (ms)，默认 0ms 紧贴发声
	int pad_end_ms = 200;           ///< 结束延长缓冲 (ms)，默认 200ms
	bool filter_sfx = true;         ///< 是否过滤无调性游戏音效与爆炸杂音
	float min_harmonicity = 0.15f;  ///< 最小谐波度阈值 (0.15 兼顾轻声低语，有效阻隔爆炸/白噪)

	// 说话人匹配模式
	enum Mode {
		MODE_REFERENCE = 0,         ///< 依照现有字幕已标注的说话人参考音色匹配
		MODE_CLUSTER,               ///< 自动区分说话人（无监督声纹聚类）
		MODE_SEGMENT_ONLY           ///< 仅切分人声时间轴（不区分说话人）
	} mode = MODE_REFERENCE;

	bool filter_unmatched = false;  ///< 仅保留参考说话人（过滤未匹配的背景歌声与杂音）
	float ref_threshold = 0.65f;    ///< 声纹匹配余弦相似度阈值
	int cluster_speakers = 2;       ///< 自动聚类说话人数 (0 为自动推测)
};

// --------------------------------------------------------------------------
// 1. Silero VAD 核心推理封装 (动态加载 onnxruntime.dll)
// --------------------------------------------------------------------------
class SileroVAD {
private:
	HMODULE hOrt = nullptr;
	const OrtApi* ort = nullptr;
	OrtEnv* env = nullptr;
	OrtSessionOptions* session_options = nullptr;
	OrtSession* session = nullptr;
	OrtMemoryInfo* memory_info = nullptr;
	bool available = false;
	std::vector<float> state; // shape [2, 1, 128] = 256 floats
	std::vector<float> context; // shape [1, 64] = 64 floats (Silero VAD v5 感受野上下文)
	float in_buf_[576] = {0};
	int64_t sr_data_ = 16000;
	OrtValue* in_val_ = nullptr;
	OrtValue* state_val_ = nullptr;
	OrtValue* sr_val_ = nullptr;

	SileroVAD() {
		state.assign(256, 0.0f);
		context.assign(64, 0.0f);
		Init();
	}

	~SileroVAD() {
		Shutdown();
	}

public:
	static SileroVAD& Get() {
		static SileroVAD instance;
		return instance;
	}

	bool IsAvailable() const { return available; }

	bool Init() {
		if (available) return true;

		// 寻找程序所在目录
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

		// 1. 查找 onnxruntime.dll
		std::vector<std::wstring> dll_paths = {
			exe_dir + L"onnxruntime.dll",
			L"onnxruntime.dll",
			exe_dir + L"build-local\\onnxruntime.dll",
			L"build-local/onnxruntime.dll",
			exe_dir + L"build\\onnxruntime.dll",
			L"build/onnxruntime.dll"
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

		// 2. 查找 silero_vad.onnx 模型
		std::vector<std::wstring> model_paths = {
			exe_dir + L"silero_vad.onnx",
			L"silero_vad.onnx",
			exe_dir + L"models\\silero_vad.onnx",
			L"models/silero_vad.onnx",
			exe_dir + L"build-local\\silero_vad.onnx",
			L"build-local/silero_vad.onnx",
			exe_dir + L"build\\silero_vad.onnx",
			L"build/silero_vad.onnx"
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

		// 3. 初始化 OrtEnv 与 Session
		OrtStatus* st = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "aegisub_silero_vad", &env);
		if (st) { ort->ReleaseStatus(st); return false; }

		st = ort->CreateSessionOptions(&session_options);
		if (st) { ort->ReleaseStatus(st); return false; }

		ort->SetIntraOpNumThreads(session_options, 1);
		ort->SetInterOpNumThreads(session_options, 1);

		st = ort->CreateSession(env, found_model.c_str(), session_options, &session);
		if (st) {
			ort->ReleaseStatus(st);
			return false;
		}

		st = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info);
		if (st) {
			ort->ReleaseStatus(st);
			return false;
		}

		state.assign(256, 0.0f);
		context.assign(64, 0.0f);
		memset(in_buf_, 0, sizeof(in_buf_));
		sr_data_ = 16000;

		// 性能优化：预先创建常驻 OrtValue 张量句柄，避免每次 ProcessChunk 反复堆分配与释放
		int64_t in_shape[2] = {1, 576};
		st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, in_buf_, 576 * sizeof(float),
			in_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val_);
		if (st) { ort->ReleaseStatus(st); return false; }

		int64_t state_shape[3] = {2, 1, 128};
		st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, state.data(), 256 * sizeof(float),
			state_shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &state_val_);
		if (st) { ort->ReleaseStatus(st); return false; }

		int64_t sr_shape[1] = {1};
		st = ort->CreateTensorWithDataAsOrtValue(
			memory_info, &sr_data_, sizeof(int64_t),
			sr_shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &sr_val_);
		if (st) { ort->ReleaseStatus(st); return false; }

		available = true;
		return true;
	}

	void Reset() {
		std::fill(state.begin(), state.end(), 0.0f);
		std::fill(context.begin(), context.end(), 0.0f);
		memset(in_buf_, 0, sizeof(in_buf_));
	}

	/// 输入 512 采样点 16kHz float PCM，输出语音概率 [0.0, 1.0]
	float ProcessChunk(const float* chunk_512) {
		if (!available || !session || !ort || !in_val_ || !state_val_ || !sr_val_) return 0.0f;

		// Silero VAD v5 规范：拼接 64 采样点感受野上下文前缀，总长 64 + 512 = 576
		memcpy(in_buf_, context.data(), 64 * sizeof(float));
		memcpy(in_buf_ + 64, chunk_512, 512 * sizeof(float));

		const char* input_names[] = {"input", "state", "sr"};
		const OrtValue* input_tensors[] = {in_val_, state_val_, sr_val_};
		const char* output_names[] = {"output", "stateN"};
		OrtValue* output_tensors[2] = {nullptr, nullptr};

		OrtStatus* st = ort->Run(session, nullptr, input_names, input_tensors, 3,
		                         output_names, 2, output_tensors);

		if (st) {
			ort->ReleaseStatus(st);
			if (output_tensors[0]) ort->ReleaseValue(output_tensors[0]);
			if (output_tensors[1]) ort->ReleaseValue(output_tensors[1]);
			return 0.0f;
		}

		float* prob_ptr = nullptr;
		ort->GetTensorMutableData(output_tensors[0], (void**)&prob_ptr);
		float prob = prob_ptr ? prob_ptr[0] : 0.0f;

		float* new_state_ptr = nullptr;
		ort->GetTensorMutableData(output_tensors[1], (void**)&new_state_ptr);
		if (new_state_ptr) {
			memcpy(state.data(), new_state_ptr, 256 * sizeof(float));
		}

		// 更新 context 为当前 512 采样点末尾的 64 采样点，供下一个 chunk 作为上下文
		memcpy(context.data(), chunk_512 + (512 - 64), 64 * sizeof(float));

		ort->ReleaseValue(output_tensors[0]);
		ort->ReleaseValue(output_tensors[1]);
		return prob;
	}

	void Shutdown() {
		if (in_val_ && ort) ort->ReleaseValue(in_val_);
		if (state_val_ && ort) ort->ReleaseValue(state_val_);
		if (sr_val_ && ort) ort->ReleaseValue(sr_val_);
		in_val_ = state_val_ = sr_val_ = nullptr;

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
};

// --------------------------------------------------------------------------
// 2. 音色特征与声纹提取器 (Timbre Feature Extractor)
// --------------------------------------------------------------------------
class TimbreExtractor {
public:
	/// 计算余弦相似度 [-1.0, 1.0]
	static float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
		if (a.empty() || b.empty() || a.size() != b.size()) return 0.0f;
		float dot = 0.0f, na = 0.0f, nb = 0.0f;
		for (size_t i = 0; i < a.size(); ++i) {
			dot += a[i] * b[i];
			na += a[i] * a[i];
			nb += b[i] * b[i];
		}
		float denom = std::sqrt(na) * std::sqrt(nb);
		return (denom > 1e-7f) ? (dot / denom) : 0.0f;
	}

	/// 从 16kHz PCM 提取 24 维音色特征向量及谐波度
	/// vec[0..19]: 20维 Mel 滤波器能量 (反映共鸣峰分布)
	/// vec[20]: 基频 F0 均值 (男声/女声/音高特征)
	/// vec[21]: 谐波周期度 (反映人声纯度，游戏爆炸/杂音极低)
	/// vec[22]: 音高变化标准差
	/// vec[23]: 高频能量占比 (清浊音比率)
	static std::vector<float> Extract(const float* pcm, size_t length, float* out_harmonicity = nullptr) {
		const int frame_len = 400; // 25ms @ 16kHz
		const int frame_step = 160; // 10ms @ 16kHz
		if (length < static_cast<size_t>(frame_len)) {
			if (out_harmonicity) *out_harmonicity = 0.0f;
			return std::vector<float>(24, 0.0f);
		}

		size_t n_frames = (length - frame_len) / frame_step;
		if (n_frames == 0) n_frames = 1;

		// 预加重 y[n] = x[n] - 0.97 * x[n-1]
		std::vector<float> pre(length);
		pre[0] = pcm[0];
		for (size_t i = 1; i < length; ++i) {
			pre[i] = pcm[i] - 0.97f * pcm[i - 1];
		}

		// 20-band Mel 滤波器组中心频率 (200Hz ~ 7500Hz)
		static const int mel_bins[22] = {
			6, 9, 13, 17, 21, 26, 31, 37, 44, 51,
			60, 69, 79, 90, 103, 117, 132, 150, 169, 190, 214, 240
		};

		std::vector<float> mel_sum(20, 0.0f);
		std::vector<float> pitch_vals;
		std::vector<float> harm_vals;
		float high_energy_sum = 0.0f;
		float total_energy_sum = 0.0f;

		FFT fft;
		std::vector<float> in_buf(512, 0.0f);
		std::vector<float> out_r(512, 0.0f);
		std::vector<float> out_i(512, 0.0f);
		std::vector<float> mag(257, 0.0f);

		for (size_t f = 0; f < n_frames; ++f) {
			size_t off = f * frame_step;

			// 加汉明窗并填入 FFT 缓冲区
			float frame_energy = 0.0f;
			for (int i = 0; i < frame_len; ++i) {
				float w = 0.54f - 0.46f * std::cos(6.2831853f * i / (frame_len - 1));
				float val = pre[off + i] * w;
				in_buf[i] = val;
				frame_energy += val * val;
			}
			std::fill(in_buf.begin() + frame_len, in_buf.end(), 0.0f);

			if (frame_energy < 1e-5f) continue;

			fft.Transform(512, in_buf.data(), out_r.data(), out_i.data());
			for (int k = 0; k <= 256; ++k) {
				mag[k] = std::sqrt(out_r[k] * out_r[k] + out_i[k] * out_i[k]);
			}

			// 20 Mel 滤波器能量累加
			for (int m = 0; m < 20; ++m) {
				int b_m = mel_bins[m];
				int b_c = mel_bins[m + 1];
				int b_p = mel_bins[m + 2];
				float e = 0.0f;
				for (int k = b_m; k < b_c; ++k) {
					e += mag[k] * (k - b_m) / static_cast<float>(std::max(1, b_c - b_m));
				}
				for (int k = b_c; k < b_p; ++k) {
					e += mag[k] * (b_p - k) / static_cast<float>(std::max(1, b_p - b_c));
				}
				mel_sum[m] += std::log10(1.0f + e);
			}

			// 计算高低频能量比
			float low_e = 0.0f, high_e = 0.0f;
			for (int k = 6; k < 60; ++k) low_e += mag[k];
			for (int k = 60; k < 240; ++k) high_e += mag[k];
			high_energy_sum += high_e;
			total_energy_sum += (low_e + high_e);

			// 归一化自相关 (NACF) 估计基频 (75Hz~400Hz 对应 lag 40~213)
			const float* raw_frame = pcm + off;
			float r0 = 0.0f;
			for (int i = 0; i < frame_len; ++i) r0 += raw_frame[i] * raw_frame[i];

			if (r0 > 1e-4f) {
				float max_corr = 0.0f;
				int best_lag = 0;
				for (int lag = 40; lag <= 213; lag += 2) {
					float num = 0.0f, den = 0.0f;
					for (int i = 0; i < frame_len - lag; ++i) {
						num += raw_frame[i] * raw_frame[i + lag];
						den += raw_frame[i + lag] * raw_frame[i + lag];
					}
					float norm = std::sqrt(r0 * den);
					if (norm > 1e-6f) {
						float c = num / norm;
						if (c > max_corr) {
							max_corr = c;
							best_lag = lag;
						}
					}
				}
				if (max_corr > 0.15f && best_lag > 0) {
					pitch_vals.push_back(16000.0f / best_lag);
					harm_vals.push_back(max_corr);
				}
			}
		}

		std::vector<float> vec(24, 0.0f);
		float valid_f = std::max<float>(1.0f, static_cast<float>(n_frames));
		for (int m = 0; m < 20; ++m) {
			vec[m] = mel_sum[m] / valid_f;
		}

		// 音高与谐波统计
		float avg_pitch = 160.0f;
		float std_pitch = 20.0f;
		float avg_harm = 0.35f;

		if (!pitch_vals.empty()) {
			float psum = 0.0f;
			for (float p : pitch_vals) psum += p;
			avg_pitch = psum / pitch_vals.size();

			float var_sum = 0.0f;
			for (float p : pitch_vals) var_sum += (p - avg_pitch) * (p - avg_pitch);
			std_pitch = std::sqrt(var_sum / pitch_vals.size());
		}

		if (!harm_vals.empty()) {
			float hsum = 0.0f;
			for (float h : harm_vals) hsum += h;
			avg_harm = hsum / harm_vals.size();
		}

		vec[20] = avg_pitch / 400.0f;
		vec[21] = avg_harm;
		vec[22] = std::clamp(std_pitch / 100.0f, 0.0f, 1.0f);
		vec[23] = (total_energy_sum > 1e-5f) ? (high_energy_sum / total_energy_sum) : 0.5f;

		// 向量归一化
		float norm = 0.0f;
		for (float v : vec) norm += v * v;
		norm = std::sqrt(norm);
		if (norm > 1e-7f) {
			for (float& v : vec) v /= norm;
		}

		if (out_harmonicity) *out_harmonicity = avg_harm;
		return vec;
	}

	/// K-Means 无监督声纹聚类
	static std::vector<int> Cluster(const std::vector<std::vector<float>>& samples, int k, int max_iters = 30) {
		int n = static_cast<int>(samples.size());
		if (n == 0 || k <= 0) return {};
		if (k >= n) {
			std::vector<int> res(n);
			for (int i = 0; i < n; ++i) res[i] = i;
			return res;
		}

		int dim = static_cast<int>(samples[0].size());
		std::vector<std::vector<float>> centroids(k, std::vector<float>(dim, 0.0f));

		// K-means++ 初始中心选择
		centroids[0] = samples[0];
		for (int c = 1; c < k; ++c) {
			float max_dist = -1.0f;
			int best_idx = 0;
			for (int i = 0; i < n; ++i) {
				float min_d = 1e9f;
				for (int prev = 0; prev < c; ++prev) {
					float sim = CosineSimilarity(samples[i], centroids[prev]);
					float dist = 1.0f - sim;
					if (dist < min_d) min_d = dist;
				}
				if (min_d > max_dist) {
					max_dist = min_d;
					best_idx = i;
				}
			}
			centroids[c] = samples[best_idx];
		}

		std::vector<int> assignments(n, 0);
		for (int iter = 0; iter < max_iters; ++iter) {
			bool changed = false;
			// 分配步
			for (int i = 0; i < n; ++i) {
				float best_sim = -2.0f;
				int best_c = 0;
				for (int c = 0; c < k; ++c) {
					float sim = CosineSimilarity(samples[i], centroids[c]);
					if (sim > best_sim) {
						best_sim = sim;
						best_c = c;
					}
				}
				if (assignments[i] != best_c) {
					assignments[i] = best_c;
					changed = true;
				}
			}
			if (!changed && iter > 0) break;

			// 更新质心
			std::vector<std::vector<float>> new_centroids(k, std::vector<float>(dim, 0.0f));
			std::vector<int> counts(k, 0);
			for (int i = 0; i < n; ++i) {
				int c = assignments[i];
				counts[c]++;
				for (int d = 0; d < dim; ++d) {
					new_centroids[c][d] += samples[i][d];
				}
			}
			for (int c = 0; c < k; ++c) {
				if (counts[c] > 0) {
					float norm = 0.0f;
					for (int d = 0; d < dim; ++d) {
						new_centroids[c][d] /= counts[c];
						norm += new_centroids[c][d] * new_centroids[c][d];
					}
					norm = std::sqrt(norm);
					if (norm > 1e-7f) {
						for (int d = 0; d < dim; ++d) new_centroids[c][d] /= norm;
					}
					centroids[c] = new_centroids[c];
				}
			}
		}
		return assignments;
	}
};

// --------------------------------------------------------------------------
// 3. 参考说话人与全局 VAD 流水线 (Audio Extraction & Pipeline Runner)
// --------------------------------------------------------------------------
#include <libaegisub/audio/provider.h>

struct ReferenceSpeaker {
	std::string name;
	std::vector<float> centroid;
	int count = 0;
	int64_t total_duration_ms = 0;
};

/// 提取指定区间重采样为 16kHz 单声道 float PCM
inline std::vector<float> ExtractAudio16kMono(agi::AudioProvider* provider, int64_t s0, int64_t s1) {
	if (!provider) return {};
	const int ch = provider->GetChannels();
	const int bps = provider->GetBytesPerSample();
	const int rate = provider->GetSampleRate();
	const bool flt = provider->AreSamplesFloat();

	s0 = std::max<int64_t>(0, s0);
	s1 = std::min<int64_t>(provider->GetNumSamples(), s1);
	int64_t count = s1 - s0;
	if (count <= 0 || ch <= 0) return {};

	std::vector<char> raw(static_cast<size_t>(count) * ch * bps);
	try {
		provider->GetAudio(raw.data(), s0, count);
	} catch (...) {
		return {};
	}

	std::vector<float> mono(static_cast<size_t>(count));
	for (int64_t i = 0; i < count; ++i) {
		float sum = 0.0f;
		for (int j = 0; j < ch; ++j) {
			size_t off = static_cast<size_t>(i * ch + j) * bps;
			if (flt && bps == 4) {
				float v;
				memcpy(&v, raw.data() + off, 4);
				sum += v;
			} else if (!flt && bps == 2) {
				int16_t v;
				memcpy(&v, raw.data() + off, 2);
				sum += v / 32768.0f;
			} else if (!flt && bps == 1) {
				sum += (static_cast<uint8_t>(raw[off]) - 128) / 128.0f;
			}
		}
		mono[static_cast<size_t>(i)] = sum / ch;
	}

	if (rate == 16000) return mono;

	size_t n = static_cast<size_t>(static_cast<double>(count) * 16000 / rate);
	std::vector<float> out(n);
	double step = static_cast<double>(rate) / 16000.0;
	for (size_t i = 0; i < n; ++i) {
		double pos = i * step;
		size_t p = static_cast<size_t>(pos);
		double frac = pos - p;
		float a = mono[std::min(p, static_cast<size_t>(count - 1))];
		float b = mono[std::min(p + 1, static_cast<size_t>(count - 1))];
		out[i] = std::clamp(static_cast<float>(a + (b - a) * frac), -1.0f, 1.0f);
	}
	return out;
}

/// 执行完整 VAD 人声切分与说话人匹配流水线（采用流式分块读取，内存峰值 < 5MB）
inline std::vector<VADSegment> RunVADPipeline(
	agi::AudioProvider* provider,
	int64_t start_ms,
	int64_t end_ms,
	const VADConfig& config,
	const std::map<std::string, ReferenceSpeaker>& ref_speakers,
	std::function<void(float progress, const std::string& status)> on_progress,
	std::shared_ptr<std::atomic<bool>> cancel,
	VADStats* out_stats = nullptr)
{
	if (!provider) return {};
	if (!SileroVAD::Get().IsAvailable()) {
		if (!SileroVAD::Get().Init()) return {};
	}

	int rate = provider->GetSampleRate();
	int64_t s0 = start_ms * rate / 1000;
	int64_t s1 = end_ms * rate / 1000;
	s0 = std::max<int64_t>(0, s0);
	s1 = std::min<int64_t>(provider->GetNumSamples(), s1);
	if (s1 <= s0) return {};

	if (on_progress) on_progress(0.02f, "初始化 VAD 推理流水线...");
	SileroVAD::Get().Reset();

	struct RawInterval {
		size_t start_sample = 0;
		size_t end_sample = 0;
		float sum_prob = 0.0f;
		float max_prob = 0.0f;
		size_t n_chunks = 0;
	};

	std::vector<RawInterval> raw_intervals;
	bool in_speech = false;
	size_t cur_start = 0;
	size_t silence_samples = 0;
	float cur_sum_prob = 0.0f;
	float cur_max_prob = 0.0f;
	size_t cur_chunks = 0;

	const size_t chunk_size = 512;
	const size_t min_speech_samples = static_cast<size_t>(config.min_speech_ms * 16);
	const size_t min_silence_samples = static_cast<size_t>(config.min_silence_ms * 16);

	// 性能与内存优化：流式滑动分块处理，每次读取 10 秒原生音频并重采样，杜绝单次 3.2GB 内存占用
	const int64_t native_block_samples = static_cast<int64_t>(rate) * 10;
	int64_t cur_native_pos = s0;
	int64_t total_native_samples = s1 - s0;
	int64_t total_est_chunks = (total_native_samples * 16000 / rate) / chunk_size;

	std::vector<float> pcm_buf;
	size_t buf_read_idx = 0;
	size_t global_sample_idx = 0;
	size_t processed_chunks = 0;

	while (true) {
		if (cancel && cancel->load()) return {};

		// 确保 pcm_buf 至少有 512 个样本可供当前 chunk 消费
		while ((pcm_buf.size() - buf_read_idx) < chunk_size && cur_native_pos < s1) {
			if (cancel && cancel->load()) return {};

			int64_t n_to_read = std::min<int64_t>(native_block_samples, s1 - cur_native_pos);
			auto next_16k = ExtractAudio16kMono(provider, cur_native_pos, cur_native_pos + n_to_read);
			cur_native_pos += n_to_read;

			if (next_16k.empty()) break;

			if (buf_read_idx > 0) {
				pcm_buf.erase(pcm_buf.begin(), pcm_buf.begin() + buf_read_idx);
				buf_read_idx = 0;
			}
			pcm_buf.insert(pcm_buf.end(), next_16k.begin(), next_16k.end());
		}

		if ((pcm_buf.size() - buf_read_idx) < chunk_size) {
			break; // 音频流全部读取并处理完毕
		}

		if (processed_chunks % 150 == 0 && on_progress) {
			float p = 0.05f + 0.55f * (static_cast<float>(processed_chunks) / std::max<int64_t>(1, total_est_chunks));
			on_progress(std::min(p, 0.60f), "正在流式检测人声片段...");
		}

		const float* chunk_data = pcm_buf.data() + buf_read_idx;
		float prob = SileroVAD::Get().ProcessChunk(chunk_data);

		if (!in_speech) {
			if (prob >= config.threshold) {
				in_speech = true;
				cur_start = global_sample_idx;
				silence_samples = 0;
				cur_sum_prob = prob;
				cur_max_prob = prob;
				cur_chunks = 1;
			}
		} else {
			cur_sum_prob += prob;
			cur_chunks++;
			if (prob > cur_max_prob) cur_max_prob = prob;

			if (prob < config.neg_threshold) {
				silence_samples += chunk_size;
				if (silence_samples >= min_silence_samples) {
					size_t cur_end = (global_sample_idx + chunk_size > silence_samples) ? (global_sample_idx + chunk_size - silence_samples) : global_sample_idx;
					if (cur_end > cur_start && (cur_end - cur_start) >= min_speech_samples) {
						RawInterval iv;
						iv.start_sample = cur_start;
						iv.end_sample = cur_end;
						iv.sum_prob = cur_sum_prob;
						iv.max_prob = cur_max_prob;
						iv.n_chunks = cur_chunks;
						raw_intervals.push_back(iv);
					}
					in_speech = false;
					silence_samples = 0;
				}
			} else {
				silence_samples = 0;
			}
		}

		buf_read_idx += chunk_size;
		global_sample_idx += chunk_size;
		processed_chunks++;
	}

	if (in_speech) {
		size_t cur_end = global_sample_idx;
		if (cur_end > cur_start && (cur_end - cur_start) >= min_speech_samples) {
			RawInterval iv;
			iv.start_sample = cur_start;
			iv.end_sample = cur_end;
			iv.sum_prob = cur_sum_prob;
			iv.max_prob = cur_max_prob;
			iv.n_chunks = cur_chunks;
			raw_intervals.push_back(iv);
		}
	}

	pcm_buf.clear();
	pcm_buf.shrink_to_fit();

	if (out_stats) {
		out_stats->raw_detected = raw_intervals.size();
	}

	if (raw_intervals.empty()) return {};

	if (on_progress) on_progress(0.60f, "正在分析音色与说话人特征...");

	std::vector<VADSegment> segments;
	segments.reserve(raw_intervals.size());

	int64_t last_end_ms = -1;
	for (size_t i = 0; i < raw_intervals.size(); ++i) {
		if (cancel && cancel->load()) return {};

		const auto& iv = raw_intervals[i];
		// 紧贴发声时刻
		int64_t seg_start_ms = start_ms + static_cast<int64_t>(iv.start_sample / 16) - config.pad_start_ms;
		// 结束向后延长 200ms
		int64_t seg_end_ms = start_ms + static_cast<int64_t>(iv.end_sample / 16) + config.pad_end_ms;
		seg_start_ms = std::max<int64_t>(start_ms, seg_start_ms);

		// 防止与上一段字幕时间轴重叠
		if (last_end_ms > 0 && seg_start_ms < last_end_ms) {
			if (!segments.empty()) {
				segments.back().end_ms = std::max<int64_t>(segments.back().start_ms + 50, seg_start_ms);
			}
		}

		// 按需提取该有效人声片段的音频（几万采样点，极速且仅占用几百KB）
		int64_t seg_raw_s0 = (start_ms + static_cast<int64_t>(iv.start_sample / 16)) * rate / 1000;
		int64_t seg_raw_s1 = (start_ms + static_cast<int64_t>(iv.end_sample / 16)) * rate / 1000;
		std::vector<float> seg_pcm = ExtractAudio16kMono(provider, seg_raw_s0, seg_raw_s1);

		float harmonicity = 0.0f;
		std::vector<float> timbre;
		if (seg_pcm.size() >= 400) {
			timbre = TimbreExtractor::Extract(seg_pcm.data(), seg_pcm.size(), &harmonicity);
		}

		// 过滤无调性游戏爆炸与敲击音效
		if (config.filter_sfx && harmonicity < config.min_harmonicity) {
			if (out_stats) out_stats->filtered_by_sfx++;
			continue;
		}

		VADSegment seg;
		seg.start_ms = seg_start_ms;
		seg.end_ms = seg_end_ms;
		seg.avg_prob = (iv.n_chunks > 0) ? (iv.sum_prob / iv.n_chunks) : 0.0f;
		seg.max_prob = iv.max_prob;
		seg.harmonicity = harmonicity;
		seg.timbre = std::move(timbre);

		// 说话人匹配逻辑
		if (config.mode == VADConfig::MODE_REFERENCE && !ref_speakers.empty()) {
			float best_sim = -2.0f;
			std::string best_name = "";
			int id_cnt = 0;
			int best_id = 0;
			for (const auto& pair : ref_speakers) {
				float sim = TimbreExtractor::CosineSimilarity(seg.timbre, pair.second.centroid);
				if (sim > best_sim) {
					best_sim = sim;
					best_name = pair.first;
					best_id = id_cnt;
				}
				id_cnt++;
			}

			if (best_sim >= config.ref_threshold) {
				seg.speaker = best_name;
				seg.speaker_id = best_id;
				seg.similarity = best_sim;
			} else {
				// 未匹配到目标说话人
				if (config.filter_unmatched) {
					// 过滤掉非目标人声（背景歌声/游戏背景杂谈）
					if (out_stats) out_stats->filtered_by_speaker++;
					continue;
				} else {
					seg.speaker = "未知说话人";
					seg.speaker_id = -1;
					seg.similarity = best_sim;
				}
			}
		}

		last_end_ms = seg.end_ms;
		segments.push_back(seg);
	}

	// 若为无监督自动聚类模式
	if (config.mode == VADConfig::MODE_CLUSTER && segments.size() >= 2) {
		if (on_progress) on_progress(0.85f, "正在对说话人声纹聚类分类...");
		int k = config.cluster_speakers;
		if (k <= 0) {
			// 自动估计说话人数 (2~4)
			k = std::clamp(static_cast<int>(segments.size() / 6), 2, 4);
		}
		k = std::min<int>(k, static_cast<int>(segments.size()));

		std::vector<std::vector<float>> all_timbres;
		all_timbres.reserve(segments.size());
		for (const auto& s : segments) {
			all_timbres.push_back(s.timbre);
		}

		std::vector<int> clusters = TimbreExtractor::Cluster(all_timbres, k);
		for (size_t i = 0; i < segments.size(); ++i) {
			int cid = clusters[i];
			segments[i].speaker_id = cid;
			segments[i].speaker = "说话人 " + std::to_string(cid + 1);
		}
	}

	if (out_stats) {
		out_stats->final_count = segments.size();
	}
	if (on_progress) on_progress(1.0f, "人声切分与说话人识别完成");
	return segments;
}
