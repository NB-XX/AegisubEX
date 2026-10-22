// AI 辅助翻译内嵌实现：
//   功能1：在视频画面上直接框选截图，调用百炼 qwen 视觉模型识别并翻译；
//   功能2：按选中行时间轴切取音频，调用百炼 omni 模型转写并翻译。
// 结果在右下角常驻面板展示，可编辑后一键填入/替换当前行或选中行（支持撤销）。

#include "ai_assistant.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"
#include "visual_tool_cross.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/filename.h>
#include <wx/imagjpeg.h>
#include <wx/mstream.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#ifdef WITH_UPDATE_CHECKER
#include <curl/curl.h>
#endif
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

// ---------------------------------------------------------------- 基础工具

const char* const b64chars =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const char* data, size_t len) {
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	size_t i = 0;
	while (i + 2 < len) {
		uint32_t v = (static_cast<uint8_t>(data[i]) << 16)
			| (static_cast<uint8_t>(data[i+1]) << 8)
			| static_cast<uint8_t>(data[i+2]);
		out += b64chars[(v >> 18) & 63];
		out += b64chars[(v >> 12) & 63];
		out += b64chars[(v >> 6) & 63];
		out += b64chars[v & 63];
		i += 3;
	}
	if (i + 1 == len) {
		uint32_t v = static_cast<uint8_t>(data[i]) << 16;
		out += b64chars[(v >> 18) & 63];
		out += b64chars[(v >> 12) & 63];
		out += "==";
	} else if (i + 2 == len) {
		uint32_t v = (static_cast<uint8_t>(data[i]) << 16)
			| (static_cast<uint8_t>(data[i+1]) << 8);
		out += b64chars[(v >> 18) & 63];
		out += b64chars[(v >> 12) & 63];
		out += b64chars[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

/// JSON 字符串转义（UTF-8 多字节原样通过）
std::string jesc(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 16);
	for (unsigned char c : s) {
		switch (c) {
		case '\"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else
				out += static_cast<char>(c);
		}
	}
	return out;
}

/// ASS 文本：换行统一为 \N
std::string newlines_to_N(std::string s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\r') {
			out += "\\N";
			if (i + 1 < s.size() && s[i+1] == '\n') ++i;
		} else if (s[i] == '\n')
			out += "\\N";
		else
			out += s[i];
	}
	return out;
}

std::string ltrim_delims(const std::string& s) {
	static const char* ws = " \t\r\n=:,";
	size_t i = 0;
	while (i < s.size()) {
		bool hit = false;
		for (const char* p = ws; *p; ++p)
			if (s[i] == *p) { hit = true; ++i; break; }
		// 全角冒号 ： = EF BC 9A
		if (!hit && i + 2 < s.size()
			&& static_cast<unsigned char>(s[i])   == 0xEF
			&& static_cast<unsigned char>(s[i+1]) == 0xBC
			&& static_cast<unsigned char>(s[i+2]) == 0x9A) { i += 3; hit = true; }
		if (!hit) break;
	}
	return s.substr(i);
}

std::string trim_ws(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

/// 取指定标记（如"译文"）之后的内容，去掉前导 = ： 等分隔符；
/// 找不到时按 whole_fallback 决定回退为整段或空串。
std::string extract_after(const std::string& text, const char* marker, bool whole_fallback) {
	size_t pos = text.find(marker);
	if (pos == std::string::npos)
		return whole_fallback ? trim_ws(text) : std::string();
	std::string rest = ltrim_delims(text.substr(pos + strlen(marker)));
	rest = trim_ws(rest);
	if (!rest.empty()) return rest;
	return whole_fallback ? trim_ws(text) : std::string();
}

/// 取两个标记之间的内容（"原文"到"译文"之间）
std::string extract_between(const std::string& text, const char* from, const char* to) {
	size_t pos = text.find(from);
	if (pos == std::string::npos) return "";
	size_t end = text.find(to, pos + strlen(from));
	std::string mid = text.substr(pos + strlen(from),
		end == std::string::npos ? std::string::npos : end - pos - strlen(from));
	return trim_ws(ltrim_delims(mid));
}

std::string ms_to_hms(int64_t ms) {
	int64_t s = ms / 1000;
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
		static_cast<long long>(s / 3600),
		static_cast<long long>((s / 60) % 60),
		static_cast<long long>(s % 60));
	return buf;
}

// 常用 UTF-8 标记
const char* const k_orig   = "\xE5\x8E\x9F\xE6\x96\x87"; // 原文
const char* const k_yiwen  = "\xE8\xAF\x91\xE6\x96\x87"; // 译文

// ------------------------------------------------------------ 配置存取

std::string opt_str(const char* key, const char* fallback = "") {
	std::string s = OPT_GET(key)->GetString();
	return s.empty() ? std::string(fallback) : s;
}

/// 首次使用时从旧版 Lua 脚本的 %APPDATA%\ai_translate.conf 里取密钥
std::string seed_legacy_api_key() {
	wxString appdata = wxGetenv("APPDATA");
	if (appdata.empty()) return "";
	std::ifstream f(from_wx(wxFileName(appdata, "ai_translate.conf").GetFullPath()));
	if (!f) return "";
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("api_key=", 0) == 0)
			return line.substr(8);
	}
	return "";
}

// ------------------------------------------------------------ 网络请求

#ifdef WITH_UPDATE_CHECKER
size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* out = static_cast<std::string*>(userdata);
	if (out->size() + size * nmemb > 64 * 1024 * 1024) return 0; // 64MB 上限
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

int curl_xfer_cb(void* client, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto* cancel = static_cast<std::atomic<bool>*>(client);
	return cancel->load() ? 1 : 0;
}

boost::property_tree::ptree parse_json(const std::string& s) {
	boost::property_tree::ptree pt;
	std::istringstream iss(s);
	boost::property_tree::json_parser::read_json(iss, pt);
	return pt;
}

/// content 可能是字符串，也可能是 [{text:..},{..}] 形式；统一取文本
std::string ptree_content(const boost::property_tree::ptree& node) {
	std::string direct = node.get_value<std::string>();
	if (!direct.empty()) return direct;
	std::string acc;
	for (auto const& ch : node) {
		std::string t = ch.second.get<std::string>("text", "");
		if (!t.empty()) acc += t;
	}
	return acc;
}
#endif

// ------------------------------------------------------------ 音频切取

/// 按采样区间取音频，重采样为 16k 单声道 PCM16，输出完整 WAV 字节
std::string build_wav16k(agi::AudioProvider* provider, int64_t s0, int64_t s1) {
	if (!provider) return "";
	const int ch  = provider->GetChannels();
	const int bps = provider->GetBytesPerSample();
	const int rate = provider->GetSampleRate();
	const bool flt = provider->AreSamplesFloat();

	s0 = std::max<int64_t>(0, s0);
	s1 = std::min<int64_t>(provider->GetNumSamples(), s1);
	int64_t count = s1 - s0;
	if (count <= 0 || ch <= 0) return "";

	std::vector<char> raw(static_cast<size_t>(count) * ch * bps);
	try {
		provider->GetAudio(raw.data(), s0, count);
	} catch (...) {
		return "";
	}

	// 下混单声道（float）
	std::vector<float> mono(static_cast<size_t>(count));
	for (int64_t i = 0; i < count; ++i) {
		float sum = 0;
		for (int j = 0; j < ch; ++j) {
			size_t off = static_cast<size_t>(i * ch + j) * bps;
			if (flt && bps == 4) {
				float v;
				memcpy(&v, raw.data() + off, 4);
				sum += v;
			} else if (!flt && bps == 2) {
				int16_t v;
				memcpy(&v, raw.data() + off, 2);
				sum += v / 32768.0f;
			} else if (!flt && bps == 1) {
				sum += (static_cast<uint8_t>(raw[off]) - 128) / 128.0f;
			} else {
				return "";
			}
		}
		mono[static_cast<size_t>(i)] = sum / ch;
	}

	// 线性插值重采样到 16000
	std::vector<int16_t> out;
	if (rate == 16000) {
		out.resize(mono.size());
		for (size_t i = 0; i < mono.size(); ++i)
			out[i] = static_cast<int16_t>(std::clamp(mono[i], -1.f, 1.f) * 32767);
	} else {
		size_t n = static_cast<size_t>(static_cast<double>(count) * 16000 / rate);
		out.resize(n);
		double step = static_cast<double>(rate) / 16000.0;
		for (size_t i = 0; i < n; ++i) {
			double pos = i * step;
			size_t p = static_cast<size_t>(pos);
			double frac = pos - p;
			float a = mono[std::min(p, static_cast<size_t>(count - 1))];
			float b = mono[std::min(p + 1, static_cast<size_t>(count - 1))];
			float v = static_cast<float>(a + (b - a) * frac);
			out[i] = static_cast<int16_t>(std::clamp(v, -1.f, 1.f) * 32767);
		}
	}

	// WAV 头 + 数据（小端）
	std::string wav;
	size_t data_len = out.size() * 2;
	auto u32 = [&wav](uint32_t v) {
		wav += static_cast<char>(v & 0xFF);
		wav += static_cast<char>((v >> 8) & 0xFF);
		wav += static_cast<char>((v >> 16) & 0xFF);
		wav += static_cast<char>((v >> 24) & 0xFF);
	};
	auto u16 = [&wav](uint16_t v) {
		wav += static_cast<char>(v & 0xFF);
		wav += static_cast<char>((v >> 8) & 0xFF);
	};
	wav += "RIFF"; u32(static_cast<uint32_t>(36 + data_len));
	wav += "WAVE";
	wav += "fmt "; u32(16); u16(1); u16(1);
	u32(16000); u32(32000); u16(2); u16(16);
	wav += "data"; u32(static_cast<uint32_t>(data_len));
	wav.append(reinterpret_cast<const char*>(out.data()), data_len);
	return wav;
}

// ------------------------------------------------------------ 设置对话框

class AISettingsDialog final : public wxDialog {
	wxTextCtrl* api_key_ = nullptr;
	wxCheckBox* show_key_ = nullptr;
	wxTextCtrl* vision_model_ = nullptr;
	wxTextCtrl* audio_model_ = nullptr;
	wxTextCtrl* target_lang_ = nullptr;
	wxSpinCtrl* pad_start_ = nullptr;
	wxSpinCtrl* pad_end_ = nullptr;

public:
	AISettingsDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, to_wx("AI 辅助翻译设置"),
		wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	{
		auto* main_sizer = new wxBoxSizer(wxVERTICAL);
		auto* grid = new wxFlexGridSizer(2, wxSize(8, 6));
		grid->AddGrowableCol(1, 1);

		auto add_row = [&](const char* label, wxWindow* ctrl) {
			grid->Add(new wxStaticText(this, wxID_ANY, to_wx(label)), 0, wxALIGN_CENTER_VERTICAL);
			grid->Add(ctrl, 1, wxEXPAND);
		};
		auto add_sizer_row = [&](const char* label, wxSizer* sizer) {
			grid->Add(new wxStaticText(this, wxID_ANY, to_wx(label)), 0, wxALIGN_CENTER_VERTICAL);
			grid->Add(sizer, 1, wxEXPAND);
		};

		std::string key = opt_str("Tool/AI/API Key");
		if (key.empty()) key = seed_legacy_api_key();

		auto* key_sizer = new wxBoxSizer(wxHORIZONTAL);
		api_key_ = new wxTextCtrl(this, wxID_ANY, to_wx(key), wxDefaultPosition, wxSize(280, -1), wxTE_PASSWORD);
		show_key_ = new wxCheckBox(this, wxID_ANY, to_wx("显示密钥"));
		key_sizer->Add(api_key_, 1, wxEXPAND | wxRIGHT, 6);
		key_sizer->Add(show_key_, 0, wxALIGN_CENTER_VERTICAL);
		add_sizer_row("API 密钥：", key_sizer);

		vision_model_ = new wxTextCtrl(this, wxID_ANY, to_wx(opt_str("Tool/AI/Vision Model", "qwen3.8-flash")));
		add_row("视觉模型：", vision_model_);

		audio_model_ = new wxTextCtrl(this, wxID_ANY, to_wx(opt_str("Tool/AI/Audio Model", "qwen3.8-omni-flash")));
		add_row("音频模型：", audio_model_);

		target_lang_ = new wxTextCtrl(this, wxID_ANY, to_wx(opt_str("Tool/AI/Target Lang", "简体中文")));
		add_row("目标语言：", target_lang_);

		auto* pad_sizer = new wxBoxSizer(wxHORIZONTAL);
		pad_start_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
			wxSP_ARROW_KEYS, 0, 5000, OPT_GET("Tool/AI/Pad Start")->GetInt());
		pad_end_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
			wxSP_ARROW_KEYS, 0, 5000, OPT_GET("Tool/AI/Pad End")->GetInt());
		pad_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("前补齐(ms)：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		pad_sizer->Add(pad_start_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		pad_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("后补齐(ms)：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		pad_sizer->Add(pad_end_, 0, wxALIGN_CENTER_VERTICAL);
		add_sizer_row("音频切取补齐：", pad_sizer);

		main_sizer->Add(grid, 1, wxEXPAND | wxALL, 12);
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

		show_key_->Bind(wxEVT_CHECKBOX, [this, key_sizer](wxCommandEvent& e) {
			wxString val = api_key_->GetValue();
			long style = e.IsChecked() ? 0 : wxTE_PASSWORD;
			auto* p = api_key_->GetParent();
			api_key_->Destroy();
			api_key_ = new wxTextCtrl(p, wxID_ANY, val, wxDefaultPosition, wxSize(280, -1), style);
			key_sizer->Insert(0, api_key_, 1, wxEXPAND | wxRIGHT, 6);
			key_sizer->Layout();
		});

		SetSizerAndFit(main_sizer);
		Centre();
	}

	void Save() {
		OPT_SET("Tool/AI/API Key")->SetString(from_wx(api_key_->GetValue()));
		OPT_SET("Tool/AI/Vision Model")->SetString(from_wx(vision_model_->GetValue()));
		OPT_SET("Tool/AI/Audio Model")->SetString(from_wx(audio_model_->GetValue()));
		OPT_SET("Tool/AI/Target Lang")->SetString(from_wx(target_lang_->GetValue()));
		OPT_SET("Tool/AI/Pad Start")->SetInt(pad_start_->GetValue());
		OPT_SET("Tool/AI/Pad End")->SetInt(pad_end_->GetValue());
	}
};

} // anonymous namespace

// ------------------------------------------------------------ 视觉截图工具

VisualToolAIScreenshot::VisualToolAIScreenshot(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
{
}

void VisualToolAIScreenshot::ClearRegion() {
	has_region = false;
	dragging = false;
	if (parent->HasCapture()) parent->ReleaseMouse();
}

bool VisualToolAIScreenshot::OnKeyDown(wxKeyEvent &event) {
	if (event.GetKeyCode() == WXK_ESCAPE) {
		ClearRegion();
		parent->Render();
		c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
		return true;
	}
	return false;
}

void VisualToolAIScreenshot::OnMouseEvent(wxMouseEvent &event) {
	mouse_pos = Vector2D(event.GetX(), event.GetY());

	if (event.RightDown()) {
		if (dragging || has_region) {
			ClearRegion();
			parent->Render();
		} else {
			c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
		}
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

		auto provider = c->project->VideoProvider();
		if (provider && script_res.X() > 0 && script_res.Y() > 0) {
			int x0 = std::clamp(static_cast<int>(std::floor(std::min(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth() - 1);
			int x1 = std::clamp(static_cast<int>(std::ceil(std::max(start.X(), end.X()) * provider->GetWidth() / script_res.X())), 0, provider->GetWidth());
			int y0 = std::clamp(static_cast<int>(std::floor(std::min(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight() - 1);
			int y1 = std::clamp(static_cast<int>(std::ceil(std::max(start.Y(), end.Y()) * provider->GetHeight() / script_res.Y())), 0, provider->GetHeight());

			if (x1 - x0 >= 8 && y1 - y0 >= 8) {
				OPT_SET("Tool/AI/Region X0")->SetDouble(static_cast<double>(x0) * 100.0 / provider->GetWidth());
				OPT_SET("Tool/AI/Region Y0")->SetDouble(static_cast<double>(y0) * 100.0 / provider->GetHeight());
				OPT_SET("Tool/AI/Region X1")->SetDouble(static_cast<double>(x1) * 100.0 / provider->GetWidth());
				OPT_SET("Tool/AI/Region Y1")->SetDouble(static_cast<double>(y1) * 100.0 / provider->GetHeight());

				RunAIFrameTranslate(c, x0, y0, x1, y1);
			} else {
				if (c->aiPanel)
					c->aiPanel->SetStatus("选区太小，请按住鼠标左键在画面上拖拽出一个矩形框选字幕");
			}
		}
		parent->Render();
	}
	else if (event.Leaving()) {
		mouse_pos = Vector2D(-1, -1);
	}
	event.Skip(false);
}

void VisualToolAIScreenshot::Draw() {
	if (!has_region) return;
	gl.SetLineColour(wxColour(0, 180, 255), 1.0f, 2);
	gl.SetFillColour(wxColour(0, 180, 255), 0.22f);
	gl.DrawRectangle(FromScriptCoords(start), FromScriptCoords(end));
}

// ------------------------------------------------------------ 常驻右下角面板

AIResultPanel::AIResultPanel(wxWindow* parent, agi::Context* ctx)
: wxPanel(parent, wxID_ANY)
, c(ctx)
{
	SetMinSize(wxSize(350, -1));

	auto* root = new wxBoxSizer(wxVERTICAL);

	// ---- 顶部栏（标题 + 状态 + 设置 + 关闭）
	auto* top_row = new wxBoxSizer(wxHORIZONTAL);
	auto* title = new wxStaticText(this, wxID_ANY, to_wx("AI 辅助翻译"));
	wxFont font = title->GetFont();
	font.SetWeight(wxFONTWEIGHT_BOLD);
	title->SetFont(font);
	top_row->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

	status_ = new wxStaticText(this, wxID_ANY, to_wx("就绪"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	status_->SetForegroundColour(wxColour(110, 110, 110));
	top_row->Add(status_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

	settings_btn_ = new wxButton(this, wxID_ANY, to_wx("⚙ 设置"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	close_btn_ = new wxButton(this, wxID_ANY, to_wx("✕"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	top_row->Add(settings_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
	top_row->Add(close_btn_, 0, wxALIGN_CENTER_VERTICAL);
	root->Add(top_row, 0, wxEXPAND | wxALL, 6);

	// ---- Tab 切换区
	notebook_ = new wxNotebook(this, wxID_ANY);

	// Tab 0: 截图翻译
	auto* frame_page = new wxPanel(notebook_, wxID_ANY);
	auto* frame_sizer = new wxBoxSizer(wxVERTICAL);

	frame_sizer->Add(new wxStaticText(frame_page, wxID_ANY, to_wx("识别原文：")), 0, wxBOTTOM, 2);
	frame_orig_ = new wxTextCtrl(frame_page, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 52), wxTE_MULTILINE);
	frame_sizer->Add(frame_orig_, 0, wxEXPAND | wxBOTTOM, 6);

	frame_sizer->Add(new wxStaticText(frame_page, wxID_ANY, to_wx("翻译结果（可编辑）：")), 0, wxBOTTOM, 2);
	frame_trans_ = new wxTextCtrl(frame_page, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 72), wxTE_MULTILINE);
	frame_sizer->Add(frame_trans_, 1, wxEXPAND | wxBOTTOM, 6);

	auto* frame_btn_row = new wxBoxSizer(wxHORIZONTAL);
	frame_apply_ = new wxButton(frame_page, wxID_ANY, to_wx("填入当前行"));
	frame_apply_->Disable();
	frame_resnap_ = new wxButton(frame_page, wxID_ANY, to_wx("重新截图"));
	frame_retry_ = new wxButton(frame_page, wxID_ANY, to_wx("重试"));
	cancel_btn_ = new wxButton(frame_page, wxID_ANY, to_wx("取消"));
	cancel_btn_->Disable();

	frame_btn_row->Add(frame_apply_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_resnap_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_retry_, 0, wxRIGHT, 4);
	frame_btn_row->Add(cancel_btn_, 0);
	frame_sizer->Add(frame_btn_row, 0, wxEXPAND);

	frame_page->SetSizer(frame_sizer);
	notebook_->AddPage(frame_page, to_wx("截图翻译"), true);

	// Tab 1: 音频翻译
	auto* audio_page = new wxPanel(notebook_, wxID_ANY);
	auto* audio_sizer = new wxBoxSizer(wxVERTICAL);

	audio_sizer->Add(new wxStaticText(audio_page, wxID_ANY, to_wx("转写与翻译结果（可直接编辑）：")), 0, wxBOTTOM, 2);
	audio_combined_ = new wxTextCtrl(audio_page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
	audio_sizer->Add(audio_combined_, 1, wxEXPAND | wxBOTTOM, 6);

	auto* audio_btn_row = new wxBoxSizer(wxHORIZONTAL);
	audio_apply_ = new wxButton(audio_page, wxID_ANY, to_wx("替换选中行"));
	audio_apply_->Disable();
	audio_retry_ = new wxButton(audio_page, wxID_ANY, to_wx("重新翻译"));
	audio_clear_ = new wxButton(audio_page, wxID_ANY, to_wx("清空"));
	audio_cancel_btn_ = new wxButton(audio_page, wxID_ANY, to_wx("取消"));
	audio_cancel_btn_->Disable();

	audio_btn_row->Add(audio_apply_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_retry_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_clear_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_cancel_btn_, 0);
	audio_sizer->Add(audio_btn_row, 0, wxEXPAND);

	audio_page->SetSizer(audio_sizer);
	notebook_->AddPage(audio_page, to_wx("音频翻译"), false);

	root->Add(notebook_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
	SetSizer(root);

	// 绑定事件
	frame_apply_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameApply, this);
	frame_resnap_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameResnap, this);
	frame_retry_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameRetry, this);
	cancel_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCancelClick, this);

	audio_apply_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioApply, this);
	audio_retry_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioRetry, this);
	audio_clear_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioClear, this);
	audio_cancel_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCancelClick, this);

	settings_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnSettingsClick, this);
	close_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCloseClick, this);
}

AIResultPanel::~AIResultPanel() {
	alive_->store(false);
	cancel_->store(true);
}

void AIResultPanel::SelectTab(int tab_idx) {
	if (notebook_ && tab_idx >= 0 && static_cast<size_t>(tab_idx) < notebook_->GetPageCount())
		notebook_->SetSelection(tab_idx);
}

void AIResultPanel::SetStatus(const std::string& s) {
	if (status_) {
		status_->SetLabel(to_wx(s));
		status_->GetParent()->Layout();
	}
}

void AIResultPanel::ShowSettings() {
	AISettingsDialog dlg(this);
	if (dlg.ShowModal() == wxID_OK) {
		dlg.Save();
		SetStatus("设置已保存");
	}
}

void AIResultPanel::BeginJob(const std::string& doing) {
	running_ = true;
	cancel_->store(false);
	cancel_btn_->Enable();
	audio_cancel_btn_->Enable();
	frame_apply_->Disable();
	audio_apply_->Disable();
	SetStatus(doing);
}

void AIResultPanel::EndJob() {
	running_ = false;
	cancel_btn_->Disable();
	audio_cancel_btn_->Disable();
}

void AIResultPanel::SetFrameResult(const std::string& orig, const std::string& trans) {
	frame_orig_->ChangeValue(to_wx(orig));
	frame_trans_->ChangeValue(to_wx(trans));
	frame_apply_->Enable(!trans.empty());
}

void AIResultPanel::AppendAudioResult(const std::string& block, int done, int total) {
	audio_combined_->AppendText(to_wx(block));
	SetStatus(agi::format("第 %d/%d 条完成", done, total));
}

void AIResultPanel::AudioJobFinished() {
	EndJob();
	if (cancel_->load()) {
		SetStatus("已取消");
	} else {
		SetStatus("音频翻译完成，可编辑后点击【替换选中行】");
	}
	if (!audio_combined_->IsEmpty())
		audio_apply_->Enable();
}

void AIResultPanel::OnFrameApply(wxCommandEvent&) {
	if (running_) return;
	auto* line = c->selectionController->GetActiveLine();
	if (!line) { SetStatus("没有当前活动行"); return; }
	std::string v = from_wx(frame_trans_->GetValue());
	if (v.empty()) { SetStatus("译文为空"); return; }
	line->Text = newlines_to_N(v);
	c->ass->Commit(to_wx("AI截图翻译"), AssFile::COMMIT_DIAG_TEXT);
	SetStatus("已填入当前行");
}

void AIResultPanel::OnFrameResnap(wxCommandEvent&) {
	if (c->videoDisplay) {
		c->videoDisplay->SetTool(std::make_unique<VisualToolAIScreenshot>(c->videoDisplay, c));
		SetStatus("请在视频画面上拖动鼠标框选要翻译的区域");
	}
}

void AIResultPanel::OnFrameRetry(wxCommandEvent&) {
	RunAIFrameTranslate(c);
}

void AIResultPanel::OnAudioApply(wxCommandEvent&) {
	if (running_) return;
	std::string combined = from_wx(audio_combined_->GetValue());
	int applied = apply_combined(combined);
	if (applied > 0) {
		c->ass->Commit(to_wx("AI音频翻译"), AssFile::COMMIT_DIAG_TEXT);
		SetStatus(agi::format("已替换 %d 行", applied));
	} else {
		SetStatus("没有可识别的【第n条】结果块");
	}
}

void AIResultPanel::OnAudioRetry(wxCommandEvent&) {
	RunAIAudioTranslate(c);
}

void AIResultPanel::OnAudioClear(wxCommandEvent&) {
	audio_combined_->Clear();
	audio_apply_->Disable();
	SetStatus("已清空");
}

void AIResultPanel::OnCancelClick(wxCommandEvent&) {
	if (running_) {
		cancel_->store(true);
		SetStatus("正在取消后台任务…");
		cancel_btn_->Disable();
		audio_cancel_btn_->Disable();
	}
}

void AIResultPanel::OnSettingsClick(wxCommandEvent&) {
	ShowSettings();
}

void AIResultPanel::OnCloseClick(wxCommandEvent&) {
	Show(false);
	GetParent()->Layout();
}

int AIResultPanel::apply_combined(const std::string& combined) {
	struct Head { size_t hstart, start; int num; };
	std::vector<Head> heads;
	const std::string tag = "【第"; // 【 = E3 80 90，第 = E7 AC AC
	const char* close_tag = "\xE3\x80\x91"; // 】
	size_t init = 0;
	while (true) {
		size_t s = combined.find(tag, init);
		if (s == std::string::npos) break;
		size_t e = combined.find(close_tag, s);
		if (e == std::string::npos) break;
		std::string body = combined.substr(s + tag.size(), e - s - tag.size());
		int n = 0; bool any = false;
		for (char ch : body) {
			if (ch >= '0' && ch <= '9') { n = n * 10 + (ch - '0'); any = true; }
			else if (any) break;
		}
		if (any && n >= 1)
			heads.push_back({ s, e + strlen(close_tag), n });
		init = e + strlen(close_tag);
	}
	int applied = 0;
	for (size_t i = 0; i < heads.size(); ++i) {
		size_t stop = (i + 1 < heads.size()) ? heads[i + 1].hstart : combined.size();
		std::string block = combined.substr(heads[i].start,
			stop > heads[i].start ? stop - heads[i].start : 0);
		int idx = heads[i].num - 1;
		if (idx < 0 || static_cast<size_t>(idx) >= audio_lines_.size()) continue;
		std::string trans = extract_after(block, k_yiwen, false);
		if (trans.empty()) trans = trim_ws(block);
		if (trans.empty()) continue;
		audio_lines_[idx]->Text = newlines_to_N(trans);
		++applied;
	}
	return applied;
}

// ------------------------------------------------------------ 执行翻译函数

void RunAIFrameTranslate(agi::Context* c, int x0, int y0, int x1, int y1) {
	if (c->aiPanel) {
		c->aiPanel->Show(true);
		c->aiPanel->GetParent()->Layout();
		c->aiPanel->SelectTab(0);
	}

	auto* provider = c->project->VideoProvider();
	if (!provider) {
		if (c->aiPanel) c->aiPanel->SetStatus("未打开视频");
		return;
	}

	std::string key = opt_str("Tool/AI/API Key");
	if (key.empty()) key = seed_legacy_api_key();
	if (key.empty()) {
		if (c->aiPanel) {
			c->aiPanel->SetStatus("请先配置 API 密钥");
			c->aiPanel->ShowSettings();
		}
		return;
	}

	int w = provider->GetWidth(), h = provider->GetHeight();
	if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0) {
		double rx0 = OPT_GET("Tool/AI/Region X0")->GetDouble();
		double ry0 = OPT_GET("Tool/AI/Region Y0")->GetDouble();
		double rx1 = OPT_GET("Tool/AI/Region X1")->GetDouble();
		double ry1 = OPT_GET("Tool/AI/Region Y1")->GetDouble();

		if (rx0 <= 0 && rx1 <= 0 && OPT_GET("Tool/Frame Match/Region X0")->GetDouble() >= 0.0) {
			rx0 = OPT_GET("Tool/Frame Match/Region X0")->GetDouble() * 100.0;
			ry0 = OPT_GET("Tool/Frame Match/Region Y0")->GetDouble() * 100.0;
			rx1 = OPT_GET("Tool/Frame Match/Region X1")->GetDouble() * 100.0;
			ry1 = OPT_GET("Tool/Frame Match/Region Y1")->GetDouble() * 100.0;
		}
		if (rx1 <= rx0 || ry1 <= ry0) {
			rx0 = 0.0; ry0 = 70.0; rx1 = 100.0; ry1 = 100.0;
		}
		x0 = std::clamp(static_cast<int>(w * rx0 / 100.0), 0, w - 1);
		y0 = std::clamp(static_cast<int>(h * ry0 / 100.0), 0, h - 1);
		x1 = std::clamp(static_cast<int>(w * rx1 / 100.0), 0, w);
		y1 = std::clamp(static_cast<int>(h * ry1 / 100.0), 0, h);
	}

	std::string img_b64;
	const char* mime = "image/jpeg";
	try {
		int frame = c->videoController->GetFrameN();
		double time_s = c->videoController->TimeAtFrame(frame, agi::vfr::EXACT) / 1000.0;
		auto vframe = provider->GetFrame(frame, time_s, true);
		wxImage img = GetImage(*vframe);
		int cw = x1 - x0, ch = y1 - y0;
		if (cw < 8 || ch < 8) {
			if (c->aiPanel) c->aiPanel->SetStatus("截取区域太小，请重新框选");
			return;
		}
		wxImage cropped = img.GetSubImage(wxRect(x0, y0, cw, ch));

		// 适当缩放：字幕识别最佳宽度不超过 960px，兼顾清晰度与极快的上传/推理速度
		if (cropped.GetWidth() > 960) {
			int nh = std::max(1, static_cast<int>(960.0 * ch / cw));
			cropped = cropped.Rescale(960, nh);
		}

		// 使用高质量 JPEG 压缩（品质 85%），体积比 PNG 减小 85%~90%，极大提升传输速度
		if (!wxImage::FindHandler(wxBITMAP_TYPE_JPEG)) {
			wxImage::AddHandler(new wxJPEGHandler);
		}
		cropped.SetOption(wxIMAGE_OPTION_QUALITY, 85);
		wxMemoryOutputStream mos;
		if (!cropped.SaveFile(mos, wxBITMAP_TYPE_JPEG)) {
			mime = "image/png";
			if (!cropped.SaveFile(mos, wxBITMAP_TYPE_PNG)) {
				if (c->aiPanel) c->aiPanel->SetStatus("图像编码失败");
				return;
			}
		}

		size_t img_len = static_cast<size_t>(mos.GetLength());
		std::vector<char> img_buf(img_len);
		if (mos.CopyTo(img_buf.data(), img_len) != img_len) {
			if (c->aiPanel) c->aiPanel->SetStatus("图像缓冲读取失败");
			return;
		}
		img_b64 = base64_encode(img_buf.data(), img_len);
	} catch (std::exception& e) {
		if (c->aiPanel) c->aiPanel->SetStatus(std::string("取帧失败：") + e.what());
		return;
	} catch (...) {
		if (c->aiPanel) c->aiPanel->SetStatus("取帧失败");
		return;
	}

	std::string model = opt_str("Tool/AI/Vision Model", "qwen3.8-flash");
	std::string lang = opt_str("Tool/AI/Target Lang", "简体中文");
	std::string prompt = std::string("这是动画/影视视频的画面截图。识别图中的字幕或对白文字（忽略水印和台标），并翻译成")
		+ lang + "，用词自然、符合字幕阅读习惯。严格按两行输出：\n原文=识别出的文字\n译文=翻译结果";
	std::string body = "{\"model\":\"" + jesc(model) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
		+ "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:" + mime + ";base64," + img_b64 + "\"}},"
		+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}],\"max_tokens\":512}";

	auto panel = c->aiPanel;
	if (!panel) return;
	panel->BeginJob("识别中…");
	panel->SetStatus("正在调用大模型识别并翻译，请稍候…");
	auto alive = panel->GetAlive();
	auto cancel = panel->GetCancel();

	agi::dispatch::Background().Async([panel, alive, cancel, key, body] {
		AIResult r = AIChatComplete(key, body, false, cancel);
		agi::dispatch::Main().Async([panel, alive, cancel, r] {
			if (!*alive) return;
			panel->EndJob();
			if (cancel->load()) {
				panel->SetStatus("已取消");
				return;
			}
			if (!r.ok) {
				panel->SetStatus("翻译失败：" + r.error);
				panel->SetFrameResult("", "错误：" + r.error);
				return;
			}
			std::string orig = extract_between(r.text, k_orig, k_yiwen);
			std::string trans = extract_after(r.text, k_yiwen, false);
			if (trans.empty()) trans = trim_ws(r.text);
			panel->SetFrameResult(orig, trans);
			panel->SetStatus("完成，可编辑后点击【填入当前行】");
		});
	});
}

void RunAIAudioTranslate(agi::Context* c) {
	if (c->aiPanel) {
		c->aiPanel->Show(true);
		c->aiPanel->GetParent()->Layout();
		c->aiPanel->SelectTab(1);
	}

	auto* provider = c->project->AudioProvider();
	if (!provider) {
		if (c->aiPanel) c->aiPanel->SetStatus("未打开音频（需要项目已加载音频）");
		return;
	}

	auto lines = c->selectionController->GetSortedSelection();
	if (lines.empty()) {
		if (c->aiPanel) c->aiPanel->SetStatus("请先在字幕网格中选中要翻译的行");
		return;
	}

	std::string key = opt_str("Tool/AI/API Key");
	if (key.empty()) key = seed_legacy_api_key();
	if (key.empty()) {
		if (c->aiPanel) {
			c->aiPanel->SetStatus("请先配置 API 密钥");
			c->aiPanel->ShowSettings();
		}
		return;
	}

	struct Job { int64_t s0, s1; int idx; int start_ms; };
	int pad0 = OPT_GET("Tool/AI/Pad Start")->GetInt();
	int pad1 = OPT_GET("Tool/AI/Pad End")->GetInt();
	int rate = provider->GetSampleRate();
	std::vector<Job> jobs;
	jobs.reserve(lines.size());
	for (size_t i = 0; i < lines.size(); ++i) {
		int start = lines[i]->Start;
		int end = lines[i]->End;
		if (end <= start) end = start + 1000;
		jobs.push_back({ static_cast<int64_t>(start - pad0) * rate / 1000,
			static_cast<int64_t>(end + pad1) * rate / 1000,
			static_cast<int>(i), start });
	}

	std::string model = opt_str("Tool/AI/Audio Model", "qwen3.8-omni-flash");
	std::string lang = opt_str("Tool/AI/Target Lang", "简体中文");
	bool is_omni = model.find("omni") != std::string::npos;
	std::string prompt = is_omni
		? "听这段音频：先转写说话内容原文，再翻译成" + lang + "（字幕风格）。严格按两行输出：\n原文=...\n译文=..."
		: "转写这段音频中的语音内容。";

	auto panel = c->aiPanel;
	if (!panel) return;
	panel->SetAudioLines(lines);
	panel->BeginJob("音频翻译中…");
	panel->SetStatus(agi::format("共 %d 行，开始切取音频并翻译…", static_cast<int>(jobs.size())));

	auto alive = panel->GetAlive();
	auto cancel = panel->GetCancel();
	int total = static_cast<int>(jobs.size());

	agi::dispatch::Background().Async([panel, alive, cancel, provider, key, model, prompt, is_omni, jobs, total] {
		int done = 0;
		for (auto const& j : jobs) {
			if (cancel->load()) break;
			std::string block = "【第" + std::to_string(j.idx + 1) + "条 "
				+ ms_to_hms(j.start_ms) + "】\n";
			std::string wav = build_wav16k(provider, j.s0, j.s1);
			AIResult r;
			if (wav.empty()) {
				r.error = "切分音频失败（时间区间无效或解码错误）";
			} else {
				std::string b64 = base64_encode(wav.data(), wav.size());
				std::string body = "{\"model\":\"" + jesc(model) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
					+ "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:;base64," + b64 + "\",\"format\":\"wav\"}},"
					+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}]}";
				r = AIChatComplete(key, body, false, cancel);
			}
			block += r.ok ? r.text : std::string("错误: ") + r.error;
			block += "\n";
			int dn = ++done;
			agi::dispatch::Main().Async([panel, alive, blk = std::move(block), dn, total] {
				if (!*alive) return;
				panel->AppendAudioResult(blk, dn, total);
			});
		}
		agi::dispatch::Main().Async([panel, alive] {
			if (!*alive) return;
			panel->AudioJobFinished();
		});
	});
}

void ShowAITranslationDialog(agi::Context *c, AIMode mode) {
	if (mode == AIMode::Frame)
		RunAIFrameTranslate(c);
	else
		RunAIAudioTranslate(c);
}

// ------------------------------------------------------------ 阿里百炼网络调用

AIResult AIChatComplete(const std::string& api_key, const std::string& body,
                        bool stream,
                        const std::shared_ptr<std::atomic<bool>>& cancel) {
	AIResult r;
#ifndef WITH_UPDATE_CHECKER
	(void)api_key; (void)body; (void)stream; (void)cancel;
	r.error = "此版本编译时未启用网络（libcurl）支持";
	return r;
#else
	const char* const url =
		"https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
	CURL* h = curl_easy_init();
	if (!h) { r.error = "curl 初始化失败"; return r; }

	std::string response;
	struct curl_slist* hdrs = nullptr;
	std::string auth = "Authorization: Bearer " + api_key;
	hdrs = curl_slist_append(hdrs, auth.c_str());
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, "Expect:"); // 禁用 100-continue，彻底解决大包等待挂起/延迟问题

	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, curl_xfer_cb);
	curl_easy_setopt(h, CURLOPT_XFERINFODATA, cancel.get());
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "Aegisub AI Assistant");

	CURLcode res = curl_easy_perform(h);
	long code = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(h);

	if (res == CURLE_ABORTED_BY_CALLBACK || (cancel && cancel->load())) {
		r.error = "已取消";
		return r;
	}
	if (res != CURLE_OK) {
		r.error = std::string("网络请求失败：") + curl_easy_strerror(res)
			+ "（请检查网络或代理）";
		return r;
	}

	auto raw_snip = [&response]() {
		return response.substr(0, 300);
	};

	if (code >= 400) {
		std::string detail;
		try {
			auto pt = parse_json(response);
			detail = pt.get<std::string>("error.message", "");
			if (detail.empty()) detail = pt.get<std::string>("message", "");
		} catch (...) { }
		if (detail.empty()) detail = raw_snip();

		if (code == 401 || detail.find("api_key") != std::string::npos
			|| detail.find("API key") != std::string::npos)
			r.error = "API 密钥无效，请核对密钥设置（" + detail + "）";
		else if (code == 403 || detail.find("AccessDenied") != std::string::npos)
			r.error = "模型未开通：" + detail + "\n请到百炼控制台「模型广场」开通该模型后重试";
		else if (code == 429)
			r.error = "请求过于频繁或额度不足（HTTP 429）：" + detail;
		else
			r.error = "HTTP " + std::to_string(code) + "：" + detail;
		return r;
	}

	std::string content;
	if (stream) {
		// SSE：data: {...} 行，提取 delta.content 拼接
		std::istringstream iss(response);
		std::string line;
		while (std::getline(iss, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.rfind("data:", 0) != 0) continue;
			std::string payload = line.substr(5);
			payload.erase(0, payload.find_first_not_of(" \t"));
			if (payload == "[DONE]") break;
			try {
				auto pt = parse_json(payload);
				auto choices = pt.get_child_optional("choices");
				if (choices && !choices->empty()) {
					auto const& choice = choices->front().second;
					auto delta = choice.get_child_optional("delta");
					if (delta) {
						auto cnt = delta->get_child_optional("content");
						if (cnt) {
							std::string t = ptree_content(*cnt);
							if (!t.empty()) content += t;
						}
					}
				}
			} catch (...) { }
		}
	} else {
		try {
			auto pt = parse_json(response);
			auto choices = pt.get_child_optional("choices");
			if (choices && !choices->empty()) {
				auto const& choice = choices->front().second;
				auto msg = choice.get_child_optional("message");
				if (msg) {
					auto cnt = msg->get_child_optional("content");
					if (cnt) content = ptree_content(*cnt);
					if (content.empty()) {
						auto rsn = msg->get_child_optional("reasoning_content");
						if (rsn) content = ptree_content(*rsn);
					}
				}
				if (content.empty()) {
					auto delta = choice.get_child_optional("delta");
					if (delta) {
						auto cnt = delta->get_child_optional("content");
						if (cnt) content = ptree_content(*cnt);
					}
				}
			}
		} catch (...) {
			r.error = "响应格式异常：" + raw_snip();
			return r;
		}
	}

	if (content.empty()) {
		r.ok = true;
		r.text = "（未识别到有效内容）";
		return r;
	}
	r.ok = true;
	r.text = content;
	return r;
#endif
}
