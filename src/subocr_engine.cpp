#include "subocr_engine.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>

static bool SafeSubocrMatchLine(
	SubocrEngine::Fn_subocr_init fn_init,
	SubocrEngine::Fn_subocr_deinit fn_deinit,
	SubocrEngine::Fn_subocr_start_pipeline fn_start_pipeline,
	SubocrEngine::Fn_subocr_query_progress fn_query_progress,
	SubocrEngine::Fn_subocr_query_new_subtitles fn_query_new_subtitles,
	SubocrEngine::Fn_subocr_stop_all fn_stop_all,
	SubocrEngine::Fn_free_array fn_free_anchors,
	SubocrEngine::Fn_free_array fn_free_subtitles,
	SubocrEngine::Fn_free_array fn_free_cstrings,
	SubocrEngine::Fn_free_array fn_free_char,
	SubocrEngine::Fn_anchor_malloc fn_malloc_anchors,
	const char* video_path, int ref_time_ms,
	int x0, int y0, int x1, int y1,
	int& out_start_ms, int& out_end_ms)
{
	__try {
		void* ctx = fn_init("", 0);
		if (!ctx) return false;

		SubocrArray<SubocrSubtitleAnchor> anchors{};
		bool heap_allocated = false;
		if (fn_malloc_anchors) {
			anchors = fn_malloc_anchors(1);
		} else {
			anchors.size = 1;
			anchors.data = (SubocrSubtitleAnchor*)malloc(sizeof(SubocrSubtitleAnchor));
			heap_allocated = true;
		}

		if (!anchors.data) {
			fn_deinit(ctx);
			return false;
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

		int started = fn_start_pipeline(ctx, video_path, 15, anchors, 250000);
		if (started == 0) {
			if (heap_allocated) free(anchors.data);
			else if (fn_free_anchors) fn_free_anchors(&anchors);
			fn_deinit(ctx);
			return false;
		}

		bool found = false;
		int found_start = 0, found_end = 0;
		int64_t target_ms = static_cast<int64_t>(ref_time_ms);

		for (int iter = 0; iter < 1000; ++iter) {
			SubocrProgress prog = fn_query_progress(ctx);
			SubocrArray<SubocrSubtitle> subs = fn_query_new_subtitles(ctx);

			for (size_t i = 0; i < subs.size; ++i) {
				auto& sub = subs.data[i];
				int64_t s_ms = sub.start_us / 1000;
				int64_t e_ms = sub.end_us / 1000;

				if (target_ms >= (s_ms - 150) && target_ms <= (e_ms + 150)) {
					found_start = static_cast<int>(s_ms);
					found_end = static_cast<int>(e_ms);
					found = true;
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

			if (found) {
				fn_stop_all(ctx);
				break;
			}

			if (prog.is_finished) break;
			int64_t current_ms = prog.current_us / 1000;
			if (current_ms > target_ms + 5000) {
				fn_stop_all(ctx);
				break;
			}

			Sleep(25);
		}

		if (heap_allocated) free(anchors.data);
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
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}
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
	std::vector<std::wstring> candidate_dirs;
	if (!custom_dir.empty()) {
		candidate_dirs.push_back(custom_dir);
	}

	// 本应用程序当前执行目录及 subocr 子目录
	wchar_t exe_path_buf[MAX_PATH] = {0};
	GetModuleFileNameW(NULL, exe_path_buf, MAX_PATH);
	std::wstring app_dir(exe_path_buf);
	size_t slash = app_dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		std::wstring base = app_dir.substr(0, slash);
		candidate_dirs.push_back(base + L"\\subocr");
		candidate_dirs.push_back(base);
	}

	HMODULE h = nullptr;
	std::wstring matched_dir;

	for (const auto& dir : candidate_dirs) {
		if (dir.empty()) continue;
		// 严禁加载 .exe！只加载规范的动态链接库 subocr.dll，彻底杜绝未初始化 CRT 崩溃
		std::wstring full_path = dir + L"\\subocr.dll";
		DWORD attr = GetFileAttributesW(full_path.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
			SetDllDirectoryW(dir.c_str());
			h = LoadLibraryW(full_path.c_str());
			if (h) {
				matched_dir = dir;
				break;
			}
		}
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

#ifdef _WIN32
	return SafeSubocrMatchLine(
		fn_init, fn_deinit, fn_start_pipeline, fn_query_progress,
		fn_query_new_subtitles, fn_stop_all, fn_free_anchors,
		fn_free_subtitles, fn_free_cstrings, fn_free_char,
		fn_malloc_anchors, video_path.c_str(), ref_time_ms,
		x0, y0, x1, y1, out_start_ms, out_end_ms);
#else
	return false;
#endif
}

bool SubocrEngine::ExtractIntervals(const std::string& video_path,
                                    int x0, int y0, int x1, int y1,
                                    std::vector<std::pair<int, int>>& out_intervals,
                                    const std::function<bool(double, double)>& progress_cb) {
	if (!m_available && !Load()) return false;
	if (video_path.empty()) return false;

	return false;
}
