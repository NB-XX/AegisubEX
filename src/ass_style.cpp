// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file ass_style.cpp
/// @brief Class for style definitions in subtitles
/// @ingroup subs_storage
///

#include "ass_style.h"

#include "subtitle_format.h"
#include "utils.h"

#include <libaegisub/format.h>
#include <libaegisub/split.h>

#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <wx/intl.h>

AssStyle::AssStyle() {
	std::fill(Margin.begin(), Margin.end(), 10);

	UpdateData();
}

AssEntryGroup AssStyle::Group() const { return AssEntryGroup::STYLE; }

namespace {
class parser {
	agi::split_iterator<char> pos;

	std::string_view next_tok() {
		if (pos.eof())
			throw SubtitleFormatParseError("Malformed style: not enough fields");
		return agi::Trim(*pos++);
	}

public:
	parser(std::string_view str) {
		if (auto colon = str.find(':'); colon != str.npos) {
			str.remove_prefix(colon + 1);
			pos = agi::Split(str, ',');
		}
	}

	void check_done() const {
		if (!pos.eof())
			throw SubtitleFormatParseError("Malformed style: too many fields");
	}

	std::string next_str() { return std::string(next_tok()); }
	agi::Color next_color() { return next_tok(); }

	int next_int() {
		try {
			return boost::lexical_cast<int>(next_tok());
		}
		catch (boost::bad_lexical_cast const&) {
			throw SubtitleFormatParseError("Malformed style: bad int field");
		}
	}

	double next_double() {
		try {
			return boost::lexical_cast<double>(next_tok());
		}
		catch (boost::bad_lexical_cast const&) {
			throw SubtitleFormatParseError("Malformed style: bad double field");
		}
	}

	void skip_token() {
		if (!pos.eof())
			++pos;
	}
};
}

AssStyle::AssStyle(std::string_view str, int version) {
	parser p(str);

	name = p.next_str();
	font = p.next_str();
	fontsize = p.next_double();

	if (version != 0) {
		primary = p.next_color();
		secondary = p.next_color();
		outline = p.next_color();
		shadow = p.next_color();
	}
	else {
		primary = p.next_color();
		secondary = p.next_color();

		// Skip tertiary color
		p.skip_token();

		// Read shadow/outline color
		outline = p.next_color();
		shadow = outline;
	}

	bold = !!p.next_int();
	italic = !!p.next_int();

	if (version != 0) {
		underline = !!p.next_int();
		strikeout = !!p.next_int();

		scalex = p.next_double();
		scaley = p.next_double();
		spacing = p.next_double();
		angle = p.next_double();
	}
	else {
		// SSA defaults
		underline = false;
		strikeout = false;

		scalex = 100;
		scaley = 100;
		spacing = 0;
		angle = 0.0;
	}

	borderstyle = p.next_int();
	outline_w = p.next_double();
	shadow_w = p.next_double();
	alignment = p.next_int();

	if (version == 0)
		alignment = SsaToAss(alignment);

	Margin[0] = mid(-9999, p.next_int(), 99999);
	Margin[1] = mid(-9999, p.next_int(), 99999);
	Margin[2] = mid(-9999, p.next_int(), 99999);

	// Skip alpha level
	if (version == 0)
		p.skip_token();

	encoding = p.next_int();

	p.check_done();

	UpdateData();
}

void AssStyle::UpdateData() {
	replace(name.begin(), name.end(), ',', ';');
	replace(font.begin(), font.end(), ',', ';');

	data = agi::format("Style: %s,%s,%g,%s,%s,%s,%s,%d,%d,%d,%d,%g,%g,%g,%g,%d,%g,%g,%i,%i,%i,%i,%i",
		name, font, fontsize,
		primary.GetAssStyleFormatted(),
		secondary.GetAssStyleFormatted(),
		outline.GetAssStyleFormatted(),
		shadow.GetAssStyleFormatted(),
		(bold? -1 : 0), (italic ? -1 : 0),
		(underline ? -1 : 0), (strikeout ? -1 : 0),
		scalex, scaley, spacing, angle,
		borderstyle, outline_w, shadow_w, alignment,
		Margin[0], Margin[1], Margin[2], encoding);
}

std::string AssStyle::GetFxData() const {
	std::string data = agi::format("3,%d,%g,%g,%s,%d,%d,%zu",
		glow_enabled, std::max(0.0, glow_width), std::max(0.0, glow_blur), glow_color.GetAssStyleFormatted(),
		std::max(0, fade_in_ms), std::max(0, fade_out_ms), strokes.size());
	for (auto const& stroke : strokes)
		data += agi::format(",%g,%g,%s", std::max(0.0, stroke.width), std::max(0.0, stroke.blur), stroke.color.GetAssStyleFormatted());
	return data;
}

void AssStyle::SetFxData(std::string_view data) {
	std::vector<std::string_view> fields;
	for (size_t begin = 0; begin <= data.size();) {
		auto end = data.find(',', begin);
		fields.push_back(data.substr(begin, end == data.npos ? data.size() - begin : end - begin));
		if (end == data.npos) break;
		begin = end + 1;
	}
	auto integer = [&](size_t i) { return std::stoi(std::string(fields.at(i))); };
	auto number = [&](size_t i) { return std::stod(std::string(fields.at(i))); };
	try {
		if (fields.empty()) return;
		if (fields[0] == "1" && fields.size() == 7) {
			strokes.clear();
			if (integer(1)) strokes.push_back({number(2), 0.0, agi::Color(fields[3])});
			glow_enabled = false;
			fade_in_ms = fade_out_ms = 0;
			return;
		}
		bool next_glow_enabled = false;
		double next_glow_width = glow_width, next_glow_blur = glow_blur;
		agi::Color next_glow_color = glow_color;
		int next_fade_in = 0, next_fade_out = 0;
		size_t count = 0, first_stroke = 0;
		if (fields[0] == "2" && fields.size() >= 7) {
			// Version 2 stored a gradient; retain its strokes and fades while
			// intentionally dropping the non-native gradient effect.
			next_fade_in = std::clamp(integer(4), 0, 10000);
			next_fade_out = std::clamp(integer(5), 0, 10000);
			count = static_cast<size_t>(std::clamp(integer(6), 0, 32));
			first_stroke = 7;
		}
		else if (fields[0] == "3" && fields.size() >= 8) {
			next_glow_enabled = integer(1) != 0;
			next_glow_width = std::clamp(number(2), 0.0, 1000.0);
			next_glow_blur = std::clamp(number(3), 0.0, 100.0);
			next_glow_color = agi::Color(fields[4]);
			next_fade_in = std::clamp(integer(5), 0, 10000);
			next_fade_out = std::clamp(integer(6), 0, 10000);
			count = static_cast<size_t>(std::clamp(integer(7), 0, 32));
			first_stroke = 8;
		}
		else return;
		if (fields.size() != first_stroke + count * 3) return;
		std::vector<AssStyleStroke> next_strokes;
		next_strokes.reserve(count);
		for (size_t i = 0; i < count; ++i)
			next_strokes.push_back({std::clamp(number(first_stroke + i * 3), 0.0, 1000.0),
				std::clamp(number(first_stroke + 1 + i * 3), 0.0, 100.0), agi::Color(fields[first_stroke + 2 + i * 3])});
		strokes = std::move(next_strokes);
		glow_enabled = next_glow_enabled;
		glow_width = next_glow_width;
		glow_blur = next_glow_blur;
		glow_color = next_glow_color;
		fade_in_ms = next_fade_in;
		fade_out_ms = next_fade_out;
	}
	catch (...) { }
}

void AssStyle::GetEncodings(wxArrayString &encodingStrings) {
	encodingStrings.Clear();
	encodingStrings.Add(wxString("-1 - ") + _("Auto-detect base direction (libass only)"));
	encodingStrings.Add(wxString("0 - ") + _("ANSI"));
	encodingStrings.Add(wxString("1 - ") + _("Default"));
	encodingStrings.Add(wxString("2 - ") + _("Symbol"));
	encodingStrings.Add(wxString("77 - ") + _("Mac"));
	encodingStrings.Add(wxString("128 - ") + _("Shift_JIS"));
	encodingStrings.Add(wxString("129 - ") + _("Hangeul"));
	encodingStrings.Add(wxString("130 - ") + _("Johab"));
	encodingStrings.Add(wxString("134 - ") + _("GB2312"));
	encodingStrings.Add(wxString("136 - ") + _("Chinese BIG5"));
	encodingStrings.Add(wxString("161 - ") + _("Greek"));
	encodingStrings.Add(wxString("162 - ") + _("Turkish"));
	encodingStrings.Add(wxString("163 - ") + _("Vietnamese"));
	encodingStrings.Add(wxString("177 - ") + _("Hebrew"));
	encodingStrings.Add(wxString("178 - ") + _("Arabic"));
	encodingStrings.Add(wxString("186 - ") + _("Baltic"));
	encodingStrings.Add(wxString("204 - ") + _("Russian"));
	encodingStrings.Add(wxString("222 - ") + _("Thai"));
	encodingStrings.Add(wxString("238 - ") + _("East European"));
	encodingStrings.Add(wxString("255 - ") + _("OEM"));
}

int AssStyle::AssToSsa(int ass_align) {
	switch (ass_align) {
		case 1:  return 1;
		case 2:  return 2;
		case 3:  return 3;
		case 4:  return 9;
		case 5:  return 10;
		case 6:  return 11;
		case 7:  return 5;
		case 8:  return 6;
		case 9:  return 7;
		default: return 2;
	}
}

int AssStyle::SsaToAss(int ssa_align) {
	switch(ssa_align) {
		case 1:  return 1;
		case 2:  return 2;
		case 3:  return 3;
		case 5:  return 7;
		case 6:  return 8;
		case 7:  return 9;
		case 9:  return 4;
		case 10: return 5;
		case 11: return 6;
		default: return 2;
	}
}
