#include "subtitle_format_audit.h"

#include "compat.h"
#include "text_file_reader.h"
#include "utils.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
struct Finding {
	int line;
	bool error;
	std::string issue;
	std::string suggestion;
};

std::vector<std::string> split_fields(std::string_view text, size_t max_fields = 0) {
	std::vector<std::string> fields;
	size_t begin = 0;
	while (begin <= text.size()) {
		auto end = text.find(',', begin);
		if (max_fields && fields.size() + 1 == max_fields) end = std::string_view::npos;
		fields.emplace_back(text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
		boost::trim(fields.back());
		if (end == std::string_view::npos) break;
		begin = end + 1;
	}
	return fields;
}

bool integer(std::string const& value) {
	try { (void)boost::lexical_cast<long long>(value); return true; }
	catch (...) { return false; }
}

bool number(std::string const& value) {
	try {
		size_t used = 0;
		std::stod(value, &used);
		return used == value.size();
	}
	catch (...) { return false; }
}

bool ass_time(std::string const& value) {
	static std::regex const pattern(R"(^\d{1,2}:[0-5]\d:[0-5]\d\.\d{2}$)");
	return std::regex_match(value, pattern);
}

void audit_ass(std::vector<std::string> const& lines, std::vector<Finding>& findings) {
	enum class Section { OTHER, STYLES, EVENTS } section = Section::OTHER;
	bool legacy_ssa_styles = false;
	std::set<std::string> styles;
	std::vector<std::pair<int, std::string>> event_styles;
	for (size_t i = 0; i < lines.size(); ++i) {
		auto const& raw = lines[i];
		std::string line = raw;
		boost::trim(line);
		auto lower = line;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
		if (lower == "[v4+ styles]") { section = Section::STYLES; legacy_ssa_styles = false; continue; }
		if (lower == "[v4 styles]") { section = Section::STYLES; legacy_ssa_styles = true; continue; }
		if (lower == "[events]") { section = Section::EVENTS; continue; }
		if (!line.empty() && line[0] == '[') { section = Section::OTHER; continue; }
		if (line.empty() || line[0] == ';') continue;
		if (raw.find("\xEF\xBF\xBD") != std::string::npos)
			findings.push_back({static_cast<int>(i + 1), false, "文本含有替换字符，可能是编码识别错误", "尝试“使用字符集打开”，检查 UTF-8、GBK 或 Big5。"});

		if (section == Section::STYLES && line.starts_with("Style:")) {
			auto fields = split_fields(std::string_view(line).substr(6));
			int expected_fields = legacy_ssa_styles ? 18 : 23;
			if (fields.size() != static_cast<size_t>(expected_fields)) {
				findings.push_back({static_cast<int>(i + 1), true, "样式字段数量错误：应为 " + std::to_string(expected_fields) + " 项，实际为 " + std::to_string(fields.size()), legacy_ssa_styles ? "检查 [V4 Styles] 的旧版 SSA 字段顺序和逗号。" : "检查逗号是否缺失或多出；确认样式行符合 [V4+ Styles] 的字段顺序。"});
				continue;
			}
			styles.insert(fields[0]);
			if (!number(fields[2])) findings.push_back({static_cast<int>(i + 1), true, "字体大小不是有效数字：" + fields[2], "填写数字，例如 48 或 36.5。"});
			auto integer_fields = legacy_ssa_styles ? std::vector<int>{7, 8, 9, 12, 13, 14, 15, 17} : std::vector<int>{7, 8, 9, 10, 15, 18, 19, 20, 21, 22};
			auto number_fields = legacy_ssa_styles ? std::vector<int>{10, 11, 16} : std::vector<int>{11, 12, 13, 14, 16, 17};
			for (int f : integer_fields)
				if (!integer(fields[f])) findings.push_back({static_cast<int>(i + 1), true, "样式字段“" + std::to_string(f + 1) + "”应为整数：" + fields[f], "检查粗体、斜体、边框、对齐、边距和编码字段。"});
			for (int f : number_fields)
				if (!number(fields[f])) findings.push_back({static_cast<int>(i + 1), true, "样式数值字段格式错误：" + fields[f], "该字段应为数字，请删除多余字符或替换错误的小数符号。"});
			static std::regex const color(R"(^&H(?:[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$)");
			for (int f : {3, 4, 5, 6})
				if (!std::regex_match(fields[f], color)) findings.push_back({static_cast<int>(i + 1), true, "颜色字段格式错误：" + fields[f], "ASS 颜色使用 &HBBGGRR 或 &HAABBGGRR 格式，例如 &H00FFFFFF。"});
		}
		else if (section == Section::EVENTS && (line.starts_with("Dialogue:") || line.starts_with("Comment:"))) {
			size_t prefix = line.starts_with("Dialogue:") ? 9 : 8;
			if (line.size() > prefix && line[prefix] == ' ') ++prefix;
			auto fields = split_fields(std::string_view(line).substr(prefix), 10);
			if (fields.size() != 10) {
				findings.push_back({static_cast<int>(i + 1), true, "字幕行字段不足：需要 Layer、起止时间、样式等 10 项", "检查逗号和字段顺序；Text 内容中的逗号可以保留。"});
				continue;
			}
			if (!integer(fields[0]) && !fields[0].starts_with("Marked=")) findings.push_back({static_cast<int>(i + 1), true, "图层编号不是整数：" + fields[0], "将 Layer 改为整数，例如 0。"});
			if (!ass_time(fields[1])) findings.push_back({static_cast<int>(i + 1), true, "开始时间格式错误：" + fields[1], "ASS 时间格式为 H:MM:SS.cc，例如 0:01:23.45。"});
			if (!ass_time(fields[2])) findings.push_back({static_cast<int>(i + 1), true, "结束时间格式错误：" + fields[2], "ASS 时间格式为 H:MM:SS.cc，例如 0:01:25.00。"});
			if (ass_time(fields[1]) && ass_time(fields[2]) && agi::Time(fields[2]) <= agi::Time(fields[1]))
				findings.push_back({static_cast<int>(i + 1), true, "结束时间不晚于开始时间", "确认起止时间没有写反，且字幕时长大于 0。"});
			for (int f : {5, 6, 7}) if (!integer(fields[f])) findings.push_back({static_cast<int>(i + 1), true, "字幕边距字段不是整数：" + fields[f], "MarginL、MarginR、MarginV 都应为整数。"});
			event_styles.emplace_back(static_cast<int>(i + 1), fields[3]);
		}
	}
	for (auto const& [line, style] : event_styles)
		if (!style.empty() && !styles.empty() && !styles.contains(style))
			findings.push_back({line, false, "引用了未定义的样式：" + style, "在 [V4+ Styles] 添加同名样式，或修正此行的 Style 名称。"});
}

void audit_srt(std::vector<std::string> const& lines, std::vector<Finding>& findings) {
	auto is_digits = [](std::string const& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); };
	std::regex timestamp(R"(^\s*\d{1,}:\d{1,2}:\d{1,2}[,.]\d{1,3}\s*-->\s*\d{1,}:\d{1,2}:\d{1,2}[,.]\d{1,3}\s*$)");
	size_t i = 0;
	while (i < lines.size()) {
		while (i < lines.size() && lines[i].empty()) ++i;
		if (i >= lines.size()) break;
		int block_line = static_cast<int>(i + 1);
		if (is_digits(lines[i])) ++i;
		if (i >= lines.size() || !std::regex_match(lines[i], timestamp)) {
			findings.push_back({static_cast<int>(i < lines.size() ? i + 1 : lines.size()), true, "未找到有效的 SRT 时间戳行", "格式示例：00:00:01,200 --> 00:00:03,400；检查箭头、冒号和毫秒逗号。"});
			while (i < lines.size() && !lines[i].empty()) ++i;
			continue;
		}
		std::string stamp = lines[i++];
		std::smatch match;
		if (std::regex_match(stamp, match, timestamp)) {
			// Validate clock ranges and compare normalized millisecond values.
			auto parse_stamp = [](std::string part) -> long long {
				std::replace(part.begin(), part.end(), ',', '.');
				int h = 0, m = 0, s = 0, ms = 0;
				char c1, c2, c3;
				std::istringstream in(part);
				in >> h >> c1 >> m >> c2 >> s >> c3 >> ms;
				if (!in || c1 != ':' || c2 != ':' || (c3 != ',' && c3 != '.')) return -1;
				if (m > 59 || s > 59 || ms < 0 || ms > 999) return -1;
				return ((static_cast<long long>(h) * 60 + m) * 60 + s) * 1000 + ms;
			};
			// The captures are checked below with a separate arrow split to allow whitespace variants.
			auto arrow = stamp.find("-->");
			if (arrow != std::string::npos) {
				auto lhs = stamp.substr(0, arrow), rhs = stamp.substr(arrow + 3);
				boost::trim(lhs); boost::trim(rhs);
				auto start = parse_stamp(lhs), end = parse_stamp(rhs);
				if (start < 0 || end < 0) findings.push_back({block_line + (is_digits(lines[block_line - 1]) ? 1 : 0), true, "时间戳中的分钟、秒或毫秒超出范围", "分钟和秒应小于 60，毫秒应为 0–999。"});
				else if (end <= start) findings.push_back({block_line + (is_digits(lines[block_line - 1]) ? 1 : 0), true, "结束时间不晚于开始时间", "确认时间没有写反，且字幕时长大于 0。"});
			}
		}
		if (i >= lines.size() || lines[i].empty()) findings.push_back({block_line, false, "字幕块没有正文文本", "在时间戳下一行添加字幕内容。"});
		while (i < lines.size() && !lines[i].empty()) ++i;
	}
}
}

bool ShowSubtitleFormatAudit(wxWindow *parent, agi::fs::path const& filename, char const *encoding, std::string const& parser_error) {
	auto ext = filename.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
	if (ext != ".ass" && ext != ".ssa" && ext != ".srt") return false;

	std::vector<std::string> lines;
	try {
		TextFileReader reader(filename, encoding);
		while (reader.HasMoreLines()) lines.push_back(reader.ReadLineFromFile());
	}
	catch (...) { return false; }

	std::vector<Finding> findings;
	if (ext == ".srt") audit_srt(lines, findings);
	else audit_ass(lines, findings);
	if (findings.empty()) {
		int error_line = 0;
		auto marker = parser_error.find("line ");
		if (marker != std::string::npos) {
			try { error_line = std::stoi(parser_error.substr(marker + 5)); } catch (...) { }
		}
		if (error_line <= 0) {
			for (size_t i = 0; i < lines.size(); ++i)
				if (parser_error.find(lines[i]) != std::string::npos && !lines[i].empty()) { error_line = static_cast<int>(i + 1); break; }
		}
		findings.push_back({error_line, true, "解析器拒绝了文件内容：" + parser_error, "检查对应行的字段、时间戳和字符编码；若错误描述未指出字段，可复制该行到审查器或手动修正。"});
	}

	int errors = static_cast<int>(std::count_if(findings.begin(), findings.end(), [](Finding const& f) { return f.error; }));
	wxDialog dialog(parent, wxID_ANY, to_wx("字幕格式审查"), wxDefaultPosition, wxSize(900, 500), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto outer = new wxBoxSizer(wxVERTICAL);
	auto summary = wxString::Format(to_wx("发现 %d 个错误、%d 个提醒。字幕未载入，当前文档没有被修改。"), errors, static_cast<int>(findings.size()) - errors);
	outer->Add(new wxStaticText(&dialog, wxID_ANY, summary), 0, wxALL | wxEXPAND, 10);
	auto list = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);
	list->InsertColumn(0, to_wx("级别"), wxLIST_FORMAT_LEFT, 70);
	list->InsertColumn(1, to_wx("行号"), wxLIST_FORMAT_RIGHT, 55);
	list->InsertColumn(2, to_wx("问题"), wxLIST_FORMAT_LEFT, 330);
	list->InsertColumn(3, to_wx("建议修复"), wxLIST_FORMAT_LEFT, 390);
	for (size_t i = 0; i < findings.size(); ++i) {
		auto const& f = findings[i];
		long row = list->InsertItem(static_cast<long>(i), to_wx(f.error ? "错误" : "提醒"));
		list->SetItem(row, 1, f.line > 0 ? std::to_string(f.line) : "—");
		list->SetItem(row, 2, to_wx(f.issue));
		list->SetItem(row, 3, to_wx(f.suggestion));
	}
	outer->Add(list, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
	auto detail = new wxTextCtrl(&dialog, wxID_ANY, to_wx("原始解析错误：" + parser_error), wxDefaultPosition, wxDefaultSize, wxTE_READONLY | wxTE_MULTILINE | wxTE_RICH2);
	detail->SetMinSize(wxSize(-1, 55));
	outer->Add(detail, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
	auto buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->AddStretchSpacer();
	buttons->Add(new wxButton(&dialog, wxID_OK, to_wx("关闭")), 0, wxRIGHT | wxBOTTOM, 10);
	outer->Add(buttons, 0, wxEXPAND);
	dialog.SetSizer(outer);
	dialog.CentreOnParent();
	dialog.ShowModal();
	return true;
}
