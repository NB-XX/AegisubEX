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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace VideoShuffler {

inline std::string PathToUtf8(const std::filesystem::path& p) {
	const std::wstring& w = p.wstring();
	if (w.empty()) return "";
	int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0) return "";
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
	return out;
}

inline std::string FormatMsSeconds(int ms) {
	if (ms <= 0) return "0.000";
	int s = ms / 1000;
	int rem = ms % 1000;
	char buf[32];
	snprintf(buf, sizeof(buf), "%d.%03d", s, rem);
	return std::string(buf);
}

struct ShufflerLogger {
	mutable std::mutex mtx;
	std::ostringstream ss;
	void Log(const std::string& line) {
		auto now = std::chrono::system_clock::now();
		auto t = std::chrono::system_clock::to_time_t(now);
		struct tm tm_buf;
		localtime_s(&tm_buf, &t);
		char t_str[32];
		strftime(t_str, sizeof(t_str), "[%H:%M:%S] ", &tm_buf);
		std::lock_guard<std::mutex> lk(mtx);
		ss << t_str << line << "\r\n";
	}
	std::string Str() const {
		std::lock_guard<std::mutex> lk(mtx);
		return ss.str();
	}
};

struct SpeechInterval {
	int start_ms = 0;          // Padded start time in source video
	int end_ms = 0;            // Padded end time in source video
	int orig_start_ms = 0;     // Earliest unpadded subtitle start
	int orig_end_ms = 0;       // Latest unpadded subtitle end
	std::vector<AssDialogue*> lines;
};

enum ShufflerMode {
	MODE_MERGE_HIGHLIGHT = 0,   // 切分并拼接为完整精简视频（去除无声与废话）
	MODE_SELECTED_LINES = 1     // 仅切分当前选中的字幕行
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

// 解析字幕并按时间排序合并重叠或相邻的语音区间
inline std::vector<SpeechInterval> ParseSpeechIntervals(
	agi::Context* c,
	const ShufflerConfig& opt,
	int video_duration_ms,
	const std::set<AssDialogue*>& selected_lines = {})
{
	std::vector<SpeechInterval> result;
	if (!c || !c->ass) return result;
	if (opt.mode == MODE_SELECTED_LINES && selected_lines.empty()) return result;

	struct RawInterval {
		int pad_s;
		int pad_e;
		int raw_s;
		int raw_e;
		AssDialogue* diag;
	};
	std::vector<RawInterval> items;

	for (auto& entry : c->ass->Events) {
		auto diag = &entry;
		if (!diag || diag->Comment) continue;

		if (opt.mode == MODE_SELECTED_LINES) {
			if (selected_lines.find(diag) == selected_lines.end()) continue;
		}

		int raw_s = static_cast<int>(diag->Start);
		int raw_e = static_cast<int>(diag->End);
		if (raw_e <= raw_s) continue;

		int pad_s = std::max(0, raw_s - opt.lead_in_ms);
		int pad_e = (video_duration_ms > 0) ? std::min(video_duration_ms, raw_e + opt.lead_out_ms) : (raw_e + opt.lead_out_ms);

		if (video_duration_ms > 0 && pad_s >= video_duration_ms) continue;
		if (pad_e <= pad_s) continue;

		items.push_back({ pad_s, pad_e, raw_s, raw_e, diag });
	}

	if (items.empty()) return result;

	// 按起始时间（相同时按结束时间）稳定排序，确保重叠或乱序字幕合并为同一段而不重复剪切
	std::stable_sort(items.begin(), items.end(), [](const RawInterval& a, const RawInterval& b) {
		if (a.pad_s != b.pad_s) return a.pad_s < b.pad_s;
		return a.pad_e < b.pad_e;
	});

	for (const auto& item : items) {
		if (result.empty()) {
			SpeechInterval inv;
			inv.start_ms = item.pad_s;
			inv.end_ms = item.pad_e;
			inv.orig_start_ms = item.raw_s;
			inv.orig_end_ms = item.raw_e;
			inv.lines.push_back(item.diag);
			result.push_back(std::move(inv));
		} else {
			auto& cur = result.back();
			if (item.pad_s <= cur.end_ms + opt.gap_threshold_ms) {
				cur.end_ms = std::max(cur.end_ms, item.pad_e);
				cur.orig_start_ms = std::min(cur.orig_start_ms, item.raw_s);
				cur.orig_end_ms = std::max(cur.orig_end_ms, item.raw_e);
				cur.lines.push_back(item.diag);
			} else {
				SpeechInterval inv;
				inv.start_ms = item.pad_s;
				inv.end_ms = item.pad_e;
				inv.orig_start_ms = item.raw_s;
				inv.orig_end_ms = item.raw_e;
				inv.lines.push_back(item.diag);
				result.push_back(std::move(inv));
			}
		}
	}

	return result;
}

// 生成对齐新视频时间轴的 ASS 字幕文件
inline bool WriteRetimedAssFile(
	agi::Context* c,
	const std::vector<SpeechInterval>& intervals,
	const std::filesystem::path& out_path,
	bool raw_time = false,
	const std::wstring& linked_media_filename = L"",
	bool is_audio = false)
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

	if (!linked_media_filename.empty()) {
		ofs << "[Aegisub Project Garbage]\n";
		std::string media_utf8 = PathToUtf8(std::filesystem::path(linked_media_filename));
		ofs << (is_audio ? "Audio File: " : "Video File: ") << media_utf8 << "\n\n";
	}

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
		std::string* out_err_log = nullptr,
		bool capture_on_success = false)
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
						if (!capture_on_success && accumulated_output.size() > 16384) {
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

		if (out_err_log) {
			if (exitCode != 0) {
				*out_err_log = accumulated_output;
				if (out_err_log->empty()) {
					*out_err_log = "进程非正常退出，退出代码: " + std::to_string(exitCode);
				}
			} else if (capture_on_success) {
				*out_err_log = std::move(accumulated_output);
			}
		}
		return (exitCode == 0);
	}
};

inline std::wstring FindFFprobeExecutable(const std::wstring& ffmpeg_exe) {
	std::error_code ec;
	if (!ffmpeg_exe.empty()) {
		std::filesystem::path probe = std::filesystem::path(ffmpeg_exe).parent_path() / L"ffprobe.exe";
		if (std::filesystem::exists(probe, ec)) {
			return probe.wstring();
		}
	}
	wchar_t found[MAX_PATH];
	if (SearchPathW(NULL, L"ffprobe.exe", NULL, MAX_PATH, found, NULL) > 0) {
		return std::wstring(found);
	}
	return L"";
}

struct KeyframePoint {
	int ms = 0;
	double sec = 0.0;
	std::string pts_str;
};

struct SourceMediaProbe {
	bool valid = false;
	std::string v_codec;
	std::string v_profile;
	std::string v_pix_fmt;
	std::string v_fps;
	std::string v_timescale;
	std::string a_codec;
	std::string a_sample_rate;
	std::string a_channels;
	std::vector<KeyframePoint> keyframes;
};

inline SourceMediaProbe ProbeSourceMedia(
	const std::wstring& ffprobe_exe,
	const std::filesystem::path& video_path,
	std::atomic<bool>* cancel_flag,
	ShufflerLogger& logger)
{
	SourceMediaProbe info;
	if (ffprobe_exe.empty()) {
		logger.Log("未找到 ffprobe.exe，无损快剪将回退为普通关键帧直拷。");
		return info;
	}

	// 1. 探测音视频流编码参数（用于局部重编码与原流无缝拼接）
	std::wostringstream stream_cmd;
	stream_cmd << L"\"" << ffprobe_exe << L"\" -v error -show_entries "
	           << L"stream=codec_type,codec_name,profile,pix_fmt,r_frame_rate,time_base,sample_rate,channels "
	           << L"-of default=noprint_wrappers=1 \"" << video_path.wstring() << L"\"";

	std::string stream_out;
	if (!FFmpegRunner::Execute(stream_cmd.str(), cancel_flag, NULL, &stream_out, true)) {
		logger.Log("ffprobe 流参数探测失败，将使用普通直拷模式。");
		return info;
	}

	std::string cur_type;
	std::istringstream iss(stream_out);
	std::string line;
	while (std::getline(iss, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq);
		std::string v = line.substr(eq + 1);
		if (k == "codec_type") {
			cur_type = v;
		} else if (cur_type == "video") {
			if (k == "codec_name" && info.v_codec.empty()) info.v_codec = v;
			else if (k == "profile" && info.v_profile.empty()) info.v_profile = v;
			else if (k == "pix_fmt" && info.v_pix_fmt.empty()) info.v_pix_fmt = v;
			else if (k == "r_frame_rate" && info.v_fps.empty()) info.v_fps = v;
			else if (k == "time_base" && info.v_timescale.empty()) {
				auto slash = v.find('/');
				if (slash != std::string::npos) info.v_timescale = v.substr(slash + 1);
			}
		} else if (cur_type == "audio") {
			if (k == "codec_name" && info.a_codec.empty()) info.a_codec = v;
			else if (k == "sample_rate" && info.a_sample_rate.empty()) info.a_sample_rate = v;
			else if (k == "channels" && info.a_channels.empty()) info.a_channels = v;
		}
	}

	if (info.v_codec != "h264" && info.v_codec != "hevc") {
		logger.Log("源视频编码 (" + info.v_codec + ") 暂不支持局部重编码拼接，将使用普通直拷。");
		return info;
	}
	if (!info.a_codec.empty() && info.a_codec != "aac" && info.a_codec != "mp3") {
		logger.Log("源音频编码 (" + info.a_codec + ") 暂不支持局部重编码拼接，将使用普通直拷。");
		return info;
	}

	// 2. 仅读取包头关键帧时间戳（不解码像素，极快）
	std::wostringstream kf_cmd;
	kf_cmd << L"\"" << ffprobe_exe << L"\" -v error -select_streams v:0 "
	       << L"-show_entries packet=pts_time,flags -of csv=p=0 \"" << video_path.wstring() << L"\"";

	std::string kf_out;
	if (!FFmpegRunner::Execute(kf_cmd.str(), cancel_flag, NULL, &kf_out, true)) {
		logger.Log("ffprobe 关键帧索引读取失败，将使用普通直拷模式。");
		return info;
	}

	std::istringstream kss(kf_out);
	while (std::getline(kss, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		auto comma = line.find(',');
		if (comma == std::string::npos) continue;
		if (line.find('K', comma + 1) == std::string::npos) continue;
		std::string ts_str = line.substr(0, comma);
		if (ts_str.empty() || ts_str == "N/A") continue;
		try {
			double sec = std::stod(ts_str);
			int ms = static_cast<int>(std::llround(sec * 1000.0));
			if (ms >= 0) info.keyframes.push_back({ ms, sec, ts_str });
		} catch (...) {}
	}

	std::sort(info.keyframes.begin(), info.keyframes.end(), [](const KeyframePoint& a, const KeyframePoint& b) {
		return a.sec < b.sec;
	});
	info.keyframes.erase(
		std::unique(info.keyframes.begin(), info.keyframes.end(), [](const KeyframePoint& a, const KeyframePoint& b) {
			return a.ms == b.ms;
		}),
		info.keyframes.end());

	info.valid = !info.keyframes.empty();
	if (info.valid) {
		logger.Log("智能局部重编码已启用: 视频=" + info.v_codec + ", 音频=" + (info.a_codec.empty() ? "无" : info.a_codec) +
		           ", pix_fmt=" + info.v_pix_fmt + ", fps=" + info.v_fps +
		           ", 关键帧数=" + std::to_string(info.keyframes.size()));
	}
	return info;
}

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
		res.message = to_wx("错误：未在系统环境变量 PATH 或常见目录中检测到 FFmpeg (ffmpeg.exe)！\n\n请安装 FFmpeg 并添加至系统 PATH 环境变量，或在弹窗界面中点击【浏览…】进行指定。");
		logger.Log("❌ 未找到 FFmpeg 可执行文件。");
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("FFmpeg 引擎路径: " + PathToUtf8(std::filesystem::path(ffmpeg_exe)));

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
		logger.Log("❌ 视频源文件不存在: " + PathToUtf8(video_fs_path));
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("视频源路径: " + PathToUtf8(video_fs_path));

	auto provider = c->project->VideoProvider();
	int video_dur_ms = provider ? static_cast<int>(provider->GetFrameCount() * 1000.0 / provider->GetFPS().FPS()) : 0;
	logger.Log("视频规格: 时长 " + FormatMsDuration(video_dur_ms) + " (" + std::to_string(video_dur_ms) + " ms)");

	// 3. 解析并合并有效语音片段
	auto all_intervals = ParseSpeechIntervals(c, opt, video_dur_ms, selected_lines);
	if (all_intervals.empty()) {
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
		logger.Log("❌ 无法创建输出目录: " + PathToUtf8(out_dir_path));
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("输出目录: " + PathToUtf8(out_dir_path));

	std::wstring prefix = opt.output_prefix.empty() ? video_fs_path.stem().wstring() : opt.output_prefix;

	// 临时工作目录
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::filesystem::path temp_dir = out_dir_path / (L".vs_temp_" + std::to_wstring(now_ms));
	std::filesystem::create_directories(temp_dir, ec);
	if (ec) {
		res.message = fmt_wx("错误：无法创建临时工作目录：\n%s\n系统错误: %s", temp_dir.wstring(), to_wx(ec.message()));
		logger.Log("❌ 无法创建临时工作目录: " + PathToUtf8(temp_dir));
		res.detailed_log = logger.Str();
		return res;
	}
	logger.Log("临时工作目录: " + PathToUtf8(temp_dir));

	int total_intervals = static_cast<int>(all_intervals.size());

	logger.Log("模式: " + std::string(opt.mode == MODE_SELECTED_LINES ? "切分选中行" : "智能精简合并"));
	logger.Log("有效切片总数: " + std::to_string(total_intervals));

	if (total_intervals == 0) {
		res.message = to_wx("所有字幕区间均为空，无需切分。");
		logger.Log("❌ 所有字幕区间均为空，无需切分。");
		std::filesystem::remove_all(temp_dir, ec);
		res.detailed_log = logger.Str();
		return res;
	}

	auto report = [&](int step_pct, const wxString& msg) {
		if (on_progress) on_progress(step_pct, msg);
	};

	report(2, to_wx("正在探测视频关键帧与编码参数..."));

	SourceMediaProbe probe;
	if (opt.fast_stream_copy && !is_audio) {
		std::wstring ffprobe_exe = FindFFprobeExecutable(ffmpeg_exe);
		probe = ProbeSourceMedia(ffprobe_exe, video_fs_path, cancel_flag.get(), logger);
	}

	std::mutex err_mtx;
	std::string last_cut_err;
	std::string last_concat_err;
	std::atomic<int> processed_intervals{0};

	auto ascii_to_wide = [](const std::string& s) -> std::wstring {
		return std::wstring(s.begin(), s.end());
	};

	auto format_sec_exact = [](double sec) -> std::string {
		if (sec <= 0.0) return "0.000000";
		char buf[48];
		snprintf(buf, sizeof(buf), "%.6f", sec);
		return std::string(buf);
	};

	const bool use_smart_cut = opt.fast_stream_copy && !is_audio && probe.valid && !probe.keyframes.empty();
	const std::wstring clip_ext = use_smart_cut ? L".ts" : ext;

	// 辅助切分单个时间段（支持精确关键帧直拷与参数对齐的局部重编码）
	auto cut_segment = [&](int idx, const char* tag, const std::string& ss_arg, const std::string& dur_arg,
	                       const std::filesystem::path& temp_out, int ffmpeg_threads, bool use_stream_copy) -> bool {
		logger.Log("切片 #" + std::to_string(idx + 1) + (tag ? std::string(" [") + tag + "]" : "") +
		           ": ss=" + ss_arg + ", dur=" + dur_arg + "s");

		std::wostringstream cmd;
		cmd << L"\"" << ffmpeg_exe << L"\" -hide_banner -loglevel warning -y -ss "
		    << ascii_to_wide(ss_arg)
		    << L" -i \"" << video_fs_path.wstring() << L"\" -t "
		    << ascii_to_wide(dur_arg);

		if (is_audio) {
			cmd << L" -vn -sn -dn";
			if (opt.format == FMT_WAV) {
				cmd << L" -c:a pcm_s16le";
			} else if (opt.format == FMT_MP3) {
				cmd << L" -c:a libmp3lame -q:a 2";
			} else {
				cmd << L" -c:a aac -b:a 192k";
			}
		} else {
			cmd << L" -map 0:v:0? -map 0:a:0? -sn -dn";
			if (use_stream_copy) {
				cmd << L" -c copy -avoid_negative_ts make_zero";
			} else {
				std::wstring vcodec = (probe.valid && probe.v_codec == "hevc") ? L"libx265" : L"libx264";
				cmd << L" -c:v " << vcodec << L" -preset veryfast -crf 18 -threads " << ffmpeg_threads;
				if (use_smart_cut) {
					cmd << L" -bf 0";
				}
				if (probe.valid) {
					if (!probe.v_pix_fmt.empty()) {
						cmd << L" -pix_fmt " << ascii_to_wide(probe.v_pix_fmt);
					}
					if (!probe.v_fps.empty() && probe.v_fps != "0/0") {
						cmd << L" -r " << ascii_to_wide(probe.v_fps);
					}
					if (!use_smart_cut && opt.format == FMT_MP4 && !probe.v_timescale.empty()) {
						cmd << L" -video_track_timescale " << ascii_to_wide(probe.v_timescale);
					}
					if (vcodec == L"libx264") {
						if (probe.v_profile == "High") cmd << L" -profile:v high";
						else if (probe.v_profile == "Main") cmd << L" -profile:v main";
						else if (probe.v_profile == "Baseline" || probe.v_profile == "Constrained Baseline") cmd << L" -profile:v baseline";
					}
				}
				if (use_smart_cut && probe.a_codec == "mp3") {
					cmd << L" -c:a libmp3lame -q:a 2";
				} else {
					cmd << L" -c:a aac -b:a 192k";
				}
				if (probe.valid) {
					if (!probe.a_sample_rate.empty()) cmd << L" -ar " << ascii_to_wide(probe.a_sample_rate);
					if (!probe.a_channels.empty()) cmd << L" -ac " << ascii_to_wide(probe.a_channels);
				}
				if (!use_smart_cut && opt.format == FMT_MP4) {
					cmd << L" -movflags +faststart";
				}
			}
			if (use_smart_cut) {
				cmd << L" -f mpegts";
			}
		}

		cmd << L" \"" << temp_out.wstring() << L"\"";

		std::string err_log;
		bool ok = FFmpegRunner::Execute(cmd.str(), cancel_flag.get(), temp_dir.wstring().c_str(), &err_log);
		if (!ok) {
			logger.Log("  ❌ 切片 #" + std::to_string(idx + 1) + " FFmpeg 返回执行失败！");
			if (!err_log.empty()) logger.Log("  输出: " + err_log);
			std::lock_guard<std::mutex> lk(err_mtx);
			if (last_cut_err.empty()) last_cut_err = err_log;
			return false;
		}

		std::error_code f_ec;
		auto sz = std::filesystem::file_size(temp_out, f_ec);
		if (f_ec || sz == 0) {
			logger.Log("  ❌ 切片 #" + std::to_string(idx + 1) + " 生成的文件大小为 0 字节");
			std::lock_guard<std::mutex> lk(err_mtx);
			if (last_cut_err.empty()) last_cut_err = "FFmpeg 生成的文件大小为 0。";
			return false;
		}
		logger.Log("  ✅ 切片 #" + std::to_string(idx + 1) + " 生成成功 (" + std::to_string(sz) + " 字节)");
		return true;
	};

	// 多线程并行切分一批区间（局部重编码模式使用 MPEG-TS 中间切片 + 精确关键帧 PTS 寻道）
	auto cut_intervals_parallel = [&](const std::vector<SpeechInterval>& intervals, const std::wstring& clip_prefix, const wxString& label_prefix) -> std::vector<std::filesystem::path> {
		const size_t n = intervals.size();
		std::vector<std::vector<std::filesystem::path>> ordered_slots(n);
		if (n == 0) return {};

		size_t hw = std::thread::hardware_concurrency();
		if (hw == 0) hw = 4;
		size_t max_workers = (opt.fast_stream_copy && !use_smart_cut) ? 4 : 6;
		size_t num_workers = std::min({ n, max_workers, std::max<size_t>(2, hw) });
		int ffmpeg_threads = std::max(1, static_cast<int>(hw / num_workers));

		logger.Log("并行切分 " + std::to_string(n) + " 个片段，并发工作线程数: " + std::to_string(num_workers) +
		           (use_smart_cut ? " (关键帧边界局部重编码 / TS 无缝拼接)" : (opt.fast_stream_copy ? " (直拷模式)" : " (全片重编码)")));

		std::atomic<size_t> next_idx{0};
		std::vector<std::thread> workers;
		workers.reserve(num_workers);

		for (size_t w = 0; w < num_workers; ++w) {
			workers.emplace_back([&]() {
				while (true) {
					if (cancel_flag && cancel_flag->load()) break;
					size_t k = next_idx.fetch_add(1);
					if (k >= n) break;

					const auto& inv = intervals[k];
					if (inv.end_ms > inv.start_ms) {
						if (use_smart_cut) {
							auto kf_it = std::lower_bound(
								probe.keyframes.begin(), probe.keyframes.end(), inv.start_ms - 40,
								[](const KeyframePoint& kf, int t_ms) { return kf.ms < t_ms; });

							if (kf_it == probe.keyframes.end() || kf_it->ms >= inv.end_ms - 40) {
								// 整段位于两关键帧之间：整段局部重编码
								std::filesystem::path p = temp_dir / (clip_prefix + std::to_wstring(k) + L"_reenc" + clip_ext);
								if (cut_segment(static_cast<int>(k), "局部重编码",
								                FormatMsTime(inv.start_ms), FormatMsSeconds(inv.end_ms - inv.start_ms),
								                p, ffmpeg_threads, false)) {
									ordered_slots[k].push_back(std::move(p));
								}
							} else if (kf_it->ms <= inv.start_ms + 40) {
								// 起始点落在关键帧上：用原始精确 pts_str 无损直拷
								double dur_s = std::max(0.01, inv.end_ms / 1000.0 - kf_it->sec);
								std::filesystem::path p = temp_dir / (clip_prefix + std::to_wstring(k) + L"_copy" + clip_ext);
								if (cut_segment(static_cast<int>(k), "关键帧直拷",
								                kf_it->pts_str, format_sec_exact(dur_s),
								                p, ffmpeg_threads, true)) {
									ordered_slots[k].push_back(std::move(p));
								}
							} else {
								// 混合切分：头部 [start_ms, kf.sec] 局部重编码 + 主体 [kf.pts_str, end_ms] 无损直拷
								const KeyframePoint& kf = *kf_it;
								double head_dur_s = std::max(0.01, kf.sec - inv.start_ms / 1000.0);
								double body_dur_s = std::max(0.01, inv.end_ms / 1000.0 - kf.sec);
								std::filesystem::path p_head = temp_dir / (clip_prefix + std::to_wstring(k) + L"_0_head" + clip_ext);
								std::filesystem::path p_body = temp_dir / (clip_prefix + std::to_wstring(k) + L"_1_body" + clip_ext);
								bool ok_h = cut_segment(static_cast<int>(k), "头部局部重编码",
								                        FormatMsTime(inv.start_ms), format_sec_exact(head_dur_s),
								                        p_head, ffmpeg_threads, false);
								bool ok_b = cut_segment(static_cast<int>(k), "主体无损直拷",
								                        kf.pts_str, format_sec_exact(body_dur_s),
								                        p_body, ffmpeg_threads, true);
								if (ok_h && ok_b) {
									ordered_slots[k].push_back(std::move(p_head));
									ordered_slots[k].push_back(std::move(p_body));
								}
							}
						} else {
							std::filesystem::path temp_clip = temp_dir / (clip_prefix + std::to_wstring(k) + clip_ext);
							if (cut_segment(static_cast<int>(k), nullptr,
							                FormatMsTime(inv.start_ms), FormatMsSeconds(inv.end_ms - inv.start_ms),
							                temp_clip, ffmpeg_threads, opt.fast_stream_copy)) {
								ordered_slots[k].push_back(std::move(temp_clip));
							}
						}
					}

					int done = ++processed_intervals;
					int pct = static_cast<int>(10 + 80.0 * done / std::max(1, total_intervals));
					report(pct, fmt_wx("%s [%d/%d]...", label_prefix, done, total_intervals));
				}
			});
		}
		for (auto& t : workers) {
			if (t.joinable()) t.join();
		}

		std::vector<std::filesystem::path> temp_clips;
		temp_clips.reserve(n * 2);
		for (size_t k = 0; k < n; ++k) {
			for (auto& p : ordered_slots[k]) {
				temp_clips.push_back(std::move(p));
			}
		}
		return temp_clips;
	};

	// 辅助合并多个片段
	auto concat_clips = [&](const std::vector<std::filesystem::path>& clip_files, const std::filesystem::path& final_out) -> bool {
		if (clip_files.empty()) return false;
		if (clip_files.size() == 1 && !use_smart_cut) {
			// 单个同容器片段无需 concat，优先同盘 O(1) 移动，失败再复制
			logger.Log("仅有 1 个切片，直接输出到最终媒体: " + PathToUtf8(final_out));
			std::error_code mv_ec;
			std::filesystem::remove(final_out, mv_ec);
			mv_ec.clear();
			std::filesystem::rename(clip_files[0], final_out, mv_ec);
			if (!mv_ec) return true;

			std::error_code cp_ec;
			std::filesystem::copy_file(clip_files[0], final_out, std::filesystem::copy_options::overwrite_existing, cp_ec);
			if (!cp_ec) return true;

			last_concat_err = "写入最终文件失败 (" + PathToUtf8(final_out) + "): " + cp_ec.message();
			logger.Log("  ❌ " + last_concat_err);
			return false;
		}

		logger.Log("开始合并 " + std::to_string(clip_files.size()) + " 个片段到: " + PathToUtf8(final_out));
		std::filesystem::path list_path = temp_dir / L"concat_list.txt";
		{
			std::ofstream list_fs(list_path, std::ios::binary);
			if (!list_fs.is_open()) {
				last_concat_err = "无法创建合并清单文件: concat_list.txt";
				logger.Log("  ❌ " + last_concat_err);
				return false;
			}
			for (const auto& cp : clip_files) {
				std::string fn = PathToUtf8(cp.filename());
				std::string escaped;
				escaped.reserve(fn.size());
				for (char ch : fn) {
					if (ch == '\'') escaped += "'\\''";
					else escaped += ch;
				}
				list_fs << "file '" << escaped << "'\n";
			}
		}

		std::wostringstream cmd;
		cmd << L"\"" << ffmpeg_exe << L"\" -hide_banner -loglevel warning -y -f concat -safe 0 -i \""
		    << list_path.wstring() << L"\" -c copy";
		if (use_smart_cut && opt.format == FMT_MP4) {
			if (probe.a_codec == "aac") {
				cmd << L" -bsf:a aac_adtstoasc";
			}
			cmd << L" -movflags +faststart";
		}
		cmd << L" \"" << final_out.wstring() << L"\"";

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
		std::filesystem::path out_media = out_dir_path / (prefix + (opt.mode == MODE_SELECTED_LINES ? L"_selection" : L"_cut") + ext);
		std::filesystem::path out_ass = out_dir_path / (prefix + (opt.mode == MODE_SELECTED_LINES ? L"_selection" : L"_cut") + L".ass");

		report(10, fmt_wx("正在并行切分 %d 个片段...", static_cast<int>(all_intervals.size())));
		std::vector<std::filesystem::path> temp_clips = cut_intervals_parallel(
			all_intervals,
			L"clip_",
			to_wx("正在切分片段"));

		if (cancel_flag && !cancel_flag->load() && !temp_clips.empty()) {
			report(92, to_wx("正在合并最终媒体文件..."));
			if (concat_clips(temp_clips, out_media)) {
				res.output_media_files.push_back(out_media.wstring());
				res.primary_media_file = out_media.wstring();
			}

			if (opt.generate_synced_ass) {
				report(97, to_wx("正在生成严格对齐的新 ASS 字幕..."));
				if (WriteRetimedAssFile(c, all_intervals, out_ass, false, out_media.filename().wstring(), is_audio)) {
					res.output_ass_files.push_back(out_ass.wstring());
					res.primary_ass_file = out_ass.wstring();
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
		res.total_clips_cut = processed_intervals.load();
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
