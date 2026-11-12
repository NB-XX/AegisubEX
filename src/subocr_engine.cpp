#include "subocr_engine.h"

#include <algorithm>
#include <vector>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>

static bool IsPureAscii(const std::wstring& s) {
	for (wchar_t c : s) {
		if (c > 127) return false;
	}
	return true;
}

static bool IsPureAscii(const std::string& s) {
	for (unsigned char c : s) {
		if (c > 127) return false;
	}
	return true;
}

static bool FileExists(const std::wstring& path) {
	DWORD attr = GetFileAttributesW(path.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static bool ValidateResourceDirectory(const std::wstring& dir) {
	if (dir.empty()) return false;
	std::wstring keys_zh = dir + L"\\keys\\keys_zh.txt";
	std::wstring model_det = dir + L"\\models\\det_v3_b4_960_fp16.onnx";
	return FileExists(keys_zh) && FileExists(model_det);
}

static std::wstring ResolveResourceDirectory(const std::vector<std::wstring>& dirs) {
	for (const auto& dir : dirs) {
		if (dir.empty()) continue;
		if (ValidateResourceDirectory(dir)) return dir;
		if (ValidateResourceDirectory(dir + L"\\alg-resources")) return dir + L"\\alg-resources";
		if (ValidateResourceDirectory(dir + L"\\subocr")) return dir + L"\\subocr";
		if (ValidateResourceDirectory(dir + L"\\subocr\\alg-resources")) return dir + L"\\subocr\\alg-resources";
	}
	return std::wstring();
}

static std::string GetSafeAsciiResourceDir(const std::wstring& unicode_dir) {
	if (unicode_dir.empty()) return "";
	if (IsPureAscii(unicode_dir)) {
		return std::string(unicode_dir.begin(), unicode_dir.end());
	}

	wchar_t temp_buf[MAX_PATH] = {0};
	GetTempPathW(MAX_PATH, temp_buf);
	std::wstring temp_dir(temp_buf);
	if (!temp_dir.empty() && temp_dir.back() == L'\\') temp_dir.pop_back();

	std::wstring junction_path;
	if (IsPureAscii(temp_dir)) {
		junction_path = temp_dir + L"\\aegisub_subocr_models";
	} else {
		junction_path = L"C:\\ProgramData\\Aegisub\\subocr_models";
		CreateDirectoryW(L"C:\\ProgramData", NULL);
		CreateDirectoryW(L"C:\\ProgramData\\Aegisub", NULL);
	}

	std::wstring cmd = L"cmd.exe /c (rmdir \"" + junction_path + L"\" 2>nul & mklink /J \"" + junction_path + L"\" \"" + unicode_dir + L"\" >nul 2>nul)";
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if (CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 3000);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}

	if (ValidateResourceDirectory(junction_path) && IsPureAscii(junction_path)) {
		return std::string(junction_path.begin(), junction_path.end());
	}
	return "";
}

static std::string GetSafeAsciiVideoPath(const std::string& video_path, std::wstring& out_cleanup_hardlink) {
	out_cleanup_hardlink.clear();
	if (video_path.empty()) return "";
	if (IsPureAscii(video_path)) {
		return video_path;
	}

	int wlen = MultiByteToWideChar(CP_UTF8, 0, video_path.c_str(), -1, nullptr, 0);
	if (wlen <= 0) return video_path;
	std::wstring w_vid(wlen, 0);
	MultiByteToWideChar(CP_UTF8, 0, video_path.c_str(), -1, &w_vid[0], wlen);
	while (!w_vid.empty() && w_vid.back() == 0) w_vid.pop_back();

	std::wstring ext = L".mp4";
	size_t dot = w_vid.find_last_of(L'.');
	if (dot != std::wstring::npos) ext = w_vid.substr(dot);

	wchar_t temp_buf[MAX_PATH] = {0};
	GetTempPathW(MAX_PATH, temp_buf);
	std::wstring temp_dir(temp_buf);
	if (!temp_dir.empty() && temp_dir.back() == L'\\') temp_dir.pop_back();

	DWORD pid = GetCurrentProcessId();
	static unsigned int link_seq = 0;
	std::wstring link_name = L"\\subocr_link_" + std::to_wstring(pid) + L"_" + std::to_wstring(++link_seq) + ext;

	std::wstring link_candidate;
	if (w_vid.size() >= 2 && temp_dir.size() >= 2 && towupper(w_vid[0]) == towupper(temp_dir[0])) {
		link_candidate = temp_dir + link_name;
	} else if (w_vid.size() >= 3 && w_vid[1] == L':') {
		link_candidate = w_vid.substr(0, 2) + link_name;
	} else {
		link_candidate = temp_dir + link_name;
	}

	DeleteFileW(link_candidate.c_str());
	if (CreateHardLinkW(link_candidate.c_str(), w_vid.c_str(), NULL)) {
		if (IsPureAscii(link_candidate)) {
			out_cleanup_hardlink = link_candidate;
			return std::string(link_candidate.begin(), link_candidate.end());
		}
	}

	wchar_t short_buf[MAX_PATH] = {0};
	if (GetShortPathNameW(w_vid.c_str(), short_buf, MAX_PATH) > 0) {
		std::wstring s_w(short_buf);
		if (IsPureAscii(s_w)) {
			return std::string(s_w.begin(), s_w.end());
		}
	}
	return video_path;
}

static void* CallSubocrInitSafe(SubocrEngine::Fn_subocr_init fn_init, const char* path, int device) {
	__try {
		return fn_init(path, device);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

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
	const char* safe_resource_path,
	const char* video_path, int ref_time_ms,
	int x0, int y0, int x1, int y1,
	int& out_start_ms, int& out_end_ms)
{
	if (!fn_init || !fn_deinit || !fn_start_pipeline || !safe_resource_path || !safe_resource_path[0]) {
		return false;
	}

	__try {
		void* ctx = fn_init(safe_resource_path, 0);
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

				if (target_ms >= (s_ms - 250) && target_ms <= (e_ms + 250)) {
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
	m_resource_path.clear();
	m_safe_ascii_res_path.clear();
	m_status_message.clear();

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
	if (m_available && (custom_dir.empty() || custom_dir == m_loaded_path)) {
		return true;
	}

#ifdef _WIN32
	if (m_hLib) {
		Unload();
	}

	std::vector<std::wstring> candidate_dll_dirs;
	if (!custom_dir.empty()) {
		candidate_dll_dirs.push_back(custom_dir);
		candidate_dll_dirs.push_back(custom_dir + L"\\subocr");
	}

	wchar_t exe_path_buf[MAX_PATH] = {0};
	GetModuleFileNameW(NULL, exe_path_buf, MAX_PATH);
	std::wstring app_dir(exe_path_buf);
	size_t slash = app_dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		std::wstring base = app_dir.substr(0, slash);
		candidate_dll_dirs.push_back(base + L"\\subocr");
		candidate_dll_dirs.push_back(base);
		size_t parent_slash = base.find_last_of(L"\\/");
		if (parent_slash != std::wstring::npos) {
			std::wstring parent = base.substr(0, parent_slash);
			candidate_dll_dirs.push_back(parent + L"\\subocr");
			candidate_dll_dirs.push_back(parent);
		}
	}
	candidate_dll_dirs.push_back(L"C:\\Program Files\\望言OCR");
	candidate_dll_dirs.push_back(L"C:\\Program Files (x86)\\望言OCR");

	HMODULE h = nullptr;
	std::wstring matched_dll_dir;

	for (const auto& dir : candidate_dll_dirs) {
		if (dir.empty()) continue;
		std::wstring full_path = dir + L"\\subocr.dll";
		if (FileExists(full_path)) {
			SetDllDirectoryW(dir.c_str());
			h = LoadLibraryW(full_path.c_str());
			if (h) {
				matched_dll_dir = dir;
				break;
			}
		}
	}

	if (!h) {
		m_status_message = "未找到有效的 subocr.dll 动态链接库";
		return false;
	}

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
		m_status_message = "subocr.dll 缺少必要的 C-ABI 导出函数";
		return false;
	}

	std::vector<std::wstring> resource_candidates;
	if (!custom_dir.empty()) resource_candidates.push_back(custom_dir);
	resource_candidates.push_back(matched_dll_dir);
	if (slash != std::wstring::npos) {
		std::wstring base = app_dir.substr(0, slash);
		resource_candidates.push_back(base + L"\\subocr");
		size_t parent_slash = base.find_last_of(L"\\/");
		if (parent_slash != std::wstring::npos) {
			resource_candidates.push_back(base.substr(0, parent_slash) + L"\\subocr");
		}
	}
	resource_candidates.push_back(L"C:\\Program Files\\望言OCR\\alg-resources");
	resource_candidates.push_back(L"C:\\Program Files\\望言OCR");

	std::wstring resolved_res_dir = ResolveResourceDirectory(resource_candidates);
	if (resolved_res_dir.empty()) {
		m_hLib = h;
		m_loaded_path = matched_dll_dir;
		m_available = false;
		m_status_message = "已加载 subocr.dll，但未找到模型资源 (缺少 keys/models 文件夹)";
		return false;
	}

	std::string safe_res = GetSafeAsciiResourceDir(resolved_res_dir);
	if (safe_res.empty()) {
		m_hLib = h;
		m_loaded_path = matched_dll_dir;
		m_available = false;
		m_status_message = "模型资源路径包含非ASCII字符，且无法建立临时转接映射";
		return false;
	}

	void* probe_ctx = CallSubocrInitSafe(fn_init, safe_res.c_str(), 0);

	if (!probe_ctx) {
		FreeLibrary(h);
		m_status_message = "subocr_init 初始化探针失败 (DirectML/GPU 环境异常)";
		return false;
	}

	fn_deinit(probe_ctx);

	m_hLib = h;
	m_available = true;
	m_loaded_path = matched_dll_dir;
	m_resource_path = resolved_res_dir;
	m_safe_ascii_res_path = safe_res;
	m_status_message = "望言OCR (SubtitleOCR) 引擎已成功就绪并验证通过";
	return true;
#else
	return false;
#endif
}

bool SubocrEngine::MatchLine(const std::string& video_path, int ref_time_ms,
                             int x0, int y0, int x1, int y1,
                             int& out_start_ms, int& out_end_ms) {
	if (!m_available && !Load()) return false;
	if (video_path.empty() || m_safe_ascii_res_path.empty()) return false;

#ifdef _WIN32
	std::wstring cleanup_hardlink;
	std::string safe_video = GetSafeAsciiVideoPath(video_path, cleanup_hardlink);

	bool ret = SafeSubocrMatchLine(
		fn_init, fn_deinit, fn_start_pipeline, fn_query_progress,
		fn_query_new_subtitles, fn_stop_all, fn_free_anchors,
		fn_free_subtitles, fn_free_cstrings, fn_free_char,
		fn_malloc_anchors, m_safe_ascii_res_path.c_str(),
		safe_video.c_str(), ref_time_ms,
		x0, y0, x1, y1, out_start_ms, out_end_ms);

	if (!cleanup_hardlink.empty()) {
		DeleteFileW(cleanup_hardlink.c_str());
	}
	return ret;
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
