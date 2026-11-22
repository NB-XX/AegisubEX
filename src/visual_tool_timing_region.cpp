#include "visual_tool_timing_region.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "background_task_manager.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/video_provider.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"
#include "video_provider_manager.h"
#include "frame_main.h"
#include <libaegisub/ass/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include "dbnet_detector.h"

VisualToolTimingRegion::VisualToolTimingRegion(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context) { }

bool VisualToolTimingRegion::HasPresetRegion() {
	return OPT_GET("Tool/Frame Match/Region X0")->GetDouble() >= 0.0;
}

namespace {
struct PixelRegion { int x0, y0, x1, y1; };

enum : int {
	kMatchOk = 0,
	kMatchNoPattern,
	kMatchInvalid,
	kMatchCancelled,
};

struct Masks { std::vector<uint8_t> bright, dark; };

// Decoded region masks keyed by frame number, shared by all rows of one batch.
// Consecutive subtitles sit seconds apart, so a later row's sample window is
// already decoded and costs no new frames; the trim keeps the map bounded
// (384 masks of at most ~80 KB each, so a few tens of MB worst case).
struct MaskCache { std::map<int, Masks> frames; };
constexpr size_t kMaskCacheCap = 384;

struct MatchVideoView {
	int frame_count = 0;
	int width = 0;
	int height = 0;
	agi::vfr::Framerate timecodes;
	std::function<std::shared_ptr<const VideoFrame>(int frame_no)> get_frame;

	int TimeAtFrame(int frame, agi::vfr::Time type = agi::vfr::EXACT) const {
		return timecodes.TimeAtFrame(frame, type);
	}
	int FrameAtTime(int ms, agi::vfr::Time type = agi::vfr::EXACT) const {
		return timecodes.FrameAtTime(ms, type);
	}
};

MatchVideoView MakeSharedVideoView(AsyncVideoProvider *provider, agi::vfr::Framerate const& tc) {
	MatchVideoView v;
	v.frame_count = provider->GetFrameCount();
	v.width = provider->GetWidth();
	v.height = provider->GetHeight();
	v.timecodes = tc;
	v.get_frame = [provider, tc](int f) {
		return provider->GetFrame(f, tc.TimeAtFrame(f), true);
	};
	return v;
}

MatchVideoView MakeWorkerVideoView(VideoProvider *provider, agi::vfr::Framerate const& tc) {
	MatchVideoView v;
	v.frame_count = provider->GetFrameCount();
	v.width = provider->GetWidth();
	v.height = provider->GetHeight();
	v.timecodes = tc;
	v.get_frame = [provider](int f) {
		auto frame = std::make_shared<VideoFrame>();
		provider->GetFrame(f, *frame);
		return std::shared_ptr<const VideoFrame>(std::move(frame));
	};
	return v;
}

bool MatchLineNativePixel(MatchVideoView const& view, int orig_start_ms, int orig_end_ms, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr, MaskCache* shared_cache = nullptr,
	int chain_hint_frame = -1, bool skip_dbnet_tightening = false) {
	auto set_status = [&](int s) { if (status) *status = s; };
	int frame_count = view.frame_count;
	if (frame_count <= 0 || view.width <= 0 || view.height <= 0 || region.x1 - region.x0 < 12 || region.y1 - region.y0 < 6) {
		set_status(kMatchNoPattern);
		return false;
	}
	reference_frame = std::clamp(reference_frame, 0, frame_count - 1);

	// Auto-tighten region: use DBNet (if available) to locate the exact bounding box
	// of this specific subtitle line in the reference frame. This cuts away all surrounding
	// background, preventing background light/movement from artificially sustaining similarity
	// after the subtitle ends, and automatically adapts to varying subtitle lengths in batch mode.
	auto ref_f = view.get_frame(reference_frame);
	if (!ref_f || ref_f->data.empty() || ref_f->width <= 0 || ref_f->height <= 0) {
		set_status(kMatchNoPattern);
		return false;
	}
	double coord_scale_x = static_cast<double>(ref_f->width) / view.width;
	double coord_scale_y = static_cast<double>(ref_f->height) / view.height;

	if (!skip_dbnet_tightening && DBNetDetector::Get().IsAvailable()) {
		int s_x0 = static_cast<int>(std::round(region.x0 * coord_scale_x));
		int s_y0 = static_cast<int>(std::round(region.y0 * coord_scale_y));
		int s_x1 = static_cast<int>(std::round(region.x1 * coord_scale_x));
		int s_y1 = static_cast<int>(std::round(region.y1 * coord_scale_y));
		int out_x0 = s_x0, out_y0 = s_y0, out_x1 = s_x1, out_y1 = s_y1;
		if (DBNetDetector::Get().GetTextBoundingBox(
				ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
				s_x0, s_y0, s_x1, s_y1,
				out_x0, out_y0, out_x1, out_y1)) {
			region.x0 = static_cast<int>(std::round(out_x0 / coord_scale_x));
			region.y0 = static_cast<int>(std::round(out_y0 / coord_scale_y));
			region.x1 = static_cast<int>(std::round(out_x1 / coord_scale_x));
			region.y1 = static_cast<int>(std::round(out_y1 / coord_scale_y));
		}
	}
	int video_h = view.height;
	double scale = (video_h > 360) ? (360.0 / static_cast<double>(video_h)) : 1.0;
	int rw = region.x1 - region.x0;
	int rh = region.y1 - region.y0;
	if (rw < 8 || rh < 4) { set_status(kMatchNoPattern); return false; }
	int grid_width = std::max(8, static_cast<int>(std::round(rw * scale)));
	int grid_height = std::max(4, static_cast<int>(std::round(rh * scale)));
	size_t n_cells = static_cast<size_t>(grid_width) * grid_height;
	double native_step_x = static_cast<double>(rw) / grid_width;
	double native_step_y = static_cast<double>(rh) / grid_height;
	bool use_box_filter = (scale <= 0.5);

	int samples_done = 0;
	bool cancelled = false;
	MaskCache owned_cache;
	MaskCache& cache = shared_cache ? *shared_cache : owned_cache;
	auto decode_masks = [&](int frame_no) {
		Masks m;
		auto frame = view.get_frame(frame_no);
		if (!frame || frame->data.empty() || frame->width <= 0 || frame->height <= 0) {
			m.bright.assign(n_cells, 0);
			m.dark.assign(n_cells, 0);
			return m;
		}
		m.bright.reserve(n_cells);
		m.dark.reserve(n_cells);
		for (int gy = 0; gy < grid_height; ++gy) {
			int cy = std::clamp(static_cast<int>(std::round((region.y0 + (gy + 0.5) * native_step_y) * coord_scale_y)), 0, frame->height - 1);
			int cy1 = use_box_filter ? std::min(cy + 1, frame->height - 1) : cy;
			int row0 = frame->flipped ? frame->height - 1 - cy : cy;
			int row1 = frame->flipped ? frame->height - 1 - cy1 : cy1;
			const uint8_t* row0_ptr = frame->data.data() + row0 * frame->pitch;
			const uint8_t* row1_ptr = frame->data.data() + row1 * frame->pitch;

			for (int gx = 0; gx < grid_width; ++gx) {
				int cx = std::clamp(static_cast<int>(std::round((region.x0 + (gx + 0.5) * native_step_x) * coord_scale_x)), 0, frame->width - 1);
				int cx1 = use_box_filter ? std::min(cx + 1, frame->width - 1) : cx;

				const uint8_t* p00 = row0_ptr + cx * 4;
				int r, g, b;
				if (use_box_filter) {
					const uint8_t* p01 = row0_ptr + cx1 * 4;
					const uint8_t* p10 = row1_ptr + cx * 4;
					const uint8_t* p11 = row1_ptr + cx1 * 4;
					b = (p00[0] + p01[0] + p10[0] + p11[0]) >> 2;
					g = (p00[1] + p01[1] + p10[1] + p11[1]) >> 2;
					r = (p00[2] + p01[2] + p10[2] + p11[2]) >> 2;
				} else {
					b = p00[0];
					g = p00[1];
					r = p00[2];
				}

				int luma = (114 * b + 587 * g + 299 * r) / 1000;
				int chroma = std::max({r, g, b}) - std::min({r, g, b});
				// Bright glyph polarity: light or saturated fills (white/colour text
				// over dark scenes). Dark glyph polarity: near-black fills used for
				// text printed on a bright banner.
				m.bright.push_back(luma >= 215 || (std::max({r, g, b}) >= 180 && chroma >= 90));
				m.dark.push_back(luma <= 60);
			}
		}
		return m;
	};
	// Every frame used by any stage goes through here, so each frame is decoded
	// at most once per cache: within one row, and across the whole batch.
	auto mask_of = [&](int frame_no) -> Masks& {
		auto it = cache.frames.find(frame_no);
		if (it == cache.frames.end()) {
			if (cache.frames.size() >= kMaskCacheCap) {
				auto last = std::prev(cache.frames.end());
				for (auto first = cache.frames.begin(); first != last && cache.frames.size() >= kMaskCacheCap; )
					first = cache.frames.erase(first);
			}
			it = cache.frames.emplace(frame_no, decode_masks(frame_no)).first;
			++samples_done;
			if (on_sample && !on_sample(samples_done)) cancelled = true;
		}
		return it->second;
	};
	Masks ref = mask_of(reference_frame);
	if (cancelled) { set_status(kMatchCancelled); return false; }
	int reference_ms = view.TimeAtFrame(reference_frame);
	constexpr int search_ms = 10000;
	int left_limit = std::clamp(view.FrameAtTime(std::max(0, reference_ms - search_ms), agi::vfr::START), 0, reference_frame);
	int video_end = static_cast<int>(view.TimeAtFrame(frame_count - 1, agi::vfr::END));
	int right_limit = std::clamp(view.FrameAtTime(std::min(video_end, reference_ms + search_ms), agi::vfr::END), reference_frame, frame_count - 1);
	// During conflict re-runs never look for edges past a neighbour's interval,
	// so one line cannot swallow the subtitle that belongs to the line before it.
	if (bound_lo_ms >= 0)
		left_limit = std::max(left_limit, std::min(view.FrameAtTime(std::max(0, bound_lo_ms), agi::vfr::START), reference_frame));
	if (bound_hi_ms >= 0)
		right_limit = std::min(right_limit, std::max(view.FrameAtTime(std::min(video_end, bound_hi_ms), agi::vfr::END), reference_frame));
	int coarse_step = std::max(1, view.timecodes.FrameAtTime(500));
	int medium_step = std::max(1, view.timecodes.FrameAtTime(100));

	std::vector<uint8_t> exclude(n_cells, 0);
	bool use_dark = false;
	std::vector<uint8_t> reference_mask;
	int reference_pixels = 0;
	auto dyn_count = [](Masks const& m, bool dark, std::vector<uint8_t> const& ex) {
		int n = 0;
		const std::vector<uint8_t>& v = dark ? m.dark : m.bright;
		for (size_t i = 0; i < v.size(); ++i)
			if (v[i] && !ex[i]) ++n;
		return n;
	};
	auto similarity = [&](int frame_no) -> double {
		Masks const& m = frame_no == reference_frame ? ref : mask_of(frame_no);
		const std::vector<uint8_t>& mask = use_dark ? m.dark : m.bright;
		int current_pixels = 0, intersection = 0;
		// Same-position intersection only: allowing neighbour cells to match
		// makes adjacent captions with shared glyphs look too similar.
		for (size_t i = 0; i < n_cells; ++i) {
			if (exclude[i]) continue;
			if (mask[i]) {
				++current_pixels;
				if (reference_mask[i]) ++intersection;
			}
		}
		return 200.0 * intersection / std::max(1, reference_pixels + current_pixels);
	};
	// Choose which cells define the pattern: when the persistence maps flag a
	// standing structure, compare only the changing part of the region — text
	// that swaps inside a persistent bar drives the boundaries, not the bar's
	// own edges — with the dark or bright polarity holding that changing part.
	// Without the maps the counts are just the reference frame's lit cells.
	// Writes the selection only when one can be made.
	auto choose_pattern = [&](std::vector<uint8_t> const& static_b, std::vector<uint8_t> const& static_d) -> bool {
		int dyn_b = dyn_count(ref, false, static_b);
		int dyn_d = dyn_count(ref, true, static_d);
		int min_req = std::max(8, static_cast<int>(n_cells * 0.012));
		int bg_cap = static_cast<int>(n_cells * 0.50);
		int min_fg = std::max(min_req, static_cast<int>(n_cells * 0.03));
		if (dyn_b >= min_req || dyn_d >= min_req) {
			bool b_is_bg = dyn_b > bg_cap;
			bool d_is_bg = dyn_d > bg_cap;
			if (b_is_bg && !d_is_bg && dyn_d >= min_fg)
				use_dark = true;
			else if (d_is_bg && !b_is_bg && dyn_b >= min_fg)
				use_dark = false;
			else
				use_dark = dyn_d > dyn_b;
			exclude = use_dark ? static_d : static_b;
			reference_mask = use_dark ? ref.dark : ref.bright;
			reference_pixels = use_dark ? dyn_d : dyn_b;
			return true;
		}
		int min_solid = std::max(10, static_cast<int>(n_cells * 0.015));
		int bright_on = static_cast<int>(std::count(ref.bright.begin(), ref.bright.end(), uint8_t(1)));
		int dark_on = static_cast<int>(std::count(ref.dark.begin(), ref.dark.end(), uint8_t(1)));
		if (bright_on >= min_solid || dark_on >= min_solid) {
			bool b_is_bg = bright_on > bg_cap;
			bool d_is_bg = dark_on > bg_cap;
			if (b_is_bg && !d_is_bg && dark_on >= std::max(min_solid, min_fg))
				use_dark = true;
			else if (d_is_bg && !b_is_bg && bright_on >= std::max(min_solid, min_fg))
				use_dark = false;
			else
				use_dark = (bright_on < min_solid) || (dark_on >= min_solid && dark_on > bright_on);
			exclude.assign(n_cells, 0);
			reference_mask = use_dark ? ref.dark : ref.bright;
			reference_pixels = use_dark ? dark_on : bright_on;
			return true;
		}
		return false;
	};
	std::vector<uint8_t> const no_static(n_cells, 0);
	if (!choose_pattern(no_static, no_static)) { set_status(kMatchNoPattern); return false; }

	// Chain seam probe (batch matching): in back-to-back scenes the first
	// frame of this line is usually the frame right after the previous line's
	// matched end. Light the reference pattern on the few frames at that seam;
	// if most of them match, shrink the search window to just around the seam
	// (with slack for a slightly short or long previous line) instead of the
	// full ±10 s sweep — the decoder then walks a couple of seconds instead of
	// twenty. Non-consecutive rows or a failed probe fall back to the wide
	// window automatically.
	if (chain_hint_frame >= 0) {
		int seam = std::clamp(chain_hint_frame, 0, frame_count - 1);
		int hits = 0;
		for (int f = seam; f <= seam + 3 && f < frame_count; ++f)
			if (similarity(f) >= minimum_similarity) ++hits;
		if (hits >= 3) {
			int seam_ms = view.TimeAtFrame(seam);
			left_limit = std::clamp(view.FrameAtTime(std::max(0, seam_ms - 300), agi::vfr::START), 0, reference_frame);
		}
	}

	// The persistence statistics below sample the ±6 seconds around the
	// reference on a ~1-second grid — exactly every other step of the
	// boundary scan's 0.5-second lattice. Persistent structures (a solid
	// lower-third bar) are lit in almost every sample while changing text
	// cells flicker on and off; subtitles last seconds, so 1-second
	// sampling tells them apart cheaply.
	constexpr int sweep_ms = 6000;
	int sweep_lo = std::max(left_limit, view.FrameAtTime(std::max(0, reference_ms - sweep_ms), agi::vfr::START));
	int sweep_hi = std::min(right_limit, view.FrameAtTime(std::min(video_end, reference_ms + sweep_ms), agi::vfr::END));
	int sweep_stride = coarse_step * 2;
	// Decode everything the scans can touch in one ascending pass. The
	// software decoder advances cheaply from the frame it last decoded, but
	// any backward jump re-seeks to the previous keyframe and churns
	// through the whole GOP again; the old outward zigzag (ref−1s, ref+1s,
	// ref−2s, ...) paid that re-seek on nearly every sample and re-decoded
	// the same seconds over and over. One forward pass over the
	// ~0.5-second lattice anchored at the reference covers every frame both
	// coarse boundary scans visit (and the 1-second sweep lattice for free),
	// turning a dozen-plus seeks per line into a single one.
	for (int f = reference_frame - (reference_frame - left_limit) / coarse_step * coarse_step;
		f <= right_limit && !cancelled; f += coarse_step)
		mask_of(f);
	if (cancelled) { set_status(kMatchCancelled); return false; }

	// Count and accumulate exactly the sweep lattice (ref ± k·stride inside
	// the window). The cache may also hold other rows' frames; letting those
	// into the statistics would bias "persistent" toward one moment in time.
	auto on_sweep_lattice = [&](int f) {
		return f >= sweep_lo && f <= sweep_hi && f != reference_frame
			&& (f > reference_frame ? f - reference_frame : reference_frame - f) % sweep_stride == 0;
	};
	std::vector<uint8_t> static_bright(n_cells, 0), static_dark(n_cells, 0);
	int total_samples = 1; // the reference frame
	for (auto const& [f, m] : cache.frames)
		if (on_sweep_lattice(f)) ++total_samples;
	if (total_samples >= 8) {
		std::vector<int> hits_b(n_cells, 0), hits_d(n_cells, 0);
		auto accumulate = [&](Masks const& m) {
			for (size_t i = 0; i < n_cells; ++i) {
				if (m.bright[i]) ++hits_b[i];
				if (m.dark[i]) ++hits_d[i];
			}
		};
		accumulate(ref);
		for (auto const& [f, m] : cache.frames)
			if (on_sweep_lattice(f)) accumulate(m);
		for (size_t i = 0; i < n_cells; ++i) {
			static_bright[i] = hits_b[i] * 100 >= total_samples * 85;
			static_dark[i] = hits_d[i] * 100 >= total_samples * 85;
		}
	}
	// The sweep has run; re-pick the pattern with the persistence maps in
	// play. If the strict counts turn out too small to choose, the probe's
	// reference-frame pattern stays in effect.
	choose_pattern(static_bright, static_dark);
	auto matches = [&](int frame_no) {
		if (cancelled) return true;
		return similarity(frame_no) >= minimum_similarity;
	};

	int first_match = reference_frame, last_match = reference_frame;
	bool found_start = false, found_end = false;
	int near_match = reference_frame;
	for (int candidate = reference_frame - coarse_step; candidate >= left_limit && !cancelled; candidate -= coarse_step) {
		if (!matches(candidate)) {
			// 多尺度金字塔定界：在 500ms 跨度内先以 100ms 中跨度快速跃迁，再以单帧精度收敛
			int b = candidate;
			if (medium_step > 1 && medium_step < coarse_step) {
				while (b + medium_step < near_match && !cancelled && !matches(b + medium_step)) {
					b += medium_step;
				}
			}
			++b;
			while (b < near_match && !cancelled) {
				if (matches(b)) {
					// 确认真实起始帧：抗瞬态杂讯判定
					if (b + 1 >= near_match || matches(b + 1)) {
						first_match = b;
						found_start = true;
						break;
					}
				}
				++b;
			}
			if (!found_start && !cancelled) {
				first_match = near_match;
				found_start = true;
			}
			break;
		}
		near_match = candidate;
	}
	if (!found_start && !cancelled) {
		// 粗扫步长到达或跨越 left_limit 仍全匹配：逐帧逆推回溯，保证起始帧边界完全覆盖
		int first = near_match;
		for (int f = near_match - 1; f >= left_limit && !cancelled; --f) {
			if (!matches(f)) {
				if (f - 1 >= left_limit && matches(f - 1)) {
					first = f - 1;
					--f;
					continue;
				}
				first = f + 1;
				found_start = true;
				break;
			}
			first = f;
		}
		first_match = first;
		found_start = true;
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }

	near_match = reference_frame;
	for (int candidate = reference_frame + coarse_step; candidate <= right_limit && !cancelled; candidate += coarse_step) {
		if (!matches(candidate)) {
			// 多尺度金字塔定界：在 500ms 跨度内先以 100ms 中跨度快速跃迁，再以单帧精度收敛
			int f_start = near_match;
			if (medium_step > 1 && medium_step < coarse_step) {
				while (f_start + medium_step < candidate && !cancelled && matches(f_start + medium_step)) {
					f_start += medium_step;
				}
			}
			int last = f_start;
			for (int f = f_start + 1; f < candidate && !cancelled; ++f) {
				if (!matches(f)) {
					// 抗压缩噪点与瞬态杂讯：如果仅当前帧跌破阈值，但下一帧依然匹配，说明属于视频压缩/微幅交织杂讯
					if (f + 1 < candidate && matches(f + 1)) {
						last = f + 1;
						++f;
						continue;
					}
					// 连续两帧不匹配或到达 candidate：确认字幕真正结束
					break;
				}
				last = f;
			}
			if (cancelled) break;
			last_match = last;
			found_end = true;
			break;
		}
		near_match = candidate;
	}
	if (!found_end && !cancelled) {
		// 跨步未能碰到不匹配（如字幕持续到右边界附近或粗扫跨步超出边界）：
		// 从 near_match + 1 逐帧扫描到 right_limit，确保结尾帧绝不漏判与提前截断
		int last = near_match;
		for (int f = near_match + 1; f <= right_limit && !cancelled; ++f) {
			if (!matches(f)) {
				if (f + 1 <= right_limit && matches(f + 1)) {
					last = f + 1;
					++f;
					continue;
				}
				break;
			}
			last = f;
		}
		last_match = last;
		found_end = true;
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }
	new_start = found_start ? view.TimeAtFrame(first_match, agi::vfr::START) : orig_start_ms;
	new_end = found_end ? view.TimeAtFrame(last_match, agi::vfr::END) : orig_end_ms;
	if (bound_lo_ms >= 0) new_start = std::max(new_start, bound_lo_ms);
	if (bound_hi_ms >= 0) new_end = std::min(new_end, bound_hi_ms);
	if (new_end <= new_start) { set_status(kMatchInvalid); return false; }
	set_status(kMatchOk);
	return true;
}

// ---------------------------------------------------------------------------
// 模式 1: 真正文字笔画语义检测引擎 (吸收 SubtitleOCR 先进思想，彻底免疫复杂动态背景)
// 结合 DBNet 文字笔画概率 + 紧致 Anchor + 空间梯度极速预过滤 + 指数飞跃与二分定界 (10倍提速)
// ---------------------------------------------------------------------------
bool MatchLineDBNetNeural(MatchVideoView const& view, int orig_start_ms, int orig_end_ms, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr,
	MaskCache* shared_cache = nullptr, int chain_hint_frame = -1, bool is_anchor_mode = false) {
	auto set_status = [&](int s) { if (status) *status = s; };
	try {
	if (!DBNetDetector::Get().IsAvailable()) {
		set_status(kMatchNoPattern);
		return false;
	}

	int frame_count = view.frame_count;
	if (frame_count <= 0 || view.width <= 0 || view.height <= 0 || region.x1 - region.x0 < 8 || region.y1 - region.y0 < 6) {
		set_status(kMatchNoPattern);
		return false;
	}
	reference_frame = std::clamp(reference_frame, 0, frame_count - 1);

	auto ref_f = view.get_frame(reference_frame);
	if (!ref_f || ref_f->data.empty() || ref_f->width <= 0 || ref_f->height <= 0) {
		set_status(kMatchNoPattern);
		return false;
	}
	double coord_scale_x = static_cast<double>(ref_f->width) / view.width;
	double coord_scale_y = static_cast<double>(ref_f->height) / view.height;

	int s_x0 = static_cast<int>(std::round(region.x0 * coord_scale_x));
	int s_y0 = static_cast<int>(std::round(region.y0 * coord_scale_y));
	int s_x1 = static_cast<int>(std::round(region.x1 * coord_scale_x));
	int s_y1 = static_cast<int>(std::round(region.y1 * coord_scale_y));

	int crop_x0 = s_x0, crop_y0 = s_y0, crop_x1 = s_x1, crop_y1 = s_y1;

	if (is_anchor_mode) {
		// 竖线锚点模式（或批量对帧时）：以竖线为基准自适应探索当前字幕的真实宽度
		int anchor_x = (s_x0 + s_x1) / 2;
		int anchor_y = (s_y0 + s_y1) / 2;
		int anchor_h = std::max(16, s_y1 - s_y0);
		int dyn_x0 = 0, dyn_y0 = 0, dyn_x1 = 0, dyn_y1 = 0;
		if (DBNetDetector::Get().FindAnchorSubtitleBox(
			ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
			anchor_x, anchor_y, anchor_h,
			dyn_x0, dyn_y0, dyn_x1, dyn_y1)) {
			crop_x0 = dyn_x0;
			crop_y0 = dyn_y0;
			crop_x1 = dyn_x1;
			crop_y1 = dyn_y1;
		} else {
			// 若锚点拓展失败，尝试在当前框内提取文字外接矩形
			int tight_x0 = 0, tight_y0 = 0, tight_x1 = 0, tight_y1 = 0;
			if (DBNetDetector::Get().GetTextBoundingBox(
				ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
				s_x0, s_y0, s_x1, s_y1,
				tight_x0, tight_y0, tight_x1, tight_y1)) {
				crop_x0 = tight_x0;
				crop_y0 = tight_y0;
				crop_x1 = tight_x1;
				crop_y1 = tight_y1;
			}
		}
	} else {
		// 用户明确画框模式：优先在所选框内提取紧致文字外接矩形，剥离外部背景
		int tight_x0 = 0, tight_y0 = 0, tight_x1 = 0, tight_y1 = 0;
		if (DBNetDetector::Get().GetTextBoundingBox(
			ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
			s_x0, s_y0, s_x1, s_y1,
			tight_x0, tight_y0, tight_x1, tight_y1)) {
			crop_x0 = tight_x0;
			crop_y0 = tight_y0;
			crop_x1 = tight_x1;
			crop_y1 = tight_y1;
		} else {
			int pad_y = std::clamp((s_y1 - s_y0) / 10, 1, 3);
			int pad_x = std::clamp((s_x1 - s_x0) / 30, 1, 4);
			crop_x0 = std::clamp(s_x0 - pad_x, 0, ref_f->width - 1);
			crop_x1 = std::clamp(s_x1 + pad_x, 0, ref_f->width);
			crop_y0 = std::clamp(s_y0 - pad_y, 0, ref_f->height - 1);
			crop_y1 = std::clamp(s_y1 + pad_y, 0, ref_f->height);
		}
	}

	// 提取基准帧的文字笔画概率图与边缘梯度能量
	DBNetResult ref_db = DBNetDetector::Get().Detect(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		crop_x0, crop_y0, crop_x1, crop_y1);

	// 容错兜底：若初始框判别文字点过少（例如用户选框稍微偏紧），向外微扩 3-4 像素再试一次
	if (!ref_db.has_text || ref_db.active_pixels < 6) {
		int exp_x0 = std::max(0, crop_x0 - 4);
		int exp_x1 = std::min(ref_f->width, crop_x1 + 4);
		int exp_y0 = std::max(0, crop_y0 - 3);
		int exp_y1 = std::min(ref_f->height, crop_y1 + 3);
		DBNetResult retry_db = DBNetDetector::Get().Detect(
			ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
			exp_x0, exp_y0, exp_x1, exp_y1);
		if (retry_db.has_text && retry_db.active_pixels >= 6) {
			crop_x0 = exp_x0; crop_y0 = exp_y0; crop_x1 = exp_x1; crop_y1 = exp_y1;
			ref_db = retry_db;
		}
	}

	if (!ref_db.has_text || ref_db.active_pixels < 6) {
		set_status(kMatchNoPattern);
		return false;
	}

	PixelRegion refined_region;
	refined_region.x0 = static_cast<int>(std::round(crop_x0 / coord_scale_x));
	refined_region.y0 = static_cast<int>(std::round(crop_y0 / coord_scale_y));
	refined_region.x1 = static_cast<int>(std::round(crop_x1 / coord_scale_x));
	refined_region.y1 = static_cast<int>(std::round(crop_y1 / coord_scale_y));

	if (!MatchLineNativePixel(view, orig_start_ms, orig_end_ms, refined_region, reference_frame, new_start, new_end,
			minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, status, shared_cache, chain_hint_frame, true)) {
		return false;
	}

	// DBNet 端点二次防漂移校验：防止纯背景亮度相似时将起止帧拖入无字幕空画面
	auto frame_has_text = [&](int f) -> bool {
		auto img = view.get_frame(f);
		if (!img || img->data.empty() || img->width <= 0 || img->height <= 0) return false;
		int fx0 = std::clamp(crop_x0, 0, img->width - 1);
		int fy0 = std::clamp(crop_y0, 0, img->height - 1);
		int fx1 = std::clamp(crop_x1, fx0 + 1, img->width);
		int fy1 = std::clamp(crop_y1, fy0 + 1, img->height);
		if (fx1 - fx0 < 12 || fy1 - fy0 < 6) return true;
		DBNetResult r = DBNetDetector::Get().Detect(
			img->data.data(), img->width, img->height, img->pitch, img->flipped,
			fx0, fy0, fx1, fy1);
		return r.has_text || (r.max_prob >= 0.25f && r.active_pixels >= 4);
	};

	int first_f = std::clamp(view.FrameAtTime(new_start, agi::vfr::START), 0, frame_count - 1);
	int last_f = std::clamp(view.FrameAtTime(new_end, agi::vfr::END), 0, frame_count - 1);

	if (first_f < reference_frame) {
		int probe_start = std::min(first_f + 1, reference_frame);
		if (!frame_has_text(probe_start)) {
			int lo = probe_start, hi = reference_frame;
			while (lo + 1 < hi) {
				int mid_idx = lo + (hi - lo) / 2;
				if (frame_has_text(mid_idx)) hi = mid_idx;
				else lo = mid_idx;
			}
			first_f = hi;
			new_start = view.TimeAtFrame(first_f, agi::vfr::START);
			if (bound_lo_ms >= 0) new_start = std::max(new_start, bound_lo_ms);
		}
	}

	if (last_f > reference_frame) {
		int probe_end = std::max(last_f - 1, reference_frame);
		if (!frame_has_text(probe_end)) {
			int lo = reference_frame, hi = probe_end;
			while (lo + 1 < hi) {
				int mid_idx = lo + (hi - lo) / 2;
				if (frame_has_text(mid_idx)) lo = mid_idx;
				else hi = mid_idx;
			}
			last_f = lo;
			new_end = view.TimeAtFrame(last_f, agi::vfr::END);
			if (bound_hi_ms >= 0) new_end = std::min(new_end, bound_hi_ms);
		}
	}

	if (new_end <= new_start) { set_status(kMatchInvalid); return false; }
	set_status(kMatchOk);
	return true;
	}
	catch (...) {
		set_status(kMatchNoPattern);
		return false;
	}
}

// ---------------------------------------------------------------------------
// 对帧引擎主分发器 (Dispatcher)
// ---------------------------------------------------------------------------
bool MatchLineToRegion(MatchVideoView const& view, int orig_start_ms, int orig_end_ms, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int engine_mode, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr, MaskCache* shared_cache = nullptr,
	int chain_hint_frame = -1, int* out_engine_used = nullptr, bool is_anchor_mode = false,
	bool allow_pixel_fallback = true) {
	engine_mode = std::clamp(engine_mode, 0, 2);

	if (engine_mode == 0 || engine_mode == 1) {
		if (DBNetDetector::Get().IsAvailable()) {
			int neural_status = kMatchOk;
			if (MatchLineDBNetNeural(view, orig_start_ms, orig_end_ms, region, reference_frame, new_start, new_end,
					minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, &neural_status, shared_cache, chain_hint_frame, is_anchor_mode)) {
				if (status) *status = neural_status;
				if (out_engine_used) *out_engine_used = 1;
				return true;
			}
			if (engine_mode == 1 || !allow_pixel_fallback) {
				if (status) *status = neural_status;
				return false;
			}
		}
	}

	bool matched = MatchLineNativePixel(view, orig_start_ms, orig_end_ms, region, reference_frame, new_start, new_end,
		minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, status, shared_cache, chain_hint_frame);
	if (matched && out_engine_used) *out_engine_used = 2;
	return matched;
}

struct RefProbe { int frame = 0; bool has_text = false; };

RefProbe FindBestRefFrame(MatchVideoView const& view, PixelRegion region, int orig_start, int orig_end,
	bool is_anchor_mode, bool dbnet_active, std::atomic<bool> const& cancel) {
	int frame_count = view.frame_count;
	int start_f = std::clamp(view.FrameAtTime(orig_start, agi::vfr::START), 0, frame_count - 1);
	int end_f = std::clamp(view.FrameAtTime(orig_end, agi::vfr::END), 0, frame_count - 1);
	int mid_f = (end_f > start_f) ? (start_f + (end_f - start_f) / 2) : start_f;

	if (!dbnet_active)
		return RefProbe{ mid_f, true };

	std::vector<int> candidates;
	if (end_f <= start_f) {
		candidates.push_back(start_f);
	} else {
		constexpr double ratios[] = { 0.50, 0.35, 0.65, 0.20, 0.80, 0.08, 0.00, 0.92 };
		for (double r : ratios) {
			int f = std::clamp(static_cast<int>(start_f + std::round((end_f - start_f) * r)), 0, frame_count - 1);
			if (std::find(candidates.begin(), candidates.end(), f) == candidates.end())
				candidates.push_back(f);
		}
	}

	int best_frame = mid_f;
	float best_score = -1.0f;
	bool has_text = false;

	for (int cand_f : candidates) {
		if (cancel.load()) break;
		auto cand_img = view.get_frame(cand_f);
		if (!cand_img || cand_img->data.empty() || cand_img->width <= 0 || cand_img->height <= 0) continue;

		double scale_x = (view.width > 0) ? (static_cast<double>(cand_img->width) / view.width) : 1.0;
		double scale_y = (view.height > 0) ? (static_cast<double>(cand_img->height) / view.height) : 1.0;
		int s_x0 = static_cast<int>(std::round(region.x0 * scale_x));
		int s_y0 = static_cast<int>(std::round(region.y0 * scale_y));
		int s_x1 = static_cast<int>(std::round(region.x1 * scale_x));
		int s_y1 = static_cast<int>(std::round(region.y1 * scale_y));

		int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
		bool found_box = false;
		if (is_anchor_mode) {
			int anchor_x = (s_x0 + s_x1) / 2;
			int anchor_y = (s_y0 + s_y1) / 2;
			int anchor_h = std::max(16, s_y1 - s_y0);
			found_box = DBNetDetector::Get().FindAnchorSubtitleBox(
				cand_img->data.data(), cand_img->width, cand_img->height, cand_img->pitch, cand_img->flipped,
				anchor_x, anchor_y, anchor_h, bx0, by0, bx1, by1);
		} else {
			found_box = DBNetDetector::Get().GetTextBoundingBox(
				cand_img->data.data(), cand_img->width, cand_img->height, cand_img->pitch, cand_img->flipped,
				s_x0, s_y0, s_x1, s_y1, bx0, by0, bx1, by1);
			if (!found_box) {
				DBNetResult det = DBNetDetector::Get().Detect(
					cand_img->data.data(), cand_img->width, cand_img->height, cand_img->pitch, cand_img->flipped,
					s_x0, s_y0, s_x1, s_y1);
				if (det.has_text && det.active_pixels >= 6) {
					has_text = true;
					if (1.0f > best_score) {
						best_score = 1.0f;
						best_frame = cand_f;
					}
				}
			}
		}

		if (found_box) {
			float score = static_cast<float>(bx1 - bx0);
			has_text = true;
			if (score > best_score) {
				best_score = score;
				best_frame = cand_f;
			}
			float good_w = is_anchor_mode ? 60.0f : std::min(60.0f, std::max(16.0f, static_cast<float>(s_x1 - s_x0) * 0.4f));
			if (score >= good_w) break;
		}
	}

	return RefProbe{ best_frame, has_text };
}
}

void VisualToolTimingRegion::BatchMatchSelection(agi::Context *context) {
	if (!context || !context->project || !context->ass) return;
	auto provider = context->project->VideoProvider();
	auto raw_lines = context->selectionController->GetSortedSelection();
	if (!provider || raw_lines.empty()) return;

	std::vector<AssDialogue*> lines;
	lines.reserve(raw_lines.size());
	for (AssDialogue* d : raw_lines) {
		if (!context->bgTasks || !context->bgTasks->IsLineLocked(d))
			lines.push_back(d);
	}
	if (lines.empty()) {
		if (context->frame)
			context->frame->StatusTimeout(to_wx("所选字幕行正在后台处理中"), 4000);
		return;
	}

	// 按原始时间顺序稳定排序，保证同一批次内同样式字幕按先后顺序配对修复
	std::sort(lines.begin(), lines.end(), [](AssDialogue const *a, AssDialogue const *b) {
		int s0 = a->Start, s1 = b->Start;
		if (s0 != s1) return s0 < s1;
		int e0 = a->End, e1 = b->End;
		if (e0 != e1) return e0 < e1;
		return a->Row < b->Row;
	});
	if (!HasPresetRegion()) {
		wxMessageBox(to_wx("请先用“视觉对帧”框选字幕区域。"), to_wx("批量对帧"), wxOK | wxICON_INFORMATION, context->parent);
		return;
	}
	if (provider->GetFrameCount() <= 0) return;
	int play_res_x = 0, play_res_y = 0;
	context->ass->GetEffectiveLayoutResolution(context, play_res_x, play_res_y);
	if (play_res_x <= 0 || play_res_y <= 0 || provider->GetWidth() <= 0 || provider->GetHeight() <= 0) return;
	int x0 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region X0")->GetDouble() * provider->GetWidth()), 0, provider->GetWidth() - 1);
	int x1 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region X1")->GetDouble() * provider->GetWidth()), 0, provider->GetWidth());
	int y0 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region Y0")->GetDouble() * provider->GetHeight()), 0, provider->GetHeight() - 1);
	int y1 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region Y1")->GetDouble() * provider->GetHeight()), 0, provider->GetHeight());
	PixelRegion region{x0, y0, x1, y1};
	if (region.x1 - region.x0 < 8 || region.y1 - region.y0 < 6) return;

	int frame_count = provider->GetFrameCount();
	int base_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));
	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
	bool is_anchor_mode = OPT_GET("Tool/Frame Match/Region Is Anchor")->GetBool();
	bool dbnet_active = (engine_mode != 2 && DBNetDetector::Get().IsAvailable());
	agi::vfr::Framerate timecodes = context->project->Timecodes();
	agi::fs::path video_path = context->project->VideoName();
	agi::ycbcr::Header colormatrix = context->ass->GetYCbCrMatrix();
	WorkerVideoOptions worker_opts = SnapshotWorkerVideoOptions(video_path);

	struct BatchJob {
		int line_id = 0;
		int orig_start = 0;
		int orig_end = 0;
		std::string style;
	};
	struct BatchResult {
		int new_start = 0;
		int new_end = 0;
		bool matched = false;
	};

	auto jobs = std::make_shared<std::vector<BatchJob>>();
	jobs->reserve(lines.size());
	std::vector<int> locked_ids;
	locked_ids.reserve(lines.size());
	for (AssDialogue *line : lines) {
		jobs->push_back(BatchJob{line->Id, static_cast<int>(line->Start), static_cast<int>(line->End), line->Style.get()});
		locked_ids.push_back(line->Id);
	}

	auto handle = context->bgTasks->BeginTask(to_wx("批量对帧"), to_wx("对帧中"), locked_ids, static_cast<int>(jobs->size()));
	uint64_t task_id = handle.id;
	auto cancel = handle.cancel;
	auto alive = handle.alive;
	agi::Context *ctx = context;

	std::thread coord([ctx, task_id, cancel, alive, jobs, region, frame_count, base_similarity, engine_mode, is_anchor_mode, dbnet_active, timecodes, video_path, colormatrix, worker_opts, provider]() {
		int total = static_cast<int>(jobs->size());
		int hw = static_cast<int>(std::thread::hardware_concurrency());
		int max_workers = std::clamp(hw > 0 ? hw / 2 : 2, 2, 6);
		max_workers = std::min(max_workers, total);

		std::vector<std::unique_ptr<VideoProvider>> worker_providers;
		if (!video_path.empty()) {
			worker_providers.reserve(max_workers);
			for (int w = 0; w < max_workers; ++w) {
				if (cancel->load()) break;
				auto wp = CreateFFmpegSourceWorkerProvider(video_path, colormatrix, worker_opts);
				if (!wp) break;
				worker_providers.push_back(std::move(wp));
			}
		}

		auto results = std::make_shared<std::vector<BatchResult>>(jobs->size());
		std::atomic<size_t> next_idx{0};
		std::atomic<int> done_count{0};
		std::mutex prog_mtx;
		auto last_report = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

		auto on_sample_abort = [&](int /*samples*/) -> bool {
			return !cancel->load();
		};

		auto run_worker = [&](MatchVideoView const& view) {
			constexpr size_t kChunk = 4;
			while (!cancel->load()) {
				size_t begin = next_idx.fetch_add(kChunk);
				if (begin >= jobs->size()) break;
				size_t end = std::min(begin + kChunk, jobs->size());

				for (size_t i = begin; i < end && !cancel->load(); ++i) {
					auto const& job = (*jobs)[i];
					int start = job.orig_start;
					int end_ms = job.orig_end;
					int start_f = std::clamp(view.FrameAtTime(job.orig_start, agi::vfr::START), 0, frame_count - 1);
					int end_f = std::clamp(view.FrameAtTime(job.orig_end, agi::vfr::END), 0, frame_count - 1);
					int mid_f = (end_f > start_f) ? (start_f + (end_f - start_f) / 2) : start_f;

					RefProbe probe = FindBestRefFrame(view, region, job.orig_start, job.orig_end, is_anchor_mode, dbnet_active, *cancel);
					if (!cancel->load() && (!dbnet_active || probe.has_text)) {
						try {
							MaskCache line_cache;
							bool ok = MatchLineToRegion(view, job.orig_start, job.orig_end, region, probe.frame,
								start, end_ms, base_similarity, engine_mode, -1, -1,
								on_sample_abort, nullptr, &line_cache, -1, nullptr, is_anchor_mode, !dbnet_active);
							if (!ok && !dbnet_active && probe.frame != mid_f && !cancel->load()) {
								MaskCache retry_cache;
								start = job.orig_start;
								end_ms = job.orig_end;
								ok = MatchLineToRegion(view, job.orig_start, job.orig_end, region, mid_f,
									start, end_ms, base_similarity, engine_mode, -1, -1,
									on_sample_abort, nullptr, &retry_cache, -1, nullptr, is_anchor_mode, true);
							}
							if (ok && start < end_ms) {
								(*results)[i] = BatchResult{start, end_ms, true};
							}
						}
						catch (...) {
						}
					}

					int done = done_count.fetch_add(1) + 1;
					bool should_post = false;
					{
						std::lock_guard<std::mutex> lk(prog_mtx);
						auto now = std::chrono::steady_clock::now();
						if (done == total || std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count() >= 80) {
							last_report = now;
							should_post = true;
						}
					}
					if (should_post && alive->load()) {
						wxTheApp->CallAfter([ctx, alive, task_id, done, total]() {
							if (!alive->load()) return;
							if (ctx && ctx->bgTasks) {
								ctx->bgTasks->UpdateProgress(task_id, done, total, wxString::Format(to_wx("批量对帧 %d/%d 行"), done, total));
							}
						});
					}
				}
			}
		};

		if (!cancel->load()) {
			if (!worker_providers.empty()) {
				std::vector<std::thread> workers;
				workers.reserve(worker_providers.size());
				for (auto& wp : worker_providers) {
					MatchVideoView view = MakeWorkerVideoView(wp.get(), timecodes);
					workers.emplace_back([&run_worker, view]() {
						run_worker(view);
					});
				}
				for (auto& t : workers) {
					if (t.joinable()) t.join();
				}
			} else {
				MatchVideoView view = MakeSharedVideoView(provider, timecodes);
				run_worker(view);
			}
		}

		worker_providers.clear();

		wxTheApp->CallAfter([ctx, alive, cancel, task_id, jobs, results, timecodes, frame_count, engine_mode]() {
			if (!alive->load()) return;
			bool was_cancelled = cancel->load();
			if (ctx && ctx->bgTasks)
				ctx->bgTasks->FinishTask(task_id);

			if (was_cancelled) {
				if (ctx && ctx->frame)
					ctx->frame->StatusTimeout(to_wx("批量对帧已停止"), 4000);
				return;
			}
			if (!ctx || !ctx->ass || !ctx->bgTasks) return;

			struct SurvivingRow {
				AssDialogue *line = nullptr;
				std::pair<int, int> orig;
				std::pair<int, int> cur;
				bool matched = false;
				std::string style;
			};

			std::vector<SurvivingRow> rows;
			rows.reserve(jobs->size());
			int skipped = 0;
			for (size_t i = 0; i < jobs->size(); ++i) {
				auto const& job = (*jobs)[i];
				AssDialogue *line = ctx->bgTasks->FindLineById(job.line_id);
				if (!line) continue;
				auto const& res = (*results)[i];
				SurvivingRow r;
				r.line = line;
				r.orig = {job.orig_start, job.orig_end};
				r.matched = (res.matched && res.new_start < res.new_end);
				r.cur = r.matched ? std::make_pair(res.new_start, res.new_end) : r.orig;
				r.style = job.style;
				if (!r.matched) ++skipped;
				rows.push_back(std::move(r));
			}

			auto moved = [&](size_t idx) {
				return rows[idx].cur != rows[idx].orig;
			};

			std::unordered_map<std::string, std::vector<size_t>> style_groups;
			for (size_t i = 0; i < rows.size(); ++i)
				style_groups[rows[i].style].push_back(i);

			auto snap_to_frame_boundary = [&](int ms, int lo, int hi) -> int {
				if (lo + 1 > hi - 1) return std::clamp(ms, lo, hi);
				int f = std::clamp(timecodes.FrameAtTime(ms, agi::vfr::START), 0, frame_count - 1);
				int snapped = timecodes.TimeAtFrame(f, agi::vfr::START);
				if (snapped > lo && snapped < hi) return snapped;
				return std::clamp(ms, lo + 1, hi - 1);
			};

			constexpr int kEdgeTolMs = 150;
			for (auto& [style_name, group] : style_groups) {
				if (group.size() < 2) continue;
				for (size_t k = 1; k < group.size(); ++k) {
					size_t pi = group[k - 1];
					size_t ci = group[k];

					if ((!rows[pi].matched && !rows[ci].matched) || (!moved(pi) && !moved(ci))) continue;

					auto [oa_s, oa_e] = rows[pi].orig;
					auto [ob_s, ob_e] = rows[ci].orig;
					int orig_overlap = std::min(oa_e, ob_e) - std::max(oa_s, ob_s);
					int orig_min_dur = std::min(oa_e - oa_s, ob_e - ob_s);
					if (orig_min_dur > 0 && orig_overlap > orig_min_dur * 7 / 10)
						continue;

					int a_s = rows[pi].cur.first, a_e = rows[pi].cur.second;
					int b_s = rows[ci].cur.first, b_e = rows[ci].cur.second;
					if (std::min(a_e, b_e) <= std::max(a_s, b_s)) continue;

					if (b_s > a_s + kEdgeTolMs && a_e >= b_e - kEdgeTolMs && b_s < b_e) {
						rows[pi].cur.second = b_s;
					} else if (b_s <= a_s + kEdgeTolMs && a_e < b_e - kEdgeTolMs && a_s < a_e) {
						rows[ci].cur.first = a_e;
					} else if (b_s <= a_s + kEdgeTolMs && a_e >= b_e - kEdgeTolMs) {
						int outer_s = std::min(a_s, b_s);
						int outer_e = std::max(a_e, b_e);
						if (outer_e - outer_s >= 2) {
							int orig_split = (oa_e + ob_s) / 2;
							int split_ms = (orig_split > outer_s + kEdgeTolMs && orig_split < outer_e - kEdgeTolMs)
								? snap_to_frame_boundary(orig_split, outer_s, outer_e)
								: snap_to_frame_boundary((outer_s + outer_e) / 2, outer_s, outer_e);
							if (split_ms > outer_s && split_ms < outer_e) {
								rows[pi].cur = { outer_s, split_ms };
								rows[ci].cur = { split_ms, outer_e };
							}
						}
					} else {
						if (!moved(ci) && b_s > a_s) {
							rows[pi].cur.second = b_s;
						} else if (!moved(pi) && a_e < b_e) {
							rows[ci].cur.first = a_e;
						} else if (b_e - a_s >= 2) {
							int mid_snap = snap_to_frame_boundary((a_e + b_s) / 2, a_s, b_e);
							rows[pi].cur.second = mid_snap;
							rows[ci].cur.first = mid_snap;
						}
					}

					if (rows[pi].cur.first >= rows[pi].cur.second) rows[pi].cur = rows[pi].orig;
					if (rows[ci].cur.first >= rows[ci].cur.second) rows[ci].cur = rows[ci].orig;
				}
			}

			int changed = 0;
			for (auto const& r : rows) {
				if (r.cur.first < r.cur.second &&
					(static_cast<int>(r.line->Start) != r.cur.first || static_cast<int>(r.line->End) != r.cur.second)) {
					r.line->Start = r.cur.first;
					r.line->End = r.cur.second;
					++changed;
				}
			}
			if (changed > 0) {
				ctx->ass->Commit(_("batch match subtitle timings"), AssFile::COMMIT_DIAG_TIME);
			}

			if (ctx->frame) {
				if (skipped > 0) {
					ctx->frame->StatusTimeout(wxString::Format(
						to_wx("批量对帧完成：已更新 %d 行，跳过 %d 行（未检出文字）"),
						changed, skipped), 6000);
				} else {
					ctx->frame->StatusTimeout(wxString::Format(
						to_wx("批量对帧完成：已更新 %d 行"),
						changed), 6000);
				}
			}
		});
	});

	context->bgTasks->AttachCoordinator(task_id, std::move(coord));
}

void VisualToolTimingRegion::MatchCurrentLine(agi::Context *context) {
	if (!context) return;
	auto provider = context->project->VideoProvider();
	auto line = context->selectionController->GetActiveLine();
	if (!provider) {
		wxMessageBox(to_wx("请先加载视频再执行快速对帧。"), to_wx("快速对帧 (Ctrl+Alt+D)"), wxOK | wxICON_WARNING, context->parent);
		return;
	}
	if (!line) {
		wxMessageBox(to_wx("请先在字幕表格中选择一行字幕。"), to_wx("快速对帧 (Ctrl+Alt+D)"), wxOK | wxICON_INFORMATION, context->parent);
		return;
	}
	if (context->bgTasks && context->bgTasks->IsLineLocked(line)) {
		if (context->frame)
			context->frame->StatusTimeout(to_wx("当前字幕行正在后台处理中"), 4000);
		return;
	}
	if (!HasPresetRegion()) {
		wxMessageBox(to_wx("尚未设定字幕区域。请先使用“视觉对帧”工具框选或标定字幕位置。"), to_wx("快速对帧 (Ctrl+Alt+D)"), wxOK | wxICON_INFORMATION, context->parent);
		return;
	}
	if (provider->GetFrameCount() <= 0 || provider->GetWidth() <= 0 || provider->GetHeight() <= 0) return;

	int x0 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region X0")->GetDouble() * provider->GetWidth()), 0, provider->GetWidth() - 1);
	int x1 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region X1")->GetDouble() * provider->GetWidth()), 0, provider->GetWidth());
	int y0 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region Y0")->GetDouble() * provider->GetHeight()), 0, provider->GetHeight() - 1);
	int y1 = std::clamp(static_cast<int>(OPT_GET("Tool/Frame Match/Region Y1")->GetDouble() * provider->GetHeight()), 0, provider->GetHeight());
	PixelRegion region{x0, y0, x1, y1};
	if (region.x1 - region.x0 < 8 || region.y1 - region.y0 < 6) return;

	int frame_count = provider->GetFrameCount();
	int reference_frame = std::clamp(context->videoController->GetFrameN(), 0, frame_count - 1);
	int minimum_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));

	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
	bool is_anchor_mode = OPT_GET("Tool/Frame Match/Region Is Anchor")->GetBool();
	wxString mode_label;
	if (engine_mode == 0) mode_label = to_wx("智能双模");
	else if (engine_mode == 1) mode_label = to_wx("深度学习笔画检测");
	else mode_label = to_wx("经典原生像素");

	int total_work = 30;
	wxProgressDialog progress(
		wxString::Format(to_wx("快速对帧 (Ctrl+Alt+D) [%s]"), mode_label),
		wxString::Format(to_wx("正在使用 %s 扫描匹配当前行字幕区间..."), mode_label),
		total_work, context->parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE);

	auto on_sample_pump = [&](int samples) {
		wxSafeYield(context->parent, true);
		return progress.Update(std::min(samples, total_work));
	};

	MatchVideoView view = MakeSharedVideoView(provider, context->project->Timecodes());
	int new_start = 0, new_end = 0;
	int status = kMatchOk;
	int engine_used = 0;
	MaskCache cache;

	bool ok = MatchLineToRegion(view, line->Start, line->End, region, reference_frame, new_start, new_end,
		minimum_similarity, engine_mode, -1, -1, on_sample_pump, &status, &cache, -1, &engine_used, is_anchor_mode);

	if ((!ok || status == kMatchNoPattern) && line->End > line->Start) {
		int mid_time = static_cast<int>(line->Start + (line->End - line->Start) / 2);
		int mid_frame = std::clamp(context->videoController->FrameAtTime(mid_time, agi::vfr::START), 0, frame_count - 1);
		if (mid_frame != reference_frame) {
			MaskCache retry_cache;
			ok = MatchLineToRegion(view, line->Start, line->End, region, mid_frame, new_start, new_end,
				minimum_similarity, engine_mode, -1, -1, on_sample_pump, &status, &retry_cache, -1, &engine_used, is_anchor_mode);
		}
	}

	if (!ok || status != kMatchOk || new_end <= new_start) {
		if (context->frame)
			context->frame->SetStatusText(to_wx("快速对帧：当前画面未检出清晰字幕或相似度未达标"));
		return;
	}

	progress.Update(total_work);

	line->Start = new_start;
	line->End = new_end;
	context->ass->Commit(_("match subtitle timing to picked region"), AssFile::COMMIT_DIAG_TIME, -1, line);
	context->selectionController->SetActiveLine(line);
	context->videoController->JumpToTime(line->Start);

	if (context->frame) {
		context->frame->SetStatusText(wxString::Format(
			to_wx("单键快速对帧完成 [%s - %s] (第 %d 行)"),
			to_wx(agi::Time(line->Start).GetAssFormatted()),
			to_wx(agi::Time(line->End).GetAssFormatted()),
			line->Row + 1));
	}
}

void VisualToolTimingRegion::ClearRegion() {
	has_region = false;
	dragging = false;
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->Render();
}

bool VisualToolTimingRegion::OnKeyDown(wxKeyEvent &event) {
	if (event.GetKeyCode() != WXK_ESCAPE || (!dragging && !has_region)) return false;
	ClearRegion();
	return true;
}

void VisualToolTimingRegion::OnMouseEvent(wxMouseEvent &event) {
	mouse_pos = Vector2D(event.GetX(), event.GetY());
	if (event.RightDown() && (dragging || has_region)) {
		ClearRegion();
	}
	else if (event.LeftDown()) {
		start = end = ToScriptCoords(mouse_pos);
		has_region = true;
		dragging = true;
		if (!parent->HasCapture()) parent->CaptureMouse();
		parent->Render();
	}
	else if (event.Dragging() && event.LeftIsDown() && dragging) {
		end = ToScriptCoords(mouse_pos);
		parent->Render();
	}
	else if (event.LeftUp() && dragging) {
		end = ToScriptCoords(mouse_pos);
		dragging = false;
		if (parent->HasCapture()) parent->ReleaseMouse();

		double dx = std::abs(end.X() - start.X());
		double dy = std::abs(end.Y() - start.Y());

		if (script_res.X() > 0 && script_res.Y() > 0) {
			// 支持纯竖线拖拽 (宽度极窄 dx < 15 且 高度明显 dy >= 8)
			if (dx < 15.0 && dy >= 8.0) {
				double ax = (start.X() + end.X()) / 2.0;
				double ay = (start.Y() + end.Y()) / 2.0;
				double ah = dy;
				double half_w = std::max(ah * 3.5, 120.0);
				OPT_SET("Tool/Frame Match/Region X0")->SetDouble(std::clamp((ax - half_w) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y0")->SetDouble(std::clamp((ay - ah / 2.0) / script_res.Y(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region X1")->SetDouble(std::clamp((ax + half_w) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y1")->SetDouble(std::clamp((ay + ah / 2.0) / script_res.Y(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Is Anchor")->SetBool(true);
			} else {
				OPT_SET("Tool/Frame Match/Region X0")->SetDouble(std::clamp<double>(std::min(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y0")->SetDouble(std::clamp<double>(std::min(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region X1")->SetDouble(std::clamp<double>(std::max(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y1")->SetDouble(std::clamp<double>(std::max(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Is Anchor")->SetBool(false);
			}
		}
		DetectInterval();
		ClearRegion();
	}
	else if (event.Leaving()) {
		mouse_pos = Vector2D(-1, -1);
	}
	event.Skip(false);
}

void VisualToolTimingRegion::Draw() {
	if (dragging && has_region) {
		double dx = std::abs(end.X() - start.X());
		double dy = std::abs(end.Y() - start.Y());
		if (dx < 15.0 && dy >= 8.0) {
			// 拖拽中：绘制红色测量标尺竖线与中心锚点
			Vector2D p_top = FromScriptCoords(Vector2D(start.X(), std::min(start.Y(), end.Y())));
			Vector2D p_bot = FromScriptCoords(Vector2D(start.X(), std::max(start.Y(), end.Y())));
			Vector2D p_mid = (p_top + p_bot) / 2.0;

			gl.SetLineColour(wxColour(245, 45, 45), 1.0f, 3);
			gl.DrawLine(p_top, p_bot);
			gl.DrawLine(p_top - Vector2D(8, 0), p_top + Vector2D(8, 0));
			gl.DrawLine(p_bot - Vector2D(8, 0), p_bot + Vector2D(8, 0));
			gl.SetFillColour(wxColour(245, 45, 45), 1.0f);
			gl.DrawCircle(p_mid, 4.0f);
		} else {
			gl.SetLineColour(wxColour(255, 190, 40), 1.f, 2);
			gl.SetFillColour(wxColour(255, 190, 40), .15f);
			gl.DrawRectangle(FromScriptCoords(start), FromScriptCoords(end));
		}
		return;
	}

	// 当已设置有基准区域时，在画面上绘制类似 SubtitleOCR 的红线基准锚点
	if (HasPresetRegion() && script_res.X() > 0 && script_res.Y() > 0) {
		double x0 = OPT_GET("Tool/Frame Match/Region X0")->GetDouble() * script_res.X();
		double x1 = OPT_GET("Tool/Frame Match/Region X1")->GetDouble() * script_res.X();
		double y0 = OPT_GET("Tool/Frame Match/Region Y0")->GetDouble() * script_res.Y();
		double y1 = OPT_GET("Tool/Frame Match/Region Y1")->GetDouble() * script_res.Y();

		double ax = (x0 + x1) / 2.0;
		double ay = (y0 + y1) / 2.0;
		double ah = std::max(20.0, y1 - y0);

		Vector2D p_top = FromScriptCoords(Vector2D(ax, ay - ah / 2.0));
		Vector2D p_bot = FromScriptCoords(Vector2D(ax, ay + ah / 2.0));
		Vector2D p_mid = (p_top + p_bot) / 2.0;

		// 1. 绘制红色的竖线基准锚点
		gl.SetLineColour(wxColour(245, 45, 45), 1.0f, 3);
		gl.DrawLine(p_top, p_bot);
		gl.DrawLine(p_top - Vector2D(8, 0), p_top + Vector2D(8, 0));
		gl.DrawLine(p_bot - Vector2D(8, 0), p_bot + Vector2D(8, 0));
		gl.SetFillColour(wxColour(245, 45, 45), 1.0f);
		gl.DrawCircle(p_mid, 4.0f);

		// 2. 绘制参考范围框
		gl.SetLineColour(wxColour(255, 190, 40), 0.7f, 1);
		gl.SetFillColour(wxColour(255, 190, 40), 0.08f);
		gl.DrawRectangle(FromScriptCoords(Vector2D(x0, y0)), FromScriptCoords(Vector2D(x1, y1)));
	}
}

void VisualToolTimingRegion::DetectInterval() {
	auto provider = c->project->VideoProvider();
	auto line = c->selectionController->GetActiveLine();
	if (!provider || !line) {
		wxMessageBox(to_wx("请先选中字幕行，停在字幕可见帧后，在画面上框选字幕区域。"), to_wx("视觉对帧"), wxOK | wxICON_INFORMATION, parent);
		return;
	}
	if (c->bgTasks && c->bgTasks->IsLineLocked(line)) {
		if (c->frame)
			c->frame->StatusTimeout(to_wx("当前字幕行正在后台处理中"), 4000);
		return;
	}
	if (script_res.X() <= 0 || script_res.Y() <= 0 || provider->GetWidth() <= 0 || provider->GetHeight() <= 0) {
		wxMessageBox(to_wx("视频或脚本分辨率不可用。"), to_wx("视觉对帧"), wxOK | wxICON_WARNING, parent);
		return;
	}

	int x0 = std::clamp(static_cast<int>(std::floor(std::min(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth() - 1);
	int x1 = std::clamp(static_cast<int>(std::ceil(std::max(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth());
	int y0 = std::clamp(static_cast<int>(std::floor(std::min(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight() - 1);
	int y1 = std::clamp(static_cast<int>(std::ceil(std::max(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight());

	double dx = std::abs(end.X() - start.X());
	double dy = std::abs(end.Y() - start.Y());
	bool is_anchor_mode = (dx < 15.0 && dy >= 8.0);
	if (is_anchor_mode) {
		int ax = (x0 + x1) / 2;
		int ah = std::max(16, y1 - y0);
		int half_w = std::max(ah * 3, 120);
		x0 = std::clamp(ax - half_w, 0, provider->GetWidth() - 1);
		x1 = std::clamp(ax + half_w, 0, provider->GetWidth());
	}

	if (x1 - x0 < 8 || y1 - y0 < 6) {
		wxMessageBox(to_wx("选区过小，请在字幕文字周围拖拽出更完整的框或竖线锚点。"), to_wx("视觉对帧"), wxOK | wxICON_WARNING, parent);
		return;
	}
	int frame_count = provider->GetFrameCount();
	if (frame_count <= 0) {
		wxMessageBox(to_wx("当前视频没有有效帧。"), to_wx("视觉对帧"), wxOK | wxICON_WARNING, parent);
		return;
	}

	int reference_frame = std::clamp(c->videoController->GetFrameN(), 0, frame_count - 1);
	int minimum_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));

	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
	wxString mode_label;
	if (engine_mode == 0) mode_label = to_wx("智能双模");
	else if (engine_mode == 1) mode_label = to_wx("深度学习笔画检测");
	else mode_label = to_wx("经典原生像素");

	int total_work = 30;
	wxProgressDialog progress(
		wxString::Format(to_wx("视觉对帧 [%s]"), mode_label),
		wxString::Format(to_wx("正在使用 %s 扫描匹配字幕区间..."), mode_label),
		total_work, parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);

	MatchVideoView view = MakeSharedVideoView(provider, c->project->Timecodes());
	int new_start = 0, new_end = 0;
	int status = kMatchOk;
	bool ok = false;
	MaskCache cache;
	try {
		ok = MatchLineToRegion(view, line->Start, line->End, PixelRegion{x0, y0, x1, y1}, reference_frame, new_start, new_end,
			minimum_similarity, engine_mode, -1, -1,
			[&](int samples) {
				wxSafeYield(parent, true);
				return progress.Update(std::min(samples, total_work));
			}, &status, &cache, -1, nullptr, is_anchor_mode);

		// 智能自适应兜底：若当前画面暂停帧未能提取到清晰图案（例如停留在无字/提前淡出时刻）
		// 自动在当前字幕行的时间中点 (Start + End) / 2 所对应的画面帧重试
		if ((!ok || status == kMatchNoPattern) && line && line->End > line->Start) {
			int mid_time = static_cast<int>(line->Start + (line->End - line->Start) / 2);
			int mid_frame = std::clamp(c->videoController->FrameAtTime(mid_time, agi::vfr::START), 0, frame_count - 1);
			if (mid_frame != reference_frame) {
				int retry_status = kMatchOk;
				MaskCache retry_cache;
				bool retry_ok = MatchLineToRegion(view, line->Start, line->End, PixelRegion{x0, y0, x1, y1}, mid_frame, new_start, new_end,
					minimum_similarity, engine_mode, -1, -1,
					[&](int samples) {
						wxSafeYield(parent, true);
						return progress.Update(std::min(samples, total_work));
					}, &retry_status, &retry_cache, -1, nullptr, is_anchor_mode);
				if (retry_ok && retry_status == kMatchOk) {
					ok = true;
					status = retry_status;
				}
			}
		}
	}
	catch (std::exception const& e) {
		wxMessageBox(to_wx("无法扫描视频帧：") + to_wx(e.what()), to_wx("视觉对帧"), wxOK | wxICON_ERROR, parent);
		return;
	}
	if (status == kMatchCancelled) return;
	if (status == kMatchNoPattern) {
		wxMessageBox(to_wx("选区内未检出清晰字幕，请在字幕可见帧重新框选文字区域。"), to_wx("视觉对帧"), wxOK | wxICON_INFORMATION, parent);
		return;
	}
	if (!ok || status == kMatchInvalid) {
		wxMessageBox(to_wx("选区未能匹配到有效时间区间，请调整选框或相似度后重试。"), to_wx("视觉对帧"), wxOK | wxICON_INFORMATION, parent);
		return;
	}

	progress.Update(total_work);
	line->Start = new_start;
	line->End = new_end;
	Commit(_("match subtitle timing to picked region"));
	c->selectionController->SetActiveLine(line);
	c->videoController->JumpToTime(line->Start);

	if (c->frame) {
		c->frame->SetStatusText(wxString::Format(
			to_wx("视觉对帧完成：%s - %s"),
			to_wx(agi::Time(new_start).GetAssFormatted()),
			to_wx(agi::Time(new_end).GetAssFormatted())));
	}
}
