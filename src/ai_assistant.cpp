// AI 辅助翻译内嵌实现：
//   功能1：在视频画面上直接框选截图，调用大模型识别并翻译；
//   功能2：按选中行时间轴切取音频，调用 omni 语音大模型转写并翻译。
// 结果在右侧辅助面板以表格（ListCtrl）形式按行展示，支持多选填入或一键全部替换。
// 配置支持方案管理（Endpoint URL、密钥、模型、语言、HTTP/SOCKS5 代理等为一组）。

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
#include "frame_main.h"
#include "utils.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/split.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/imagjpeg.h>
#include <wx/intl.h>
#include <wx/checklst.h>
#include <wx/gauge.h>
#include <wx/radiobox.h>
#include <wx/radiobut.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include "silero_vad.h"
#include "dialog_motion_tracker.h"
#include "dialog_video_shuffler.h"
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#ifdef WITH_UPDATE_CHECKER
#include <curl/curl.h>
#endif
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
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

/// 取指定标记（如"译文"）之后的内容
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
	return (cancel && cancel->load()) ? 1 : 0;
}

boost::property_tree::ptree parse_json(const std::string& s) {
	boost::property_tree::ptree pt;
	std::istringstream iss(s);
	boost::property_tree::json_parser::read_json(iss, pt);
	return pt;
}

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

} // anonymous namespace

// ------------------------------------------------------------ 方案存取实现

std::vector<AIProfile> AILoadProfiles(std::string* active_name) {
	std::vector<AIProfile> list;
	std::string json_str = opt_str("Tool/AI/Profiles");
	if (!json_str.empty()) {
		try {
			boost::property_tree::ptree root;
			std::istringstream iss(json_str);
			boost::property_tree::json_parser::read_json(iss, root);
			for (auto const& item : root) {
				AIProfile p;
				p.name = item.second.get<std::string>("name", "默认配置");
				p.endpoint = item.second.get<std::string>("endpoint", "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
				p.api_key = item.second.get<std::string>("api_key", "");
				p.vision_model = item.second.get<std::string>("vision_model", "qwen3.8-flash");
				p.audio_model = item.second.get<std::string>("audio_model", "qwen3.8-omni-flash");
				p.source_lang = item.second.get<std::string>("source_lang", "自动检测");
				p.target_lang = item.second.get<std::string>("target_lang", "简体中文");
				p.proxy_type = item.second.get<std::string>("proxy_type", "None");
				p.proxy_host = item.second.get<std::string>("proxy_host", "");
				p.proxy_port = item.second.get<int>("proxy_port", 0);
				p.proxy_user = item.second.get<std::string>("proxy_user", "");
				p.proxy_pass = item.second.get<std::string>("proxy_pass", "");
				p.timeout_sec = item.second.get<int>("timeout_sec", 60);
				p.max_retries = item.second.get<int>("max_retries", 2);
				list.push_back(p);
			}
		} catch (...) { }
	}

	if (list.empty()) {
		AIProfile def;
		def.name = "默认配置";
		def.endpoint = opt_str("Tool/AI/Endpoint", "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
		def.api_key = opt_str("Tool/AI/API Key");
		if (def.api_key.empty()) def.api_key = seed_legacy_api_key();
		def.vision_model = opt_str("Tool/AI/Vision Model", "qwen3.8-flash");
		def.audio_model = opt_str("Tool/AI/Audio Model", "qwen3.8-omni-flash");
		def.source_lang = opt_str("Tool/AI/Source Lang", "自动检测");
		def.target_lang = opt_str("Tool/AI/Target Lang", "简体中文");
		def.proxy_type = opt_str("Tool/AI/Proxy Type", "None");
		def.proxy_host = opt_str("Tool/AI/Proxy Host", "");
		def.proxy_port = OPT_GET("Tool/AI/Proxy Port")->GetInt();
		def.proxy_user = opt_str("Tool/AI/Proxy User", "");
		def.proxy_pass = opt_str("Tool/AI/Proxy Password", "");
		def.timeout_sec = OPT_GET("Tool/AI/Timeout")->GetInt();
		if (def.timeout_sec <= 0) def.timeout_sec = 60;
		def.max_retries = OPT_GET("Tool/AI/Max Retries")->GetInt();
		if (def.max_retries < 0) def.max_retries = 2;
		list.push_back(def);
	}

	std::string act = opt_str("Tool/AI/Active Profile", "默认配置");
	if (active_name) *active_name = act;
	return list;
}

void AISaveProfiles(const std::vector<AIProfile>& profiles, const std::string& active_name) {
	boost::property_tree::ptree root;
	for (auto const& p : profiles) {
		boost::property_tree::ptree node;
		node.put("name", p.name);
		node.put("endpoint", p.endpoint);
		node.put("api_key", p.api_key);
		node.put("vision_model", p.vision_model);
		node.put("audio_model", p.audio_model);
		node.put("source_lang", p.source_lang);
		node.put("target_lang", p.target_lang);
		node.put("proxy_type", p.proxy_type);
		node.put("proxy_host", p.proxy_host);
		node.put("proxy_port", p.proxy_port);
		node.put("proxy_user", p.proxy_user);
		node.put("proxy_pass", p.proxy_pass);
		node.put("timeout_sec", p.timeout_sec);
		node.put("max_retries", p.max_retries);
		root.push_back(std::make_pair("", node));
	}
	std::ostringstream oss;
	boost::property_tree::json_parser::write_json(oss, root, false);
	OPT_SET("Tool/AI/Profiles")->SetString(oss.str());
	OPT_SET("Tool/AI/Active Profile")->SetString(active_name);

	for (auto const& p : profiles) {
		if (p.name == active_name) {
			OPT_SET("Tool/AI/Endpoint")->SetString(p.endpoint);
			OPT_SET("Tool/AI/API Key")->SetString(p.api_key);
			OPT_SET("Tool/AI/Vision Model")->SetString(p.vision_model);
			OPT_SET("Tool/AI/Audio Model")->SetString(p.audio_model);
			OPT_SET("Tool/AI/Source Lang")->SetString(p.source_lang);
			OPT_SET("Tool/AI/Target Lang")->SetString(p.target_lang);
			OPT_SET("Tool/AI/Proxy Type")->SetString(p.proxy_type);
			OPT_SET("Tool/AI/Proxy Host")->SetString(p.proxy_host);
			OPT_SET("Tool/AI/Proxy Port")->SetInt(p.proxy_port);
			OPT_SET("Tool/AI/Proxy User")->SetString(p.proxy_user);
			OPT_SET("Tool/AI/Proxy Password")->SetString(p.proxy_pass);
			OPT_SET("Tool/AI/Timeout")->SetInt(p.timeout_sec);
			OPT_SET("Tool/AI/Max Retries")->SetInt(p.max_retries);
			break;
		}
	}
}

AIProfile AIGetActiveProfile() {
	std::string act;
	auto list = AILoadProfiles(&act);
	for (auto const& p : list) {
		if (p.name == act) return p;
	}
	return list.empty() ? AIProfile{} : list.front();
}

// ------------------------------------------------------------ 方案与模型设置对话框

class AISettingsDialog final : public wxDialog {
	std::vector<AIProfile> profiles_;
	std::string active_name_;
	int cur_sel_ = 0;

	wxComboBox* profile_cb_ = nullptr;
	wxButton* btn_new_ = nullptr;
	wxButton* btn_delete_ = nullptr;

	wxTextCtrl* endpoint_ = nullptr;
	wxTextCtrl* api_key_ = nullptr;
	wxCheckBox* show_key_ = nullptr;
	wxTextCtrl* vision_model_ = nullptr;
	wxTextCtrl* audio_model_ = nullptr;
	wxComboBox* source_lang_ = nullptr;
	wxTextCtrl* target_lang_ = nullptr;

	wxComboBox* proxy_type_ = nullptr;
	wxTextCtrl* proxy_host_ = nullptr;
	wxSpinCtrl* proxy_port_ = nullptr;
	wxTextCtrl* proxy_user_ = nullptr;
	wxTextCtrl* proxy_pass_ = nullptr;

	wxSpinCtrl* timeout_sec_ = nullptr;
	wxSpinCtrl* max_retries_ = nullptr;
	wxSpinCtrl* pad_start_ = nullptr;
	wxSpinCtrl* pad_end_ = nullptr;

	void LoadProfileToUI(const AIProfile& p) {
		endpoint_->SetValue(to_wx(p.endpoint));
		api_key_->SetValue(to_wx(p.api_key));
		vision_model_->SetValue(to_wx(p.vision_model));
		audio_model_->SetValue(to_wx(p.audio_model));
		source_lang_->SetValue(to_wx(p.source_lang));
		target_lang_->SetValue(to_wx(p.target_lang));

		proxy_type_->SetStringSelection(to_wx(p.proxy_type));
		if (proxy_type_->GetSelection() == wxNOT_FOUND) proxy_type_->SetSelection(0);
		proxy_host_->SetValue(to_wx(p.proxy_host));
		proxy_port_->SetValue(p.proxy_port);
		proxy_user_->SetValue(to_wx(p.proxy_user));
		proxy_pass_->SetValue(to_wx(p.proxy_pass));

		timeout_sec_->SetValue(p.timeout_sec > 0 ? p.timeout_sec : 60);
		max_retries_->SetValue(p.max_retries >= 0 ? p.max_retries : 2);
	}

	void SaveUIToProfile(AIProfile& p) {
		p.endpoint = from_wx(endpoint_->GetValue());
		p.api_key = from_wx(api_key_->GetValue());
		p.vision_model = from_wx(vision_model_->GetValue());
		p.audio_model = from_wx(audio_model_->GetValue());
		p.source_lang = from_wx(source_lang_->GetValue());
		p.target_lang = from_wx(target_lang_->GetValue());

		p.proxy_type = from_wx(proxy_type_->GetStringSelection());
		p.proxy_host = from_wx(proxy_host_->GetValue());
		p.proxy_port = proxy_port_->GetValue();
		p.proxy_user = from_wx(proxy_user_->GetValue());
		p.proxy_pass = from_wx(proxy_pass_->GetValue());

		p.timeout_sec = timeout_sec_->GetValue();
		p.max_retries = max_retries_->GetValue();
	}

public:
	AISettingsDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, to_wx("AI 辅助翻译与模型配置"),
		wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	{
		profiles_ = AILoadProfiles(&active_name_);
		cur_sel_ = 0;
		for (size_t i = 0; i < profiles_.size(); ++i) {
			if (profiles_[i].name == active_name_) { cur_sel_ = static_cast<int>(i); break; }
		}

		auto* main_sizer = new wxBoxSizer(wxVERTICAL);

		// 1. 方案选择栏
		auto* profile_sizer = new wxBoxSizer(wxHORIZONTAL);
		profile_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("配置方案：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		wxArrayString pnames;
		for (auto const& p : profiles_) pnames.Add(to_wx(p.name));
		profile_cb_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(200, -1), pnames, wxCB_READONLY);
		profile_cb_->SetSelection(cur_sel_);
		profile_sizer->Add(profile_cb_, 1, wxEXPAND | wxRIGHT, 6);

		btn_new_ = new wxButton(this, wxID_ANY, to_wx("新建方案…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		btn_delete_ = new wxButton(this, wxID_ANY, to_wx("删除"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		profile_sizer->Add(btn_new_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		profile_sizer->Add(btn_delete_, 0, wxALIGN_CENTER_VERTICAL);
		main_sizer->Add(profile_sizer, 0, wxEXPAND | wxALL, 10);

		// 2. 表单网格
		auto* grid = new wxFlexGridSizer(2, wxSize(10, 8));
		grid->AddGrowableCol(1, 1);

		auto add_row = [&](const char* label, wxWindow* ctrl) {
			grid->Add(new wxStaticText(this, wxID_ANY, to_wx(label)), 0, wxALIGN_CENTER_VERTICAL);
			grid->Add(ctrl, 1, wxEXPAND);
		};
		auto add_sizer_row = [&](const char* label, wxSizer* sizer) {
			grid->Add(new wxStaticText(this, wxID_ANY, to_wx(label)), 0, wxALIGN_CENTER_VERTICAL);
			grid->Add(sizer, 1, wxEXPAND);
		};

		endpoint_ = new wxTextCtrl(this, wxID_ANY, "");
		add_row("接口 URL：", endpoint_);

		auto* key_sizer = new wxBoxSizer(wxHORIZONTAL);
		api_key_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(260, -1), wxTE_PASSWORD);
		show_key_ = new wxCheckBox(this, wxID_ANY, to_wx("显示密钥"));
		key_sizer->Add(api_key_, 1, wxEXPAND | wxRIGHT, 6);
		key_sizer->Add(show_key_, 0, wxALIGN_CENTER_VERTICAL);
		add_sizer_row("API 密钥：", key_sizer);

		vision_model_ = new wxTextCtrl(this, wxID_ANY, "");
		add_row("视觉模型：", vision_model_);

		audio_model_ = new wxTextCtrl(this, wxID_ANY, "");
		add_row("音频模型：", audio_model_);

		// 语言选择
		wxArrayString src_langs;
		src_langs.Add(to_wx("自动检测"));
		src_langs.Add(to_wx("韩语 (Korean)"));
		src_langs.Add(to_wx("日语 (Japanese)"));
		src_langs.Add(to_wx("英语 (English)"));
		src_langs.Add(to_wx("汉语 (Chinese)"));
		src_langs.Add(to_wx("俄语 (Russian)"));
		src_langs.Add(to_wx("法语 (French)"));
		src_langs.Add(to_wx("德语 (German)"));
		src_langs.Add(to_wx("西班牙语 (Spanish)"));
		source_lang_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, src_langs);
		add_row("原语言：", source_lang_);

		target_lang_ = new wxTextCtrl(this, wxID_ANY, "");
		add_row("目标语言：", target_lang_);

		// 代理设置
		auto* proxy_sizer = new wxBoxSizer(wxHORIZONTAL);
		wxArrayString ptypes;
		ptypes.Add("None");
		ptypes.Add("HTTP");
		ptypes.Add("SOCKS5");
		proxy_type_ = new wxComboBox(this, wxID_ANY, "None", wxDefaultPosition, wxSize(85, -1), ptypes, wxCB_READONLY);
		proxy_host_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(140, -1));
		proxy_port_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(75, -1), wxSP_ARROW_KEYS, 0, 65535, 0);
		proxy_sizer->Add(proxy_type_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		proxy_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("地址:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		proxy_sizer->Add(proxy_host_, 1, wxEXPAND | wxRIGHT, 6);
		proxy_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("端口:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		proxy_sizer->Add(proxy_port_, 0, wxALIGN_CENTER_VERTICAL);
		add_sizer_row("网络代理：", proxy_sizer);

		auto* proxy_auth_sizer = new wxBoxSizer(wxHORIZONTAL);
		proxy_user_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(120, -1));
		proxy_pass_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxTE_PASSWORD);
		proxy_auth_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("用户名(可选):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		proxy_auth_sizer->Add(proxy_user_, 1, wxEXPAND | wxRIGHT, 8);
		proxy_auth_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("密码:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		proxy_auth_sizer->Add(proxy_pass_, 1, wxEXPAND);
		add_sizer_row("代理认证：", proxy_auth_sizer);

		// 超时与自动重试
		auto* net_ctl_sizer = new wxBoxSizer(wxHORIZONTAL);
		timeout_sec_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 10, 300, 60);
		max_retries_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 0, 5, 2);
		net_ctl_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("超时(秒)：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		net_ctl_sizer->Add(timeout_sec_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);
		net_ctl_sizer->Add(new wxStaticText(this, wxID_ANY, to_wx("失败自动重试次数：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		net_ctl_sizer->Add(max_retries_, 0, wxALIGN_CENTER_VERTICAL);
		add_sizer_row("网络与容错：", net_ctl_sizer);

		// 切取音频前后补齐
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

		main_sizer->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

		if (cur_sel_ >= 0 && cur_sel_ < static_cast<int>(profiles_.size())) {
			LoadProfileToUI(profiles_[cur_sel_]);
		}

		show_key_->Bind(wxEVT_CHECKBOX, [this, key_sizer](wxCommandEvent& e) {
			wxString val = api_key_->GetValue();
			long style = e.IsChecked() ? 0 : wxTE_PASSWORD;
			auto* p = api_key_->GetParent();
			api_key_->Destroy();
			api_key_ = new wxTextCtrl(p, wxID_ANY, val, wxDefaultPosition, wxSize(260, -1), style);
			key_sizer->Insert(0, api_key_, 1, wxEXPAND | wxRIGHT, 6);
			key_sizer->Layout();
		});

		profile_cb_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
			int sel = profile_cb_->GetSelection();
			if (sel != wxNOT_FOUND && sel != cur_sel_) {
				if (cur_sel_ >= 0 && cur_sel_ < static_cast<int>(profiles_.size())) {
					SaveUIToProfile(profiles_[cur_sel_]);
				}
				cur_sel_ = sel;
				LoadProfileToUI(profiles_[cur_sel_]);
			}
		});

		btn_new_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			wxString name = wxGetTextFromUser(to_wx("请输入新方案名称："), to_wx("新建模型配置方案"), "", this);
			name = name.Trim().Trim(false);
			if (name.empty()) return;
			for (auto const& p : profiles_) {
				if (p.name == from_wx(name)) {
					wxMessageBox(to_wx("已存在同名方案，请使用不同名称。"), to_wx("方案重名"), wxOK | wxICON_WARNING, this);
					return;
				}
			}
			if (cur_sel_ >= 0 && cur_sel_ < static_cast<int>(profiles_.size())) {
				SaveUIToProfile(profiles_[cur_sel_]);
			}
			AIProfile new_p;
			if (cur_sel_ >= 0 && cur_sel_ < static_cast<int>(profiles_.size()))
				new_p = profiles_[cur_sel_]; // 复制当前方案内容作为模板
			new_p.name = from_wx(name);
			profiles_.push_back(new_p);
			profile_cb_->Append(name);
			cur_sel_ = static_cast<int>(profiles_.size()) - 1;
			profile_cb_->SetSelection(cur_sel_);
			LoadProfileToUI(new_p);
		});

		btn_delete_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (profiles_.size() <= 1) {
				wxMessageBox(to_wx("至少保留一个默认方案，无法删除。"), to_wx("提示"), wxOK | wxICON_INFORMATION, this);
				return;
			}
			int sel = profile_cb_->GetSelection();
			if (sel == wxNOT_FOUND) return;
			if (wxMessageBox(fmt_wx("确定要删除方案「%s」吗？", profile_cb_->GetStringSelection()),
				to_wx("删除确认"), wxYES_NO | wxICON_QUESTION, this) != wxYES) return;

			profiles_.erase(profiles_.begin() + sel);
			profile_cb_->Delete(sel);
			cur_sel_ = std::max(0, sel - 1);
			profile_cb_->SetSelection(cur_sel_);
			LoadProfileToUI(profiles_[cur_sel_]);
		});

		SetSizerAndFit(main_sizer);
		Centre();
	}

	void Save() {
		if (cur_sel_ >= 0 && cur_sel_ < static_cast<int>(profiles_.size())) {
			SaveUIToProfile(profiles_[cur_sel_]);
		}
		active_name_ = profiles_[cur_sel_].name;
		AISaveProfiles(profiles_, active_name_);

		OPT_SET("Tool/AI/Pad Start")->SetInt(pad_start_->GetValue());
		OPT_SET("Tool/AI/Pad End")->SetInt(pad_end_->GetValue());
	}
};

// ------------------------------------------------------------ 视觉截图工具

VisualToolAIScreenshot::VisualToolAIScreenshot(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
{
	if (c && c->videoController) {
		c->videoController->SetShowSubtitles(false);
	}
}

VisualToolAIScreenshot::~VisualToolAIScreenshot() {
	ClearRegion();
	if (c && c->videoController) {
		c->videoController->SetShowSubtitles(true);
	}
}

void VisualToolAIScreenshot::ClearRegion() {
	has_region = false;
	dragging = false;
	if (parent->HasCapture()) parent->ReleaseMouse();
}

bool VisualToolAIScreenshot::OnKeyDown(wxKeyEvent &event) {
	if (event.GetKeyCode() == WXK_ESCAPE) {
		ClearRegion();
		if (c && c->videoController) {
			c->videoController->SetShowSubtitles(true);
		}
		parent->Render();
		c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
		return true;
	}
	return false;
}

void VisualToolAIScreenshot::OnMouseEvent(wxMouseEvent &event) {
	mouse_pos = Vector2D(event.GetX(), event.GetY());

	if (event.RightDown()) {
		ClearRegion();
		if (c && c->videoController) {
			c->videoController->SetShowSubtitles(true);
		}
		parent->Render();
		c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
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
		has_region = false;
		if (parent->HasCapture()) parent->ReleaseMouse();

		if (c && c->videoController) {
			c->videoController->SetShowSubtitles(true);
		}

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
			}
		}
		parent->Render();
		c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
	}
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
	SetMinSize(wxSize(380, -1));

	auto* root = new wxBoxSizer(wxVERTICAL);

	// ---- 顶部栏（标题 + 状态 + 设置 + 关闭）
	auto* top_row = new wxBoxSizer(wxHORIZONTAL);
	auto* title = new wxStaticText(this, wxID_ANY, to_wx("辅助工具"));
	wxFont title_font = title->GetFont();
	title_font.SetWeight(wxFONTWEIGHT_BOLD);
	title->SetFont(title_font);
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
	frame_copy_ = new wxButton(frame_page, wxID_ANY, to_wx("复制译文"));
	frame_copy_->Disable();
	frame_resnap_ = new wxButton(frame_page, wxID_ANY, to_wx("重新截图"));
	frame_retry_ = new wxButton(frame_page, wxID_ANY, to_wx("重试"));
	auto* frame_motion_btn = new wxButton(frame_page, wxID_ANY, to_wx("运动追踪…"));
	frame_motion_btn->SetToolTip(to_wx("追踪视频运动花字并生成逐帧相对位移字幕（类似 Mocha）"));
	frame_motion_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		c->videoDisplay->SetTool(std::make_unique<VisualToolMotionTrack>(c->videoDisplay, c));
	});
	auto* frame_shuffler_btn = new wxButton(frame_page, wxID_ANY, to_wx("按字幕切片/合并…"));
	frame_shuffler_btn->SetToolTip(to_wx("根据字幕时间轴、注释与章节标记，自动对视频进行精简切分、去除静音与合并重组（类似洋片箱）"));
	frame_shuffler_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		ShowVideoShufflerDialog(c);
	});
	cancel_btn_ = new wxButton(frame_page, wxID_ANY, to_wx("取消"));
	cancel_btn_->Disable();

	frame_btn_row->Add(frame_apply_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_copy_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_resnap_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_retry_, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_motion_btn, 0, wxRIGHT, 4);
	frame_btn_row->Add(frame_shuffler_btn, 0, wxRIGHT, 4);
	frame_btn_row->Add(cancel_btn_, 0);
	frame_sizer->Add(frame_btn_row, 0, wxEXPAND);

	frame_page->SetSizer(frame_sizer);
	notebook_->AddPage(frame_page, to_wx("截图翻译"), true);

	// Tab 1: 音频翻译（表格视图）
	auto* audio_page = new wxPanel(notebook_, wxID_ANY);
	auto* audio_sizer = new wxBoxSizer(wxVERTICAL);

	audio_list_ = new wxListCtrl(audio_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		wxLC_REPORT | wxLC_HRULES | wxLC_VRULES | wxBORDER_SUNKEN);
	audio_list_->EnableCheckBoxes(true);
	audio_list_->InsertColumn(0, to_wx("#"), wxLIST_FORMAT_RIGHT, 42);
	audio_list_->InsertColumn(1, to_wx("时间"), wxLIST_FORMAT_CENTER, 75);
	audio_list_->InsertColumn(2, to_wx("转写原文"), wxLIST_FORMAT_LEFT, 150);
	audio_list_->InsertColumn(3, to_wx("翻译结果"), wxLIST_FORMAT_LEFT, 180);
	audio_list_->InsertColumn(4, to_wx("状态"), wxLIST_FORMAT_LEFT, 120);

	audio_list_->SetToolTip(to_wx("支持 Ctrl+C 复制选中行，Ctrl+A 全选；右键可弹出菜单复制原文/译文"));

	audio_sizer->Add(audio_list_, 1, wxEXPAND | wxBOTTOM, 6);

	auto* audio_btn_row = new wxBoxSizer(wxHORIZONTAL);
	audio_apply_sel_ = new wxButton(audio_page, wxID_ANY, to_wx("填入所选/勾选行"));
	audio_apply_sel_->Disable();
	audio_apply_all_ = new wxButton(audio_page, wxID_ANY, to_wx("一键替换全部"));
	audio_apply_all_->Disable();
	audio_copy_btn_ = new wxButton(audio_page, wxID_ANY, to_wx("复制所选"));
	audio_copy_btn_->Disable();
	audio_copy_btn_->SetToolTip(to_wx("复制选中的转写原文与翻译结果（快捷键 Ctrl+C）；右键列表可选择仅复制译文或原文"));
	audio_vad_btn_ = new wxButton(audio_page, wxID_ANY, to_wx("人声切分…"));
	audio_vad_btn_->SetToolTip(to_wx("智能人声切分与说话人识别：精准检测说话发声瞬间、结束延长200ms，支持过滤背景乐与按音色匹配角色"));
	audio_vad_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowVADDiarizeDialog(c); });
	audio_retry_ = new wxButton(audio_page, wxID_ANY, to_wx("重新识别"));
	audio_clear_ = new wxButton(audio_page, wxID_ANY, to_wx("清空"));
	audio_cancel_btn_ = new wxButton(audio_page, wxID_ANY, to_wx("取消"));
	audio_cancel_btn_->Disable();

	audio_btn_row->Add(audio_apply_sel_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_apply_all_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_copy_btn_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_vad_btn_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_retry_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_clear_, 0, wxRIGHT, 4);
	audio_btn_row->Add(audio_cancel_btn_, 0);
	audio_sizer->Add(audio_btn_row, 0, wxEXPAND);

	audio_page->SetSizer(audio_sizer);
	notebook_->AddPage(audio_page, to_wx("音频翻译"), false);

	// Tab 2: 样式快切
	auto* style_page = new wxPanel(notebook_, wxID_ANY);
	auto* style_sizer = new wxBoxSizer(wxVERTICAL);

	quick_style_chk_ = new wxCheckBox(style_page, wxID_ANY, to_wx("开启样式快切（上下键联动画面，数字键 1-0 切样式）"));
	quick_style_chk_->SetValue(OPT_GET("Tool/Playback/Review Quick Style")->GetBool());
	quick_style_chk_->Bind(wxEVT_CHECKBOX, &AIResultPanel::OnQuickStyleToggle, this);
	style_sizer->Add(quick_style_chk_, 0, wxEXPAND | wxALL, 6);

	style_list_ = new wxListBox(style_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);
	style_list_->SetToolTip(to_wx("前10个样式映射数字键 1~9 和 0；双击直接应用，选中后点下方按钮可调整排序"));
	style_list_->Bind(wxEVT_LISTBOX_DCLICK, &AIResultPanel::OnStyleListDClick, this);
	style_sizer->Add(style_list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

	auto* style_btn_row = new wxBoxSizer(wxHORIZONTAL);
	btn_style_up_ = new wxButton(style_page, wxID_ANY, to_wx("▲ 上移"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	btn_style_down_ = new wxButton(style_page, wxID_ANY, to_wx("▼ 下移"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	btn_style_apply_ = new wxButton(style_page, wxID_ANY, to_wx("应用"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	btn_style_up_->Bind(wxEVT_BUTTON, &AIResultPanel::OnMoveStyleUp, this);
	btn_style_down_->Bind(wxEVT_BUTTON, &AIResultPanel::OnMoveStyleDown, this);
	btn_style_apply_->Bind(wxEVT_BUTTON, &AIResultPanel::OnStyleApply, this);
	style_btn_row->Add(btn_style_up_, 1, wxEXPAND | wxRIGHT, 4);
	style_btn_row->Add(btn_style_down_, 1, wxEXPAND | wxRIGHT, 4);
	style_btn_row->Add(btn_style_apply_, 1, wxEXPAND);
	style_sizer->Add(style_btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

	auto* tip_label = new wxStaticText(style_page, wxID_ANY, to_wx("方向键上下：切换行并自动跳至该行起始帧画面\n按数字键 1~9, 0：一键将对应序号样式应用至所选行"));
	wxFont tip_font = tip_label->GetFont();
	tip_font.SetPointSize(std::max(8, tip_font.GetPointSize() - 1));
	tip_label->SetFont(tip_font);
	tip_label->SetForegroundColour(wxColour(120, 120, 120));
	style_sizer->Add(tip_label, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 6);

	style_page->SetSizer(style_sizer);
	notebook_->AddPage(style_page, to_wx("样式快切"), false);

	root->Add(notebook_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
	SetSizer(root);

	// 绑定事件
	frame_apply_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameApply, this);
	frame_copy_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameCopy, this);
	frame_resnap_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameResnap, this);
	frame_retry_->Bind(wxEVT_BUTTON, &AIResultPanel::OnFrameRetry, this);
	cancel_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCancelClick, this);

	audio_apply_sel_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioApplySelected, this);
	audio_apply_all_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioApplyAll, this);
	audio_copy_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioCopy, this);
	audio_retry_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioRetry, this);
	audio_clear_->Bind(wxEVT_BUTTON, &AIResultPanel::OnAudioClear, this);
	audio_cancel_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCancelClick, this);

	audio_list_->Bind(wxEVT_KEY_DOWN, &AIResultPanel::OnAudioListKeyDown, this);
	audio_list_->Bind(wxEVT_CONTEXT_MENU, &AIResultPanel::OnAudioListContextMenu, this);

	settings_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnSettingsClick, this);
	close_btn_->Bind(wxEVT_BUTTON, &AIResultPanel::OnCloseClick, this);

	style_connections_ = agi::signal::make_vector({
		c->ass->AddCommitListener([this](int type, const AssDialogue *) {
			if (type == AssFile::COMMIT_NEW || (type & (AssFile::COMMIT_STYLES | AssFile::COMMIT_ORDER)))
				SyncStyles();
		}),
	});
	SyncStyles();
}

AIResultPanel::~AIResultPanel() {
	alive_->store(false);
	cancel_->store(true);
}

void AIResultPanel::SelectTab(int tab_idx) {
	if (notebook_ && tab_idx >= 0 && static_cast<size_t>(tab_idx) < notebook_->GetPageCount()) {
		notebook_->SetSelection(tab_idx);
		if (tab_idx == 2) {
			if (quick_style_chk_)
				quick_style_chk_->SetValue(OPT_GET("Tool/Playback/Review Quick Style")->GetBool());
			SyncStyles();
		}
	}
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
	if (frame_copy_) frame_copy_->Disable();
	audio_apply_sel_->Disable();
	audio_apply_all_->Disable();
	if (audio_copy_btn_) audio_copy_btn_->Disable();
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
	if (frame_copy_) frame_copy_->Enable(!trans.empty() || !orig.empty());
}

void AIResultPanel::InitAudioItems(int total) {
	audio_items_.clear();
	audio_items_.resize(total);
	audio_list_->DeleteAllItems();
	if (audio_copy_btn_) audio_copy_btn_->Disable();
	for (int i = 0; i < total; ++i) {
		audio_items_[i].idx = i;
		long item = audio_list_->InsertItem(i, std::to_string(i + 1));
		audio_list_->SetItem(item, 1, "--:--:--");
		audio_list_->SetItem(item, 2, "等待处理…");
		audio_list_->SetItem(item, 3, "");
		audio_list_->SetItem(item, 4, "排队中");
		audio_list_->CheckItem(item, true);
	}
}

void AIResultPanel::UpdateAudioItem(const AIAudioItem& item, int done, int total) {
	if (item.idx >= 0 && item.idx < static_cast<int>(audio_items_.size())) {
		audio_items_[item.idx] = item;
		audio_list_->SetItem(item.idx, 1, to_wx(item.time_str));
		audio_list_->SetItem(item.idx, 2, to_wx(item.orig));
		audio_list_->SetItem(item.idx, 3, to_wx(item.trans));
		audio_list_->SetItem(item.idx, 4, to_wx(item.success ? "完成" : ("失败: " + item.error)));
		audio_list_->CheckItem(item.idx, item.success && !item.trans.empty());
		audio_list_->EnsureVisible(item.idx);
		if (!item.orig.empty() || !item.trans.empty()) {
			if (audio_copy_btn_) audio_copy_btn_->Enable();
		}
	}
	SetStatus(agi::format("第 %d/%d 条完成", done, total));
}

void AIResultPanel::AudioJobFinished() {
	EndJob();
	if (cancel_->load()) {
		SetStatus("已取消");
	} else {
		SetStatus("音频识别与翻译完成，勾选或选中行后点击填入或复制");
	}
	bool has_any = false;
	for (auto const& it : audio_items_) {
		if (it.success && (!it.trans.empty() || !it.orig.empty())) { has_any = true; break; }
	}
	if (has_any) {
		audio_apply_sel_->Enable();
		audio_apply_all_->Enable();
		if (audio_copy_btn_) audio_copy_btn_->Enable();
	}
}

void AIResultPanel::OnFrameCopy(wxCommandEvent&) {
	std::string trans = from_wx(frame_trans_->GetValue());
	std::string orig = from_wx(frame_orig_->GetValue());
	if (trans.empty() && orig.empty()) {
		SetStatus("没有可复制的内容");
		return;
	}
	if (!trans.empty() && !orig.empty()) {
		SetClipboard(orig + "\t" + trans);
		SetStatus("已复制原文与译文到剪贴板");
	} else if (!trans.empty()) {
		SetClipboard(trans);
		SetStatus("已复制译文到剪贴板");
	} else {
		SetClipboard(orig);
		SetStatus("已复制原文到剪贴板");
	}
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

int AIResultPanel::apply_items(const std::vector<int>& indices) {
	int applied = 0;
	for (int idx : indices) {
		if (idx < 0 || idx >= static_cast<int>(audio_items_.size()) || idx >= static_cast<int>(audio_lines_.size()))
			continue;
		std::string trans = audio_items_[idx].trans;
		if (trans.empty()) continue;
		audio_lines_[idx]->Text = newlines_to_N(trans);
		++applied;
	}
	return applied;
}

void AIResultPanel::OnAudioApplySelected(wxCommandEvent&) {
	if (running_) return;
	std::vector<int> targets;
	// 1. 先查高亮选中的行
	long item = -1;
	while (true) {
		item = audio_list_->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1) break;
		targets.push_back(static_cast<int>(item));
	}
	// 2. 如果没有单独高亮选中的行，则采用勾选复选框（CheckItem）的行
	if (targets.empty()) {
		for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
			if (audio_list_->IsItemChecked(i))
				targets.push_back(i);
		}
	}
	if (targets.empty()) {
		SetStatus("未选择或勾选任何有效的翻译结果行");
		return;
	}
	int applied = apply_items(targets);
	if (applied > 0) {
		c->ass->Commit(to_wx("AI音频翻译"), AssFile::COMMIT_DIAG_TEXT);
		SetStatus(agi::format("已成功替换 %d 行", applied));
	} else {
		SetStatus("所选行译文为空，未进行替换");
	}
}

void AIResultPanel::OnAudioApplyAll(wxCommandEvent&) {
	if (running_) return;
	std::vector<int> all;
	for (int i = 0; i < static_cast<int>(audio_items_.size()); ++i)
		all.push_back(i);
	int applied = apply_items(all);
	if (applied > 0) {
		c->ass->Commit(to_wx("AI音频翻译(全部)"), AssFile::COMMIT_DIAG_TEXT);
		SetStatus(agi::format("已替换全部 %d 行", applied));
	} else {
		SetStatus("没有可填入的有效译文");
	}
}

void AIResultPanel::CopyItems(const std::vector<int>& indices, CopyMode mode) {
	if (indices.empty()) {
		SetStatus("没有可复制的内容");
		return;
	}
	std::string result;
	int count = 0;
	for (int idx : indices) {
		if (idx < 0 || idx >= static_cast<int>(audio_items_.size()))
			continue;
		const auto& it = audio_items_[idx];
		if (it.orig.empty() && it.trans.empty() && it.error.empty())
			continue;

		std::string line_str;
		switch (mode) {
		case COPY_BOTH_TAB:
			if (!it.orig.empty() && !it.trans.empty())
				line_str = it.orig + "\t" + it.trans;
			else if (!it.trans.empty())
				line_str = it.trans;
			else
				line_str = it.orig;
			break;
		case COPY_TRANS_ONLY:
			line_str = it.trans;
			break;
		case COPY_ORIG_ONLY:
			line_str = it.orig;
			break;
		case COPY_DETAILED: {
			std::string time_s = it.time_str.empty() ? ms_to_hms(it.start_ms) : it.time_str;
			line_str = agi::format("【第%d条 %s】\n原文=%s\n译文=%s",
				it.idx + 1, time_s, it.orig, it.trans);
			if (!it.error.empty()) {
				line_str += "\n错误: " + it.error;
			}
			break;
		}
		}

		if (!result.empty()) {
			result += (mode == COPY_DETAILED ? "\n\n" : "\n");
		}
		result += line_str;
		++count;
	}

	if (result.empty()) {
		SetStatus("没有可复制的有效文本");
		return;
	}

	SetClipboard(result);
	SetStatus(agi::format("已复制 %d 条记录到剪贴板", count));
}

std::vector<int> AIResultPanel::GetSelectedOrCheckedAudioIndices() const {
	std::vector<int> targets;
	long item = -1;
	while (true) {
		item = audio_list_->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1) break;
		targets.push_back(static_cast<int>(item));
	}
	if (targets.empty()) {
		for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
			if (audio_list_->IsItemChecked(i))
				targets.push_back(i);
		}
	}
	if (targets.empty()) {
		for (int i = 0; i < audio_list_->GetItemCount(); ++i)
			targets.push_back(i);
	}
	return targets;
}

void AIResultPanel::ShowAudioContextMenu(const wxPoint& pos) {
	enum {
		MENU_COPY_SEL_BOTH = 11001,
		MENU_COPY_SEL_TRANS,
		MENU_COPY_SEL_ORIG,
		MENU_COPY_SEL_DETAILED,
		MENU_SELECT_ALL,
		MENU_CHECK_ALL,
		MENU_UNCHECK_ALL,
		MENU_COPY_ALL_BOTH,
		MENU_COPY_ALL_TRANS,
	};

	wxPoint client_pos = pos;
	if (client_pos == wxDefaultPosition) {
		client_pos = wxPoint(10, 10);
	}
	int flags = 0;
	long hit = audio_list_->HitTest(client_pos, flags);
	if (hit != wxNOT_FOUND) {
		int state = audio_list_->GetItemState(hit, wxLIST_STATE_SELECTED);
		if (!(state & wxLIST_STATE_SELECTED)) {
			if (audio_list_->GetSelectedItemCount() <= 1) {
				for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
					audio_list_->SetItemState(i, (i == hit ? wxLIST_STATE_SELECTED : 0), wxLIST_STATE_SELECTED);
				}
			}
		}
	}

	wxMenu menu;
	menu.Append(MENU_COPY_SEL_BOTH, to_wx("复制所选行 (原文 + 译文 制表符)\tCtrl+C"));
	menu.Append(MENU_COPY_SEL_TRANS, to_wx("仅复制所选译文"));
	menu.Append(MENU_COPY_SEL_ORIG, to_wx("仅复制所选原文"));
	menu.Append(MENU_COPY_SEL_DETAILED, to_wx("复制所选详细内容 (带序号/时间)"));
	menu.AppendSeparator();
	menu.Append(MENU_SELECT_ALL, to_wx("全选行\tCtrl+A"));
	menu.Append(MENU_CHECK_ALL, to_wx("勾选所选行"));
	menu.Append(MENU_UNCHECK_ALL, to_wx("取消勾选全部行"));
	menu.AppendSeparator();
	menu.Append(MENU_COPY_ALL_BOTH, to_wx("复制全部行 (原文 + 译文)"));
	menu.Append(MENU_COPY_ALL_TRANS, to_wx("仅复制全部译文"));

	menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
		switch (e.GetId()) {
		case MENU_COPY_SEL_BOTH:
			CopyItems(GetSelectedOrCheckedAudioIndices(), COPY_BOTH_TAB);
			break;
		case MENU_COPY_SEL_TRANS:
			CopyItems(GetSelectedOrCheckedAudioIndices(), COPY_TRANS_ONLY);
			break;
		case MENU_COPY_SEL_ORIG:
			CopyItems(GetSelectedOrCheckedAudioIndices(), COPY_ORIG_ONLY);
			break;
		case MENU_COPY_SEL_DETAILED:
			CopyItems(GetSelectedOrCheckedAudioIndices(), COPY_DETAILED);
			break;
		case MENU_SELECT_ALL:
			for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
				audio_list_->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
			}
			break;
		case MENU_CHECK_ALL: {
			auto sel = GetSelectedOrCheckedAudioIndices();
			for (int idx : sel) audio_list_->CheckItem(idx, true);
			break;
		}
		case MENU_UNCHECK_ALL:
			for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
				audio_list_->CheckItem(i, false);
			}
			break;
		case MENU_COPY_ALL_BOTH: {
			std::vector<int> all;
			for (int i = 0; i < audio_list_->GetItemCount(); ++i) all.push_back(i);
			CopyItems(all, COPY_BOTH_TAB);
			break;
		}
		case MENU_COPY_ALL_TRANS: {
			std::vector<int> all;
			for (int i = 0; i < audio_list_->GetItemCount(); ++i) all.push_back(i);
			CopyItems(all, COPY_TRANS_ONLY);
			break;
		}
		}
	});

	audio_list_->PopupMenu(&menu, client_pos);
}

void AIResultPanel::OnAudioListContextMenu(wxContextMenuEvent& event) {
	wxPoint pos = event.GetPosition();
	if (pos != wxDefaultPosition) {
		pos = audio_list_->ScreenToClient(pos);
	}
	ShowAudioContextMenu(pos);
}

void AIResultPanel::OnAudioListKeyDown(wxKeyEvent& event) {
	int code = event.GetKeyCode();
	if (event.CmdDown() && (code == 'C' || code == 'c')) {
		auto sel = GetSelectedOrCheckedAudioIndices();
		if (sel.empty()) {
			SetStatus("没有选中的内容可复制");
		} else {
			CopyItems(sel, COPY_BOTH_TAB);
		}
	} else if (event.CmdDown() && (code == 'A' || code == 'a')) {
		for (int i = 0; i < audio_list_->GetItemCount(); ++i) {
			audio_list_->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
		}
	} else {
		event.Skip();
	}
}

void AIResultPanel::OnAudioCopy(wxCommandEvent&) {
	auto targets = GetSelectedOrCheckedAudioIndices();
	if (targets.empty()) {
		SetStatus("没有可复制的内容");
		return;
	}
	CopyItems(targets, COPY_BOTH_TAB);
}

void AIResultPanel::OnAudioRetry(wxCommandEvent&) {
	RunAIAudioTranslate(c);
}

void AIResultPanel::OnAudioClear(wxCommandEvent&) {
	audio_items_.clear();
	audio_list_->DeleteAllItems();
	audio_apply_sel_->Disable();
	audio_apply_all_->Disable();
	if (audio_copy_btn_) audio_copy_btn_->Disable();
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

	AIProfile profile = AIGetActiveProfile();
	if (profile.api_key.empty()) {
		if (c->aiPanel) {
			c->aiPanel->SetStatus("请先在【设置】中配置当前方案的 API 密钥");
			c->aiPanel->ShowSettings();
		}
		return;
	}

	int frame_n = c->videoController->GetFrameN();
	std::shared_ptr<VideoFrame> frame;
	try {
		double time_sec = c->project->Timecodes().TimeAtFrame(frame_n) / 1000.0;
		frame = provider->GetFrame(frame_n, time_sec, false);
	} catch (std::exception& e) {
		if (c->aiPanel) c->aiPanel->SetStatus(std::string("获取视频帧失败: ") + e.what());
		return;
	}
	if (!frame) {
		if (c->aiPanel) c->aiPanel->SetStatus("获取视频帧失败（空帧）");
		return;
	}

	int w = provider->GetWidth();
	int h = provider->GetHeight();
	if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0) {
		double rx0 = OPT_GET("Tool/AI/Region X0")->GetDouble();
		double ry0 = OPT_GET("Tool/AI/Region Y0")->GetDouble();
		double rx1 = OPT_GET("Tool/AI/Region X1")->GetDouble();
		double ry1 = OPT_GET("Tool/AI/Region Y1")->GetDouble();
		if (rx1 > rx0 && ry1 > ry0) {
			x0 = std::clamp(static_cast<int>(rx0 * w / 100.0), 0, w - 1);
			y0 = std::clamp(static_cast<int>(ry0 * h / 100.0), 0, h - 1);
			x1 = std::clamp(static_cast<int>(rx1 * w / 100.0), 0, w);
			y1 = std::clamp(static_cast<int>(ry1 * h / 100.0), 0, h);
		} else {
			x0 = 0; y0 = static_cast<int>(h * 0.70);
			x1 = w; y1 = h;
		}
	}
	int rw = x1 - x0;
	int rh = y1 - y0;
	if (rw <= 0 || rh <= 0) {
		if (c->aiPanel) c->aiPanel->SetStatus("截图区域无效");
		return;
	}

	std::vector<uint8_t> rgb(static_cast<size_t>(rw) * rh * 3);
	const uint8_t* src = frame->data.data();
	int pitch = frame->width * 4;
	for (int y = 0; y < rh; ++y) {
		const uint8_t* row = src + (y0 + y) * pitch + x0 * 4;
		uint8_t* dst = rgb.data() + y * rw * 3;
		for (int x = 0; x < rw; ++x) {
			dst[x * 3 + 0] = row[x * 4 + 2]; // R
			dst[x * 3 + 1] = row[x * 4 + 1]; // G
			dst[x * 3 + 2] = row[x * 4 + 0]; // B
		}
	}

	std::string img_b64;
	std::string mime = "image/jpeg";
	try {
		wxImage img(rw, rh, rgb.data(), true);
		wxMemoryOutputStream os;
		if (!img.SaveFile(os, wxBITMAP_TYPE_JPEG)) {
			if (c->aiPanel) c->aiPanel->SetStatus("图像编码失败");
			return;
		}
		wxStreamBuffer* buf = os.GetOutputStreamBuffer();
		const char* img_buf = static_cast<const char*>(buf->GetBufferStart());
		size_t img_len = buf->GetBufferSize();
		img_b64 = base64_encode(img_buf, img_len);
	} catch (std::exception& e) {
		if (c->aiPanel) c->aiPanel->SetStatus(std::string("取帧失败：") + e.what());
		return;
	} catch (...) {
		if (c->aiPanel) c->aiPanel->SetStatus("取帧失败");
		return;
	}

	std::string prompt = "这是动画/影视视频的画面截图。识别图中的字幕或对白文字（忽略水印和台标）";
	if (profile.source_lang != "自动检测" && !profile.source_lang.empty()) {
		prompt += "，原语言为" + profile.source_lang + "，请准确转写";
	}
	prompt += "，并翻译成" + profile.target_lang + "，用词自然、符合字幕阅读习惯。严格按两行输出：\n原文=识别出的文字\n译文=翻译结果";

	std::string body = "{\"model\":\"" + jesc(profile.vision_model) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
		+ "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:" + mime + ";base64," + img_b64 + "\"}},"
		+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}],\"max_tokens\":512}";

	auto panel = c->aiPanel;
	if (!panel) return;
	panel->BeginJob("识别中…");
	panel->SetStatus("正在调用大模型识别并翻译，请稍候…");
	auto alive = panel->GetAlive();
	auto cancel = panel->GetCancel();

	agi::dispatch::Background().Async([panel, alive, cancel, profile, body] {
		AIResult r = AIChatComplete(profile, body, false, cancel);
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

	AIProfile profile = AIGetActiveProfile();
	if (profile.api_key.empty()) {
		if (c->aiPanel) {
			c->aiPanel->SetStatus("请先在【设置】中配置当前方案的 API 密钥");
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

	std::string prompt;
	std::string model_lower = profile.audio_model;
	for (char &ch : model_lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

	bool is_omni = (model_lower.find("omni") != std::string::npos
		|| model_lower.find("gemini") != std::string::npos
		|| model_lower.find("gpt") != std::string::npos);

	if (is_omni) {
		prompt = "听这段音频：先转写说话内容原文";
		if (profile.source_lang != "自动检测" && !profile.source_lang.empty()) {
			prompt += "（原语言为" + profile.source_lang + "，请准确转写该语言，切勿写成其他语言）";
		}
		prompt += "，再翻译成" + profile.target_lang + "（字幕风格）。严格按两行输出：\n原文=...\n译文=...";
	} else {
		prompt = "转写这段音频中的语音内容";
		if (profile.source_lang != "自动检测" && !profile.source_lang.empty()) {
			prompt += "（原语言为" + profile.source_lang + "）";
		}
		prompt += "。";
	}

	auto panel = c->aiPanel;
	if (!panel) return;
	panel->SetAudioLines(lines);
	panel->InitAudioItems(static_cast<int>(jobs.size()));
	panel->BeginJob("音频翻译中…");
	panel->SetStatus(agi::format("共 %d 行，开始切取音频并识别翻译…", static_cast<int>(jobs.size())));

	auto alive = panel->GetAlive();
	auto cancel = panel->GetCancel();
	int total = static_cast<int>(jobs.size());

	agi::dispatch::Background().Async([panel, alive, cancel, provider, profile, prompt, is_omni, model_lower, jobs, total] {
		int done = 0;
		for (auto const& j : jobs) {
			if (cancel->load()) break;

			AIAudioItem item;
			item.idx = j.idx;
			item.start_ms = j.start_ms;
			item.time_str = ms_to_hms(j.start_ms);

			std::string wav = build_wav16k(provider, j.s0, j.s1);
			AIResult r;
			if (wav.empty()) {
				r.error = "切分音频失败（时间区间无效或解码错误）";
			} else {
				std::string b64 = base64_encode(wav.data(), wav.size());

				// 音频数据格式兼容：
				// 1. 阿里百炼（DashScope）OpenAI兼容模式要求标准的 Data URI 格式（data:audio/wav;base64,...）
				// 2. Google Gemini、OpenAI 官方及标准 OpenAI 兼容接口要求纯 Base64 数据串（严禁带 data:;base64, 前缀，否则报 HTTP 400 Failed to base64-decode audio data）
				bool is_dashscope = (profile.endpoint.empty()
					|| profile.endpoint.find("dashscope") != std::string::npos
					|| profile.endpoint.find("aliyuncs.com") != std::string::npos);
				if (model_lower.find("gemini") != std::string::npos || model_lower.find("gpt") != std::string::npos) {
					is_dashscope = false;
				}

				std::string audio_data_str = is_dashscope
					? ("data:audio/wav;base64," + b64)
					: b64;

				std::string body = "{\"model\":\"" + jesc(profile.audio_model) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
					+ "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"" + audio_data_str + "\",\"format\":\"wav\"}},"
					+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}]}";
				r = AIChatComplete(profile, body, false, cancel);
			}

			if (r.ok) {
				item.success = true;
				item.orig = extract_between(r.text, k_orig, k_yiwen);
				item.trans = extract_after(r.text, k_yiwen, false);
				if (item.trans.empty()) item.trans = trim_ws(r.text);
			} else {
				item.success = false;
				item.error = r.error;
			}

			int dn = ++done;
			agi::dispatch::Main().Async([panel, alive, item, dn, total] {
				if (!*alive) return;
				panel->UpdateAudioItem(item, dn, total);
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

// ------------------------------------------------------------ 网络调用核心（含重试与代理）

#ifdef WITH_UPDATE_CHECKER
static AIResult AIChatCompleteOnce(const AIProfile& profile, const std::string& body,
                                   bool stream,
                                   const std::shared_ptr<std::atomic<bool>>& cancel) {
	AIResult r;
	CURL* h = curl_easy_init();
	if (!h) { r.error = "curl 初始化失败"; return r; }

	std::string response;
	struct curl_slist* hdrs = nullptr;
	std::string auth = "Authorization: Bearer " + profile.api_key;
	hdrs = curl_slist_append(hdrs, auth.c_str());
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, "Expect:"); // 禁用 100-continue

	const std::string& url = profile.endpoint.empty()
		? "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
		: profile.endpoint;

	curl_easy_setopt(h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, curl_xfer_cb);
	curl_easy_setopt(h, CURLOPT_XFERINFODATA, cancel.get());

	long timeout_val = profile.timeout_sec > 0 ? profile.timeout_sec : 60L;
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_val);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "Aegisub AI Assistant");

	// 代理支持
	if (profile.proxy_type != "None" && !profile.proxy_host.empty()) {
		if (profile.proxy_type == "SOCKS5")
			curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
		else
			curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);

		curl_easy_setopt(h, CURLOPT_PROXY, profile.proxy_host.c_str());
		if (profile.proxy_port > 0)
			curl_easy_setopt(h, CURLOPT_PROXYPORT, static_cast<long>(profile.proxy_port));

		if (!profile.proxy_user.empty()) {
			std::string userpwd = profile.proxy_user + ":" + profile.proxy_pass;
			curl_easy_setopt(h, CURLOPT_PROXYUSERPWD, userpwd.c_str());
		}
	}

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
			+ "（请检查网络、超时或代理）";
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
			r.error = "模型未开通：" + detail + "\n请到控制台「模型广场」开通该模型后重试";
		else if (code == 429)
			r.error = "请求过于频繁或额度不足（HTTP 429）：" + detail;
		else
			r.error = "HTTP " + std::to_string(code) + "：" + detail;
		return r;
	}

	std::string content;
	if (stream) {
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
}
#endif

AIResult AIChatComplete(const AIProfile& profile, const std::string& body,
                        bool stream,
                        const std::shared_ptr<std::atomic<bool>>& cancel) {
#ifndef WITH_UPDATE_CHECKER
	(void)profile; (void)body; (void)stream; (void)cancel;
	AIResult r;
	r.error = "此版本编译时未启用网络（libcurl）支持";
	return r;
#else
	int retries = std::clamp(profile.max_retries, 0, 5);
	AIResult r;
	for (int attempt = 0; attempt <= retries; ++attempt) {
		if (cancel && cancel->load()) {
			r.ok = false;
			r.error = "已取消";
			return r;
		}
		r = AIChatCompleteOnce(profile, body, stream, cancel);
		if (r.ok || (cancel && cancel->load())) {
			return r;
		}
		// 若为认证失败或参数错误等无需重试的错误，直接返回
		if (r.error.find("API 密钥无效") != std::string::npos ||
		    r.error.find("模型未开通") != std::string::npos ||
		    r.error.find("已取消") != std::string::npos) {
			return r;
		}
		// 若为超时、网络波动或 HTTP 429 速率限制，退避重试
		if (attempt < retries) {
			int wait_ms = (attempt + 1) * 1500;
			for (int w = 0; w < wait_ms / 100; ++w) {
				if (cancel && cancel->load()) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	}
	return r;
#endif
}

// ------------------------------------------------------------ 样式快切实现

void AIResultPanel::OnQuickStyleToggle(wxCommandEvent &) {
	bool enabled = quick_style_chk_->GetValue();
	OPT_SET("Tool/Playback/Review Quick Style")->SetBool(enabled);
	if (enabled) {
		SyncStyles();
	}
	SetStatus(enabled ? "已开启样式快切与画面联动" : "已关闭样式快切");
}

void AIResultPanel::LoadStyleOrder() {
	std::string saved = OPT_GET("Tool/Playback/Review Quick Style Order")->GetString();
	if (saved.empty()) return;
	ordered_styles_.clear();
	std::vector<std::string> parts;
	agi::Split(parts, saved, ',');
	for (auto const& p : parts) {
		if (!p.empty())
			ordered_styles_.push_back(p);
	}
}

void AIResultPanel::SaveStyleOrder() {
	std::string res;
	for (size_t i = 0; i < ordered_styles_.size(); ++i) {
		if (i > 0) res += ",";
		res += ordered_styles_[i];
	}
	OPT_SET("Tool/Playback/Review Quick Style Order")->SetString(res);
}

void AIResultPanel::SyncStyles() {
	if (!c || !c->ass) return;
	auto actual_styles = c->ass->GetStyles();
	if (actual_styles.empty()) {
		ordered_styles_.clear();
		RefreshStyleList();
		return;
	}

	if (ordered_styles_.empty()) {
		LoadStyleOrder();
	}

	std::vector<std::string> updated;
	for (auto const& s : ordered_styles_) {
		if (std::find(actual_styles.begin(), actual_styles.end(), s) != actual_styles.end()) {
			if (std::find(updated.begin(), updated.end(), s) == updated.end())
				updated.push_back(s);
		}
	}
	for (auto const& s : actual_styles) {
		if (std::find(updated.begin(), updated.end(), s) == updated.end())
			updated.push_back(s);
	}
	ordered_styles_ = std::move(updated);
	RefreshStyleList();
}

void AIResultPanel::RefreshStyleList() {
	if (!style_list_) return;
	int sel = style_list_->GetSelection();
	style_list_->Clear();
	for (size_t i = 0; i < ordered_styles_.size(); ++i) {
		wxString label;
		if (i < 9) {
			label = fmt_wx("[%d]  %s", static_cast<int>(i + 1), to_wx(ordered_styles_[i]));
		} else if (i == 9) {
			label = fmt_wx("[0]  %s", to_wx(ordered_styles_[i]));
		} else {
			label = fmt_wx("      %s", to_wx(ordered_styles_[i]));
		}
		style_list_->Append(label);
	}
	if (sel >= 0 && sel < static_cast<int>(style_list_->GetCount()))
		style_list_->SetSelection(sel);
}

void AIResultPanel::OnMoveStyleUp(wxCommandEvent &) {
	int sel = style_list_->GetSelection();
	if (sel > 0 && sel < static_cast<int>(ordered_styles_.size())) {
		std::swap(ordered_styles_[sel], ordered_styles_[sel - 1]);
		RefreshStyleList();
		style_list_->SetSelection(sel - 1);
		SaveStyleOrder();
	}
}

void AIResultPanel::OnMoveStyleDown(wxCommandEvent &) {
	int sel = style_list_->GetSelection();
	if (sel >= 0 && sel + 1 < static_cast<int>(ordered_styles_.size())) {
		std::swap(ordered_styles_[sel], ordered_styles_[sel + 1]);
		RefreshStyleList();
		style_list_->SetSelection(sel + 1);
		SaveStyleOrder();
	}
}

void AIResultPanel::OnStyleListDClick(wxCommandEvent &) {
	int sel = style_list_->GetSelection();
	if (sel >= 0 && sel < static_cast<int>(ordered_styles_.size())) {
		ApplyStyleName(ordered_styles_[sel]);
	}
}

void AIResultPanel::OnStyleApply(wxCommandEvent &) {
	int sel = style_list_->GetSelection();
	if (sel >= 0 && sel < static_cast<int>(ordered_styles_.size())) {
		ApplyStyleName(ordered_styles_[sel]);
	}
}

void AIResultPanel::ApplyStyle(int number_key_index) {
	if (ordered_styles_.empty()) {
		SyncStyles();
	}
	if (number_key_index >= 0 && number_key_index < static_cast<int>(ordered_styles_.size())) {
		ApplyStyleName(ordered_styles_[number_key_index]);
		if (style_list_ && number_key_index < static_cast<int>(style_list_->GetCount()))
			style_list_->SetSelection(number_key_index);
	}
}

void AIResultPanel::ApplyStyleName(std::string const& style_name) {
	if (!c || !c->selectionController || !c->ass) return;
	auto sel = c->selectionController->GetSelectedSet();
	if (sel.empty()) {
		if (auto active = c->selectionController->GetActiveLine()) {
			sel.insert(active);
		}
	}
	if (sel.empty()) return;

	for (auto line : sel) {
		line->Style = style_name;
	}
	c->ass->Commit(_("change style"), AssFile::COMMIT_DIAG_META);
	SetStatus("已应用样式: " + style_name);
	if (c->frame) {
		c->frame->StatusTimeout(to_wx("已应用样式: " + style_name), 2000);
	}
}


// ------------------------------------------------------------ 智能人声切分与说话人识别对话框

class VADDiarizeDialog : public wxDialog {
private:
	agi::Context* c_;
	std::shared_ptr<std::atomic<bool>> cancel_;
	std::shared_ptr<std::atomic<bool>> alive_;
	std::atomic<bool> running_{false};
	std::thread worker_thread_;

	wxStaticText* model_status_lbl_ = nullptr;
	wxRadioBox* range_box_ = nullptr;

	wxSpinCtrl* pad_start_ctrl_ = nullptr;
	wxSpinCtrl* pad_end_ctrl_ = nullptr;
	wxComboBox* threshold_cb_ = nullptr;
	wxSpinCtrl* min_speech_ctrl_ = nullptr;
	wxSpinCtrl* min_silence_ctrl_ = nullptr;
	wxCheckBox* filter_sfx_chk_ = nullptr;

	wxRadioButton* rb_mode_ref_ = nullptr;
	wxRadioButton* rb_mode_cluster_ = nullptr;
	wxRadioButton* rb_mode_vad_only_ = nullptr;

	wxCheckListBox* ref_speaker_list_ = nullptr;
	wxCheckBox* filter_unmatched_chk_ = nullptr;
	wxSlider* ref_sim_slider_ = nullptr;
	wxStaticText* ref_sim_val_lbl_ = nullptr;

	wxComboBox* cluster_count_cb_ = nullptr;

	wxComboBox* insert_pos_cb_ = nullptr;
	wxCheckBox* fill_label_chk_ = nullptr;
	wxCheckBox* link_style_chk_ = nullptr;

	wxGauge* progress_gauge_ = nullptr;
	wxStaticText* progress_status_ = nullptr;
	wxButton* start_btn_ = nullptr;
	wxButton* cancel_btn_ = nullptr;
	wxButton* close_btn_ = nullptr;

	std::map<std::string, ReferenceSpeaker> detected_speakers_;

	void CollectReferenceSpeakers() {
		if (!c_ || !c_->ass || !c_->project->AudioProvider()) return;
		auto* provider = c_->project->AudioProvider();

		struct LineTime { int s, e; };
		std::map<std::string, std::vector<LineTime>> actor_lines;

		for (const auto& diag : c_->ass->Events) {
			if (diag.Comment) continue;
			std::string actor = diag.Actor.get();
			if (actor.empty()) continue;
			if (diag.End > diag.Start + 150) {
				actor_lines[actor].push_back({diag.Start, diag.End});
			}
		}

		detected_speakers_.clear();
		if (ref_speaker_list_) ref_speaker_list_->Clear();

		int idx = 0;
		for (const auto& pair : actor_lines) {
			const std::string& name = pair.first;
			const auto& lines = pair.second;
			int total_ms = 0;
			for (const auto& l : lines) total_ms += (l.e - l.s);

			ReferenceSpeaker ref;
			ref.name = name;
			ref.count = static_cast<int>(lines.size());
			ref.total_duration_ms = total_ms;

			std::vector<float> sum_timbre(24, 0.0f);
			int valid_cnt = 0;

			// 性能优化：限制最多抽样 12 条或累计 30 秒有效语音，极速完成声纹特征中心建模，避免上千行字幕冻结 UI
			int sampled_ms = 0;
			for (const auto& l : lines) {
				if (valid_cnt >= 12 || sampled_ms >= 30000) break;
				int dur = l.e - l.s;
				if (dur < 250 && lines.size() > 3) continue;
				int rate = provider->GetSampleRate();
				int64_t s0 = static_cast<int64_t>(l.s) * rate / 1000;
				int64_t s1 = static_cast<int64_t>(l.e) * rate / 1000;
				auto pcm = ExtractAudio16kMono(provider, s0, s1);
				if (pcm.size() >= 400) {
					float harm = 0.0f;
					auto t = TimbreExtractor::Extract(pcm.data(), pcm.size(), &harm);
					for (size_t d = 0; d < 24; ++d) sum_timbre[d] += t[d];
					valid_cnt++;
					sampled_ms += dur;
				}
			}

			if (valid_cnt > 0) {
				float norm = 0.0f;
				for (size_t d = 0; d < 24; ++d) {
					sum_timbre[d] /= valid_cnt;
					norm += sum_timbre[d] * sum_timbre[d];
				}
				norm = std::sqrt(norm);
				if (norm > 1e-7f) {
					for (size_t d = 0; d < 24; ++d) sum_timbre[d] /= norm;
				}
				ref.centroid = sum_timbre;
				detected_speakers_[name] = ref;

				wxString label = fmt_wx("%s  (已标注 %d 条，总长 %.1f 秒)",
					to_wx(name), ref.count, total_ms / 1000.0f);
				ref_speaker_list_->Append(label);
				ref_speaker_list_->Check(idx, true);
				idx++;
			}
		}
	}

	void OnStartClick(wxCommandEvent&) {
		if (running_) return;
		if (!c_ || !c_->project->AudioProvider()) {
			wxMessageBox(to_wx("当前未加载音频文件，请先打开音频或视频。"), to_wx("提示"), wxOK | wxICON_INFORMATION, this);
			return;
		}
		if (!SileroVAD::Get().IsAvailable()) {
			if (!SileroVAD::Get().Init()) {
				wxMessageBox(to_wx("未能初始化 Silero VAD 模型。\n请确认 silero_vad.onnx 和 onnxruntime.dll 已存在于程序目录下。"),
					to_wx("模型未就绪"), wxOK | wxICON_ERROR, this);
				return;
			}
		}

		VADConfig cfg;
		cfg.pad_start_ms = pad_start_ctrl_->GetValue();
		cfg.pad_end_ms = pad_end_ctrl_->GetValue();
		int th_sel = threshold_cb_->GetSelection();
		if (th_sel == 0) cfg.threshold = 0.35f;
		else if (th_sel == 2) cfg.threshold = 0.65f;
		else if (th_sel == 3) cfg.threshold = 0.80f;
		else cfg.threshold = 0.50f;
		cfg.neg_threshold = std::max(0.01f, cfg.threshold - 0.15f);
		cfg.min_speech_ms = min_speech_ctrl_->GetValue();
		cfg.min_silence_ms = min_silence_ctrl_->GetValue();
		cfg.filter_sfx = filter_sfx_chk_->GetValue();

		if (rb_mode_ref_->GetValue()) cfg.mode = VADConfig::MODE_REFERENCE;
		else if (rb_mode_cluster_->GetValue()) cfg.mode = VADConfig::MODE_CLUSTER;
		else cfg.mode = VADConfig::MODE_SEGMENT_ONLY;

		cfg.filter_unmatched = filter_unmatched_chk_->GetValue();
		cfg.ref_threshold = ref_sim_slider_->GetValue() / 100.0f;
		cfg.cluster_speakers = cluster_count_cb_->GetSelection();

		std::map<std::string, ReferenceSpeaker> active_refs;
		if (cfg.mode == VADConfig::MODE_REFERENCE) {
			for (size_t i = 0; i < ref_speaker_list_->GetCount(); ++i) {
				if (ref_speaker_list_->IsChecked(i)) {
					int k = 0;
					for (const auto& pair : detected_speakers_) {
						if (k == static_cast<int>(i)) {
							active_refs[pair.first] = pair.second;
							break;
						}
						k++;
					}
				}
			}
			if (active_refs.empty() && detected_speakers_.empty()) {
				wxMessageBox(to_wx("当前字幕中未检测到已标注说话人(Actor)的参考行。\n系统将自动切换为【自动区分说话人】模式。"),
					to_wx("提示"), wxOK | wxICON_INFORMATION, this);
				cfg.mode = VADConfig::MODE_CLUSTER;
				rb_mode_cluster_->SetValue(true);
			}
		}

		int64_t total_ms = c_->project->AudioProvider()->GetNumSamples() * 1000 / c_->project->AudioProvider()->GetSampleRate();
		int64_t start_ms = 0;
		int64_t end_ms = total_ms;

		if (range_box_->GetSelection() == 1) {
			auto sel = c_->selectionController->GetSelectedSet();
			if (!sel.empty()) {
				int min_s = 2147483647, max_e = 0;
				for (auto line : sel) {
					if (line->Start < min_s) min_s = line->Start;
					if (line->End > max_e) max_e = line->End;
				}
				if (max_e > min_s) {
					start_ms = min_s;
					end_ms = max_e;
				}
			}
		}

		running_ = true;
		cancel_->store(false);
		start_btn_->Disable();
		cancel_btn_->Enable();
		close_btn_->Disable();
		progress_gauge_->SetValue(0);
		progress_status_->SetLabel(to_wx("准备就绪，开始提取音频并检测..."));

		int insert_mode = insert_pos_cb_->GetSelection();
		bool fill_label = fill_label_chk_->GetValue();
		bool link_style = link_style_chk_->GetValue();

		auto* provider = c_->project->AudioProvider();

		if (worker_thread_.joinable()) worker_thread_.join();

		auto alive = alive_;
		worker_thread_ = std::thread([this, alive, provider, start_ms, end_ms, cfg, active_refs, insert_mode, fill_label, link_style]() {
			auto on_prog = [this, alive](float p, const std::string& msg) {
				wxTheApp->CallAfter([this, alive, p, msg]() {
					if (!alive->load()) return;
					if (progress_gauge_) progress_gauge_->SetValue(static_cast<int>(p * 100));
					if (progress_status_) progress_status_->SetLabel(to_wx(msg));
				});
			};

			VADStats stats;
			std::vector<VADSegment> results = RunVADPipeline(
				provider, start_ms, end_ms, cfg, active_refs, on_prog, cancel_, &stats);

			wxTheApp->CallAfter([this, alive, results, stats, insert_mode, fill_label, link_style]() {
				if (!alive->load()) return;
				running_ = false;
				start_btn_->Enable();
				cancel_btn_->Disable();
				close_btn_->Enable();

				if (cancel_->load()) {
					progress_status_->SetLabel(to_wx("已取消任务"));
					return;
				}

				if (results.empty()) {
					if (stats.raw_detected == 0) {
						progress_status_->SetLabel(to_wx("未检测到人声活动（可尝试调低抗噪灵敏度至【灵敏】或检查选区）"));
					} else if (stats.filtered_by_sfx > 0 && stats.raw_detected == stats.filtered_by_sfx) {
						progress_status_->SetLabel(fmt_wx("检测到 %d 处人声，但均被【游戏音效/背景音过滤】剔除（可尝试取消勾选抗噪）", stats.raw_detected));
					} else if (stats.filtered_by_speaker > 0) {
						progress_status_->SetLabel(fmt_wx("检测到 %d 处人声，但与已选角色音色均不匹配（可调低相似度或改用自动聚类）", stats.raw_detected));
					} else {
						progress_status_->SetLabel(to_wx("未检测到符合条件的人声片段"));
					}
					return;
				}

				ApplyResultsToSubtitles(results, insert_mode, fill_label, link_style);
			});
		});
	}

	void ApplyResultsToSubtitles(const std::vector<VADSegment>& results, int insert_mode, bool fill_label, bool link_style) {
		if (!c_ || !c_->ass) return;

		std::vector<std::string> styles = c_->ass->GetStyles();
		std::string saved_order = OPT_GET("Tool/Playback/Review Quick Style Order")->GetString();
		std::vector<std::string> ordered_styles;
		if (!saved_order.empty()) {
			agi::Split(ordered_styles, saved_order, ',');
		}
		if (ordered_styles.empty()) ordered_styles = styles;

		if (insert_mode == 1) {
			c_->ass->Events.clear_and_dispose([](AssDialogue *e) { delete e; });
		}

		int inserted_count = 0;
		for (const auto& seg : results) {
			auto* line = new AssDialogue;
			line->Start = static_cast<int>(seg.start_ms);
			line->End = static_cast<int>(seg.end_ms);
			line->Actor = seg.speaker;

			if (fill_label && !seg.speaker.empty()) {
				line->Text = "[" + seg.speaker + "]";
			} else {
				line->Text = "";
			}

			if (link_style && seg.speaker_id >= 0 && seg.speaker_id < static_cast<int>(ordered_styles.size())) {
				line->Style = ordered_styles[seg.speaker_id];
			} else if (!styles.empty()) {
				line->Style = styles[0];
			}

			c_->ass->Events.push_back(*line);
			inserted_count++;
		}

		c_->ass->Commit(to_wx("智能人声切分与说话人识别"), AssFile::COMMIT_DIAG_ADDREM);

		wxString finish_msg = fmt_wx("人声切分完成！成功生成 %d 行时间轴（开始紧贴发声，结束延长200ms）。", inserted_count);
		progress_status_->SetLabel(finish_msg);
		if (c_->frame) {
			c_->frame->StatusTimeout(finish_msg, 5000);
		}
		wxMessageBox(finish_msg, to_wx("完成"), wxOK | wxICON_INFORMATION, this);
	}

public:
	VADDiarizeDialog(wxWindow* parent, agi::Context* c)
	: wxDialog(parent, wxID_ANY, to_wx("智能人声切分与说话人识别 (AI VAD & Diarization)"),
		wxDefaultPosition, wxSize(660, 740),
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, c_(c)
	, cancel_(std::make_shared<std::atomic<bool>>(false))
	, alive_(std::make_shared<std::atomic<bool>>(true))
	{
		auto* main_sizer = new wxBoxSizer(wxVERTICAL);

		// 1. 模型状态指示
		bool vad_ok = SileroVAD::Get().IsAvailable();
		if (!vad_ok) vad_ok = SileroVAD::Get().Init();

		auto* status_box = new wxBoxSizer(wxHORIZONTAL);
		wxString status_str = vad_ok
			? to_wx("● Silero VAD v5 模型就绪（已动态启用 ONNX Runtime 引擎）")
			: to_wx("○ 未能找到 silero_vad.onnx 模型文件（请置于程序运行目录）");
		model_status_lbl_ = new wxStaticText(this, wxID_ANY, status_str);
		model_status_lbl_->SetForegroundColour(vad_ok ? wxColour(40, 160, 60) : wxColour(220, 50, 50));
		status_box->Add(model_status_lbl_, 1, wxALIGN_CENTER_VERTICAL);
		main_sizer->Add(status_box, 0, wxEXPAND | wxALL, 8);

		// 范围选择
		wxArrayString range_choices;
		range_choices.Add(to_wx("整段音频"));
		range_choices.Add(to_wx("仅当前选择的音频选区范围"));
		range_box_ = new wxRadioBox(this, wxID_ANY, to_wx("处理范围"), wxDefaultPosition, wxDefaultSize, range_choices, 1, wxRA_SPECIFY_ROWS);
		range_box_->SetSelection(0);
		main_sizer->Add(range_box_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 2. 时间轴对齐与抗噪切分
		auto* time_box = new wxStaticBox(this, wxID_ANY, to_wx("时间轴对齐与抗噪切分"));
		auto* time_sizer = new wxStaticBoxSizer(time_box, wxVERTICAL);

		auto* grid_time = new wxFlexGridSizer(2, wxSize(10, 6));
		grid_time->AddGrowableCol(1, 1);

		auto add_row = [&](const char* label, wxWindow* ctrl) {
			grid_time->Add(new wxStaticText(time_box, wxID_ANY, to_wx(label)), 0, wxALIGN_CENTER_VERTICAL);
			grid_time->Add(ctrl, 1, wxEXPAND);
		};

		// 前置缓冲
		auto* pad_start_box = new wxBoxSizer(wxHORIZONTAL);
		pad_start_ctrl_ = new wxSpinCtrl(time_box, wxID_ANY, "0", wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS, -100, 200, 0);
		pad_start_box->Add(pad_start_ctrl_, 0, wxRIGHT, 6);
		pad_start_box->Add(new wxStaticText(time_box, wxID_ANY, to_wx("ms（默认 0ms，精准紧贴说话人发声那一刻）")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(new wxStaticText(time_box, wxID_ANY, to_wx("开始时间微调：")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(pad_start_box, 1, wxEXPAND);

		// 后置延长
		auto* pad_end_box = new wxBoxSizer(wxHORIZONTAL);
		pad_end_ctrl_ = new wxSpinCtrl(time_box, wxID_ANY, "200", wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS, 0, 800, 200);
		pad_end_box->Add(pad_end_ctrl_, 0, wxRIGHT, 6);
		pad_end_box->Add(new wxStaticText(time_box, wxID_ANY, to_wx("ms（默认延长 200ms，防止漏掉句尾吞音）")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(new wxStaticText(time_box, wxID_ANY, to_wx("结束向后延长：")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(pad_end_box, 1, wxEXPAND);

		// VAD 灵敏度与抗噪
		wxArrayString th_choices;
		th_choices.Add(to_wx("灵敏 (0.35) —— 适合清晰低语、微弱人声"));
		th_choices.Add(to_wx("标准 (0.50) —— 官方推荐阈值，日常影视与对话（推荐）"));
		th_choices.Add(to_wx("抗噪 (0.65) —— 滤除较强背景杂音"));
		th_choices.Add(to_wx("强抗噪 (0.80) —— 背景有强歌声、游戏爆鸣音时推荐"));
		threshold_cb_ = new wxComboBox(time_box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, th_choices, wxCB_READONLY);
		threshold_cb_->SetSelection(1);
		add_row("抗噪灵敏度：", threshold_cb_);

		// 最小人声时长
		auto* dur_box = new wxBoxSizer(wxHORIZONTAL);
		min_speech_ctrl_ = new wxSpinCtrl(time_box, wxID_ANY, "250", wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS, 50, 1000, 250);
		dur_box->Add(min_speech_ctrl_, 0, wxRIGHT, 6);
		dur_box->Add(new wxStaticText(time_box, wxID_ANY, to_wx("ms（短于此时间的短促敲击、枪声、UI按键音将被过滤）")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(new wxStaticText(time_box, wxID_ANY, to_wx("最小人声时长：")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(dur_box, 1, wxEXPAND);

		// 连贯停顿合并
		auto* sil_box = new wxBoxSizer(wxHORIZONTAL);
		min_silence_ctrl_ = new wxSpinCtrl(time_box, wxID_ANY, "200", wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS, 50, 800, 200);
		sil_box->Add(min_silence_ctrl_, 0, wxRIGHT, 6);
		sil_box->Add(new wxStaticText(time_box, wxID_ANY, to_wx("ms（说话人在同一句话中的短暂换气停顿将自动连贯合并）")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(new wxStaticText(time_box, wxID_ANY, to_wx("连贯停顿合并：")), 0, wxALIGN_CENTER_VERTICAL);
		grid_time->Add(sil_box, 1, wxEXPAND);

		time_sizer->Add(grid_time, 0, wxEXPAND | wxALL, 6);

		filter_sfx_chk_ = new wxCheckBox(time_box, wxID_ANY, to_wx("开启游戏音效与爆炸杂音过滤（基于人声谐波周期性过滤无调性杂音）"));
		filter_sfx_chk_->SetValue(true);
		time_sizer->Add(filter_sfx_chk_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

		main_sizer->Add(time_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 3. 说话人与音色匹配模式
		auto* spk_box = new wxStaticBox(this, wxID_ANY, to_wx("说话人与音色匹配"));
		auto* spk_sizer = new wxStaticBoxSizer(spk_box, wxVERTICAL);

		rb_mode_ref_ = new wxRadioButton(spk_box, wxID_ANY, to_wx("依照现有字幕已标注的说话人 (Actor) 音色匹配（精准对齐角色）"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		rb_mode_cluster_ = new wxRadioButton(spk_box, wxID_ANY, to_wx("自动区分说话人（声纹聚类，自动划分说话人 1, 2...）"));
		rb_mode_vad_only_ = new wxRadioButton(spk_box, wxID_ANY, to_wx("仅切分人声时间轴（不区分说话人，生成空白行）"));

		spk_sizer->Add(rb_mode_ref_, 0, wxALL, 4);

		auto* ref_panel = new wxBoxSizer(wxVERTICAL);
		ref_speaker_list_ = new wxCheckListBox(spk_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 80));
		ref_panel->Add(ref_speaker_list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		filter_unmatched_chk_ = new wxCheckBox(spk_box, wxID_ANY, to_wx("【抗干扰】过滤未匹配音色（仅保留选定角色，自动剔除背景歌声、杂谈与游戏音）"));
		filter_unmatched_chk_->SetValue(true);
		ref_panel->Add(filter_unmatched_chk_, 0, wxLEFT | wxBOTTOM, 4);

		auto* sim_row = new wxBoxSizer(wxHORIZONTAL);
		sim_row->Add(new wxStaticText(spk_box, wxID_ANY, to_wx("音色匹配相似度阈值：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		ref_sim_slider_ = new wxSlider(spk_box, wxID_ANY, 65, 40, 90, wxDefaultPosition, wxSize(180, -1));
		sim_row->Add(ref_sim_slider_, 1, wxEXPAND | wxRIGHT, 6);
		ref_sim_val_lbl_ = new wxStaticText(spk_box, wxID_ANY, to_wx("65%"));
		sim_row->Add(ref_sim_val_lbl_, 0, wxALIGN_CENTER_VERTICAL);
		ref_panel->Add(sim_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		spk_sizer->Add(ref_panel, 0, wxEXPAND | wxLEFT, 18);

		spk_sizer->Add(rb_mode_cluster_, 0, wxALL, 4);

		auto* cluster_row = new wxBoxSizer(wxHORIZONTAL);
		cluster_row->Add(new wxStaticText(spk_box, wxID_ANY, to_wx("预计说话人数：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		wxArrayString cl_choices;
		cl_choices.Add(to_wx("自动推测"));
		cl_choices.Add(to_wx("1 人（单人旁白/解说）"));
		cl_choices.Add(to_wx("2 人（双人对话/采访）"));
		cl_choices.Add(to_wx("3 人"));
		cl_choices.Add(to_wx("4 人"));
		cl_choices.Add(to_wx("5 人"));
		cluster_count_cb_ = new wxComboBox(spk_box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, cl_choices, wxCB_READONLY);
		cluster_count_cb_->SetSelection(0);
		cluster_row->Add(cluster_count_cb_, 0);
		spk_sizer->Add(cluster_row, 0, wxEXPAND | wxLEFT | wxBOTTOM, 22);

		spk_sizer->Add(rb_mode_vad_only_, 0, wxALL, 4);

		main_sizer->Add(spk_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 4. 输出与应用选项
		auto* out_box = new wxStaticBox(this, wxID_ANY, to_wx("输出与应用选项"));
		auto* out_sizer = new wxBoxSizer(wxHORIZONTAL);

		out_sizer->Add(new wxStaticText(out_box, wxID_ANY, to_wx("输出位置：")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		wxArrayString pos_choices;
		pos_choices.Add(to_wx("追加到字幕末尾"));
		pos_choices.Add(to_wx("清空并重新生成"));
		insert_pos_cb_ = new wxComboBox(out_box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, pos_choices, wxCB_READONLY);
		insert_pos_cb_->SetSelection(0);
		out_sizer->Add(insert_pos_cb_, 1, wxRIGHT, 10);

		fill_label_chk_ = new wxCheckBox(out_box, wxID_ANY, to_wx("填入说话人标签 [角色名]"));
		fill_label_chk_->SetValue(false);
		out_sizer->Add(fill_label_chk_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

		link_style_chk_ = new wxCheckBox(out_box, wxID_ANY, to_wx("联动样式快切"));
		link_style_chk_->SetValue(true);
		link_style_chk_->SetToolTip(to_wx("按识别出的说话人序号自动应用样式快切面板中的对应样式"));
		out_sizer->Add(link_style_chk_, 0, wxALIGN_CENTER_VERTICAL);

		main_sizer->Add(out_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 5. 进度条与状态
		progress_gauge_ = new wxGauge(this, wxID_ANY, 100);
		main_sizer->Add(progress_gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		progress_status_ = new wxStaticText(this, wxID_ANY, to_wx("准备就绪。配置完毕后点击下方【开始切分与识别】。"));
		progress_status_->SetForegroundColour(wxColour(100, 100, 100));
		main_sizer->Add(progress_status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 6. 操作按钮
		auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
		start_btn_ = new wxButton(this, wxID_ANY, to_wx("开始切分与识别"));
		cancel_btn_ = new wxButton(this, wxID_ANY, to_wx("取消"));
		cancel_btn_->Disable();
		close_btn_ = new wxButton(this, wxID_ANY, to_wx("关闭"));

		btn_sizer->Add(start_btn_, 0, wxRIGHT, 6);
		btn_sizer->Add(cancel_btn_, 0, wxRIGHT, 6);
		btn_sizer->AddStretchSpacer(1);
		btn_sizer->Add(close_btn_, 0);

		main_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		SetSizerAndFit(main_sizer);
		Centre();

		ref_sim_slider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			ref_sim_val_lbl_->SetLabel(fmt_wx("%d%%", ref_sim_slider_->GetValue()));
		});

		start_btn_->Bind(wxEVT_BUTTON, &VADDiarizeDialog::OnStartClick, this);
		cancel_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) {
				cancel_->store(true);
				progress_status_->SetLabel(to_wx("正在取消任务…"));
				cancel_btn_->Disable();
			}
		});
		close_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) {
				cancel_->store(true);
			}
			EndModal(wxID_CANCEL);
		});

		CollectReferenceSpeakers();
		if (detected_speakers_.empty()) {
			rb_mode_cluster_->SetValue(true);
			rb_mode_ref_->Enable(false);
			filter_unmatched_chk_->Enable(false);
			ref_speaker_list_->Enable(false);
			ref_sim_slider_->Enable(false);
		} else {
			rb_mode_ref_->SetValue(true);
		}

		auto update_modes = [this]() {
			bool is_ref = rb_mode_ref_->GetValue() && !detected_speakers_.empty();
			ref_speaker_list_->Enable(is_ref);
			filter_unmatched_chk_->Enable(is_ref);
			ref_sim_slider_->Enable(is_ref);
			cluster_count_cb_->Enable(rb_mode_cluster_->GetValue());
		};

		rb_mode_ref_->Bind(wxEVT_RADIOBUTTON, [update_modes](wxCommandEvent&) { update_modes(); });
		rb_mode_cluster_->Bind(wxEVT_RADIOBUTTON, [update_modes](wxCommandEvent&) { update_modes(); });
		rb_mode_vad_only_->Bind(wxEVT_RADIOBUTTON, [update_modes](wxCommandEvent&) { update_modes(); });
		update_modes();
	}

	~VADDiarizeDialog() override {
		if (alive_) {
			alive_->store(false);
		}
		if (running_) {
			cancel_->store(true);
		}
		if (worker_thread_.joinable()) {
			worker_thread_.join();
		}
	}
};

void ShowVADDiarizeDialog(agi::Context* c) {
	if (!c) return;
	VADDiarizeDialog dlg(c->frame, c);
	dlg.ShowModal();
}
