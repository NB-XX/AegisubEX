#include "subocr_engine.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

SubocrEngine& SubocrEngine::Get() {
	static SubocrEngine instance;
	return instance;
}

SubocrEngine::SubocrEngine() {
	Load();
}

SubocrEngine::~SubocrEngine() {
	Unload();
}

void SubocrEngine::Unload() {
	if (m_hLib) {
#ifdef _WIN32
		FreeLibrary(m_hLib);
#endif
		m_hLib = nullptr;
	}
	m_available = false;
	m_loaded_path.clear();

	fn_init = nullptr;
	fn_deinit = nullptr;
	fn_detect = nullptr;
	fn_query_anchors = nullptr;
	fn_start_predet = nullptr;
	fn_start_pipeline = nullptr;
	fn_query_progress = nullptr;
	fn_query_new_subtitles = nullptr;
	fn_stop_all = nullptr;

	fn_free_anchors = nullptr;
	fn_free_subtitles = nullptr;
	fn_free_bboxes = nullptr;
	fn_free_cstrings = nullptr;
	fn_free_char = nullptr;
	fn_malloc_anchors = nullptr;
}

bool SubocrEngine::Load(const std::string& custom_dir_u8) {
	if (custom_dir_u8.empty()) return Load(std::wstring());
#ifdef _WIN32
	int len = MultiByteToWideChar(CP_UTF8, 0, custom_dir_u8.c_str(), -1, nullptr, 0);
	if (len <= 0) return Load(std::wstring());
	std::wstring w(len, 0);
	MultiByteToWideChar(CP_UTF8, 0, custom_dir_u8.c_str(), -1, &w[0], len);
	while (!w.empty() && w.back() == 0) w.pop_back();
	return Load(w);
#else
	return false;
#endif
}

bool SubocrEngine::Load(const std::wstring& custom_dir) {
	if (m_available) return true;

#ifdef _WIN32
	// 查找可用的 subocr.dll 或 subocr.exe 路径
	std::vector<std::wstring> candidate_dirs;
	if (!custom_dir.empty()) {
		candidate_dirs.push_back(custom_dir);
	}

	// 1. 常见安装路径与上级工具目录
	candidate_dirs.push_back(L"D:\\Tool\\SubtitleOCR_Community_v3.1.2\\望言OCR_v3.1.2");
	candidate_dirs.push_back(L"C:\\Tool\\SubtitleOCR_Community_v3.1.2\\望言OCR_v3.1.2");
	candidate_dirs.push_back(L"C:\\Libs\\subocr");
	candidate_dirs.push_back(L"D:\\Libs\\subocr");

	// 2. 本应用程序当前执行目录
	wchar_t exe_path_buf[MAX_PATH] = {0};
	GetModuleFileNameW(NULL, exe_path_buf, MAX_PATH);
	std::wstring app_dir(exe_path_buf);
	size_t slash = app_dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		std::wstring base = app_dir.substr(0, slash);
		candidate_dirs.push_back(base);
		candidate_dirs.push_back(base + L"\\subocr");
		candidate_dirs.push_back(base + L"\\build-local");
	}

	HMODULE h = nullptr;
	std::wstring matched_dir;

	for (const auto& dir : candidate_dirs) {
		if (dir.empty()) continue;
		std::vector<std::wstring> names = { L"\\subocr.dll", L"\\subocr.exe" };
		for (const auto& name : names) {
			std::wstring full_path = dir + name;
			DWORD attr = GetFileAttributesW(full_path.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
				// 设置 DLL 依赖搜索目录以载入 onnxruntime / opencv / ffmpeg dll
				SetDllDirectoryW(dir.c_str());
				h = LoadLibraryW(full_path.c_str());
				if (h) {
					matched_dir = dir;
					break;
				}
			}
		}
		if (h) break;
	}

	if (!h) return false;

	// 解析函数符号
	fn_init = (Fn_subocr_init)GetProcAddress(h, "subocr_init");
	fn_deinit = (Fn_subocr_deinit)GetProcAddress(h, "subocr_deinit");
	fn_detect = (Fn_subocr_detect)GetProcAddress(h, "subocr_detect");
	fn_query_anchors = (Fn_subocr_query_anchors)GetProcAddress(h, "subocr_query_anchors");
	fn_start_predet = (Fn_subocr_start_predet)GetProcAddress(h, "subocr_start_predet");
	fn_start_pipeline = (Fn_subocr_start_pipeline)GetProcAddress(h, "subocr_start_pipeline");
	fn_query_progress = (Fn_subocr_query_progress)GetProcAddress(h, "subocr_query_progress");
	fn_query_new_subtitles = (Fn_subocr_query_new_subtitles)GetProcAddress(h, "subocr_query_new_subtitles");
	fn_stop_all = (Fn_subocr_stop_all)GetProcAddress(h, "subocr_stop_all");

	fn_free_anchors = (Fn_free_array)GetProcAddress(h, "SubtitleAnchorArrayFree");
	fn_free_subtitles = (Fn_free_array)GetProcAddress(h, "SubtitleArrayFree");
	fn_free_bboxes = (Fn_free_array)GetProcAddress(h, "BoundingBoxArrayFree");
	fn_free_cstrings = (Fn_free_array)GetProcAddress(h, "CStringArrayFree");
	fn_free_char = (Fn_free_array)GetProcAddress(h, "charArrayFree");
	fn_malloc_anchors = (Fn_anchor_malloc)GetProcAddress(h, "SubtitleAnchorArrayMalloc");

	if (!fn_init || !fn_deinit || !fn_start_pipeline || !fn_query_progress || !fn_query_new_subtitles || !fn_stop_all) {
		FreeLibrary(h);
		return false;
	}

	m_hLib = h;
	m_available = true;
	m_loaded_path = matched_dir;
	return true;
#else
	return false;
#endif
}

bool SubocrEngine::MatchLine(const std::string& video_path, int ref_time_ms,
                             int x0, int y0, int x1, int y1,
                             int& out_start_ms, int& out_end_ms) {
	if (!m_available && !Load()) return false;
	if (video_path.empty()) return false;

	// 初始化算法上下文
	void* ctx = fn_init("", 0);
	if (!ctx) return false;

	SubocrArray<SubocrSubtitleAnchor> anchors;
	bool heap_allocated = false;
	if (fn_malloc_anchors) {
		anchors = fn_malloc_anchors(1);
	} else {
		anchors.size = 1;
		anchors.data = new SubocrSubtitleAnchor[1];
		heap_allocated = true;
	}

	anchors.data[0].center_x = (x0 + x1) / 2;
	anchors.data[0].center_y = (y0 + y1) / 2;
	anchors.data[0].height = std::max(12, y1 - y0);
	anchors.data[0].lang = SUBOCR_LANG_ZH;
	anchors.data[0].is_primary = 1;
	anchors.data[0].avg_width = 0;
	anchors.data[0].min_width = 0;
	anchors.data[0].mid_width = 0;
	anchors.data[0].max_width = 0;

	// 启动提取流水线：15 FPS 采样，最小字幕存续阈值 250ms (250,000 us)
	int started = fn_start_pipeline(ctx, video_path.c_str(), 15, anchors, 250000);
	if (started == 0) {
		if (heap_allocated) delete[] anchors.data;
		else if (fn_free_anchors) fn_free_anchors(&anchors);
		fn_deinit(ctx);
		return false;
	}

	bool found = false;
	int found_start = 0, found_end = 0;
	int64_t target_ms = static_cast<int64_t>(ref_time_ms);

	// 轮询提取进度与新生成的字幕行
	for (int iter = 0; iter < 1000; ++iter) {
		SubocrProgress prog = fn_query_progress(ctx);
		SubocrArray<SubocrSubtitle> subs = fn_query_new_subtitles(ctx);

		for (size_t i = 0; i < subs.size; ++i) {
			auto& sub = subs.data[i];
			int64_t s_ms = sub.start_us / 1000;
			int64_t e_ms = sub.end_us / 1000;

			// 如果基准时间点落在此字幕区间（允许前置/后置轻微容差）
			if (target_ms >= (s_ms - 150) && target_ms <= (e_ms + 150)) {
				found_start = static_cast<int>(s_ms);
				found_end = static_cast<int>(e_ms);
				found = true;
			}

			// 清理内部 CStringArray
			if (fn_free_cstrings) {
				for (size_t j = 0; j < sub.cstringArray.size; ++j) {
					if (fn_free_char) fn_free_char(sub.cstringArray.data[j].data);
				}
				fn_free_cstrings(&sub.cstringArray);
			}
		}

		if (fn_free_subtitles) {
			fn_free_subtitles(&subs);
		}

		if (found) {
			// 已找到目标台词，立即终止后台视频流解码以节省算力
			fn_stop_all(ctx);
			break;
		}

		if (prog.is_finished) break;

		// 若解码进度已超出基准时间 5 秒以上仍未命中，提前中止退出
		int64_t current_ms = prog.current_us / 1000;
		if (current_ms > target_ms + 5000) {
			fn_stop_all(ctx);
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	if (heap_allocated) delete[] anchors.data;
	else if (fn_free_anchors) fn_free_anchors(&anchors);

	fn_stop_all(ctx);
	fn_deinit(ctx);

	if (found && found_end > found_start) {
		out_start_ms = found_start;
		out_end_ms = found_end;
		return true;
	}

	return false;
}

bool SubocrEngine::ExtractIntervals(const std::string& video_path,
                                    int x0, int y0, int x1, int y1,
                                    std::vector<std::pair<int, int>>& out_intervals,
                                    const std::function<bool(double, double)>& progress_cb) {
	if (!m_available && !Load()) return false;
	if (video_path.empty()) return false;

	void* ctx = fn_init("", 0);
	if (!ctx) return false;

	SubocrArray<SubocrSubtitleAnchor> anchors;
	bool heap_allocated = false;
	if (fn_malloc_anchors) {
		anchors = fn_malloc_anchors(1);
	} else {
		anchors.size = 1;
		anchors.data = new SubocrSubtitleAnchor[1];
		heap_allocated = true;
	}

	anchors.data[0].center_x = (x0 + x1) / 2;
	anchors.data[0].center_y = (y0 + y1) / 2;
	anchors.data[0].height = std::max(12, y1 - y0);
	anchors.data[0].lang = SUBOCR_LANG_ZH;
	anchors.data[0].is_primary = 1;

	int started = fn_start_pipeline(ctx, video_path.c_str(), 15, anchors, 250000);
	if (started == 0) {
		if (heap_allocated) delete[] anchors.data;
		else if (fn_free_anchors) fn_free_anchors(&anchors);
		fn_deinit(ctx);
		return false;
	}

	bool user_aborted = false;
	while (!user_aborted) {
		SubocrProgress prog = fn_query_progress(ctx);
		SubocrArray<SubocrSubtitle> subs = fn_query_new_subtitles(ctx);

		for (size_t i = 0; i < subs.size; ++i) {
			auto& sub = subs.data[i];
			int s_ms = static_cast<int>(sub.start_us / 1000);
			int e_ms = static_cast<int>(sub.end_us / 1000);
			if (e_ms > s_ms) {
				out_intervals.emplace_back(s_ms, e_ms);
			}

			if (fn_free_cstrings) {
				for (size_t j = 0; j < sub.cstringArray.size; ++j) {
					if (fn_free_char) fn_free_char(sub.cstringArray.data[j].data);
				}
				fn_free_cstrings(&sub.cstringArray);
			}
		}

		if (fn_free_subtitles) {
			fn_free_subtitles(&subs);
		}

		if (prog.duration_us > 0 && progress_cb) {
			double pct = static_cast<double>(prog.current_us - prog.start_us) / static_cast<double>(prog.duration_us);
			pct = std::clamp(pct, 0.0, 1.0);
			if (!progress_cb(pct, prog.speed_up)) {
				user_aborted = true;
				fn_stop_all(ctx);
				break;
			}
		}

		if (prog.is_finished) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
	}

	if (heap_allocated) delete[] anchors.data;
	else if (fn_free_anchors) fn_free_anchors(&anchors);

	fn_stop_all(ctx);
	fn_deinit(ctx);

	// 按起始时间排序
	std::sort(out_intervals.begin(), out_intervals.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});

	return !out_intervals.empty();
}
