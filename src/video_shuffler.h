// Copyright (c) 2026, Aegisub Project
// All rights reserved.
//
// Native Video Shuffler & Cutter for Aegisub.
// Cuts, splits, re-orders, and merges video/audio clips driven by ASS subtitles.
// Compatible with Video Shuffler (洋片箱) chapter syntax (# chapter / ## subchapter),
// silent gap removal, lead-in/lead-out padding, and frame-accurate re-timed ASS generation.

#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "async_video_provider.h"
#include "format.h"
#include "compat.h"
#include <libaegisub/path.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace VideoShuffler {

struct ShufflerLogger {
	std::ostringstream ss;
	void Log(const std::string& line) {
		auto now = std::chrono::system_clock::now();
		auto t = std::chrono::system_clock::to_time_t(now);
		struct tm tm_buf;
		localtime_s(&tm_buf, &t);
		char t_str[32];
		strftime(t_str, sizeof(t_str), "[%H:%M:%S] ", &tm_buf);
		ss << t_str << line << "\r\n";
	}
	std::string Str() const { return ss.str(); }
};

struct SpeechInterval {
	int start_ms = 0;          // Padded start time in source video
	int end_ms = 0;            // Padded end time in source video
	int orig_start_ms = 0;     // Earliest unpadded subtitle start
	int orig_end_ms = 0;       // Latest unpadded subtitle end
	std::vector<AssDialogue*> lines;
};

struct ChapterInfo {
	int main_idx = 1;
	int sub_idx = 1;
	std::string name;          // e.g. "1.1 开场白"
	std::string raw_title;     // e.g. "开场白"
	std::vector<SpeechInterval> intervals;
	std::vector<AssDialogue*> all_lines;
	int total_duration_ms = 0;
};

enum ShufflerMode {
	MODE_MERGE_HIGHLIGHT = 0,   // 切分并拼接为完整精简视频（去除无声与废话）
	MODE_SPLIT_CHAPTERS = 1,    // 按章节注释批量切片 (# 章节名 / ## 小节名)
	MODE_SELECTED_LINES = 2     // 仅切分当前选中的字幕行
};

enum MediaFormat {
	FMT_MP4 = 0,
	FMT_MKV = 1,
	FMT_MP3 = 2,
	FMT_WAV = 3,
	FMT_AAC = 4
};

struct ShufflerConfig {
	ShufflerMode mode = MODE_MERGE_HIGHLIGHT;
	MediaFormat format = FMT_MP4;
	bool fast_stream_copy = true;     // true: -c copy; false: libx264+aac re-encode
	int gap_threshold_ms = 1000;      // 两句之间超过此间隔则切除静音
	int lead_in_ms = 100;             // 前置缓冲 (ms)
	int lead_out_ms = 150;            // 后置缓冲 (ms)
	bool generate_synced_ass = true;  // 生成与新视频时间轴严密对齐的 ASS 字幕
	bool generate_raw_ass = false;    // 生成原时间轴 ASS 字幕
	bool skip_blank_chapters = true;  // 跳过无名章节
	std::wstring ffmpeg_path;         // 指定的 FFmpeg 可执行文件路径
	std::wstring output_dir;          // 输出目录
	std::wstring output_prefix;       // 文件名前缀
};

inline std::string FormatMsTime(int ms) {
	if (ms < 0) ms = 0;
	int total_s = ms / 1000;
	int rem_ms = ms % 1000;
	int s = total_s % 60;
	int m = (total_s / 60) % 60;
	int h = total_s / 3600;
	char buf[64];
	snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, rem_ms);
	return std::string(buf);
}

inline std::string FormatMsDuration(int ms) {
	if (ms < 0) ms = 0;
	int total_s = ms / 1000;
	int s = total_s % 60;
	int m = (total_s / 60) % 60;
	int h = total_s / 3600;
	char buf[64];
	if (h > 0)
		snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
	else
		snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
	return std::string(buf);
}

// 清理非法文件名字符 (宽字符版本，彻底避免多字节编码损坏)
inline std::wstring SanitizeFileName(std::wstring name) {
	const std::wstring invalid_chars = L"\\/:*?\"<>|";
	for (wchar_t& c : name) {
		if (invalid_chars.find(c) != std::wstring::npos || c == L'\r' || c == L'\n') {
			c = L'_';
		}
	}
	while (!name.empty() && (name.front() == L' ' || name.front() == L'\t')) name.erase(name.begin());
	while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
	return name.empty() ? L"chapter" : name;
}

// 自动搜寻 FFmpeg 可执行程序
inline std::wstring FindFFmpegExecutable(const std::wstring& preferred_path = L"") {
	std::error_code ec;
	if (!preferred_path.empty() && std::filesystem::exists(preferred_path, ec)) {
		return preferred_path;
	}

	// 1. 检查 Aegisub 程序所在安装目录
	wchar_t exe_buf[MAX_PATH];
	if (GetModuleFileNameW(NULL, exe_buf, MAX_PATH) > 0) {
		std::filesystem::path app_dir = std::filesystem::path(exe_buf).parent_path();
		if (std::filesystem::exists(app_dir / L"ffmpeg.exe", ec)) {
			return (app_dir / L"ffmpeg.exe").wstring();
		}
	}

	// 2. 检查当前工作目录
	if (std::filesystem::exists(L"ffmpeg.exe", ec)) {
		return std::filesystem::absolute(L"ffmpeg.exe").wstring();
	}
	if (std::filesystem::exists(L"build\\ffmpeg.exe", ec)) {
		return std::filesystem::absolute(L"build\\ffmpeg.exe").wstring();
	}
	if (std::filesystem::exists(L"build-local\\ffmpeg.exe", ec)) {
		return std::filesystem::absolute(L"build-local\\ffmpeg.exe").wstring();
	}

	// 3. 检查系统 SearchPath (PATH, System32, 当前目录等)
	wchar_t found[MAX_PATH];
	if (SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, found, NULL) > 0) {
		return std::wstring(found);
	}
	if (SearchPathW(NULL, L"ffmpeg", NULL, MAX_PATH, found, NULL) > 0) {
		return std::wstring(found);
	}

	// 4. 检查常见包管理器安装路径 (Winget, Scoop, Chocolatey)
	const wchar_t* env_vars[] = {
		L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\ffmpeg.exe",
		L"%USERPROFILE%\\scoop\\shims\\ffmpeg.exe",
		L"%ProgramData%\\chocolatey\\bin\\ffmpeg.exe"
	};
	for (const auto* p : env_vars) {
		wchar_t expanded[MAX_PATH];
		if (ExpandEnvironmentStringsW(p, expanded, MAX_PATH) > 0) {
			if (std::filesystem::exists(expanded, ec)) {
				return std::wstring(expanded);
			}
		}
	}

	// 5. 检查常见安装目录
	const wchar_t* common_paths[] = {
		L"C:\\ffmpeg\\bin\\ffmpeg.exe",
		L"C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
		L"C:\\Program Files (x86)\\ffmpeg\\bin\\ffmpeg.exe",
		L"D:\\Tool\\ffmpeg\\bin\\ffmpeg.exe",
		L"D:\\Tool\\video-shuffler\\ffmpeg.exe",
		L"D:\\ffmpeg\\bin\\ffmpeg.exe"
	};
	for (const auto* p : common_paths) {
		if (std::filesystem::exists(p, ec)) {
			return std::wstring(p);
		}
	}

	// 6. 遍历常见软件目录寻找带 ffmpeg 的套件
	const wchar_t* base_dirs[] = { L"C:\\Program Files", L"C:\\Program Files (x86)", L"D:\\Tool" };
	for (const auto* base : base_dirs) {
		try {
			if (!std::filesystem::exists(base, ec)) continue;
			for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
				if (entry.is_directory(ec)) {
					std::wstring fn = entry.path().filename().wstring();
					std::wstring lower_fn = fn;
					std::transform(lower_fn.begin(), lower_fn.end(), lower_fn.begin(), ::towlower);
					if (lower_fn.find(L"ffmpeg") != std::wstring::npos) {
						auto sub_bin = entry.path() / L"bin" / L"ffmpeg.exe";
						if (std::filesystem::exists(sub_bin, ec)) return sub_bin.wstring();
						auto root_bin = entry.path() / L"ffmpeg.exe";
						if (std::filesystem::exists(root_bin, ec)) return root_bin.wstring();
						for (const auto& sub_entry : std::filesystem::directory_iterator(entry.path(), ec)) {
							if (sub_entry.is_directory(ec)) {
								auto nested = sub_entry.path() / L"bin" / L"ffmpeg.exe";
								if (std::filesystem::exists(nested, ec)) return nested.wstring();
								auto nested_root = sub_entry.path() / L"ffmpeg.exe";
								if (std::filesystem::exists(nested_root, ec)) return nested_root.wstring();
							}
						}
					}
				}
			}
		} catch (...) {}
	}

	return L"";
}

// 解析字幕中的章节与语音区间
inline std::vector<ChapterInfo> ParseChapters(
	agi::Context* c,
	const ShufflerConfig& opt,
	int video_duration_ms,
	const std::set<AssDialogue*>& selected_lines = {})
{
	std::vector<ChapterInfo> chapters;
	if (!c || !c->ass) return chapters;

	int chapter_main = 1;
	int chapter_sub = 1;

	ChapterInfo cur_chap;
	cur_chap.main_idx = chapter_main;
	cur_chap.sub_idx = chapter_sub;
	cur_chap.name = "1.1 开始";
	cur_chap.raw_title = "开始";

	SpeechInterval cur_interval;
	bool has_pending_interval = false;

	auto commit_interval = [&]() {
		if (has_pending_interval) {
			cur_chap.intervals.push_back(cur_interval);
			cur_chap.total_duration_ms += (cur_interval.end_ms - cur_interval.start_ms);
			has_pending_interval = false;
		}
	};

	auto commit_chapter = [&]() {
		commit_interval();
		if (!cur_chap.intervals.empty() || !cur_chap.all_lines.empty()) {
			chapters.push_back(cur_chap);
		}
		cur_chap.intervals.clear();
		cur_chap.all_lines.clear();
		cur_chap.total_duration_ms = 0;
	};

	for (auto& entry : c->ass->Events) {
		auto diag = &entry;
		if (!diag) continue;

		if (opt.mode == MODE_SELECTED_LINES && !selected_lines.empty()) {
			if (selected_lines.find(diag) == selected_lines.end()) {
				continue;
			}
		}

		std::string text = diag->Text.get();
		// 去除首尾空白
		size_t first = text.find_first_not_of(" \t\r\n");
		std::string trimmed_text = (first == std::string::npos) ? "" : text.substr(first);

		if (diag->Comment) {
			if (trimmed_text.starts_with("##")) {
				// 二级小节
				commit_chapter();
				chapter_sub++;
				std::string title = trimmed_text.substr(2);
				size_t tf = title.find_first_not_of(" \t");
				title = (tf == std::string::npos) ? "" : title.substr(tf);
				cur_chap.main_idx = chapter_main;
				cur_chap.sub_idx = chapter_sub;
				cur_chap.raw_title = title;
				cur_chap.name = std::to_string(chapter_main) + "." + std::to_string(chapter_sub) + (title.empty() ? "" : " " + title);
			} else if (trimmed_text.starts_with("#")) {
				// 一级章节
				commit_chapter();
				chapter_main++;
				chapter_sub = 1;
				std::string title = trimmed_text.substr(1);
				size_t tf = title.find_first_not_of(" \t");
				title = (tf == std::string::npos) ? "" : title.substr(tf);
				cur_chap.main_idx = chapter_main;
				cur_chap.sub_idx = chapter_sub;
				cur_chap.raw_title = title;
				cur_chap.name = std::to_string(chapter_main) + "." + std::to_string(chapter_sub) + (title.empty() ? "" : " " + title);
			} else {
				// 普通注释：作为被裁减掉的内容，强制截断当前语音片段
				commit_interval();
			}
			continue;
		}

		// 有效对话行
		int raw_s = static_cast<int>(diag->Start);
		int raw_e = static_cast<int>(diag->End);
		if (raw_e <= raw_s) continue;

		int pad_s = std::max(0, raw_s - opt.lead_in_ms);
		int pad_e = (video_duration_ms > 0) ? std::min(video_duration_ms, raw_e + opt.lead_out_ms) : (raw_e + opt.lead_out_ms);

		// 若起始点超出视频范围或边界倒挂则忽略
		if (video_duration_ms > 0 && pad_s >= video_duration_ms) continue;
		if (pad_e <= pad_s) continue;

		cur_chap.all_lines.push_back(diag);

		if (!has_pending_interval) {
			cur_interval.start_ms = pad_s;
			cur_interval.end_ms = pad_e;
			cur_interval.orig_start_ms = raw_s;
			cur_interval.orig_end_ms = raw_e;
			cur_interval.lines.clear();
			cur_interval.lines.push_back(diag);
			has_pending_interval = true;
		} else {
			int gap = pad_s - cur_interval.end_ms;
			// 若间隔小于合并阈值且时序递增，则合并为一个连贯片段
			if (gap <= opt.gap_threshold_ms && pad_s >= cur_interval.start_ms) {
				cur_interval.end_ms = std::max(cur_interval.end_ms, pad_e);
				cur_interval.orig_end_ms = std::max(cur_interval.orig_end_ms, raw_e);
				cur_interval.lines.push_back(diag);
			} else {
				// 否则提交上一片段并开启新片段
				commit_interval();
				cur_interval.start_ms = pad_s;
				cur_interval.end_ms = pad_e;
				cur_interval.orig_start_ms = raw_s;
				cur_interval.orig_end_ms = raw_e;
				cur_interval.lines.clear();
				cur_interval.lines.push_back(diag);
				has_pending_interval = true;
			}
		}
	}

	commit_chapter();
	return chapters;
}

// 生成对齐新视频时间轴的 ASS 字幕文件
inline bool WriteRetimedAssFile(
	agi::Context* c,
	const std::vector<SpeechInterval>& intervals,
	const std::filesystem::path& out_path,
	bool raw_time = false)
{
	std::ofstream ofs(out_path, std::ios::binary);
	if (!ofs.is_open()) return false;

	// UTF-8 BOM
	ofs << "\xEF\xBB\xBF";

	ofs << "[Script Info]\n";
	ofs << "; Script generated by Aegisub Video Shuffler\n";
	ofs << "Title: Shuffled & Cut Subtitles\n";
	ofs << "ScriptType: v4.00+\n";
	ofs << "WrapStyle: 0\n";
	ofs << "ScaledBorderAndShadow: yes\n";

	int res_x = 0, res_y = 0;
	c->ass->GetResolution(res_x, res_y);
	if (res_x > 0 && res_y > 0) {
		ofs << "PlayResX: " << res_x << "\n";
		ofs << "PlayResY: " << res_y << "\n";
	}
	ofs << "\n";

	ofs << "[V4+ Styles]\n";
	ofs << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
	for (const auto& style : c->ass->Styles) {
		ofs << style.GetEntryData() << "\n";
	}
	ofs << "\n";

	ofs << "[Events]\n";
	ofs << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

	int cumulative_dur_ms = 0;
	for (const auto& interval : intervals) {
		int clip_dur = interval.end_ms - interval.start_ms;
		for (const auto* diag : interval.lines) {
			std::string start_str, end_str;
			if (raw_time) {
				start_str = diag->Start.GetAssFormatted();
				end_str = diag->End.GetAssFormatted();
			} else {
				int new_s = cumulative_dur_ms + (static_cast<int>(diag->Start) - interval.start_ms);
				int new_e = cumulative_dur_ms + (static_cast<int>(diag->End) - interval.start_ms);
				new_s = std::max(0, new_s);
				new_e = std::max(new_s + 10, new_e);
				start_str = agi::Time(new_s).GetAssFormatted();
				end_str = agi::Time(new_e).GetAssFormatted();
			}

			ofs << (diag->Comment ? "Comment: " : "Dialogue: ")
			    << diag->Layer << ","
			    << start_str << ","
			    << end_str << ","
			    << diag->Style.get() << ","
			    << diag->Actor.get() << ","
			    << diag->Margin[0] << ","
			    << diag->Margin[1] << ","
			    << diag->Margin[2] << ","
			    << diag->Effect.get() << ","
			    << diag->Text.get() << "\n";
		}
		cumulative_dur_ms += clip_dur;
	}
	return true;
}

// FFmpeg 进程调度执行器
struct FFmpegRunner {
	static bool Execute(
		const std::wstring& cmd_line,
		std::atomic<bool>* cancel_flag,
		const wchar_t* working_dir = NULL,
		std::string* out_err_log = nullptr)
	{
		SECURITY_ATTRIBUTES sa;
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;
		sa.lpSecurityDescriptor = NULL;

		HANDLE hReadPipe = NULL, hWritePipe = NULL;
		if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
			if (out_err_log) *out_err_log = "CreatePipe 创建匿名管道失败。";
			return false;
		}
		SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		si.hStdError = hWritePipe;
		si.hStdOutput = hWritePipe;
		si.dwFlags |= STARTF_USESTDHANDLES;

		PROCESS_INFORMATION pi;
		ZeroMemory(&pi, sizeof(pi));

		std::vector<wchar_t> cmd_buf(cmd_line.begin(), cmd_line.end());
		cmd_buf.push_back(L'\0');

		BOOL ok = CreateProcessW(
			NULL,
			cmd_buf.data(),
			NULL,
			NULL,
			TRUE,
			CREATE_NO_WINDOW,
			NULL,
			working_dir,
			&si,
			&pi);

		CloseHandle(hWritePipe);

		if (!ok) {
			DWORD err = GetLastError();
			CloseHandle(hReadPipe);
			if (out_err_log) {
				*out_err_log = "CreateProcessW 启动 FFmpeg 失败，错误代码: " + std::to_string(err);
				if (err == ERROR_FILE_NOT_FOUND) {
					*out_err_log += " (系统未找到指定的可执行文件)";
				} else if (err == ERROR_ACCESS_DENIED) {
					*out_err_log += " (访问被拒绝或无执行权限)";
				}
			}
			return false;
		}

		std::string accumulated_output;
		char buffer[2048];
		DWORD bytesRead = 0;

		while (true) {
			if (cancel_flag && cancel_flag->load()) {
				TerminateProcess(pi.hProcess, 1);
				break;
			}

			DWORD bytesAvail = 0;
			if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytesAvail, NULL)) {
				if (bytesAvail > 0) {
					if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
						accumulated_output.append(buffer, bytesRead);
						if (accumulated_output.size() > 16384) {
							accumulated_output.erase(0, accumulated_output.size() - 8192);
						}
					}
				} else {
					DWORD exitCode = 0;
					if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
						// 进程已退出，再次读取管道中的残留数据
						while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
							if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
								accumulated_output.append(buffer, bytesRead);
							} else break;
						}
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(15));
				}
			} else {
				break;
			}
		}

		CloseHandle(hReadPipe);

		DWORD exitCode = 0;
		WaitForSingleObject(pi.hProcess, 5000);
		GetExitCodeProcess(pi.hProcess, &exitCode);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		if (exitCode != 0 && out_err_log) {
			*out_err_log = accumulated_output;
			if (out_err_log->empty()) {
				*out_err_log = "进程非正常退出，退出代码: " + std::to_string(exitCode);
			}
		}
		return (exitCode == 0);
	}
};

struct ShufflerResult {
	bool success = false;
	wxString message;
	std::string detailed_log;
	std::vector<std::wstring> output_media_files;
	std::vector<std::wstring> output_ass_files;
	std::wstring primary_media_file;
	std::wstring primary_ass_file;
	int total_clips_cut = 0;
	int total_output_dur_ms = 0;
};

// 核心处理函数：按字幕切分/合并视频
inline ShufflerResult RunVideoShuffler(
	agi::Context* c,
	const ShufflerConfig& opt,
	const std::set<AssDialogue*>& selected_lines,
	std::function<void(int percent, const wxString& status)> on_progress = nullptr,
	std::shared_ptr<std::atomic<bool>> cancel_flag = nullptr)
{
	ShufflerResult res;
	ShufflerLogger logger;

	logger.Log("==================================================");
	logger.Log("Aegisub 视频切片与合并处理任务启动");
	logger.Log("==================================================");

	if (!c || !c->project) {
		res.message = to_wx("错误：无效的项目上下文。");
		logger.Log("❌ " + res.message.ToStdString());
		res.detailed_log = logger.Str();
		return res;
	}

	// 1. 定位 FFmpeg 可执行文件
	std::wstring ffmpeg_exe = FindFFmpegExecutable(opt.ffmpeg_path);
	if (ffmpeg_exe.empty()) {
		res.message = to_wx("错误：未在系统环境变量 PATH 或常见目录中检测到 FFmpeg (ffmpeg.exe)！\n\n请安装 FFmpeg 并添加至系统 PATH 环境变量，或在弹窗界面中点击【选择 FFmpeg...】进行指定。");
		logger.Log("❌ 未找到 FFmpeg 可执行文件。");
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("FFmpeg 引擎路径: " + agi::wxformat(wxS("%s"), ffmpeg_exe).ToStdString());

	// 2. 解析视频路径并严格确保为绝对路径
	agi::fs::path video_fs_path = c->path->MakeAbsolute(c->project->VideoName(), "?script");
	if (video_fs_path.empty() && c->ass) {
		video_fs_path = c->path->MakeAbsolute(c->ass->Properties.video_file, "?script");
	}
	if (video_fs_path.empty()) {
		res.message = to_wx("错误：当前工程尚未载入视频文件！");
		logger.Log("❌ 当前工程尚未载入视频文件！");
		res.detailed_log = logger.Str();
		return res;
	}

	std::error_code ec;
	if (!std::filesystem::exists(video_fs_path, ec)) {
		res.message = fmt_wx("错误：无法在以下路径找到视频源文件：\n%s\n请确认视频文件是否存在！", video_fs_path.wstring());
		logger.Log("❌ 视频源文件不存在: " + video_fs_path.string());
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("视频源路径: " + video_fs_path.string());

	auto provider = c->project->VideoProvider();
	int video_dur_ms = provider ? static_cast<int>(provider->GetFrameCount() * 1000.0 / provider->GetFPS().FPS()) : 0;
	logger.Log("视频规格: 时长 " + FormatMsDuration(video_dur_ms) + " (" + std::to_string(video_dur_ms) + " ms)");

	// 3. 解析章节与片段
	auto chapters = ParseChapters(c, opt, video_dur_ms, selected_lines);
	if (chapters.empty()) {
		res.message = to_wx("未找到有效的字幕行进行切分。");
		logger.Log("❌ 未找到有效的字幕行进行切分。");
		res.detailed_log = logger.Str();
		return res;
	}

	// 确定媒体文件后缀
	std::wstring ext = L".mp4";
	bool is_audio = false;
	switch (opt.format) {
		case FMT_MP4: ext = L".mp4"; break;
		case FMT_MKV: ext = L".mkv"; break;
		case FMT_MP3: ext = L".mp3"; is_audio = true; break;
		case FMT_WAV: ext = L".wav"; is_audio = true; break;
		case FMT_AAC: ext = L".aac"; is_audio = true; break;
	}

	std::filesystem::path out_dir_path;
	if (!opt.output_dir.empty()) {
		out_dir_path = std::filesystem::path(opt.output_dir);
	} else {
		out_dir_path = video_fs_path.parent_path();
	}
	std::filesystem::create_directories(out_dir_path, ec);
	if (ec) {
		res.message = fmt_wx("错误：无法创建输出目录：\n%s\n系统错误: %s", out_dir_path.wstring(), to_wx(ec.message()));
		logger.Log("❌ 无法创建输出目录: " + out_dir_path.string());
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("输出目录: " + out_dir_path.string());

	std::wstring prefix = opt.output_prefix.empty() ? video_fs_path.stem().wstring() : opt.output_prefix;

	// 临时工作目录
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::filesystem::path temp_dir = out_dir_path / (L".vs_temp_" + std::to_wstring(now_ms));
	std::filesystem::create_directories(temp_dir, ec);
	if (ec) {
		res.message = fmt_wx("错误：无法创建临时工作目录：\n%s\n系统错误: %s", temp_dir.wstring(), to_wx(ec.message()));
		logger.Log("❌ 无法创建临时工作目录: " + temp_dir.string());
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("临时工作目录: " + temp_dir.string());

	// 计算总片段数供进度条展示
	int total_intervals = 0;
	if (opt.mode == MODE_SPLIT_CHAPTERS) {
		for (const auto& ch : chapters) {
			if (opt.skip_blank_chapters && ch.raw_title.empty()) continue;
			total_intervals += static_cast<int>(ch.intervals.size());
		}
	} else {
		for (const auto& ch : chapters) {
			total_intervals += static_cast<int>(ch.intervals.size());
		}
	}

	logger.Log("模式: " + std::string(opt.mode == MODE_SPLIT_CHAPTERS ? "按章节批量切片" : (opt.mode == MODE_SELECTED_LINES ? "切分选中行" : "智能精简合并")));
	logger.Log("有效切片总数: " + std::to_string(total_intervals));

	if (total_intervals == 0) {
		res.message = to_wx("所有字幕区间均为空，无需切分。");
		logger.Log("❌ 所有字幕区间均为空，无需切分。");
		std::filesystem::remove_all(temp_dir, ec);
		res.detailed_log = logger.Str();
		return res;
	}

	int processed_intervals = 0;

	auto report = [&](int step_pct, const wxString& msg) {
		if (on_progress) on_progress(step_pct, msg);
	};

	report(2, to_wx("正在准备切分..."));

	std::string last_cut_err;
	std::string last_concat_err;

	// 辅助切分单个区间
	auto cut_single_interval = [&](int idx, const SpeechInterval& inv, const std::filesystem::path& temp_out) -> bool {
		std::string ss_str = FormatMsTime(inv.start_ms);
		std::string dur_str = std::to_string((inv.end_ms - inv.start_ms) / 1000.0);

		logger.Log("切片 #" + std::to_string(idx + 1) + ": " + ss_str + " -> " + FormatMsTime(inv.end_ms) + " (" + dur_str + "s)");

		// 将 -ss 置于 -i 之前实现高速关键帧寻道，-t 置于 -i 之后指定精确截取时长
		std::wostringstream cmd;
		cmd << L"\"" << ffmpeg_exe << L"\" -hide_banner -loglevel warning -y -ss "
		    << agi::wxformat(wxS("%s"), ss_str).wc_str()
		    << L" -i \"" << video_fs_path.wstring() << L"\" -t "
		    << agi::wxformat(wxS("%s"), dur_str).wc_str();

		if (is_audio) {
			if (opt.format == FMT_WAV) {
				cmd << L" -vn -c:a pcm_s16le";
			} else if (opt.format == FMT_MP3) {
				cmd << L" -vn -c:a libmp3lame -q:a 2";
			} else {
				cmd << L" -vn -c:a aac -b:a 192k";
			}
		} else {
			if (opt.fast_stream_copy) {
				cmd << L" -c copy -avoid_negative_ts make_zero";
			} else {
				cmd << L" -c:v libx264 -preset veryfast -crf 18 -c:a aac -b:a 192k";
			}
		}

		cmd << L" \"" << temp_out.wstring() << L"\"";

		std::string err_log;
		bool ok = FFmpegRunner::Execute(cmd.str(), cancel_flag.get(), temp_dir.wstring().c_str(), &err_log);
		if (!ok) {
			logger.Log("  ❌ FFmpeg 返回执行失败！");
			if (!err_log.empty()) logger.Log("  输出: " + err_log);
			if (last_cut_err.empty()) last_cut_err = err_log;
		}

		if (ok) {
			std::error_code f_ec;
			auto sz = std::filesystem::file_size(temp_out, f_ec);
			if (f_ec || sz == 0) {
				logger.Log("  ❌ 生成的文件大小为 0 字节");
				if (last_cut_err.empty()) last_cut_err = "FFmpeg 生成的文件大小为 0。";
				return false;
			} else {
				logger.Log("  ✅ 生成成功 (" + std::to_string(sz) + " 字节)");
			}
		}
		return ok;
	};

	// 辅助合并多个片段
	auto concat_clips = [&](const std::vector<std::filesystem::path>& clip_files, const std::filesystem::path& final_out) -> bool {
		if (clip_files.empty()) return false;
		if (clip_files.size() == 1) {
			// 单个片段无需 concat，直接复制
			logger.Log("仅有 1 个切片，直接重命名复制到最终媒体...");
			std::filesystem::copy_file(clip_files[0], final_out, std::filesystem::copy_options::overwrite_existing, ec);
			return !ec;
		}

		logger.Log("开始合并 " + std::to_string(clip_files.size()) + " 个片段到: " + final_out.string());
		std::filesystem::path list_path = temp_dir / L"concat_list.txt";
		{
			std::ofstream list_fs(list_path, std::ios::binary);
			if (!list_fs.is_open()) {
				last_concat_err = "无法创建合并清单文件: concat_list.txt";
				logger.Log("  ❌ " + last_concat_err);
				return false;
			}
			for (const auto& cp : clip_files) {
				// 相对路径写入纯文件名 clip_x.mp4，由 working_dir 指定 temp_dir
				std::string fn = cp.filename().string();
				list_fs << "file '" << fn << "'\n";
			}
		}

		std::wostringstream cmd;
		cmd << L"\"" << ffmpeg_exe << L"\" -hide_banner -loglevel warning -y -f concat -safe 0 -i \""
		    << list_path.wstring() << L"\" -c copy \""
		    << final_out.wstring() << L"\"";

		std::string err_log;
		bool ok = FFmpegRunner::Execute(cmd.str(), cancel_flag.get(), temp_dir.wstring().c_str(), &err_log);
		if (!ok) {
			logger.Log("  ❌ 合并操作 FFmpeg 返回失败！");
			if (!err_log.empty()) logger.Log("  输出: " + err_log);
			last_concat_err = err_log;
		}
		std::filesystem::remove(list_path, ec);

		if (ok) {
			std::error_code f_ec;
			auto sz = std::filesystem::file_size(final_out, f_ec);
			if (f_ec || sz == 0) {
				logger.Log("  ❌ 合并操作完成但未生成有效的最终媒体文件。");
				if (last_concat_err.empty()) last_concat_err = "合并操作完成但未生成有效的最终媒体文件。";
				return false;
			} else {
				logger.Log("  ✅ 合并操作圆满完成，最终文件大小: " + std::to_string(sz) + " 字节");
			}
		}
		return ok;
	};

	try {
		if (opt.mode == MODE_SPLIT_CHAPTERS) {
			// -------------------------------------------------------------
			// 模式 1: 按章节分别切分并导出多个分段文件
			// -------------------------------------------------------------
			int chap_count = 0;
			std::string content_txt = "章节起始时间\t章节名称\t片段数\t总时长\n";

			for (size_t ci = 0; ci < chapters.size(); ++ci) {
				if (cancel_flag && cancel_flag->load()) break;
				const auto& ch = chapters[ci];
				if (opt.skip_blank_chapters && ch.raw_title.empty()) continue;
				if (ch.intervals.empty()) continue;

				chap_count++;
				std::wstring clean_ch_name = SanitizeFileName(to_wx(ch.name).ToStdWstring());
				std::filesystem::path out_media = out_dir_path / (prefix + L"_" + clean_ch_name + ext);
				std::filesystem::path out_ass = out_dir_path / (prefix + L"_" + clean_ch_name + L".ass");

				report(static_cast<int>(10 + 80.0 * processed_intervals / total_intervals),
				       fmt_wx("正在切分章节 [%d/%d]: %s...", chap_count, static_cast<int>(chapters.size()), to_wx(ch.name)));

				std::vector<std::filesystem::path> temp_clips;
				for (size_t k = 0; k < ch.intervals.size(); ++k) {
					if (cancel_flag && cancel_flag->load()) break;
					std::filesystem::path temp_clip = temp_dir / (L"clip_" + std::to_wstring(ci) + L"_" + std::to_wstring(k) + ext);
					if (cut_single_interval(static_cast<int>(k), ch.intervals[k], temp_clip)) {
						temp_clips.push_back(temp_clip);
					}
					processed_intervals++;
				}

				if (cancel_flag && cancel_flag->load()) break;

				if (!temp_clips.empty()) {
					if (concat_clips(temp_clips, out_media)) {
						res.output_media_files.push_back(out_media.wstring());
						if (res.primary_media_file.empty()) res.primary_media_file = out_media.wstring();
					}

					// 生成新时间轴字幕
					if (opt.generate_synced_ass) {
						if (WriteRetimedAssFile(c, ch.intervals, out_ass, false)) {
							res.output_ass_files.push_back(out_ass.wstring());
							if (res.primary_ass_file.empty()) res.primary_ass_file = out_ass.wstring();
						}
					}

					int ch_dur = ch.total_duration_ms;
					content_txt += FormatMsTime(ch.intervals.front().start_ms) + "\t" + ch.name + "\t" +
					               std::to_string(ch.intervals.size()) + "\t" + FormatMsDuration(ch_dur) + "\n";
				}
			}

			// 输出目录索引 content.txt
			std::filesystem::path content_path = out_dir_path / (prefix + L"_chapters_content.txt");
			std::ofstream ofs(content_path, std::ios::binary);
			if (ofs.is_open()) ofs << content_txt;

		} else {
			// -------------------------------------------------------------
			// 模式 2 & 3: 汇集所有选中的片段，切分并合并为单个精华视频
			// -------------------------------------------------------------
			std::vector<SpeechInterval> all_intervals;
			for (const auto& ch : chapters) {
				all_intervals.insert(all_intervals.end(), ch.intervals.begin(), ch.intervals.end());
			}

			std::filesystem::path out_media = out_dir_path / (prefix + (opt.mode == MODE_SELECTED_LINES ? L"_selection" : L"_cut") + ext);
			std::filesystem::path out_ass = out_dir_path / (prefix + (opt.mode == MODE_SELECTED_LINES ? L"_selection" : L"_cut") + L".ass");

			std::vector<std::filesystem::path> temp_clips;
			for (size_t k = 0; k < all_intervals.size(); ++k) {
				if (cancel_flag && cancel_flag->load()) break;
				report(static_cast<int>(10 + 80.0 * k / all_intervals.size()),
				       fmt_wx("正在切分片段 [%d/%d] (长度: %s)...", static_cast<int>(k + 1), static_cast<int>(all_intervals.size()),
				              to_wx(FormatMsDuration(all_intervals[k].end_ms - all_intervals[k].start_ms))));

				std::filesystem::path temp_clip = temp_dir / (L"clip_" + std::to_wstring(k) + ext);
				if (cut_single_interval(static_cast<int>(k), all_intervals[k], temp_clip)) {
					temp_clips.push_back(temp_clip);
				}
				processed_intervals++;
			}

			if (cancel_flag && !cancel_flag->load() && !temp_clips.empty()) {
				report(92, to_wx("正在合并最终媒体文件..."));
				if (concat_clips(temp_clips, out_media)) {
					res.output_media_files.push_back(out_media.wstring());
					res.primary_media_file = out_media.wstring();
				}

				if (opt.generate_synced_ass) {
					report(97, to_wx("正在生成严格对齐的新 ASS 字幕..."));
					if (WriteRetimedAssFile(c, all_intervals, out_ass, false)) {
						res.output_ass_files.push_back(out_ass.wstring());
						res.primary_ass_file = out_ass.wstring();
					}
				}
			}
		}

	} catch (const std::exception& e) {
		res.message = to_wx("处理异常: " + std::string(e.what()));
		logger.Log("❌ 发生未处理异常: " + std::string(e.what()));
	}

	// 清理临时文件
	std::filesystem::remove_all(temp_dir, ec);

	if (cancel_flag && cancel_flag->load()) {
		res.success = false;
		res.message = to_wx("用户已取消切分合并操作。");
		logger.Log("用户取消操作。");
		res.detailed_log = logger.Str();
		report(0, to_wx("已取消"));
		return res;
	}

	if (!res.output_media_files.empty()) {
		res.success = true;
		res.total_clips_cut = processed_intervals;
		res.message = fmt_wx("成功生成 %d 个媒体文件与配套字幕！", static_cast<int>(res.output_media_files.size()));
		logger.Log("✅ 处理完成！成功输出 " + std::to_string(res.output_media_files.size()) + " 个文件。");
		report(100, to_wx("处理完成！"));
	} else {
		res.success = false;
		if (res.message.empty()) {
			if (!last_concat_err.empty()) {
				res.message = to_wx("切片截取成功，但在合并最终视频时失败。\n详细错误：" + last_concat_err);
			} else if (!last_cut_err.empty()) {
				res.message = to_wx("切分失败：FFmpeg 无法截取片段。\n详细错误：" + last_cut_err);
			} else {
				res.message = to_wx("切分失败：未成功导出任何片段。请检查视频源文件与字幕时间轴。");
			}
		}
		logger.Log("❌ 切分失败结束: " + res.message.ToStdString());
	}

	res.detailed_log = logger.Str();
	return res;
}

} // namespace VideoShuffler
