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
#include "subocr_engine.h"

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
	if (frame_count <= 0 || region.x1 - region.x0 < 12 || region.y1 - region.y0 < 6) {
		set_status(kMatchNoPattern);
		return false;
	}
	reference_frame = std::clamp(reference_frame, 0, frame_count - 1);

	auto ref_f = provider->GetFrame(reference_frame, c->videoController->TimeAtFrame(reference_frame), true);
	double coord_scale_x = (provider->GetWidth() > 0 && ref_f->width > 0) ? (static_cast<double>(ref_f->width) / provider->GetWidth()) : 1.0;
	double coord_scale_y = (provider->GetHeight() > 0 && ref_f->height > 0) ? (static_cast<double>(ref_f->height) / provider->GetHeight()) : 1.0;

	// 1. 定位文字紧致边界 (Tight Bounding Box)
	int s_x0 = static_cast<int>(std::round(region.x0 * coord_scale_x));
	int s_y0 = static_cast<int>(std::round(region.y0 * coord_scale_y));
	int s_x1 = static_cast<int>(std::round(region.x1 * coord_scale_x));
	int s_y1 = static_cast<int>(std::round(region.y1 * coord_scale_y));
	int out_x0 = s_x0, out_y0 = s_y0, out_x1 = s_x1, out_y1 = s_y1;
	bool has_box = DBNetDetector::Get().GetTextBoundingBox(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		s_x0, s_y0, s_x1, s_y1,
		out_x0, out_y0, out_x1, out_y1);

	int crop_x0 = has_box ? out_x0 : s_x0;
	int crop_y0 = has_box ? out_y0 : s_y0;
	int crop_x1 = has_box ? out_x1 : s_x1;
	int crop_y1 = has_box ? out_y1 : s_y1;

	// 提取基准帧的文字笔画概率图
	DBNetResult ref_db = DBNetDetector::Get().Detect(
		ref_f->data.data(), ref_f->width, ref_f->height, ref_f->pitch, ref_f->flipped,
		crop_x0, crop_y0, crop_x1, crop_y1);

	if (!ref_db.has_text || ref_db.active_pixels < 8) {
		set_status(kMatchNoPattern);
		return false;
	}

	int samples_done = 0;
	bool cancelled = false;
	std::map<int, DBNetResult> db_cache;

	auto detect_frame = [&](int frame_no) -> const DBNetResult& {
		auto it = db_cache.find(frame_no);
		if (it == db_cache.end()) {
			auto fr = provider->GetFrame(frame_no, c->videoController->TimeAtFrame(frame_no), true);
			DBNetResult r = DBNetDetector::Get().Detect(
				fr->data.data(), fr->width, fr->height, fr->pitch, fr->flipped,
				crop_x0, crop_y0, crop_x1, crop_y1);
			it = db_cache.emplace(frame_no, std::move(r)).first;
			++samples_done;
			if (on_sample && !on_sample(samples_done)) cancelled = true;
		}
		return it->second;
	};

	db_cache.emplace(reference_frame, ref_db);

	int reference_ms = c->videoController->TimeAtFrame(reference_frame);
	constexpr int search_ms = 10000;
	int left_limit = std::clamp(c->videoController->FrameAtTime(std::max(0, reference_ms - search_ms), agi::vfr::START), 0, reference_frame);
	int video_end = static_cast<int>(c->videoController->TimeAtFrame(frame_count - 1, agi::vfr::END));
	int right_limit = std::clamp(c->videoController->FrameAtTime(std::min(video_end, reference_ms + search_ms), agi::vfr::END), reference_frame, frame_count - 1);

	if (bound_lo_ms >= 0)
		left_limit = std::max(left_limit, std::min(c->videoController->FrameAtTime(std::max(0, bound_lo_ms), agi::vfr::START), reference_frame));
	if (bound_hi_ms >= 0)
		right_limit = std::min(right_limit, std::max(c->videoController->FrameAtTime(std::min(video_end, bound_hi_ms), agi::vfr::END), reference_frame));
	int coarse_step = std::max(1, provider->GetFPS().FrameAtTime(500));

	auto neural_similarity = [&](int frame_no) -> double {
		if (frame_no == reference_frame) return 100.0;
		const auto& cand = detect_frame(frame_no);
		if (!cand.has_text || cand.max_prob < 0.35f || cand.active_pixels < std::max(6, static_cast<int>(ref_db.active_pixels * 0.20f)))
			return 0.0;
		int inter = 0;
		size_t n = std::min(ref_db.prob_mask.size(), cand.prob_mask.size());
		for (size_t i = 0; i < n; ++i) {
			if (ref_db.prob_mask[i] && cand.prob_mask[i]) ++inter;
		}
		return 200.0 * inter / std::max(1, ref_db.active_pixels + cand.active_pixels);
	};

	double threshold = std::max(40.0, minimum_similarity * 0.70);
	auto matches = [&](int f) {
		if (cancelled) return true;
		return neural_similarity(f) >= threshold;
	};

	// 链式接缝优化探针
	if (chain_hint_frame >= 0) {
		int seam = std::clamp(chain_hint_frame, 0, frame_count - 1);
		int hits = 0;
		for (int f = seam; f <= seam + 3 && f < frame_count; ++f)
			if (matches(f)) ++hits;
		if (hits >= 3) {
			int seam_ms = c->videoController->TimeAtFrame(seam);
			int dur_ms = std::max(1000, static_cast<int>(line->End) - static_cast<int>(line->Start));
			left_limit = std::clamp(c->videoController->FrameAtTime(std::max(0, seam_ms - 300), agi::vfr::START), 0, reference_frame);
			right_limit = std::clamp(c->videoController->FrameAtTime(std::min(video_end, seam_ms + dur_ms + 1200), agi::vfr::END), reference_frame, frame_count - 1);
		}
	}

	// 扫描起始帧
	int first_match = reference_frame, last_match = reference_frame;
	bool found_start = false, found_end = false;
	int near_match = reference_frame;
	for (int candidate = reference_frame - coarse_step; candidate >= left_limit && !cancelled; candidate -= coarse_step) {
		if (!matches(candidate)) {
			int b = candidate + 1;
			while (b < near_match && !cancelled && !matches(b)) ++b;
			if (cancelled) break;
			first_match = b;
			found_start = true;
			break;
		}
		near_match = candidate;
	}
	if (cancelled) { set_status(kMatchCancelled); return false; }

	// 扫描结束帧
	near_match = reference_frame;
	for (int candidate = reference_frame + coarse_step; candidate <= right_limit && !cancelled; candidate += coarse_step) {
		if (!matches(candidate)) {
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
	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
	// 0: Auto (DBNet -> Classic; SubtitleOCR if available)
	// 1: SubtitleOCR External Engine (望言OCR)
	// 2: Native DBNet Neural Matcher
	// 3: Classic Native Pixel Matcher

	// 模式 1: 望言 OCR (SubtitleOCR C-ABI 引擎，仅当明确可用或用户显式指定时尝试)
	if (engine_mode == 1 || (engine_mode == 0 && SubocrEngine::Get().IsAvailable())) {
		std::string v_path = c->project->VideoName().string();
		if (!v_path.empty()) {
			std::string custom_subocr_dir = OPT_GET("Tool/Frame Match/SubtitleOCR Path")->GetString();
			if (!SubocrEngine::Get().IsAvailable() && !custom_subocr_dir.empty()) {
				SubocrEngine::Get().Load(custom_subocr_dir);
			}
			if (SubocrEngine::Get().IsAvailable()) {
				int s_ms = 0, e_ms = 0;
				int ref_ms = c->videoController->TimeAtFrame(reference_frame);
				if (SubocrEngine::Get().MatchLine(v_path, ref_ms, region.x0, region.y0, region.x1, region.y1, s_ms, e_ms)) {
					if (bound_lo_ms >= 0) s_ms = std::max(s_ms, bound_lo_ms);
					if (bound_hi_ms >= 0) e_ms = std::min(e_ms, bound_hi_ms);
					if (e_ms > s_ms) {
						new_start = s_ms;
						new_end = e_ms;
						if (status) *status = kMatchOk;
						if (out_engine_used) *out_engine_used = 1;
						return true;
					}
				}
			}
		}
		if (engine_mode == 1) {
			if (status) *status = kMatchNoPattern;
			return false;
		}
	}

	// 模式 2: 内建 DBNet 神经网络特征对帧
	if (engine_mode == 2 || (engine_mode == 0 && DBNetDetector::Get().IsAvailable())) {
		int neural_status = kMatchOk;
		if (MatchLineDBNetNeural(c, line, region, reference_frame, new_start, new_end,
				minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, &neural_status, chain_hint_frame)) {
			if (status) *status = neural_status;
			if (out_engine_used) *out_engine_used = 2;
			return true;
		}
		if (engine_mode == 2) {
			if (status) *status = neural_status;
			return false;
		}
	}

	// 模式 3: 经典原生像素 / Dice 掩模对帧（原版方案保留备选）
	bool matched = MatchLineNativePixel(c, line, region, reference_frame, new_start, new_end,
		minimum_similarity, bound_lo_ms, bound_hi_ms, on_sample, status, shared_cache, chain_hint_frame);
	if (matched && out_engine_used) *out_engine_used = 3;
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
	auto try_match = [&](AssDialogue *line, size_t pos, int similarity, int lo, int hi, int hint_frame = -1) {
		int start = line->Start, end = line->End;
		try {
			if (MatchLineToRegion(context, line, region, ref_frame(line), start, end, similarity, lo, hi, nullptr, nullptr, nullptr, hint_frame))
				cur[pos] = { start, end };
		}
		catch (...) { }
	};

	wxProgressDialog progress(_("Batch Frame Match"), _("Matching selected subtitle rows using the saved region..."), static_cast<int>(lines.size()) * 3 + 1, context->parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
	bool aborted = false;
	// Chain across rows: the frame right after the previous matched row's end
	// is usually this row's first frame. When the rows are roughly back-to-back
	// (gap within 1.5 s) that seam is passed as a hint, and the matcher probes
	// a few frames there and narrows its search to the seam when they light
	// up. Distant rows (sparse selections, scene gaps) get no hint and take
	// the regular wide search.
	int last_end_ms = -1;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (!progress.Update(static_cast<int>(i), wxString::Format(_("Matching row %d of %d..."), static_cast<int>(i + 1), static_cast<int>(lines.size())))) { aborted = true; break; }
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
			if (!progress.Update(static_cast<int>(lines.size()) * (1 + round), round == 1
				? _("Re-running rows that collided with a neighbour...")
				: _("Re-running collided rows with a stricter threshold..."))) break;
			int similarity = std::min(95, base_similarity + round * rerun_step);
			for (auto& [pos, b] : bounds) {
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
			int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
			wxString mode_label;
			if (engine_mode == 0) mode_label = _("Auto (自动: DBNet/像素)");
			else if (engine_mode == 1) mode_label = _("SubtitleOCR (望言)");
			else if (engine_mode == 2) mode_label = _("Native DBNet (内建神经网络)");
			else mode_label = _("Classic Pixel (经典原生像素)");

			context->frame->SetStatusText(wxString::Format(
				_("Batch frame match completed: %d rows updated [Mode: %s]"),
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
		if (script_res.X() > 0 && script_res.Y() > 0) {
			OPT_SET("Tool/Frame Match/Region X0")->SetDouble(std::clamp<double>(std::min(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
			OPT_SET("Tool/Frame Match/Region Y0")->SetDouble(std::clamp<double>(std::min(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
			OPT_SET("Tool/Frame Match/Region X1")->SetDouble(std::clamp<double>(std::max(start.X(), end.X()) / script_res.X(), 0.0, 1.0));
			OPT_SET("Tool/Frame Match/Region Y1")->SetDouble(std::clamp<double>(std::max(start.Y(), end.Y()) / script_res.Y(), 0.0, 1.0));
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
	if (!has_region) return;
	gl.SetLineColour(wxColour(255, 190, 40), 1.f, 2);
	gl.SetFillColour(wxColour(255, 190, 40), .15f);
	gl.DrawRectangle(FromScriptCoords(start), FromScriptCoords(end));
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
	if (x1 - x0 < 12 || y1 - y0 < 6) {
		wxMessageBox(_("The selected area is too small. Drag a box around the whole subtitle."), _("Frame Match Assistant"), wxOK | wxICON_WARNING, parent);
		return;
	}
	int frame_count = provider->GetFrameCount();
	if (frame_count <= 0) {
		wxMessageBox(_("The loaded video has no frames."), _("Frame Match Assistant"), wxOK | wxICON_WARNING, parent);
		return;
	}

	int reference_frame = std::clamp(c->videoController->GetFrameN(), 0, frame_count - 1);
	int minimum_similarity = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt(), 50, 95));

	int engine_mode = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
	wxString mode_label;
	if (engine_mode == 0) mode_label = _("Auto (自动: DBNet/像素)");
	else if (engine_mode == 1) mode_label = _("SubtitleOCR (望言)");
	else if (engine_mode == 2) mode_label = _("Native DBNet (内建神经网络)");
	else mode_label = _("Classic Pixel (经典原生像素)");

	// Fresh decodes for one line: one ascending pass over the ~0.5 s lattice
	// across the ±10 s window, plus a short forward walk refining each
	// boundary. The matcher reports every frame it actually decodes, and
	// clamps at this total. Batch runs share a decode cache, so later rows
	// usually decode far fewer.
	int total_work = 66;
	wxProgressDialog progress(
		wxString::Format(_("Frame Match Assistant [%s]"), mode_label),
		wxString::Format(_("Comparing subtitle pattern with %s..."), mode_label),
		total_work, parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);

	int new_start = 0, new_end = 0;
	int status = kMatchOk;
	int engine_used = 0;
	bool ok = false;
	MaskCache cache;
	try {
		ok = MatchLineToRegion(c, line, PixelRegion{x0, y0, x1, y1}, reference_frame, new_start, new_end,
			minimum_similarity, -1, -1,
			[&](int samples) { return progress.Update(std::min(samples, total_work)); }, &status, &cache, -1, &engine_used);
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
	if (engine_used == 1) used_desc = _("SubtitleOCR (望言OCR)");
	else if (engine_used == 2) used_desc = _("Native DBNet Neural (内建DBNet神经网络)");
	else used_desc = _("Classic Native Pixel (经典原生双极性像素)");

	if (c->frame) {
		c->frame->SetStatusText(wxString::Format(
			_("Visual frame match succeeded: %s - %s [Engine: %s]"),
			to_wx(agi::Time(new_start).GetAssFormatted()),
			to_wx(agi::Time(new_end).GetAssFormatted()),
			used_desc));
	}
}
