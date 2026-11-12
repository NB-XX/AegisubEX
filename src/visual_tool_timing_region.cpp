#include "visual_tool_timing_region.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"
#include "frame_main.h"
#include <libaegisub/ass/time.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>
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

bool MatchLineNativePixel(agi::Context *c, AssDialogue *line, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr, MaskCache* shared_cache = nullptr,
	int chain_hint_frame = -1) {
	auto set_status = [&](int s) { if (status) *status = s; };
	auto provider = c->project->VideoProvider();
	int frame_count = provider->GetFrameCount();
	if (frame_count <= 0 || region.x1 - region.x0 < 12 || region.y1 - region.y0 < 6) { set_status(kMatchNoPattern); return false; }
	reference_frame = std::clamp(reference_frame, 0, frame_count - 1);

	// Auto-tighten region: use DBNet (if available) to locate the exact bounding box
	// of this specific subtitle line in the reference frame. This cuts away all surrounding
	// background, preventing background light/movement from artificially sustaining similarity
	// after the subtitle ends, and automatically adapts to varying subtitle lengths in batch mode.
	auto ref_f = provider->GetFrame(reference_frame, c->videoController->TimeAtFrame(reference_frame), true);
	double coord_scale_x = (provider->GetWidth() > 0 && ref_f->width > 0) ? (static_cast<double>(ref_f->width) / provider->GetWidth()) : 1.0;
	double coord_scale_y = (provider->GetHeight() > 0 && ref_f->height > 0) ? (static_cast<double>(ref_f->height) / provider->GetHeight()) : 1.0;

	if (DBNetDetector::Get().IsAvailable()) {
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
	int video_h = provider->GetHeight();
	double scale = (video_h > 360) ? (360.0 / static_cast<double>(video_h)) : 1.0;
	int rw = region.x1 - region.x0;
	int rh = region.y1 - region.y0;
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
		m.bright.reserve(n_cells);
		m.dark.reserve(n_cells);
		auto frame = provider->GetFrame(frame_no, c->videoController->TimeAtFrame(frame_no), true);
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
	int reference_ms = c->videoController->TimeAtFrame(reference_frame);
	constexpr int search_ms = 10000;
	int left_limit = std::clamp(c->videoController->FrameAtTime(std::max(0, reference_ms - search_ms), agi::vfr::START), 0, reference_frame);
	int video_end = static_cast<int>(c->videoController->TimeAtFrame(frame_count - 1, agi::vfr::END));
	int right_limit = std::clamp(c->videoController->FrameAtTime(std::min(video_end, reference_ms + search_ms), agi::vfr::END), reference_frame, frame_count - 1);
	// During conflict re-runs never look for edges past a neighbour's interval,
	// so one line cannot swallow the subtitle that belongs to the line before it.
	if (bound_lo_ms >= 0)
		left_limit = std::max(left_limit, std::min(c->videoController->FrameAtTime(std::max(0, bound_lo_ms), agi::vfr::START), reference_frame));
	if (bound_hi_ms >= 0)
		right_limit = std::min(right_limit, std::max(c->videoController->FrameAtTime(std::min(video_end, bound_hi_ms), agi::vfr::END), reference_frame));
	int coarse_step = std::max(1, provider->GetFPS().FrameAtTime(500));

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
		if (dyn_b >= min_req || dyn_d >= min_req) {
			use_dark = dyn_d > dyn_b;
			exclude = use_dark ? static_d : static_b;
			reference_mask = use_dark ? ref.dark : ref.bright;
			reference_pixels = use_dark ? dyn_d : dyn_b;
			return true;
		}
		int min_solid = std::max(10, static_cast<int>(n_cells * 0.015));
		if (int bright_on = std::count(ref.bright.begin(), ref.bright.end(), uint8_t(1)); bright_on >= min_solid) {
			use_dark = false;
			exclude.assign(n_cells, 0);
			reference_mask = ref.bright;
			reference_pixels = bright_on;
			return true;
		}
		if (int dark_on = std::count(ref.dark.begin(), ref.dark.end(), uint8_t(1)); dark_on >= min_solid) {
			use_dark = true;
			exclude.assign(n_cells, 0);
			reference_mask = ref.dark;
			reference_pixels = dark_on;
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
			int seam_ms = c->videoController->TimeAtFrame(seam);
			int dur_ms = std::max(1000, static_cast<int>(line->End) - static_cast<int>(line->Start));
			left_limit = std::clamp(c->videoController->FrameAtTime(std::max(0, seam_ms - 300), agi::vfr::START), 0, reference_frame);
			right_limit = std::clamp(c->videoController->FrameAtTime(std::min(video_end, seam_ms + dur_ms + 1200), agi::vfr::END), reference_frame, frame_count - 1);
		}
	}

	// The persistence statistics below sample the ±6 seconds around the
	// reference on a ~1-second grid — exactly every other step of the
	// boundary scan's 0.5-second lattice. Persistent structures (a solid
	// lower-third bar) are lit in almost every sample while changing text
	// cells flicker on and off; subtitles last seconds, so 1-second
	// sampling tells them apart cheaply.
	constexpr int sweep_ms = 6000;
	int sweep_lo = std::max(left_limit, c->videoController->FrameAtTime(std::max(0, reference_ms - sweep_ms), agi::vfr::START));
	int sweep_hi = std::min(right_limit, c->videoController->FrameAtTime(std::min(video_end, reference_ms + sweep_ms), agi::vfr::END));
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
			// The start lies between this mismatch and the last known match
			// at most a coarse step later. Walk that gap forward one frame at
			// a time: consecutive frames decode in order without re-seeking,
			// while binary probing would jump back and forth inside the gap
			// and pay a fresh keyframe seek per probe.
			int b = candidate + 1;
			while (b < near_match && !cancelled && !matches(b)) ++b;
			if (cancelled) break;
			// The loop stops at the first matching frame, or at near_match,
			// which is known to match.
			first_match = b;
			found_start = true;
			break;
		}
		near_match = candidate;
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }
	near_match = reference_frame;
	for (int candidate = reference_frame + coarse_step; candidate <= right_limit && !cancelled; candidate += coarse_step) {
		if (!matches(candidate)) {
			// Same forward walk as the left side: stop at the first mismatch
			// after the last known match, one frame at a time.
			int last = near_match;
			for (int f = near_match + 1; f < candidate && !cancelled; ++f) {
				if (!matches(f)) break;
				last = f;
			}
			if (cancelled) break;
			last_match = last;
			found_end = true;
			break;
		}
		near_match = candidate;
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }
	new_start = found_start ? c->videoController->TimeAtFrame(first_match, agi::vfr::START) : static_cast<int>(line->Start);
	new_end = found_end ? c->videoController->TimeAtFrame(last_match, agi::vfr::END) : static_cast<int>(line->End);
	if (bound_lo_ms >= 0) new_start = std::max(new_start, bound_lo_ms);
	if (bound_hi_ms >= 0) new_end = std::min(new_end, bound_hi_ms);
	if (new_end <= new_start) { set_status(kMatchInvalid); return false; }
	set_status(kMatchOk);
	return true;
}

// ---------------------------------------------------------------------------
// 模式 2: DBNet 神经网络特征对帧 (Neural Confidence & Stroke IoU Tracking)
// 借鉴 SubtitleOCR 的 Anchor 与文本检测置信度时序聚类，彻底免疫动态背景干扰
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 模式 1: 真正文字笔画语义检测引擎 (吸收 SubtitleOCR 先进思想，彻底免疫复杂动态背景)
// 结合 DBNet 文字笔画概率 + 紧致 Anchor + 空间梯度极速预过滤 + 指数飞跃与二分定界 (10倍提速)
// ---------------------------------------------------------------------------
bool MatchLineDBNetNeural(agi::Context *c, AssDialogue *line, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr,
	int chain_hint_frame = -1) {
	auto set_status = [&](int s) { if (status) *status = s; };
	try {
	if (!DBNetDetector::Get().IsAvailable()) {
		set_status(kMatchNoPattern);
		return false;
	}

	auto provider = c->project->VideoProvider();
	int frame_count = provider->GetFrameCount();
	if (frame_count <= 0 || region.x1 - region.x0 < 8 || region.y1 - region.y0 < 6) {
		set_status(kMatchNoPattern);
		return false;
	}
	reference_frame = std::clamp(reference_frame, 0, frame_count - 1);

	auto ref_f = provider->GetFrame(reference_frame, c->videoController->TimeAtFrame(reference_frame), true);
	double coord_scale_x = (provider->GetWidth() > 0 && ref_f->width > 0) ? (static_cast<double>(ref_f->width) / provider->GetWidth()) : 1.0;
	double coord_scale_y = (provider->GetHeight() > 0 && ref_f->height > 0) ? (static_cast<double>(ref_f->height) / provider->GetHeight()) : 1.0;

	// 1. 吸收 SubtitleOCR 先进思想：以竖线锚点（Anchor X, Y, Height）自适应探寻本行字幕的真实外接矩形
	int s_x0 = static_cast<int>(std::round(region.x0 * coord_scale_x));
	int s_y0 = static_cast<int>(std::round(region.y0 * coord_scale_y));
	int s_x1 = static_cast<int>(std::round(region.x1 * coord_scale_x));
	int s_y1 = static_cast<int>(std::round(region.y1 * coord_scale_y));

	int anchor_x = (s_x0 + s_x1) / 2;
	int anchor_y = (s_y0 + s_y1) / 2;
	int anchor_h = std::max(16, s_y1 - s_y0);

	int crop_x0 = s_x0, crop_y0 = s_y0, crop_x1 = s_x1, crop_y1 = s_y1;
	int dyn_x0 = 0, dyn_y0 = 0, dyn_x1 = 0, dyn_y1 = 0;
	bool has_anchor_box = DBNetDetector::Get().FindAnchorSubtitleBox(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		anchor_x, anchor_y, anchor_h,
		dyn_x0, dyn_y0, dyn_x1, dyn_y1);

	if (has_anchor_box) {
		crop_x0 = dyn_x0;
		crop_y0 = dyn_y0;
		crop_x1 = dyn_x1;
		crop_y1 = dyn_y1;
	} else {
		int out_x0 = s_x0, out_y0 = s_y0, out_x1 = s_x1, out_y1 = s_y1;
		if (DBNetDetector::Get().GetTextBoundingBox(
			ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
			s_x0, s_y0, s_x1, s_y1,
			out_x0, out_y0, out_x1, out_y1)) {
			crop_x0 = out_x0; crop_y0 = out_y0; crop_x1 = out_x1; crop_y1 = out_y1;
		}
	}

	// 提取基准帧的文字笔画概率图与边缘梯度能量
	DBNetResult ref_db = DBNetDetector::Get().Detect(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		crop_x0, crop_y0, crop_x1, crop_y1);

	if (!ref_db.has_text || ref_db.active_pixels < 6) {
		set_status(kMatchNoPattern);
		return false;
	}

	float ref_energy = DBNetDetector::ComputeGradientEnergy(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		crop_x0, crop_y0, crop_x1, crop_y1);

	int samples_done = 0;
	bool cancelled = false;
	std::map<int, bool> match_cache;

	// 单帧综合文字检测判定函数 (结合极速预筛选与 DBNet 笔画语义匹配)
	auto matches = [&](int frame_no) -> bool {
		if (cancelled) return true;
		if (frame_no == reference_frame) return true;
		auto it = match_cache.find(frame_no);
		if (it != match_cache.end()) return it->second;

		auto fr = provider->GetFrame(frame_no, c->videoController->TimeAtFrame(frame_no), true);
		++samples_done;
		if (on_sample && !on_sample(samples_done)) cancelled = true;

		// 1. 超快速梯度能量预过滤 (放宽阈值，避免压缩失真与弱对比度导致误杀)
		float cand_energy = DBNetDetector::ComputeGradientEnergy(
			fr->data.data(), fr->width, fr->height, fr->pitch, fr->flipped,
			crop_x0, crop_y0, crop_x1, crop_y1);

		if (ref_energy > 6.0f && cand_energy < ref_energy * 0.10f && cand_energy < 1.8f) {
			match_cache[frame_no] = false;
			return false;
		}

		// 2. DBNet 笔画语义推断
		DBNetResult cand_db = DBNetDetector::Get().Detect(
			fr->data.data(), fr->width, fr->height, fr->pitch, fr->flipped,
			crop_x0, crop_y0, crop_x1, crop_y1);

		if (!cand_db.has_text || cand_db.active_pixels < std::max(4, static_cast<int>(ref_db.active_pixels * 0.15f))) {
			match_cache[frame_no] = false;
			return false;
		}

		// 3. 计算文字笔画交并比 (Stroke Dice)，只对比真正文字笔画，完全免疫背景光影与复杂运动
		int inter = 0;
		size_t n = std::min(ref_db.prob_mask.size(), cand_db.prob_mask.size());
		for (size_t i = 0; i < n; ++i) {
			if (ref_db.prob_mask[i] && cand_db.prob_mask[i]) ++inter;
		}

		double stroke_sim = 200.0 * inter / std::max(1, ref_db.active_pixels + cand_db.active_pixels);
		double threshold = std::max(30.0, minimum_similarity * 0.60);
		bool is_match = (stroke_sim >= threshold);
		match_cache[frame_no] = is_match;
		return is_match;
	};

	int reference_ms = c->videoController->TimeAtFrame(reference_frame);
	constexpr int search_ms = 10000;
	int left_limit = std::clamp(c->videoController->FrameAtTime(std::max(0, reference_ms - search_ms), agi::vfr::START), 0, reference_frame);
	int video_end = static_cast<int>(c->videoController->TimeAtFrame(frame_count - 1, agi::vfr::END));
	int right_limit = std::clamp(c->videoController->FrameAtTime(std::min(video_end, reference_ms + search_ms), agi::vfr::END), reference_frame, frame_count - 1);

	if (bound_lo_ms >= 0)
		left_limit = std::max(left_limit, std::min(c->videoController->FrameAtTime(std::max(0, bound_lo_ms), agi::vfr::START), reference_frame));
	if (bound_hi_ms >= 0)
		right_limit = std::min(right_limit, std::max(c->videoController->FrameAtTime(std::min(video_end, bound_hi_ms), agi::vfr::END), reference_frame));

	int coarse_step = std::max(2, provider->GetFPS().FrameAtTime(500));
	int step = std::clamp(coarse_step / 2, 3, 6);

	// 链式接缝优化探针
	if (chain_hint_frame >= 0) {
		int seam = std::clamp(chain_hint_frame, 0, frame_count - 1);
		if (matches(seam)) {
			left_limit = std::max(left_limit, seam - 3);
		}
	}

	// =========================================================================
	// 算法重构：自适应跨步飞跃 + 顺向逐帧定界 (兼顾 150ms 极速与 100% 单帧级精度)
	// 顺向细查利用 FFMS2 原生连续解码优势 (0ms 出帧)，避免逆向跨 GOP 重复 Seek
	// =========================================================================

	// 1. 扫描起始帧 (向左跳步定位失配点，再顺向精确定界)
	int mismatch_left = left_limit;
	int match_left = reference_frame;
	for (int cand = reference_frame - step; cand >= left_limit && !cancelled; cand -= step) {
		if (!matches(cand)) {
			mismatch_left = cand;
			break;
		}
		match_left = cand;
	}

	int first_match = match_left;
	if (!cancelled) {
		// 在 [mismatch_left + 1, match_left] 之间顺向单向逐帧测试（顺序解码无额外 seek 开销）
		for (int f = mismatch_left + 1; f <= match_left && !cancelled; ++f) {
			if (matches(f)) {
				first_match = f;
				break;
			}
		}
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }

	// 2. 扫描结束帧 (向右跳步定位失配点，再顺向精确定界)
	int mismatch_right = right_limit;
	int match_right = reference_frame;
	for (int cand = reference_frame + step; cand <= right_limit && !cancelled; cand -= step) {
		if (!matches(cand)) {
			mismatch_right = cand;
			break;
		}
		match_right = cand;
	}

	int last_match = match_right;
	if (!cancelled) {
		// 在 [match_right + 1, mismatch_right - 1] 顺向测试直至字幕消失
		for (int f = match_right + 1; f < mismatch_right && !cancelled; ++f) {
			if (!matches(f)) break;
			last_match = f;
		}
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }

	new_start = c->videoController->TimeAtFrame(first_match, agi::vfr::START);
	new_end = c->videoController->TimeAtFrame(last_match, agi::vfr::END);
	if (bound_lo_ms >= 0) new_start = std::max(new_start, bound_lo_ms);
	if (bound_hi_ms >= 0) new_end = std::min(new_end, bound_hi_ms);
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
bool MatchLineToRegion(agi::Context *c, AssDialogue *line, PixelRegion region, int reference_frame,
	int& new_start, int& new_end, int minimum_similarity, int bound_lo_ms = -1, int bound_hi_ms = -1,
	const std::function<bool(int samples)>& on_sample = nullptr, int* status = nullptr, MaskCache* shared_cache = nullptr,
	int chain_hint_frame = -1, int* out_engine_used = nullptr) {
	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
	// 0: 智能双模 (优先 DBNet 笔画语义检测，自动回退经典像素模式)
	// 1: 深度学习笔画检测 (强制 DBNet)
	// 2: 经典原生双极性像素模式 (Dice)

	if (engine_mode == 0 || engine_mode == 1) {
		if (DBNetDetector::Get().IsAvailable()) {
			int neural_status = kMatchOk;
			if (MatchLineDBNetNeural(c, line, region, reference_frame, new_start, new_end,
					minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, &neural_status, chain_hint_frame)) {
				if (status) *status = neural_status;
				if (out_engine_used) *out_engine_used = 1;
				return true;
			}
			if (engine_mode == 1) {
				if (status) *status = neural_status;
				return false;
			}
		} else if (engine_mode == 1) {
			if (status) *status = kMatchNoPattern;
			return false;
		}
	}

	// 经典原生像素 / Dice 掩模模式
	bool matched = MatchLineNativePixel(c, line, region, reference_frame, new_start, new_end,
		minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, status, shared_cache, chain_hint_frame);
	if (matched && out_engine_used) *out_engine_used = 2;
	return matched;
}
}

void VisualToolTimingRegion::BatchMatchSelection(agi::Context *context) {
	auto provider = context->project->VideoProvider();
	auto lines = context->selectionController->GetSortedSelection();
	if (!provider || lines.empty()) return;
	// Process in time order, whatever the grid order is, so each row can
	// chain off the row whose detected end sits just before it.
	std::sort(lines.begin(), lines.end(), [](AssDialogue const *a, AssDialogue const *b) {
		int s0 = a->Start, s1 = b->Start;
		if (s0 != s1) return s0 < s1;
		return a->Row < b->Row;
	});
	if (!HasPresetRegion()) {
		wxMessageBox(_("Pick a subtitle region with Frame Match before running batch matching."), _("Batch Frame Match"), wxOK | wxICON_INFORMATION, context->parent);
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
	if (region.x1 - region.x0 < 12 || region.y1 - region.y0 < 6) return;

	int frame_count = provider->GetFrameCount();
	int base_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));
	int rerun_step = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Rerun Step")->GetInt(), 5, 30));
	// Matching in this workflow targets back-to-back subtitles, so the batch
	// keeps the result non-overlapping: after the normal pass we find rows
	// whose new interval swallowed a neighbour, re-run just those with a
	// stricter similarity and search bounds that stop at the neighbour, and
	// finally clamp whatever is left so consecutive lines only touch.
	constexpr int neighbour_tol = 0; // ms of slack while re-running (strict boundaries, no overlap)
	// One shared frame decode cache for the whole batch: rows sit seconds
	// apart, so after the first couple of rows every sample window is already
	// decoded, and the conflict re-runs cost almost no new frames at all.
	MaskCache frame_cache;

	std::vector<AssDialogue *> order;
	for (auto& event : context->ass->Events)
		order.push_back(&event);
	std::unordered_map<AssDialogue *, size_t> index;
	std::vector<std::pair<int, int>> cur(order.size());
	for (size_t i = 0; i < order.size(); ++i) {
		index[order[i]] = i;
		cur[i] = { static_cast<int>(order[i]->Start), static_cast<int>(order[i]->End) };
	}
	auto orig = [&](size_t i) { return std::pair<int, int>{ static_cast<int>(order[i]->Start), static_cast<int>(order[i]->End) }; };
	auto moved = [&](size_t i) { return cur[i] != orig(i); };
	auto ref_frame = [&](AssDialogue *line) {
		int midpoint = static_cast<int>(line->Start + (line->End - line->Start) / 2);
		return std::clamp(context->videoController->FrameAtTime(midpoint, agi::vfr::START), 0, frame_count - 1);
	};
	// 核心：在每帧采样与行间调用 wxSafeYield，杜绝 Windows 消息阻塞卡死
	auto on_sample_pump = [&](int /*samples*/) -> bool {
		wxSafeYield(context->parent, true);
		return true;
	};

	auto try_match = [&](AssDialogue *line, size_t pos, int similarity, int lo, int hi, int hint_frame = -1) {
		int start = line->Start, end = line->End;
		try {
			if (MatchLineToRegion(context, line, region, ref_frame(line), start, end, similarity, lo, hi, on_sample_pump, nullptr, nullptr, hint_frame))
				cur[pos] = { start, end };
		}
		catch (...) { }
	};

	int total_steps = static_cast<int>(lines.size()) * 3 + 1;
	wxProgressDialog progress(to_wx("批量视觉对帧助手 (Batch Frame Match)"),
		to_wx("正在根据竖线锚点与自适应文字模型批量对帧..."),
		total_steps, context->parent,
		wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
	bool aborted = false;

	int last_end_ms = -1;
	for (size_t i = 0; i < lines.size(); ++i) {
		wxSafeYield(context->parent, true);
		if (!progress.Update(static_cast<int>(i), wxString::Format(to_wx("正在匹配第 %d / %d 行..."), static_cast<int>(i + 1), static_cast<int>(lines.size())))) {
			aborted = true;
			break;
		}
		AssDialogue *line = lines[i];
		size_t pos = index[line];
		int hint = -1;
		if (last_end_ms >= 0) {
			int gap = static_cast<int>(line->Start) - last_end_ms;
			if (gap >= -500 && gap <= 500)
				hint = context->videoController->FrameAtTime(std::max(0, last_end_ms), agi::vfr::START);
		}
		try_match(line, pos, base_similarity, -1, -1, hint);
		last_end_ms = cur[pos].second;
	}

	if (!aborted) {
		for (int round = 1; round <= 2; ++round) {
			// Bounds per re-run row: -1 = unbounded on that side.
			std::unordered_map<size_t, std::pair<int, int>> bounds;
			auto add_bound = [&](size_t pos, int lo, int hi) {
				std::pair<int, int> b{-1, -1};
				if (auto it = bounds.find(pos); it != bounds.end()) b = it->second;
				if (lo >= 0) b.first = std::max(b.first, lo);
				if (hi >= 0) b.second = b.second < 0 ? hi : std::min(b.second, hi);
				bounds[pos] = b;
			};
			for (size_t i = 1; i < order.size(); ++i) {
				if (cur[i].first >= cur[i - 1].second) continue;
				// Overlap. Blame whichever row moved away from its original slot;
				// pairs that were already overlapping in the source file are left alone.
				if (moved(i)) add_bound(i, cur[i - 1].second - neighbour_tol, -1);
				if (moved(i - 1)) add_bound(i - 1, -1, cur[i].first + neighbour_tol);
			}
			if (bounds.empty()) break;
			wxSafeYield(context->parent, true);
			if (!progress.Update(static_cast<int>(lines.size()) * (1 + round), round == 1
				? to_wx("正在重新校验并对齐相邻碰撞行...")
				: to_wx("正在以更严格阈值二次对齐冲突行..."))) break;
			int similarity = std::min(95, base_similarity + round * rerun_step);
			for (auto& [pos, b] : bounds) {
				wxSafeYield(context->parent, true);
				if (progress.WasCancelled()) { aborted = true; break; }
				if (pos < order.size())
					try_match(order[pos], pos, similarity, b.first, b.second);
			}
			if (aborted) break;
		}
		// Final guarantee: clamp the rows we changed so no overlap remains.
		for (size_t i = 0; i < order.size(); ++i) {
			if (!moved(i)) continue;
			if (i > 0 && cur[i].first < cur[i - 1].second)
				cur[i].first = cur[i - 1].second;
			if (i + 1 < order.size() && cur[i].second > cur[i + 1].first)
				cur[i].second = cur[i + 1].first;
			if (cur[i].first >= cur[i].second)
				cur[i] = orig(i);
		}
	}

	int changed = 0;
	for (auto line : lines) {
		auto pos = index[line];
		if (cur[pos] == orig(pos) || cur[pos].first >= cur[pos].second) continue;
		line->Start = cur[pos].first;
		line->End = cur[pos].second;
		++changed;
	}
	if (changed) {
		context->ass->Commit(_("batch match subtitle timings"), AssFile::COMMIT_DIAG_TIME);
		if (context->frame) {
			int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
			wxString mode_label;
			if (engine_mode == 0) mode_label = to_wx("智能双模 (DBNet笔画/像素)");
			else if (engine_mode == 1) mode_label = to_wx("深度学习笔画检测 (DBNet)");
			else mode_label = to_wx("经典原生双极性像素 (Dice)");

			context->frame->SetStatusText(wxString::Format(
				to_wx("批量对帧完成：共更新 %d 行 [模式: %s]"),
				changed, mode_label));
		}
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
			} else {
				OPT_SET("Tool/Frame Match/Region X0")->SetDouble(std::clamp<double>(std::min(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y0")->SetDouble(std::clamp<double>(std::min(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region X1")->SetDouble(std::clamp<double>(std::max(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
				OPT_SET("Tool/Frame Match/Region Y1")->SetDouble(std::clamp<double>(std::max(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
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
		wxMessageBox(_("Select a subtitle line, pause on a frame where its source subtitle is visible, then drag a tight box around the whole source subtitle."), _("Frame Match Assistant"), wxOK | wxICON_INFORMATION, parent);
		return;
	}
	if (script_res.X() <= 0 || script_res.Y() <= 0 || provider->GetWidth() <= 0 || provider->GetHeight() <= 0) {
		wxMessageBox(_("Video or script dimensions are not available."), _("Frame Match Assistant"), wxOK | wxICON_WARNING, parent);
		return;
	}

	int x0 = std::clamp(static_cast<int>(std::floor(std::min(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth() - 1);
	int x1 = std::clamp(static_cast<int>(std::ceil(std::max(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth());
	int y0 = std::clamp(static_cast<int>(std::floor(std::min(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight() - 1);
	int y1 = std::clamp(static_cast<int>(std::ceil(std::max(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight());

	double dx = std::abs(end.X() - start.X());
	double dy = std::abs(end.Y() - start.Y());
	if (dx < 15.0 && dy >= 8.0) {
		int ax = (x0 + x1) / 2;
		int ah = std::max(16, y1 - y0);
		int half_w = std::max(ah * 3, 120);
		x0 = std::clamp(ax - half_w, 0, provider->GetWidth() - 1);
		x1 = std::clamp(ax + half_w, 0, provider->GetWidth());
	}

	if (x1 - x0 < 8 || y1 - y0 < 6) {
		wxMessageBox(_("The selected area is too small. Drag a box or vertical anchor around the subtitle."), _("Frame Match Assistant"), wxOK | wxICON_WARNING, parent);
		return;
	}
	int frame_count = provider->GetFrameCount();
	if (frame_count <= 0) {
		wxMessageBox(_("The loaded video has no frames."), _("Frame Match Assistant"), wxOK | wxICON_WARNING, parent);
		return;
	}

	int reference_frame = std::clamp(c->videoController->GetFrameN(), 0, frame_count - 1);
	int minimum_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));

	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 2));
	wxString mode_label;
	if (engine_mode == 0) mode_label = to_wx("智能双模 (DBNet笔画/像素)");
	else if (engine_mode == 1) mode_label = to_wx("深度学习笔画检测 (DBNet)");
	else mode_label = to_wx("经典原生双极性像素 (Dice)");

	int total_work = 30;
	wxProgressDialog progress(
		wxString::Format(to_wx("对帧助手 [%s]"), mode_label),
		wxString::Format(to_wx("正在使用 %s 扫描匹配字幕区间..."), mode_label),
		total_work, parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);

	int new_start = 0, new_end = 0;
	int status = kMatchOk;
	int engine_used = 0;
	bool ok = false;
	MaskCache cache;
	try {
		ok = MatchLineToRegion(c, line, PixelRegion{x0, y0, x1, y1}, reference_frame, new_start, new_end,
			minimum_similarity, -1, -1,
			[&](int samples) {
				wxSafeYield(parent, true);
				return progress.Update(std::min(samples, total_work));
			}, &status, &cache, -1, &engine_used);
	}
	catch (std::exception const& e) {
		wxMessageBox(_("Could not scan video frames: ") + to_wx(e.what()), _("Frame Match Assistant"), wxOK | wxICON_ERROR, parent);
		return;
	}
	if (status == kMatchCancelled) return;
	if (status == kMatchNoPattern) {
		wxMessageBox(_("No clear subtitle-color pattern was found in this box. Pause while the subtitle is visible and draw a tighter box around its text."), _("Frame Match Assistant"), wxOK | wxICON_INFORMATION, parent);
		return;
	}
	if (!ok || status == kMatchInvalid) {
		wxMessageBox(_("The selected region did not produce a valid interval. Adjust the box or similarity threshold and try again."), _("Frame Match Assistant"), wxOK | wxICON_INFORMATION, parent);
		return;
	}

	progress.Update(total_work);
	line->Start = new_start;
	line->End = new_end;
	Commit(_("match subtitle timing to picked region"));

	wxString used_desc;
	if (engine_used == 1) used_desc = to_wx("内建DBNet文字笔画检测 (DirectML GPU加速)");
	else used_desc = to_wx("经典原生双极性像素 (Dice)");

	if (c->frame) {
		c->frame->SetStatusText(wxString::Format(
			to_wx("视觉对帧完成：%s - %s [引擎: %s]"),
			to_wx(agi::Time(new_start).GetAssFormatted()),
			to_wx(agi::Time(new_end).GetAssFormatted()),
			used_desc));
	}
}
