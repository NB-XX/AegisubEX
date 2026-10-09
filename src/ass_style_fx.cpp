#include "ass_style_fx.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"

#include <libaegisub/ass/string_codec.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::string_view marker = "AegisubFxGroupV1:";
constexpr std::string_view style_prefix = "__AegisubFx_";
struct RenderStyleSet {
	std::string base;
	std::optional<std::pair<std::string, double>> glow;
	std::vector<std::pair<std::string, double>> strokes;
};

RenderStyleSet make_render_styles(AssFile& file, AssStyle const& source, size_t id) {
	RenderStyleSet result;
	auto add = [&](AssStyle style, std::string suffix) {
		style.name = std::string(style_prefix) + std::to_string(id) + "_" + suffix;
		style.strokes.clear();
		style.glow_enabled = false;
		style.fade_in_ms = style.fade_out_ms = 0;
		style.UpdateData();
		std::string name = style.name;
		file.Styles.push_back(*new AssStyle(style));
		return name;
	};

	AssStyle base(source);
	result.base = add(base, "base");
	if (source.glow_enabled) {
		AssStyle glow(source);
		glow.primary.a = glow.shadow.a = 255;
		glow.outline = source.glow_color;
		glow.outline_w = source.outline_w + source.glow_width;
		result.glow = std::make_pair(add(glow, "glow"), source.glow_blur);
	}

	std::vector<size_t> stroke_order(source.strokes.size());
	for (size_t i = 0; i < stroke_order.size(); ++i) stroke_order[i] = i;
	std::stable_sort(stroke_order.begin(), stroke_order.end(), [&](size_t a, size_t b) {
		return source.strokes[a].width > source.strokes[b].width;
	});
	for (size_t i : stroke_order) {
		auto const& stroke = source.strokes[i];
		AssStyle outline(source);
		outline.primary.a = 255;
		outline.shadow.a = 255;
		outline.outline = stroke.color;
		outline.outline_w = source.outline_w + stroke.width;
		result.strokes.emplace_back(add(outline, "stroke" + std::to_string(i)), stroke.blur);
	}

	return result;
}

std::string fixed_position(AssDialogue const& line, AssStyle const& style, int play_res_x, int play_res_y) {
	auto const& text = line.Text.get();
	// Explicitly positioned and moving subtitles already opt out of ASS
	// collision placement, so keep their author-specified coordinates.
	if (text.find("\\pos(") != std::string::npos || text.find("\\move(") != std::string::npos)
		return {};

	int alignment = style.alignment;
	for (size_t pos = text.find("\\an"); pos != std::string::npos; pos = text.find("\\an", pos + 1)) {
		auto digit = pos + 3;
		if (digit < text.size() && text[digit] >= '1' && text[digit] <= '9')
			alignment = text[digit] - '0';
	}
	int margin_l = line.Margin[0] ? line.Margin[0] : style.Margin[0];
	int margin_r = line.Margin[1] ? line.Margin[1] : style.Margin[1];
	int margin_v = line.Margin[2] ? line.Margin[2] : style.Margin[2];
	int x = alignment % 3 == 1 ? margin_l : alignment % 3 == 2 ? play_res_x / 2 : play_res_x - margin_r;
	int vertical = (alignment - 1) / 3;
	int y = vertical == 0 ? play_res_y - margin_v : vertical == 1 ? play_res_y / 2 : margin_v;
	return "{\\pos(" + std::to_string(x) + "," + std::to_string(y) + ")}";
}

void append_layer(std::vector<std::unique_ptr<AssDialogue>>& out, AssDialogue const& source,
		std::string const& style, std::string prefix, int layer, int fade_in_ms, int fade_out_ms, bool hide_fill_and_shadow = false) {
	auto line = std::make_unique<AssDialogue>(source);
	line->Style = style;
	line->Layer = layer;
	if (hide_fill_and_shadow)
		prefix = "{\\1a&HFF&\\2a&HFF&\\4a&HFF&}" + prefix;
	if (fade_in_ms > 0 || fade_out_ms > 0)
		prefix = "{\\fad(" + std::to_string(fade_in_ms) + "," + std::to_string(fade_out_ms) + ")}" + prefix;
	line->Text = prefix + source.Text.get();
	out.push_back(std::move(line));
}
}


void ExpandStyleEffects(AssFile& file, bool markers) {
	int play_res_x = file.GetScriptInfoAsInt("PlayResX");
	int play_res_y = file.GetScriptInfoAsInt("PlayResY");
	if (play_res_x <= 0) play_res_x = 1920;
	if (play_res_y <= 0) play_res_y = 1080;
	std::map<std::string, RenderStyleSet, std::less<>> render_styles;
	std::vector<std::unique_ptr<AssDialogue>> expanded;
	for (auto const& line : file.Events) {
		auto source_style = file.GetStyle(line.Style);
		if (line.Comment || !source_style || !source_style->HasStyleFx()) {
			expanded.push_back(std::make_unique<AssDialogue>(line));
			continue;
		}

		auto [it, inserted] = render_styles.try_emplace(line.Style);
		if (inserted)
			it->second = make_render_styles(file, *source_style, render_styles.size());
		auto const& styles = it->second;
		int group_size = 1 + static_cast<int>(styles.strokes.size()) + (styles.glow ? 1 : 0);
		if (markers) {
			auto comment = std::make_unique<AssDialogue>();
			comment->Comment = true;
			comment->Layer = line.Layer;
			comment->Start = line.Start;
			comment->End = line.End;
			comment->Style = line.Style;
			comment->Text = std::string(marker) + std::to_string(group_size) + ":" + agi::ass::inline_string_encode(line.GetEntryData());
			expanded.push_back(std::move(comment));
		}
		auto pos_tags = fixed_position(line, *source_style, play_res_x, play_res_y);
		int layer = line.Layer;

		if (styles.glow) {
			auto const& [glow_style, blur] = *styles.glow;
			std::string glow_tags = pos_tags;
			if (blur > 0.0) {
				auto blur_tag = "\\blur" + std::to_string(blur);
				glow_tags = pos_tags.empty() ? "{" + blur_tag + "}" : pos_tags.substr(0, pos_tags.size() - 1) + blur_tag + "}";
			}
			append_layer(expanded, line, glow_style, std::move(glow_tags), layer++, source_style->fade_in_ms, source_style->fade_out_ms, true);
		}
		for (auto const& [stroke_style, blur] : styles.strokes) {
			std::string stroke_tags = pos_tags;
			if (blur > 0.0) stroke_tags = pos_tags.empty() ? "{\\blur" + std::to_string(blur) + "}" : pos_tags.substr(0, pos_tags.size() - 1) + "\\blur" + std::to_string(blur) + "}";
			append_layer(expanded, line, stroke_style, std::move(stroke_tags), layer++, source_style->fade_in_ms, source_style->fade_out_ms, true);
		}
		append_layer(expanded, line, styles.base, std::move(pos_tags), layer, source_style->fade_in_ms, source_style->fade_out_ms);
	}

	file.Events.clear_and_dispose([](AssDialogue *entry) { delete entry; });
	for (auto& line : expanded)
		file.Events.push_back(*line.release());
}

void CollapseStyleEffects(AssFile& file) {
	std::vector<std::unique_ptr<AssDialogue>> collapsed;
	for (auto it = file.Events.begin(); it != file.Events.end();) {
		auto& line = *it++;
		std::string text = line.Text.get();
		if (!line.Comment || !text.starts_with(marker)) {
			collapsed.push_back(std::make_unique<AssDialogue>(line));
			continue;
		}

		auto count_end = text.find(':', marker.size());
		if (count_end == std::string::npos) {
			collapsed.push_back(std::make_unique<AssDialogue>(line));
			continue;
		}
		int count = 0;
		try { count = std::stoi(text.substr(marker.size(), count_end - marker.size())); }
		catch (...) { count = 0; }
		if (count <= 0 || std::distance(it, file.Events.end()) < count) {
			collapsed.push_back(std::make_unique<AssDialogue>(line));
			continue;
		}

		try {
			auto original = agi::ass::inline_string_decode(text.substr(count_end + 1));
			collapsed.push_back(std::make_unique<AssDialogue>(original));
			std::advance(it, count);
		}
		catch (...) {
			collapsed.push_back(std::make_unique<AssDialogue>(line));
		}
	}

	file.Events.clear_and_dispose([](AssDialogue *entry) { delete entry; });
	for (auto& line : collapsed)
		file.Events.push_back(*line.release());

	file.Styles.remove_and_dispose_if([](AssStyle const& style) {
		return style.name.starts_with(style_prefix);
	}, [](AssStyle *style) { delete style; });
}
