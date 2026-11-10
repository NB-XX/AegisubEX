#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

// ========================================================================
// SubtitleOCR (望言OCR) C-ABI 数据结构定义 (参考 interface.h)
// ========================================================================

#pragma pack(push, 8)

template<typename T>
struct SubocrArray {
	size_t size;
	T* data;
};

struct SubocrBoundingBox {
	int center_x, center_y, width, height;
};

enum SubocrLanguage : uint32_t {
	SUBOCR_LANG_ZH = 0,
	SUBOCR_LANG_EN = 1,
	SUBOCR_LANG_JA = 2,
	SUBOCR_LANG_KO = 3
};

struct SubocrSubtitleAnchor {
	int center_x, center_y, height;
	SubocrLanguage lang;
	int is_primary;
	int avg_width, min_width, mid_width, max_width;
};

struct SubocrCString {
	size_t size;
	char* data;
};

struct SubocrSubtitle {
	int64_t start_us;
	int64_t end_us;
	SubocrArray<SubocrCString> cstringArray;
};

struct SubocrProgress {
	int is_finished;
	int64_t start_us;
	int64_t current_us;
	int64_t duration_us;
	double speed_up;
};

struct SubocrCVImage {
	void* data;
	int width;
	int height;
	int cv_type;
	int bytes_per_row;
};

#pragma pack(pop)

// ========================================================================
// SubocrEngine: Aegisub 与 望言OCR (SubtitleOCR) 的无缝桥接调度单例
// ========================================================================
class SubocrEngine {
public:
	static SubocrEngine& Get();

	bool IsAvailable() const { return m_available; }
	std::wstring GetLoadedPath() const { return m_loaded_path; }

	// 动态检测与载入 subocr 动态库
	bool Load(const std::wstring& custom_dir = L"");
	bool Load(const std::string& custom_dir_u8);
	void Unload();

	// 针对单条台词在选定区域匹配起止帧（单位：毫秒）
	bool MatchLine(const std::string& video_path, int ref_time_ms,
	               int x0, int y0, int x1, int y1,
	               int& out_start_ms, int& out_end_ms);

	// 批量提取 Anchor 区域的所有对白区间（单位：毫秒）
	bool ExtractIntervals(const std::string& video_path,
	                      int x0, int y0, int x1, int y1,
	                      std::vector<std::pair<int, int>>& out_intervals,
	                      const std::function<bool(double progress, double speed)>& progress_cb = nullptr);

	// C-ABI 函数指针
	typedef void* (*Fn_subocr_init)(const char* pathResources, int device);
	typedef void (*Fn_subocr_deinit)(void* ctx);
	typedef SubocrArray<SubocrBoundingBox> (*Fn_subocr_detect)(void* ctx, SubocrCVImage input);
	typedef SubocrArray<SubocrSubtitleAnchor> (*Fn_subocr_query_anchors)(void* ctx);
	typedef int (*Fn_subocr_start_predet)(void* ctx, const char* videoPath, int64_t maxSec, int defaultLang);
	typedef int (*Fn_subocr_start_pipeline)(void* ctx, const char* videoPath, int fps, SubocrArray<SubocrSubtitleAnchor> anchors, int64_t minSubtitleUs);
	typedef SubocrProgress (*Fn_subocr_query_progress)(void* ctx);
	typedef SubocrArray<SubocrSubtitle> (*Fn_subocr_query_new_subtitles)(void* ctx);
	typedef void (*Fn_subocr_stop_all)(void* ctx);

	// 内存释放辅助指针
	typedef void (*Fn_free_array)(void* array);
	typedef SubocrArray<SubocrSubtitleAnchor> (*Fn_anchor_malloc)(size_t size);

private:
	SubocrEngine();
	~SubocrEngine();

	bool m_available = false;
	std::wstring m_loaded_path;

#ifdef _WIN32
	HMODULE m_hLib = nullptr;
#else
	void* m_hLib = nullptr;
#endif

	Fn_subocr_init fn_init = nullptr;
	Fn_subocr_deinit fn_deinit = nullptr;
	Fn_subocr_detect fn_detect = nullptr;
	Fn_subocr_query_anchors fn_query_anchors = nullptr;
	Fn_subocr_start_predet fn_start_predet = nullptr;
	Fn_subocr_start_pipeline fn_start_pipeline = nullptr;
	Fn_subocr_query_progress fn_query_progress = nullptr;
	Fn_subocr_query_new_subtitles fn_query_new_subtitles = nullptr;
	Fn_subocr_stop_all fn_stop_all = nullptr;

	Fn_free_array fn_free_anchors = nullptr;
	Fn_free_array fn_free_subtitles = nullptr;
	Fn_free_array fn_free_bboxes = nullptr;
	Fn_free_array fn_free_cstrings = nullptr;
	Fn_free_array fn_free_char = nullptr;
	Fn_anchor_malloc fn_malloc_anchors = nullptr;
};
