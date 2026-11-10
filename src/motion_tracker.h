// Copyright (c) 2026, Aegisub Project
// All rights reserved.
//
// Native In-App Motion Tracking Engine for Aegisub.
// Multi-scale Normalized Cross-Correlation (NCC) with Sub-Pixel Parabolic Refinement,
// Dual-Template Anchoring, Bidirectional Tracking, Trajectory Smoothing,
// Frame-by-Frame Relative Motion Subtitle Generation, and Mocha AE Clipboard Export.

#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_frame.h"
#include <libaegisub/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace MotionTrack {

struct TrackPoint {
	double x = 0.0;     // Center X in native video pixels
	double y = 0.0;     // Center Y in native video pixels
	double score = 1.0; // NCC correlation score [-1.0, 1.0]
	bool valid = false; // Whether tracked successfully
};

struct TrackingOptions {
	int search_radius = 40;              // Search radius around predicted center (pixels)
	int smooth_window = 3;               // Moving average smooth window (1 = off, 3 or 5 = smooth)
	bool relative_mode = true;           // True: Relative displacement to reference frame; False: Absolute
	bool split_per_frame = true;         // Generate 1 subtitle line per frame
	bool merge_static = true;            // Merge consecutive stationary/static frames into single line
	double static_threshold = 0.5;       // Static movement threshold in pixels (<= 0.5px considered stationary)
	bool fold_in_grid = true;            // Fold/collapse generated motion lines into 1 line in subtitle grid
	bool backup_original = true;         // Keep original line as commented backup
	double template_update_alpha = 0.08; // Learning rate for dynamic template
	double ncc_threshold = 0.25;         // Minimum confidence score
};

struct MotionTrajectory {
	int ref_frame = 0;
	TrackPoint ref_point;
	int box_w = 0;
	int box_h = 0;
	std::map<int, TrackPoint> frames; // frame_idx -> TrackPoint
	int video_width = 0;
	int video_height = 0;
	double fps = 24.0;
};

// --------------------------------------------------------------------------
// 1. 图像采样与归一化互相关 (NCC) 匹配
// --------------------------------------------------------------------------

inline uint8_t GetPixelGray(const VideoFrame& f, int x, int y) {
	x = std::clamp(x, 0, f.width - 1);
	y = std::clamp(y, 0, f.height - 1);
	int row = f.flipped ? (f.height - 1 - y) : y;
	const uint8_t* p = f.data.data() + static_cast<size_t>(row) * f.pitch + static_cast<size_t>(x) * 4;
	// p[0]=B, p[1]=G, p[2]=R, p[3]=A
	return static_cast<uint8_t>((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
}

struct GrayPatch {
	int w = 0;
	int h = 0;
	std::vector<float> data; // Zero-mean data
	float norm = 1.0f;       // sqrt(sum(data^2))

	void Extract(const VideoFrame& f, int x0, int y0, int pw, int ph) {
		w = pw;
		h = ph;
		size_t n = static_cast<size_t>(w * h);
		data.resize(n);
		float sum = 0.0f;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				float val = static_cast<float>(GetPixelGray(f, x0 + x, y0 + y));
				data[y * w + x] = val;
				sum += val;
			}
		}
		float mean = (n > 0) ? (sum / n) : 0.0f;
		float var_sum = 0.0f;
		for (size_t i = 0; i < n; ++i) {
			data[i] -= mean;
			var_sum += data[i] * data[i];
		}
		norm = std::sqrt(std::max(1e-4f, var_sum));
	}
};

inline float ComputeNCC(const GrayPatch& p1, const GrayPatch& p2) {
	if (p1.data.size() != p2.data.size() || p1.data.empty()) return -1.0f;
	float dot = 0.0f;
	size_t n = p1.data.size();
	const float* d1 = p1.data.data();
	const float* d2 = p2.data.data();
	for (size_t i = 0; i < n; ++i) {
		dot += d1[i] * d2[i];
	}
	return dot / (p1.norm * p2.norm);
}

// --------------------------------------------------------------------------
// 2. 多尺度搜索与亚像素抛物线拟合 (Sub-pixel Parabolic Refinement)
// --------------------------------------------------------------------------

inline TrackPoint MatchNextFrame(
	const VideoFrame& f,
	const GrayPatch& t_init,
	GrayPatch& t_curr,
	int box_w, int box_h,
	double prev_center_x, double prev_center_y,
	double vel_x, double vel_y,
	int search_radius,
	double alpha = 0.08)
{
	// 基于动量预测的初猜搜索中心
	int center_x = static_cast<int>(std::round(prev_center_x + vel_x));
	int center_y = static_cast<int>(std::round(prev_center_y + vel_y));

	int best_dx = 0;
	int best_dy = 0;
	float best_score = -2.0f;

	// 第一阶段：粗粒度粗筛 (步长 2)
	int stride = (search_radius > 25) ? 2 : 1;
	GrayPatch cand;

	for (int dy = -search_radius; dy <= search_radius; dy += stride) {
		for (int dx = -search_radius; dx <= search_radius; dx += stride) {
			int cur_x0 = center_x + dx - box_w / 2;
			int cur_y0 = center_y + dy - box_h / 2;
			cand.Extract(f, cur_x0, cur_y0, box_w, box_h);
			float s_curr = ComputeNCC(cand, t_curr);
			float s_init = ComputeNCC(cand, t_init);
			// 双模板融合：动态模板跟形变，初始模板锁原位防漂移
			float score = 0.65f * s_curr + 0.35f * s_init;
			if (score > best_score) {
				best_score = score;
				best_dx = dx;
				best_dy = dy;
			}
		}
	}

	// 第二阶段：在粗筛峰值周围 5x5 范围内以步长 1 进行精确定位
	if (stride > 1) {
		int r_fine = stride;
		int fine_best_dx = best_dx;
		int fine_best_dy = best_dy;
		for (int dy = best_dy - r_fine; dy <= best_dy + r_fine; ++dy) {
			for (int dx = best_dx - r_fine; dx <= best_dx + r_fine; ++dx) {
				int cur_x0 = center_x + dx - box_w / 2;
				int cur_y0 = center_y + dy - box_h / 2;
				cand.Extract(f, cur_x0, cur_y0, box_w, box_h);
				float s_curr = ComputeNCC(cand, t_curr);
				float s_init = ComputeNCC(cand, t_init);
				float score = 0.65f * s_curr + 0.35f * s_init;
				if (score > best_score) {
					best_score = score;
					fine_best_dx = dx;
					fine_best_dy = dy;
				}
			}
		}
		best_dx = fine_best_dx;
		best_dy = fine_best_dy;
	}

	// 第三阶段：3x3 邻域亚像素抛物线拟合 (精确到 0.05 像素)
	float c00 = best_score;
	cand.Extract(f, center_x + best_dx - 1 - box_w / 2, center_y + best_dy - box_h / 2, box_w, box_h);
	float cx_minus = 0.65f * ComputeNCC(cand, t_curr) + 0.35f * ComputeNCC(cand, t_init);

	cand.Extract(f, center_x + best_dx + 1 - box_w / 2, center_y + best_dy - box_h / 2, box_w, box_h);
	float cx_plus = 0.65f * ComputeNCC(cand, t_curr) + 0.35f * ComputeNCC(cand, t_init);

	cand.Extract(f, center_x + best_dx - box_w / 2, center_y + best_dy - 1 - box_h / 2, box_w, box_h);
	float cy_minus = 0.65f * ComputeNCC(cand, t_curr) + 0.35f * ComputeNCC(cand, t_init);

	cand.Extract(f, center_x + best_dx - box_w / 2, center_y + best_dy + 1 - box_h / 2, box_w, box_h);
	float cy_plus = 0.65f * ComputeNCC(cand, t_curr) + 0.35f * ComputeNCC(cand, t_init);

	double denom_x = 2.0 * (2.0 * c00 - cx_plus - cx_minus);
	double sub_dx = (std::abs(denom_x) > 1e-4) ? ((cx_plus - cx_minus) / denom_x) : 0.0;
	sub_dx = std::clamp(sub_dx, -0.5, 0.5);

	double denom_y = 2.0 * (2.0 * c00 - cy_plus - cy_minus);
	double sub_dy = (std::abs(denom_y) > 1e-4) ? ((cy_plus - cy_minus) / denom_y) : 0.0;
	sub_dy = std::clamp(sub_dy, -0.5, 0.5);

	TrackPoint pt;
	pt.x = center_x + best_dx + sub_dx;
	pt.y = center_y + best_dy + sub_dy;
	pt.score = best_score;
	pt.valid = (best_score >= 0.25f);

	// 动态模板 EMA 平滑更新
	if (pt.valid && alpha > 0.0) {
		cand.Extract(f, static_cast<int>(std::round(pt.x - box_w / 2.0)), static_cast<int>(std::round(pt.y - box_h / 2.0)), box_w, box_h);
		for (size_t i = 0; i < t_curr.data.size(); ++i) {
			t_curr.data[i] = static_cast<float>(t_curr.data[i] * (1.0 - alpha) + cand.data[i] * alpha);
		}
		float v_sum = 0.0f;
		for (float v : t_curr.data) v_sum += v * v;
		t_curr.norm = std::sqrt(std::max(1e-4f, v_sum));
	}

	return pt;
}

// --------------------------------------------------------------------------
// 3. 轨迹平滑滤波 (Moving Average Smoothing)
// --------------------------------------------------------------------------

inline void SmoothTrajectory(std::map<int, TrackPoint>& traj, int window_size = 3) {
	if (window_size <= 1 || traj.size() < 3) return;
	int half = window_size / 2;
	std::map<int, TrackPoint> smoothed = traj;

	for (const auto& pair : traj) {
		int f = pair.first;
		double sum_x = 0.0;
		double sum_y = 0.0;
		int count = 0;

		for (int w = -half; w <= half; ++w) {
			auto it = traj.find(f + w);
			if (it != traj.end() && it->second.valid) {
				sum_x += it->second.x;
				sum_y += it->second.y;
				count++;
			}
		}

		if (count > 0) {
			smoothed[f].x = sum_x / count;
			smoothed[f].y = sum_y / count;
		}
	}
	traj = smoothed;
}

// --------------------------------------------------------------------------
// 4. 双向全范围追踪引擎 (Bidirectional Tracking Pipeline)
// --------------------------------------------------------------------------

inline MotionTrajectory TrackFullRange(
	agi::Context* c,
	int ref_frame,
	int box_x0, int box_y0, int box_w, int box_h,
	int start_frame, int end_frame,
	const TrackingOptions& opt,
	std::function<void(int cur_frame, int total, double x, double y)> on_progress = nullptr,
	std::shared_ptr<std::atomic<bool>> cancel = nullptr)
{
	MotionTrajectory traj;
	if (!c || !c->project) return traj;
	auto provider = c->project->VideoProvider();
	if (!provider || box_w < 8 || box_h < 8) return traj;

	int total_frames = provider->GetFrameCount();
	if (total_frames <= 0) return traj;

	ref_frame = std::clamp(ref_frame, 0, total_frames - 1);
	start_frame = std::clamp(start_frame, 0, total_frames - 1);
	end_frame = std::clamp(end_frame, 0, total_frames - 1);
	if (start_frame > end_frame) std::swap(start_frame, end_frame);

	traj.ref_frame = ref_frame;
	traj.box_w = box_w;
	traj.box_h = box_h;
	traj.video_width = provider->GetWidth();
	traj.video_height = provider->GetHeight();
	traj.fps = provider->GetFPS().FPS();

	// 提取基准参考帧的初始目标模板
	auto ref_f = provider->GetFrame(ref_frame, c->videoController->TimeAtFrame(ref_frame), true);
	GrayPatch t_init;
	t_init.Extract(*ref_f, box_x0, box_y0, box_w, box_h);

	TrackPoint ref_pt;
	ref_pt.x = box_x0 + box_w / 2.0;
	ref_pt.y = box_y0 + box_h / 2.0;
	ref_pt.score = 1.0;
	ref_pt.valid = true;
	traj.ref_point = ref_pt;
	traj.frames[ref_frame] = ref_pt;

	int total_to_track = (end_frame - start_frame + 1);
	int done_count = 1;

	// 1. 向后追踪 (Forward: ref_frame + 1 -> end_frame)
	if (ref_frame < end_frame) {
		GrayPatch t_curr = t_init;
		double cur_x = ref_pt.x;
		double cur_y = ref_pt.y;
		double vel_x = 0.0, vel_y = 0.0;

		for (int f = ref_frame + 1; f <= end_frame; ++f) {
			if (cancel && cancel->load()) return traj;

			auto frame_data = provider->GetFrame(f, c->videoController->TimeAtFrame(f), true);
			TrackPoint pt = MatchNextFrame(*frame_data, t_init, t_curr, box_w, box_h,
			                               cur_x, cur_y, vel_x, vel_y,
			                               opt.search_radius, opt.template_update_alpha);
			vel_x = 0.6 * vel_x + 0.4 * (pt.x - cur_x);
			vel_y = 0.6 * vel_y + 0.4 * (pt.y - cur_y);
			cur_x = pt.x;
			cur_y = pt.y;

			traj.frames[f] = pt;
			done_count++;
			if (on_progress) on_progress(done_count, total_to_track, cur_x, cur_y);
		}
	}

	// 2. 向前追踪 (Backward: ref_frame - 1 -> start_frame)
	if (ref_frame > start_frame) {
		GrayPatch t_curr = t_init;
		double cur_x = ref_pt.x;
		double cur_y = ref_pt.y;
		double vel_x = 0.0, vel_y = 0.0;

		for (int f = ref_frame - 1; f >= start_frame; --f) {
			if (cancel && cancel->load()) return traj;

			auto frame_data = provider->GetFrame(f, c->videoController->TimeAtFrame(f), true);
			TrackPoint pt = MatchNextFrame(*frame_data, t_init, t_curr, box_w, box_h,
			                               cur_x, cur_y, vel_x, vel_y,
			                               opt.search_radius, opt.template_update_alpha);
			vel_x = 0.6 * vel_x + 0.4 * (pt.x - cur_x);
			vel_y = 0.6 * vel_y + 0.4 * (pt.y - cur_y);
			cur_x = pt.x;
			cur_y = pt.y;

			traj.frames[f] = pt;
			done_count++;
			if (on_progress) on_progress(done_count, total_to_track, cur_x, cur_y);
		}
	}

	// 3. 执行轨迹平滑滤波
	if (opt.smooth_window >= 3) {
		SmoothTrajectory(traj.frames, opt.smooth_window);
	}

	return traj;
}

// --------------------------------------------------------------------------
// 5. 相对位移计算与逐帧字幕生成 (Frame-by-Frame Relative Motion Generation)
// --------------------------------------------------------------------------

inline bool ExtractPosFromText(const std::string& text, double& out_x, double& out_y) {
	size_t pos_idx = text.find("\\pos");
	if (pos_idx != std::string::npos) {
		size_t open_paren = text.find('(', pos_idx);
		size_t close_paren = text.find(')', open_paren);
		if (open_paren != std::string::npos && close_paren != std::string::npos) {
			std::string inside = text.substr(open_paren + 1, close_paren - open_paren - 1);
			double x = 0.0, y = 0.0;
			if (sscanf(inside.c_str(), "%lf , %lf", &x, &y) == 2 ||
			    sscanf(inside.c_str(), "%lf,%lf", &x, &y) == 2) {
				out_x = x;
				out_y = y;
				return true;
			}
		}
	}
	return false;
}

inline std::string ReplaceOrInsertPos(const std::string& text, double new_x, double new_y) {
	std::string new_pos_tag = agi::format("\\pos(%.1f,%.1f)", new_x, new_y);
	size_t pos_idx = text.find("\\pos");
	if (pos_idx != std::string::npos) {
		size_t open_paren = text.find('(', pos_idx);
		size_t close_paren = text.find(')', open_paren);
		if (open_paren != std::string::npos && close_paren != std::string::npos) {
			std::string result = text;
			result.replace(pos_idx, close_paren - pos_idx + 1, new_pos_tag);
			return result;
		}
	}

	if (!text.empty() && text.front() == '{') {
		std::string result = text;
		result.insert(1, new_pos_tag);
		return result;
	}
	return "{" + new_pos_tag + "}" + text;
}

inline int ApplyTrajectoryToLine(
	agi::Context* c,
	AssDialogue* target_line,
	const MotionTrajectory& traj,
	const TrackingOptions& opt)
{
	if (!c || !c->ass || !target_line || traj.frames.empty()) return 0;
	auto vctrl = c->videoController.get();
	auto provider = c->project->VideoProvider();
	if (!vctrl || !provider) return 0;

	int res_x = c->ass->GetScriptInfoAsInt("PlayResX");
	int res_y = c->ass->GetScriptInfoAsInt("PlayResY");
	if (res_x <= 0) res_x = provider->GetWidth();
	if (res_y <= 0) res_y = provider->GetHeight();

	double scale_x = (provider->GetWidth() > 0) ? (static_cast<double>(res_x) / provider->GetWidth()) : 1.0;
	double scale_y = (provider->GetHeight() > 0) ? (static_cast<double>(res_y) / provider->GetHeight()) : 1.0;

	// 解析字幕在参考帧的固定基准坐标 (S_x0, S_y0)
	double s_x0 = res_x / 2.0;
	double s_y0 = res_y - 60.0;
	bool has_pos = ExtractPosFromText(target_line->Text.get(), s_x0, s_y0);
	if (!has_pos) {
		s_x0 = res_x / 2.0;
		s_y0 = res_y - 60.0;
	}

	double ref_tx = traj.ref_point.x;
	double ref_ty = traj.ref_point.y;

	// 生成唯一组标识符用于折叠与级联管理
	static std::atomic<uint64_t> s_group_seq{1};
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::string gid = "mg_" + std::to_string(now_ms) + "_" + std::to_string(s_group_seq++);

	std::vector<AssDialogue*> new_lines;
	new_lines.reserve(traj.frames.size());

	double prev_sx = -999999.0;
	double prev_sy = -999999.0;

	for (const auto& pair : traj.frames) {
		int f = pair.first;
		const auto& pt = pair.second;

		double target_sx = 0.0;
		double target_sy = 0.0;

		if (opt.relative_mode) {
			// 相对位移运动：花字移动多少，字幕相对其在基准帧的固定位置也移动多少
			double delta_tx = (pt.x - ref_tx) * scale_x;
			double delta_ty = (pt.y - ref_ty) * scale_y;
			target_sx = s_x0 + delta_tx;
			target_sy = s_y0 + delta_ty;
		} else {
			// 绝对坐标跟随：字幕直接附着在花字中心
			target_sx = pt.x * scale_x;
			target_sy = pt.y * scale_y;
		}

		int64_t t_start = vctrl->TimeAtFrame(f, agi::vfr::EXACT);
		int64_t t_end = vctrl->TimeAtFrame(f + 1, agi::vfr::EXACT);
		if (t_end <= t_start) t_end = t_start + 33;

		// 静止帧智能合并：与上一帧位置差低于阈值时，直接延长前一行的结束时间
		bool is_static = false;
		if (opt.merge_static && !new_lines.empty()) {
			double dx = std::abs(target_sx - prev_sx);
			double dy = std::abs(target_sy - prev_sy);
			if (dx <= opt.static_threshold && dy <= opt.static_threshold) {
				is_static = true;
			}
		}

		if (is_static) {
			new_lines.back()->End = static_cast<int>(t_end);
		} else {
			std::string new_text = ReplaceOrInsertPos(target_line->Text.get(), target_sx, target_sy);

			auto line_f = new AssDialogue(*target_line);
			line_f->Start = static_cast<int>(t_start);
			line_f->End = static_cast<int>(t_end);
			line_f->Text = new_text;
			line_f->Comment = false;
			if (opt.fold_in_grid) {
				line_f->Effect = "motion-frame:" + gid;
			}
			new_lines.push_back(line_f);

			prev_sx = target_sx;
			prev_sy = target_sy;
		}
	}

	if (new_lines.empty()) return 0;

	// 创建折叠摘要行 (Group Header)
	AssDialogue* leader_line = nullptr;
	if (opt.fold_in_grid) {
		leader_line = new AssDialogue(*target_line);
		leader_line->Comment = true; // 设为注释行，防止视频渲染器重复绘制在画面上
		leader_line->Effect = "motion-group:" + gid;
		leader_line->Start = new_lines.front()->Start;
		leader_line->End = new_lines.back()->End;

		// 提取无特效标签的纯文本摘要
		std::string raw_text = target_line->Text.get();
		if (raw_text.rfind("[原字幕备份] ", 0) == 0) {
			raw_text = raw_text.substr(std::string("[原字幕备份] ").length());
		}
		std::string clean_text;
		bool in_tag = false;
		for (char ch : raw_text) {
			if (ch == '{') { in_tag = true; continue; }
			if (ch == '}') { in_tag = false; continue; }
			if (!in_tag) clean_text += ch;
		}
		if (clean_text.empty()) clean_text = "花字追踪";
		if (clean_text.length() > 30) clean_text = clean_text.substr(0, 27) + "...";

		leader_line->Text = "▶ [运动追踪] " + clean_text + " (" + std::to_string(new_lines.size()) + " 帧/段)";
	}

	// 备份或处理原字幕行
	if (opt.backup_original) {
		target_line->Comment = true;
		target_line->Text = "[原字幕备份] " + target_line->Text.get();
	}

	std::vector<AssDialogue*> to_insert;
	if (leader_line) to_insert.push_back(leader_line);
	for (auto nl : new_lines) to_insert.push_back(nl);

	// 将逐帧字幕插入到原字幕行之后
	auto it = std::find_if(c->ass->Events.begin(), c->ass->Events.end(),
		[target_line](const AssDialogue& d) { return &d == target_line; });

	if (it != c->ass->Events.end()) {
		auto insert_pos = std::next(it);
		for (auto line : to_insert) {
			c->ass->Events.insert(insert_pos, *line);
		}
	} else {
		for (auto line : to_insert) {
			c->ass->Events.push_back(*line);
		}
	}

	if (!opt.backup_original) {
		c->ass->Events.erase(it);
		delete target_line;
	}

	c->ass->Commit("运动追踪生成逐帧相对位移字幕", AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);

	if (leader_line) {
		c->selectionController->SetActiveLine(leader_line);
		c->selectionController->SetSelectedSet({leader_line});
	} else {
		c->selectionController->SetActiveLine(new_lines.front());
		c->selectionController->SetSelectedSet({new_lines.front()});
	}
	return static_cast<int>(new_lines.size());
}

// --------------------------------------------------------------------------
// 6. Mocha / After Effects 6.0 关键帧数据剪贴板导出
// --------------------------------------------------------------------------

inline std::string BuildMochaClipboardData(
	const MotionTrajectory& traj,
	int /*ref_frame*/,
	int width, int height, double fps)
{
	std::ostringstream ss;
	ss << "Adobe After Effects 6.0 Keyframe Data\n\n";
	ss << "\tUnits Per Second\t" << std::fixed << std::setprecision(3) << fps << "\n";
	ss << "\tSource Width\t" << width << "\n";
	ss << "\tSource Height\t" << height << "\n";
	ss << "\tSource Pixel Aspect Ratio\t1\n";
	ss << "\tComp Pixel Aspect Ratio\t1\n\n";
	ss << "Transform\tPosition\n";
	ss << "\tFrame\tX pixels\tY pixels\tZ pixels\n";

	if (!traj.frames.empty()) {
		int first_f = traj.frames.begin()->first;
		for (const auto& pair : traj.frames) {
			int rel_f = pair.first - first_f;
			ss << "\t\t" << rel_f << "\t"
			   << std::fixed << std::setprecision(3)
			   << pair.second.x << "\t"
			   << pair.second.y << "\t0.000\n";
		}
	}
	ss << "\nEnd of Keyframe Data\n";
	return ss.str();
}

} // namespace MotionTrack
