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
#include "compat.h"
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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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

struct MotionVideoView {
	int frame_count = 0;
	int native_width = 0;
	int native_height = 0;
	double fps = 24.0;
	std::function<std::shared_ptr<const VideoFrame>(int frame_no)> get_frame;
};

// --------------------------------------------------------------------------
// 1. 连续灰度图像与三层金字塔零均值 NCC 匹配 (Multi-Scale Pyramid NCC)
// --------------------------------------------------------------------------

struct GrayImage {
	int w = 0;
	int h = 0;
	std::vector<uint8_t> data;
};

inline GrayImage BuildGrayImage(const VideoFrame& f) {
	GrayImage img;
	if (f.width <= 0 || f.height <= 0 || f.data.empty()) return img;
	img.w = f.width;
	img.h = f.height;
	img.data.resize(static_cast<size_t>(img.w) * img.h);
	for (int y = 0; y < img.h; ++y) {
		int src_row = f.flipped ? (img.h - 1 - y) : y;
		const uint8_t* src = f.data.data() + static_cast<size_t>(src_row) * f.pitch;
		uint8_t* dst = img.data.data() + static_cast<size_t>(y) * img.w;
		for (int x = 0; x < img.w; ++x) {
			const uint8_t* p = src + static_cast<size_t>(x) * 4;
			dst[x] = static_cast<uint8_t>((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
		}
	}
	return img;
}

inline GrayImage Downsample2x(const GrayImage& src) {
	GrayImage dst;
	int dw = src.w / 2;
	int dh = src.h / 2;
	if (dw < 2 || dh < 2 || src.data.empty()) return dst;
	dst.w = dw;
	dst.h = dh;
	dst.data.resize(static_cast<size_t>(dw) * dh);
	for (int y = 0; y < dh; ++y) {
		const uint8_t* r0 = src.data.data() + static_cast<size_t>(y * 2) * src.w;
		const uint8_t* r1 = src.data.data() + static_cast<size_t>(y * 2 + 1) * src.w;
		uint8_t* out = dst.data.data() + static_cast<size_t>(y) * dw;
		for (int x = 0; x < dw; ++x) {
			int ix = x * 2;
			out[x] = static_cast<uint8_t>((r0[ix] + r0[ix + 1] + r1[ix] + r1[ix + 1] + 2) >> 2);
		}
	}
	return dst;
}

struct ZeroMeanTemplate {
	int w = 0;
	int h = 0;
	std::vector<float> data;
	float inv_norm = 1.0f;
	float inv_n = 1.0f;

	void FinalizeZeroMean() {
		size_t n = data.size();
		if (n == 0) {
			inv_n = 1.0f;
			inv_norm = 1.0f;
			return;
		}
		inv_n = 1.0f / static_cast<float>(n);
		double sum = 0.0;
		for (float v : data) sum += v;
		float mean = static_cast<float>(sum / static_cast<double>(n));
		double var_sum = 0.0;
		for (size_t i = 0; i < n; ++i) {
			data[i] -= mean;
			var_sum += static_cast<double>(data[i]) * data[i];
		}
		inv_norm = static_cast<float>(1.0 / std::sqrt(std::max(1e-4, var_sum)));
	}

	void ExtractFromGray(const GrayImage& img, int x0, int y0, int pw, int ph) {
		w = pw;
		h = ph;
		size_t n = static_cast<size_t>(w) * h;
		data.resize(n);
		if (n == 0 || img.data.empty()) {
			FinalizeZeroMean();
			return;
		}
		bool inside = (x0 >= 0 && y0 >= 0 && x0 + pw <= img.w && y0 + ph <= img.h);
		if (inside) {
			for (int y = 0; y < h; ++y) {
				const uint8_t* row = img.data.data() + static_cast<size_t>(y0 + y) * img.w + x0;
				float* dst = data.data() + static_cast<size_t>(y) * w;
				for (int x = 0; x < w; ++x)
					dst[x] = static_cast<float>(row[x]);
			}
		} else {
			for (int y = 0; y < h; ++y) {
				int sy = std::clamp(y0 + y, 0, img.h - 1);
				const uint8_t* row = img.data.data() + static_cast<size_t>(sy) * img.w;
				float* dst = data.data() + static_cast<size_t>(y) * w;
				for (int x = 0; x < w; ++x) {
					int sx = std::clamp(x0 + x, 0, img.w - 1);
					dst[x] = static_cast<float>(row[sx]);
				}
			}
		}
		FinalizeZeroMean();
	}

	void BlendEMA(const ZeroMeanTemplate& cand, float alpha) {
		if (cand.data.size() != data.size() || data.empty()) return;
		float keep = 1.0f - alpha;
		for (size_t i = 0; i < data.size(); ++i)
			data[i] = data[i] * keep + cand.data[i] * alpha;
		FinalizeZeroMean();
	}
};

inline float EvalDualNCC(
	const GrayImage& img,
	int x0, int y0,
	const ZeroMeanTemplate& t_curr,
	const ZeroMeanTemplate& t_init,
	float w_curr, float w_init)
{
	int w = t_curr.w;
	int h = t_curr.h;
	if (w <= 0 || h <= 0 || img.data.empty()) return -1.0f;

	const float* tc = t_curr.data.data();
	const float* ti = t_init.data.data();
	uint64_t sum_c = 0;
	uint64_t sum_sq = 0;
	double dot_curr = 0.0;
	double dot_init = 0.0;

	if (x0 >= 0 && y0 >= 0 && x0 + w <= img.w && y0 + h <= img.h) {
		for (int y = 0; y < h; ++y) {
			const uint8_t* row = img.data.data() + static_cast<size_t>(y0 + y) * img.w + x0;
			const float* tc_row = tc + static_cast<size_t>(y) * w;
			const float* ti_row = ti + static_cast<size_t>(y) * w;
			for (int x = 0; x < w; ++x) {
				uint32_t c = row[x];
				sum_c += c;
				sum_sq += c * c;
				float fc = static_cast<float>(c);
				dot_curr += fc * tc_row[x];
				dot_init += fc * ti_row[x];
			}
		}
	} else {
		for (int y = 0; y < h; ++y) {
			int sy = std::clamp(y0 + y, 0, img.h - 1);
			const uint8_t* row = img.data.data() + static_cast<size_t>(sy) * img.w;
			const float* tc_row = tc + static_cast<size_t>(y) * w;
			const float* ti_row = ti + static_cast<size_t>(y) * w;
			for (int x = 0; x < w; ++x) {
				int sx = std::clamp(x0 + x, 0, img.w - 1);
				uint32_t c = row[sx];
				sum_c += c;
				sum_sq += c * c;
				float fc = static_cast<float>(c);
				dot_curr += fc * tc_row[x];
				dot_init += fc * ti_row[x];
			}
		}
	}

	double var_c = std::max(1e-4, static_cast<double>(sum_sq) - static_cast<double>(sum_c) * static_cast<double>(sum_c) * t_curr.inv_n);
	double inv_norm_c = 1.0 / std::sqrt(var_c);
	double s_curr = dot_curr * inv_norm_c * t_curr.inv_norm;
	double s_init = dot_init * inv_norm_c * t_init.inv_norm;
	return static_cast<float>(w_curr * s_curr + w_init * s_init);
}

struct GrayPyramid {
	GrayImage l0, l1, l2;
	bool has_l1 = false;
	bool has_l2 = false;
};

struct TemplatePyramid {
	ZeroMeanTemplate l0, l1, l2;
	bool has_l1 = false;
	bool has_l2 = false;
};

inline GrayPyramid BuildGrayPyramid(GrayImage l0, bool need_l1, bool need_l2) {
	GrayPyramid pyr;
	pyr.l0 = std::move(l0);
	if (need_l1) {
		pyr.l1 = Downsample2x(pyr.l0);
		pyr.has_l1 = !pyr.l1.data.empty();
	}
	if (need_l2 && pyr.has_l1) {
		pyr.l2 = Downsample2x(pyr.l1);
		pyr.has_l2 = !pyr.l2.data.empty();
	}
	return pyr;
}

inline TemplatePyramid BuildTemplatePyramid(const GrayPyramid& ref_pyr, int box_x0, int box_y0, int box_w, int box_h) {
	TemplatePyramid tp;
	tp.l0.ExtractFromGray(ref_pyr.l0, box_x0, box_y0, box_w, box_h);
	if (ref_pyr.has_l1 && box_w >= 24 && box_h >= 12) {
		tp.l1.ExtractFromGray(ref_pyr.l1, box_x0 / 2, box_y0 / 2, box_w / 2, box_h / 2);
		tp.has_l1 = (tp.l1.w >= 4 && tp.l1.h >= 4);
	}
	if (tp.has_l1 && ref_pyr.has_l2 && (box_w / 2) >= 24 && (box_h / 2) >= 12) {
		tp.l2.ExtractFromGray(ref_pyr.l2, box_x0 / 4, box_y0 / 4, box_w / 4, box_h / 4);
		tp.has_l2 = (tp.l2.w >= 4 && tp.l2.h >= 4);
	}
	return tp;
}

struct PeakCand {
	int dx = 0;
	int dy = 0;
	float score = -2.0f;
};

inline void SearchLevelGrid(
	const GrayImage& img,
	const ZeroMeanTemplate& t_curr,
	const ZeroMeanTemplate& t_init,
	int base_x0, int base_y0,
	int radius,
	float w_curr, float w_init,
	std::vector<PeakCand>& out)
{
	int cols = 2 * radius + 1;
	int rows = 2 * radius + 1;
	out.resize(static_cast<size_t>(cols) * rows);

	auto eval_rows = [&](int row_begin, int row_end) {
		for (int ry = row_begin; ry < row_end; ++ry) {
			int dy = ry - radius;
			size_t row_off = static_cast<size_t>(ry) * cols;
			for (int rx = 0; rx < cols; ++rx) {
				int dx = rx - radius;
				float sc = EvalDualNCC(img, base_x0 + dx, base_y0 + dy, t_curr, t_init, w_curr, w_init);
				out[row_off + rx] = PeakCand{dx, dy, sc};
			}
		}
	};

	if (cols * rows >= 625 && rows >= 8) {
		int hw = static_cast<int>(std::thread::hardware_concurrency());
		int n_threads = std::clamp(hw > 0 ? hw : 4, 2, 4);
		std::vector<std::thread> threads;
		threads.reserve(n_threads - 1);
		int chunk = (rows + n_threads - 1) / n_threads;
		for (int t = 1; t < n_threads; ++t) {
			int rb = t * chunk;
			int re = std::min(rows, rb + chunk);
			if (rb < re) {
				threads.emplace_back(eval_rows, rb, re);
			}
		}
		eval_rows(0, std::min(rows, chunk));
		for (auto& th : threads) {
			if (th.joinable()) th.join();
		}
	} else {
		eval_rows(0, rows);
	}
}

inline std::vector<PeakCand> ExtractTopPeaks(std::vector<PeakCand> sorted, int min_dist, size_t max_k) {
	std::sort(sorted.begin(), sorted.end(), [](PeakCand const& a, PeakCand const& b) {
		return a.score > b.score;
	});
	std::vector<PeakCand> picked;
	picked.reserve(max_k);
	for (auto const& c : sorted) {
		bool far_enough = true;
		for (auto const& p : picked) {
			if (std::abs(c.dx - p.dx) < min_dist && std::abs(c.dy - p.dy) < min_dist) {
				far_enough = false;
				break;
			}
		}
		if (far_enough) {
			picked.push_back(c);
			if (picked.size() >= max_k) break;
		}
	}
	return picked;
}

// --------------------------------------------------------------------------
// 2. 三层金字塔快速检索与亚像素抛物线拟合 (Sub-pixel Parabolic Refinement)
// --------------------------------------------------------------------------

inline TrackPoint MatchNextFramePyramid(
	const GrayPyramid& f_pyr,
	const TemplatePyramid& t_init,
	TemplatePyramid& t_curr,
	double prev_center_x, double prev_center_y,
	double vel_x, double vel_y,
	int search_radius,
	double alpha = 0.08)
{
	int box_w = t_init.l0.w;
	int box_h = t_init.l0.h;
	int center_x = static_cast<int>(std::round(prev_center_x + vel_x));
	int center_y = static_cast<int>(std::round(prev_center_y + vel_y));
	int pred_x0 = center_x - box_w / 2;
	int pred_y0 = center_y - box_h / 2;

	int best_dx = 0;
	int best_dy = 0;
	float best_score = -2.0f;

	if (t_init.has_l2 && f_pyr.has_l2) {
		int r2 = std::max(2, (search_radius + 3) / 4);
		int pred2_x0 = static_cast<int>(std::round(pred_x0 / 4.0));
		int pred2_y0 = static_cast<int>(std::round(pred_y0 / 4.0));

		std::vector<PeakCand> l2_grid;
		SearchLevelGrid(f_pyr.l2, t_curr.l2, t_init.l2, pred2_x0, pred2_y0, r2, 0.55f, 0.45f, l2_grid);
		std::vector<PeakCand> top3 = ExtractTopPeaks(std::move(l2_grid), 2, 3);

		int pred1_x0 = static_cast<int>(std::round(pred_x0 / 2.0));
		int pred1_y0 = static_cast<int>(std::round(pred_y0 / 2.0));
		int best_dx1 = 0, best_dy1 = 0;
		float best_s1 = -2.0f;

		for (auto const& pk : top3) {
			int c1x = pk.dx * 2;
			int c1y = pk.dy * 2;
			for (int dy1 = c1y - 3; dy1 <= c1y + 3; ++dy1) {
				for (int dx1 = c1x - 3; dx1 <= c1x + 3; ++dx1) {
					float sc = EvalDualNCC(f_pyr.l1, pred1_x0 + dx1, pred1_y0 + dy1, t_curr.l1, t_init.l1, 0.60f, 0.40f);
					if (sc > best_s1) {
						best_s1 = sc;
						best_dx1 = dx1;
						best_dy1 = dy1;
					}
				}
			}
		}

		int c0x = best_dx1 * 2;
		int c0y = best_dy1 * 2;
		for (int dy = c0y - 2; dy <= c0y + 2; ++dy) {
			for (int dx = c0x - 2; dx <= c0x + 2; ++dx) {
				float sc = EvalDualNCC(f_pyr.l0, pred_x0 + dx, pred_y0 + dy, t_curr.l0, t_init.l0, 0.65f, 0.35f);
				if (sc > best_score) {
					best_score = sc;
					best_dx = dx;
					best_dy = dy;
				}
			}
		}
	} else if (t_init.has_l1 && f_pyr.has_l1) {
		int r1 = std::max(2, (search_radius + 1) / 2);
		int pred1_x0 = static_cast<int>(std::round(pred_x0 / 2.0));
		int pred1_y0 = static_cast<int>(std::round(pred_y0 / 2.0));

		std::vector<PeakCand> l1_grid;
		SearchLevelGrid(f_pyr.l1, t_curr.l1, t_init.l1, pred1_x0, pred1_y0, r1, 0.55f, 0.45f, l1_grid);
		std::vector<PeakCand> top1 = ExtractTopPeaks(std::move(l1_grid), 1, 1);
		int c0x = top1.empty() ? 0 : top1[0].dx * 2;
		int c0y = top1.empty() ? 0 : top1[0].dy * 2;

		for (int dy = c0y - 2; dy <= c0y + 2; ++dy) {
			for (int dx = c0x - 2; dx <= c0x + 2; ++dx) {
				float sc = EvalDualNCC(f_pyr.l0, pred_x0 + dx, pred_y0 + dy, t_curr.l0, t_init.l0, 0.65f, 0.35f);
				if (sc > best_score) {
					best_score = sc;
					best_dx = dx;
					best_dy = dy;
				}
			}
		}
	} else {
		std::vector<PeakCand> l0_grid;
		SearchLevelGrid(f_pyr.l0, t_curr.l0, t_init.l0, pred_x0, pred_y0, search_radius, 0.60f, 0.40f, l0_grid);
		std::vector<PeakCand> top1 = ExtractTopPeaks(std::move(l0_grid), 1, 1);
		if (!top1.empty()) {
			best_dx = top1[0].dx;
			best_dy = top1[0].dy;
			best_score = top1[0].score;
		}
	}

	// 亚像素抛物线拟合 (精确到 0.05 像素)
	float c00 = best_score;
	float cx_minus = EvalDualNCC(f_pyr.l0, pred_x0 + best_dx - 1, pred_y0 + best_dy, t_curr.l0, t_init.l0, 0.65f, 0.35f);
	float cx_plus  = EvalDualNCC(f_pyr.l0, pred_x0 + best_dx + 1, pred_y0 + best_dy, t_curr.l0, t_init.l0, 0.65f, 0.35f);
	float cy_minus = EvalDualNCC(f_pyr.l0, pred_x0 + best_dx, pred_y0 + best_dy - 1, t_curr.l0, t_init.l0, 0.65f, 0.35f);
	float cy_plus  = EvalDualNCC(f_pyr.l0, pred_x0 + best_dx, pred_y0 + best_dy + 1, t_curr.l0, t_init.l0, 0.65f, 0.35f);

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

	// 动态模板 EMA 平滑更新 (增加质量门禁，防止把背景漂移学进模板)
	if (pt.valid && pt.score >= 0.45f && alpha > 0.0) {
		float fa = static_cast<float>(alpha);
		int fx0 = static_cast<int>(std::round(pt.x - box_w / 2.0));
		int fy0 = static_cast<int>(std::round(pt.y - box_h / 2.0));
		ZeroMeanTemplate cand0;
		cand0.ExtractFromGray(f_pyr.l0, fx0, fy0, box_w, box_h);
		t_curr.l0.BlendEMA(cand0, fa);

		if (t_curr.has_l1 && f_pyr.has_l1) {
			ZeroMeanTemplate cand1;
			cand1.ExtractFromGray(f_pyr.l1, fx0 / 2, fy0 / 2, t_curr.l1.w, t_curr.l1.h);
			t_curr.l1.BlendEMA(cand1, fa);
		}
		if (t_curr.has_l2 && f_pyr.has_l2) {
			ZeroMeanTemplate cand2;
			cand2.ExtractFromGray(f_pyr.l2, fx0 / 4, fy0 / 4, t_curr.l2.w, t_curr.l2.h);
			t_curr.l2.BlendEMA(cand2, fa);
		}
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
// 4. 双向并行全范围追踪引擎 (Bidirectional Parallel Tracking Pipeline)
// --------------------------------------------------------------------------

inline MotionTrajectory TrackFullRangeView(
	MotionVideoView const& view_fwd,
	MotionVideoView const& view_bwd,
	int ref_frame,
	int box_x0, int box_y0, int box_w, int box_h,
	int start_frame, int end_frame,
	const TrackingOptions& opt,
	std::function<void(int cur_frame, int total, double x, double y)> on_progress = nullptr,
	std::shared_ptr<std::atomic<bool>> cancel = nullptr)
{
	MotionTrajectory traj;
	if (!view_fwd.get_frame || view_fwd.frame_count <= 0 || view_fwd.native_width <= 0 || view_fwd.native_height <= 0)
		return traj;
	if (box_w < 8 || box_h < 8) return traj;

	int total_frames = view_fwd.frame_count;
	ref_frame = std::clamp(ref_frame, 0, total_frames - 1);
	start_frame = std::clamp(start_frame, 0, total_frames - 1);
	end_frame = std::clamp(end_frame, 0, total_frames - 1);
	if (start_frame > end_frame) std::swap(start_frame, end_frame);

	traj.ref_frame = ref_frame;
	traj.box_w = box_w;
	traj.box_h = box_h;
	traj.video_width = view_fwd.native_width;
	traj.video_height = view_fwd.native_height;
	traj.fps = view_fwd.fps;

	auto ref_f = view_fwd.get_frame(ref_frame);
	if (!ref_f || ref_f->data.empty() || ref_f->width <= 0 || ref_f->height <= 0)
		return traj;

	double sx = static_cast<double>(ref_f->width) / view_fwd.native_width;
	double sy = static_cast<double>(ref_f->height) / view_fwd.native_height;

	int f_box_w = std::max(8, static_cast<int>(std::round(box_w * sx)));
	int f_box_h = std::max(8, static_cast<int>(std::round(box_h * sy)));
	int f_box_x0 = static_cast<int>(std::round(box_x0 * sx));
	int f_box_y0 = static_cast<int>(std::round(box_y0 * sy));
	int f_search_r = std::max(4, static_cast<int>(std::round(opt.search_radius * std::max(sx, sy))));

	GrayImage ref_gray = BuildGrayImage(*ref_f);
	bool need_l1 = (f_box_w >= 24 && f_box_h >= 12);
	bool need_l2 = (need_l1 && (f_box_w / 2) >= 24 && (f_box_h / 2) >= 12);
	GrayPyramid ref_pyr = BuildGrayPyramid(std::move(ref_gray), need_l1, need_l2);
	TemplatePyramid t_init_pyr = BuildTemplatePyramid(ref_pyr, f_box_x0, f_box_y0, f_box_w, f_box_h);

	double f_ref_cx = f_box_x0 + f_box_w / 2.0;
	double f_ref_cy = f_box_y0 + f_box_h / 2.0;

	TrackPoint ref_pt;
	ref_pt.x = box_x0 + box_w / 2.0;
	ref_pt.y = box_y0 + box_h / 2.0;
	ref_pt.score = 1.0;
	ref_pt.valid = true;
	traj.ref_point = ref_pt;
	traj.frames[ref_frame] = ref_pt;

	int total_to_track = (end_frame - start_frame + 1);
	std::atomic<int> done_count{1};
	std::mutex prog_mtx;

	auto report_progress = [&](double nat_x, double nat_y) {
		int cur = done_count.fetch_add(1) + 1;
		if (on_progress) {
			std::lock_guard<std::mutex> lk(prog_mtx);
			on_progress(cur, total_to_track, nat_x, nat_y);
		}
	};

	auto track_forward = [&](std::map<int, TrackPoint>& out_map) {
		TemplatePyramid t_curr = t_init_pyr;
		double cur_x = f_ref_cx;
		double cur_y = f_ref_cy;
		double vel_x = 0.0, vel_y = 0.0;

		for (int f = ref_frame + 1; f <= end_frame; ++f) {
			if (cancel && cancel->load()) return;
			auto frame_data = view_fwd.get_frame(f);
			TrackPoint pt;
			if (frame_data && !frame_data->data.empty()) {
				GrayPyramid f_pyr = BuildGrayPyramid(BuildGrayImage(*frame_data), t_init_pyr.has_l1, t_init_pyr.has_l2);
				pt = MatchNextFramePyramid(f_pyr, t_init_pyr, t_curr,
					cur_x, cur_y, vel_x, vel_y, f_search_r, opt.template_update_alpha);
			} else {
				pt.x = cur_x;
				pt.y = cur_y;
				pt.score = 0.0;
				pt.valid = false;
			}
			vel_x = 0.6 * vel_x + 0.4 * (pt.x - cur_x);
			vel_y = 0.6 * vel_y + 0.4 * (pt.y - cur_y);
			cur_x = pt.x;
			cur_y = pt.y;

			TrackPoint nat_pt = pt;
			nat_pt.x /= sx;
			nat_pt.y /= sy;
			out_map[f] = nat_pt;
			report_progress(nat_pt.x, nat_pt.y);
		}
	};

	auto track_backward = [&](MotionVideoView const& bwd_view, std::map<int, TrackPoint>& out_map) {
		TemplatePyramid t_curr = t_init_pyr;
		double cur_x = f_ref_cx;
		double cur_y = f_ref_cy;
		double vel_x = 0.0, vel_y = 0.0;

		// 以 32 帧为块顺序正序预解码后再逆序追踪，避免 FFMS2 每帧后退反复寻址 GOP
		constexpr int kBatchSize = 32;
		for (int chunk_end = ref_frame - 1; chunk_end >= start_frame; ) {
			if (cancel && cancel->load()) return;
			int chunk_start = std::max(start_frame, chunk_end - kBatchSize + 1);
			std::vector<GrayImage> batch;
			batch.reserve(static_cast<size_t>(chunk_end - chunk_start + 1));
			for (int f = chunk_start; f <= chunk_end; ++f) {
				if (cancel && cancel->load()) return;
				auto frame_data = bwd_view.get_frame(f);
				batch.push_back(frame_data ? BuildGrayImage(*frame_data) : GrayImage{});
			}

			for (int f = chunk_end; f >= chunk_start; --f) {
				if (cancel && cancel->load()) return;
				GrayImage& g = batch[static_cast<size_t>(f - chunk_start)];
				TrackPoint pt;
				if (!g.data.empty()) {
					GrayPyramid f_pyr = BuildGrayPyramid(std::move(g), t_init_pyr.has_l1, t_init_pyr.has_l2);
					pt = MatchNextFramePyramid(f_pyr, t_init_pyr, t_curr,
						cur_x, cur_y, vel_x, vel_y, f_search_r, opt.template_update_alpha);
				} else {
					pt.x = cur_x;
					pt.y = cur_y;
					pt.score = 0.0;
					pt.valid = false;
				}
				vel_x = 0.6 * vel_x + 0.4 * (pt.x - cur_x);
				vel_y = 0.6 * vel_y + 0.4 * (pt.y - cur_y);
				cur_x = pt.x;
				cur_y = pt.y;

				TrackPoint nat_pt = pt;
				nat_pt.x /= sx;
				nat_pt.y /= sy;
				out_map[f] = nat_pt;
				report_progress(nat_pt.x, nat_pt.y);
			}
			chunk_end = chunk_start - 1;
		}
	};

	bool has_fwd = (ref_frame < end_frame);
	bool has_bwd = (ref_frame > start_frame);
	std::map<int, TrackPoint> fwd_map;
	std::map<int, TrackPoint> bwd_map;

	if (has_fwd && has_bwd && view_bwd.get_frame) {
		std::thread bwd_thread([&]() {
			track_backward(view_bwd, bwd_map);
		});
		track_forward(fwd_map);
		if (bwd_thread.joinable()) bwd_thread.join();
	} else {
		if (has_fwd) track_forward(fwd_map);
		if (has_bwd) track_backward(view_bwd.get_frame ? view_bwd : view_fwd, bwd_map);
	}

	if (cancel && cancel->load()) return traj;

	traj.frames.insert(fwd_map.begin(), fwd_map.end());
	traj.frames.insert(bwd_map.begin(), bwd_map.end());

	if (opt.smooth_window >= 3) {
		SmoothTrajectory(traj.frames, opt.smooth_window);
	}

	return traj;
}

inline MotionTrajectory TrackFullRange(
	agi::Context* c,
	int ref_frame,
	int box_x0, int box_y0, int box_w, int box_h,
	int start_frame, int end_frame,
	const TrackingOptions& opt,
	std::function<void(int cur_frame, int total, double x, double y)> on_progress = nullptr,
	std::shared_ptr<std::atomic<bool>> cancel = nullptr)
{
	if (!c || !c->project) return {};
	auto provider = c->project->VideoProvider();
	if (!provider) return {};

	agi::vfr::Framerate tc = c->project->Timecodes();
	MotionVideoView view_fwd;
	view_fwd.frame_count = provider->GetFrameCount();
	view_fwd.native_width = provider->GetWidth();
	view_fwd.native_height = provider->GetHeight();
	view_fwd.fps = provider->GetFPS().FPS();
	view_fwd.get_frame = [provider, tc](int f) {
		return provider->GetFrame(f, tc.TimeAtFrame(f), true);
	};
	return TrackFullRangeView(view_fwd, MotionVideoView{}, ref_frame, box_x0, box_y0, box_w, box_h,
		start_frame, end_frame, opt, std::move(on_progress), std::move(cancel));
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
	const TrackingOptions& opt,
	bool preserve_user_focus = false)
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

	if (!opt.backup_original && it != c->ass->Events.end()) {
		c->ass->Events.erase(it);
		delete target_line;
	}

	c->ass->Commit(to_wx("运动追踪生成逐帧相对位移字幕"), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);

	if (!preserve_user_focus) {
		if (leader_line) {
			c->selectionController->SetActiveLine(leader_line);
			c->selectionController->SetSelectedSet({leader_line});
		} else {
			c->selectionController->SetActiveLine(new_lines.front());
			c->selectionController->SetSelectedSet({new_lines.front()});
		}
	}
	return static_cast<int>(new_lines.size());
}

inline std::pair<std::mutex, MotionTrajectory>& LastTrajectoryStore() {
	static std::pair<std::mutex, MotionTrajectory> store;
	return store;
}

inline void SetLastTrajectory(MotionTrajectory const& traj) {
	auto& s = LastTrajectoryStore();
	std::lock_guard<std::mutex> lk(s.first);
	s.second = traj;
}

inline MotionTrajectory GetLastTrajectory() {
	auto& s = LastTrajectoryStore();
	std::lock_guard<std::mutex> lk(s.first);
	return s.second;
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
