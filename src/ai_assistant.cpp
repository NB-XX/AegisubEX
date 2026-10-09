// AI 辅助翻译内嵌实现：
//   功能1：对视频当前画面的指定区域截图，调用百炼 qwen 视觉模型识别并翻译；
//   功能2：按选中行时间轴切取音频（内存中重采样为 16k 单声道 WAV），
//          调用百炼 omni 模型转写并翻译。
// 请求走 libcurl（本构建已链接，Schannel TLS），在后台线程执行、可取消，
// 结果回填到可编辑文本框，一键写回当前行/选中行（走 Commit，支持撤销）。

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
#include "video_frame.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>

#include <wx/button.h>
#include <wx/filename.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
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
/// 找不到时按 whole_fallback 决定回退为整段或空串。与 Lua 版行为一致。
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
				return ""; // 非常见格式
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

// ------------------------------------------------------------ 对话框

class AITranslationDialog final : public wxDialog {
	agi::Context* c;
	AIMode mode;

	std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>> alive_  = std::make_shared<std::atomic<bool>>(true);
	bool running_ = false;

	wxTextCtrl* api_key_ = nullptr;
	wxTextCtrl* model_ = nullptr;
	wxTextCtrl* lang_ = nullptr;
	wxTextCtrl* region_[4] = {};   // 左上X 左上Y 右下X 右下Y（百分比）
	wxTextCtrl* pad_[2] = {};      // 头尾补齐 ms
	wxStaticText* status_ = nullptr;
	wxButton* run_ = nullptr;
	wxButton* cancel_btn_ = nullptr;
	wxButton* apply_ = nullptr;
	wxTextCtrl* orig_ = nullptr;     // 截图模式：原文
	wxTextCtrl* trans_ = nullptr;    // 截图模式：译文
	wxTextCtrl* combined_ = nullptr; // 音频模式：合并结果（可编辑）
	std::vector<AssDialogue*> audio_lines;

	std::string field(wxTextCtrl* t) const {
		return from_wx(t->GetValue());
	}
	void set_field(wxTextCtrl* t, const std::string& s) {
		t->ChangeValue(to_wx(s));
	}
	void status(const std::string& s) {
		status_->SetLabel(to_wx(s));
		status_->GetParent()->Layout();
	}

public:
	AITranslationDialog(agi::Context* ctx, AIMode m)
	: wxDialog(ctx->parent, wxID_ANY,
		m == AIMode::Frame ? to_wx("区域截图翻译") : to_wx("音频翻译选中行"),
		wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, c(ctx), mode(m)
	{
		// ---- 载入配置
		std::string key = opt_str("Tool/AI/API Key");
		if (key.empty()) key = seed_legacy_api_key();
		std::string model = mode == AIMode::Frame
			? opt_str("Tool/AI/Vision Model", "qwen3.8-flash")
			: opt_str("Tool/AI/Audio Model", "qwen3.8-omni-flash");
		std::string lang = opt_str("Tool/AI/Target Lang", "简体中文");

		double rx0 = OPT_GET("Tool/AI/Region X0")->GetDouble();
		double ry0 = OPT_GET("Tool/AI/Region Y0")->GetDouble();
		double rx1 = OPT_GET("Tool/AI/Region X1")->GetDouble();
		double ry1 = OPT_GET("Tool/AI/Region Y1")->GetDouble();
		// 优先复用自动对帧拖过的区域
		if (OPT_GET("Tool/Frame Match/Region X0")->GetDouble() >= 0.0) {
			rx0 = OPT_GET("Tool/Frame Match/Region X0")->GetDouble() * 100.0;
			ry0 = OPT_GET("Tool/Frame Match/Region Y0")->GetDouble() * 100.0;
			rx1 = OPT_GET("Tool/Frame Match/Region X1")->GetDouble() * 100.0;
			ry1 = OPT_GET("Tool/Frame Match/Region Y1")->GetDouble() * 100.0;
		}

		auto* root = new wxBoxSizer(wxVERTICAL);
		auto* grid = new wxFlexGridSizer(2, wxSize(6, 4));
		grid->AddGrowableCol(1, 1);

		auto add_row = [&](const char* label, wxTextCtrl* ctrl) {
			grid->Add(new wxStaticText(this, wxID_ANY, to_wx(label)),
				0, wxALIGN_CENTER_VERTICAL);
			grid->Add(ctrl, 1, wxEXPAND);
		};

		api_key_ = new wxTextCtrl(this, wxID_ANY, to_wx(key), wxDefaultPosition,
			wxSize(320, -1), wxTE_PASSWORD);
		add_row("API 密钥：", api_key_);

		model_ = new wxTextCtrl(this, wxID_ANY, to_wx(model));
		add_row(mode == AIMode::Frame ? "视觉模型：" : "音频模型：", model_);

		lang_ = new wxTextCtrl(this, wxID_ANY, to_wx(lang));
		add_row("目标语言：", lang_);

		auto* extra = new wxBoxSizer(wxHORIZONTAL);
		if (mode == AIMode::Frame) {
			extra->Add(new wxStaticText(this, wxID_ANY,
				to_wx("截取区域 左上X / 左上Y / 右下X / 右下Y（%，0-100）：")),
				0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
			double vals[4] = { rx0, ry0, rx1, ry1 };
			for (int i = 0; i < 4; ++i) {
				region_[i] = new wxTextCtrl(this, wxID_ANY,
					to_wx(std::to_string(std::clamp(static_cast<int>(lround(vals[i])), 0, 100))),
					wxDefaultPosition, wxSize(44, -1));
				extra->Add(region_[i], 0);
				if (i < 3) extra->AddSpacer(4);
			}
		} else {
			extra->Add(new wxStaticText(this, wxID_ANY, to_wx("音频前后补齐(ms)：")),
				0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
			pad_[0] = new wxTextCtrl(this, wxID_ANY,
				to_wx(std::to_string(OPT_GET("Tool/AI/Pad Start")->GetInt())),
				wxDefaultPosition, wxSize(52, -1));
			extra->Add(pad_[0], 0);
			extra->AddSpacer(4);
			pad_[1] = new wxTextCtrl(this, wxID_ANY,
				to_wx(std::to_string(OPT_GET("Tool/AI/Pad End")->GetInt())),
				wxDefaultPosition, wxSize(52, -1));
			extra->Add(pad_[1], 0);
		}
		root->Add(grid, 0, wxEXPAND | wxALL, 8);
		root->Add(extra, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// ---- 操作按钮
		auto* btns = new wxBoxSizer(wxHORIZONTAL);
		run_ = new wxButton(this, wxID_ANY, to_wx("开始翻译"));
		cancel_btn_ = new wxButton(this, wxID_ANY, to_wx("取消"));
		cancel_btn_->Disable();
		btns->Add(run_, 0, wxRIGHT, 6);
		btns->Add(cancel_btn_, 0);
		root->Add(btns, 0, wxLEFT | wxRIGHT, 8);

		status_ = new wxStaticText(this, wxID_ANY, to_wx("就绪"));
		root->Add(status_, 0, wxALL, 8);

		// ---- 结果区
		if (mode == AIMode::Frame) {
			auto* res = new wxFlexGridSizer(2, wxSize(6, 4));
			res->AddGrowableCol(1, 1);
			res->AddGrowableRow(0, 1);
			res->AddGrowableRow(1, 1);
			orig_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
				wxSize(360, 60), wxTE_MULTILINE);
			trans_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
				wxSize(360, 60), wxTE_MULTILINE);
			res->Add(new wxStaticText(this, wxID_ANY, to_wx("原文：")),
				0, wxALIGN_TOP | wxALIGN_RIGHT);
			res->Add(orig_, 1, wxEXPAND);
			res->Add(new wxStaticText(this, wxID_ANY, to_wx("译文：")),
				0, wxALIGN_TOP | wxALIGN_RIGHT);
			res->Add(trans_, 1, wxEXPAND);
			root->Add(res, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
		} else {
			combined_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
				wxSize(440, 260), wxTE_MULTILINE);
			root->Add(combined_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
		}

		auto* apply_row = new wxBoxSizer(wxHORIZONTAL);
		apply_ = new wxButton(this, wxID_ANY,
			mode == AIMode::Frame ? to_wx("填入当前行") : to_wx("替换选中行"));
		apply_->Disable();
		auto* close_btn = new wxButton(this, wxID_CANCEL, to_wx("关闭"));
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			// 强制关闭：中断后台任务后立即退出模态循环
			cancel_->store(true);
			alive_->store(false);
			EndModal(wxID_CANCEL);
		}, close_btn->GetId());
		apply_row->Add(apply_, 0, wxRIGHT, 6);
		apply_row->AddStretchSpacer(1);
		apply_row->Add(close_btn, 0);
		root->Add(apply_row, 0, wxEXPAND | wxALL, 8);

		SetSizerAndFit(root);
		Centre();

		Bind(wxEVT_BUTTON, &AITranslationDialog::OnRun, this, run_->GetId());
		Bind(wxEVT_BUTTON, &AITranslationDialog::OnCancelClick, this, cancel_btn_->GetId());
		Bind(wxEVT_BUTTON, &AITranslationDialog::OnApply, this, apply_->GetId());
		Bind(wxEVT_CLOSE_WINDOW, &AITranslationDialog::OnClose, this);
	}

	~AITranslationDialog() override {
		alive_->store(false);
		cancel_->store(true);
	}

private:
	void save_config() {
		OPT_SET("Tool/AI/API Key")->SetString(field(api_key_));
		OPT_SET(mode == AIMode::Frame ? "Tool/AI/Vision Model" : "Tool/AI/Audio Model")
			->SetString(field(model_));
		OPT_SET("Tool/AI/Target Lang")->SetString(field(lang_));
	}

	void save_frame_region() {
		const char* const keys[4] = { "Tool/AI/Region X0", "Tool/AI/Region Y0",
			"Tool/AI/Region X1", "Tool/AI/Region Y1" };
		for (int i = 0; i < 4; ++i) {
			int p = std::clamp(wxAtoi(region_[i]->GetValue()), 0, 100);
			OPT_SET(keys[i])->SetDouble(p);
		}
	}

	void save_audio_pad() {
		OPT_SET("Tool/AI/Pad Start")->SetInt(std::clamp(wxAtoi(pad_[0]->GetValue()), 0, 5000));
		OPT_SET("Tool/AI/Pad End")->SetInt(std::clamp(wxAtoi(pad_[1]->GetValue()), 0, 5000));
	}

	int percent(wxTextCtrl* t) { return std::clamp(wxAtoi(t->GetValue()), 0, 100); }

	void begin_job(const char* doing) {
		running_ = true;
		cancel_->store(false);
		run_->Disable();
		run_->SetLabel(to_wx(doing));
		cancel_btn_->Enable();
		apply_->Disable();
	}

	void end_job() {
		running_ = false;
		run_->Enable();
		run_->SetLabel(to_wx("重新翻译"));
		cancel_btn_->Disable();
	}

	void OnRun(wxCommandEvent&) {
		std::string key = field(api_key_);
		std::string model = field(model_);
		std::string lang = field(lang_);
		if (key.empty()) { status("请先填写 API 密钥"); return; }
		if (model.empty()) { status("请填写模型名"); return; }

		if (mode == AIMode::Frame)
			run_frame(key, model, lang);
		else
			run_audio(key, model, lang);
	}

	void run_frame(const std::string& key, const std::string& model, const std::string& lang) {
		save_config();
		save_frame_region();

		auto* provider = c->project->VideoProvider();
		if (!provider) { status("未打开视频"); return; }

		// 主线程取当前帧并编码（短暂阻塞，换取无锁安全）
		std::string png_b64;
		try {
			int frame = c->videoController->GetFrameN();
			double time_s = c->videoController->TimeAtFrame(frame, agi::vfr::EXACT) / 1000.0;
			auto vframe = provider->GetFrame(frame, time_s, true);
			wxImage img = GetImage(*vframe);
			int w = img.GetWidth(), h = img.GetHeight();
			int x0 = w * percent(region_[0]) / 100, x1 = w * percent(region_[2]) / 100;
			int y0 = h * percent(region_[1]) / 100, y1 = h * percent(region_[3]) / 100;
			int cw = x1 - x0, ch = y1 - y0;
			if (cw < 8 || ch < 8) { status("截取区域太小，请检查区域百分比（或在自动对帧工具中拖框）"); return; }
			wxImage cropped = img.GetSubImage(wxRect(x0, y0, cw, ch));
			if (cropped.GetWidth() > 1280) {
				int nh = std::max(1, static_cast<int>(1280.0 * ch / cw));
				cropped = cropped.Rescale(1280, nh);
			}
			wxMemoryOutputStream mos;
			if (!cropped.SaveFile(mos, wxBITMAP_TYPE_PNG)) { status("PNG 编码失败"); return; }
			size_t png_len = static_cast<size_t>(mos.GetLength());
			std::vector<char> png_buf(png_len);
			if (mos.CopyTo(png_buf.data(), png_len) != png_len) { status("PNG 缓冲读取失败"); return; }
			png_b64 = base64_encode(png_buf.data(), png_len);
		} catch (std::exception& e) {
			status(std::string("取帧失败：") + e.what());
			return;
		} catch (...) {
			status("取帧失败");
			return;
		}

		std::string prompt = std::string("这是动画/影视视频的画面截图。识别图中的字幕或对白文字（忽略水印和台标），并翻译成")
			+ lang + "，用词自然、符合字幕阅读习惯。严格按两行输出：\n原文=识别出的文字\n译文=翻译结果";
		std::string body = "{\"model\":\"" + jesc(model) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
			+ "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + png_b64 + "\"}},"
			+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}]}";

		begin_job("识别中…");
		status("翻译中，请稍候…");

		auto self = this;
		auto alive = alive_;
		auto cancel = cancel_;
		agi::dispatch::Background().Async([self, alive, cancel, key, body] {
			AIResult r = AIChatComplete(key, body, false, cancel);
			agi::dispatch::Main().Async([self, alive, r] {
				if (!*alive) return;
				self->frame_done(r);
			});
		});
	}

	void frame_done(const AIResult& r) {
		end_job();
		if (cancel_->load()) { status("已取消"); return; }
		if (!r.ok) {
			status(r.error);
			return;
		}
		set_field(orig_, extract_between(r.text, k_orig, k_yiwen));
		std::string trans = extract_after(r.text, k_yiwen, false);
		if (trans.empty()) trans = trim_ws(r.text);
		set_field(trans_, trans);
		status("完成，可编辑后填入");
		apply_->Enable();
	}

	void run_audio(const std::string& key, const std::string& model, const std::string& lang) {
		save_config();
		save_audio_pad();

		auto* provider = c->project->AudioProvider();
		if (!provider) { status("未打开音频（需要项目已加载音频）"); return; }
		audio_lines = c->selectionController->GetSortedSelection();
		if (audio_lines.empty()) { status("请先选中要翻译的行"); return; }

		struct Job { int64_t s0, s1; int idx; int start_ms; };
		int pad0 = std::clamp(wxAtoi(pad_[0]->GetValue()), 0, 5000);
		int pad1 = std::clamp(wxAtoi(pad_[1]->GetValue()), 0, 5000);
		int rate = provider->GetSampleRate();
		std::vector<Job> jobs;
		jobs.reserve(audio_lines.size());
		for (size_t i = 0; i < audio_lines.size(); ++i) {
			int start = audio_lines[i]->Start;
			int end = audio_lines[i]->End;
			if (end <= start) end = start + 1000;
			jobs.push_back({ static_cast<int64_t>(start - pad0) * rate / 1000,
				static_cast<int64_t>(end + pad1) * rate / 1000,
				static_cast<int>(i), start });
		}

		bool is_omni = model.find("omni") != std::string::npos;
		std::string prompt = is_omni
			? "听这段音频：先转写说话内容原文，再翻译成" + lang + "（字幕风格）。严格按两行输出：\n原文=...\n译文=..."
			: "转写这段音频中的语音内容。";

		combined_->ChangeValue("");
		begin_job("翻译中…");
		status(agi::format("共 %d 行，开始处理…", static_cast<int>(jobs.size())));

		auto self = this;
		auto alive = alive_;
		auto cancel = cancel_;
		int total = static_cast<int>(jobs.size());
		agi::dispatch::Background().Async([self, alive, cancel, provider, key, model,
		                                          prompt, is_omni, jobs, total] {
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
						+ "{\"type\":\"text\",\"text\":\"" + jesc(prompt) + "\"}]}]"
						+ (is_omni ? ",\"stream\":true" : "") + "}";
					r = AIChatComplete(key, body, is_omni, cancel);
				}
				block += r.ok ? r.text : std::string("错误: ") + r.error;
				block += "\n";
				int dn = ++done;
				agi::dispatch::Main().Async([self, alive, blk = std::move(block), dn, total] {
					if (!*alive) return;
					self->combined_->AppendText(to_wx(blk));
					self->status(agi::format("第 %d/%d 条完成", dn, total));
				});
			}
			agi::dispatch::Main().Async([self, alive] {
				if (!*alive) return;
				self->audio_job_finished();
			});
		});
	}

	void audio_job_finished() {
		end_job();
		if (cancel_->load()) {
			status("已取消");
			if (!combined_->IsEmpty()) apply_->Enable();
			return;
		}
		status("完成，可编辑后替换（换行自动转为 \\N）");
		apply_->Enable();
	}

	void OnCancelClick(wxCommandEvent&) {
		if (running_) {
			cancel_->store(true);
			status("正在取消…");
			cancel_btn_->Disable();
		}
	}

	void OnApply(wxCommandEvent&) {
		if (running_) return;
		if (mode == AIMode::Frame) {
			auto* line = c->selectionController->GetActiveLine();
			if (!line) { status("没有当前行"); return; }
			std::string v = field(trans_);
			if (v.empty()) { status("译文为空"); return; }
			line->Text = newlines_to_N(v);
			c->ass->Commit(to_wx("AI截图翻译"), AssFile::COMMIT_DIAG_TEXT);
			status("已填入当前行");
		} else {
			std::string combined = field(combined_);
			int applied = apply_combined(combined);
			if (applied > 0) {
				c->ass->Commit(to_wx("AI音频翻译"), AssFile::COMMIT_DIAG_TEXT);
				status(agi::format("已替换 %d 行", applied));
			} else
				status("没有可识别的【第n条】结果块");
		}
	}

	/// 与 Lua 版 apply_translations 一致：按【第n条…】块定位行并写回
	int apply_combined(const std::string& combined) {
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
				else if (any) break; // 数字读完即止
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
			if (idx < 0 || static_cast<size_t>(idx) >= audio_lines.size()) continue;
			std::string trans = extract_after(block, k_yiwen, false);
			if (trans.empty()) trans = trim_ws(block);
			if (trans.empty()) continue;
			audio_lines[idx]->Text = newlines_to_N(trans);
			++applied;
		}
		return applied;
	}

	void OnClose(wxCloseEvent& event) {
		cancel_->store(true);
		if (event.CanVeto() && running_) {
			event.Veto();
			status("正在取消后台任务，请稍候…");
			return;
		}
		alive_->store(false);
		EndModal(wxID_CANCEL);
	}
};

} // anonymous namespace

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
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 180L);
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
			r.error = "API 密钥无效，请核对上方密钥（" + detail + "）";
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
		// SSE：data: {...} 行，取 choices.0.delta.content 拼接
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
				auto it = pt.get_child_optional("choices.0.delta.content");
				if (it) content += ptree_content(*it);
			} catch (...) { }
		}
	} else {
		try {
			auto pt = parse_json(response);
			auto it = pt.get_child_optional("choices.0.message.content");
			if (!it) it = pt.get_child_optional("choices.0.delta.content");
			if (it) content = ptree_content(*it);
		} catch (...) {
			r.error = "响应格式异常：" + raw_snip();
			return r;
		}
	}

	if (content.empty()) {
		r.error = "接口未返回内容：" + raw_snip();
		return r;
	}
	r.ok = true;
	r.text = content;
	return r;
#endif
}

void ShowAITranslationDialog(agi::Context *c, AIMode mode) {
	auto* dlg = new AITranslationDialog(c, mode);
	dlg->ShowModal();
	dlg->Destroy();
}
