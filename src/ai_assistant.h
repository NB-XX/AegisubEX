// AI 辅助翻译（阿里百炼 OpenAI 兼容接口）
// 区域截图翻译 + 音频翻译选中行，内嵌版实现。

#pragma once

#include "visual_tool.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <libaegisub/signal.h>
class wxListBox;
class wxCheckBox;
class wxListCtrl;

namespace agi { struct Context; }
class wxNotebook;
class wxTextCtrl;
class wxButton;
class wxStaticText;
class VideoDisplay;
class AssDialogue;

enum class AIMode { Frame, Audio };

struct AIResult {
	bool ok = false;
	std::string text;   ///< 成功时：模型回复全文（UTF-8）
	std::string error;  ///< 失败时：面向用户的中文错误说明
};

struct AIAudioItem {
	int idx = 0;              ///< 0-based index
	int start_ms = 0;
	std::string time_str;
	std::string orig;
	std::string trans;
	std::string error;
	bool success = false;
};

/// 方案配置结构体（端口URL、密钥、视觉/音频模型、代理设置等为一组）
struct AIProfile {
	std::string name;
	std::string endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
	std::string api_key;
	std::string vision_model = "qwen3.8-flash";
	std::string audio_model = "qwen3.8-omni-flash";
	std::string source_lang = "自动检测";
	std::string target_lang = "简体中文";
	std::string proxy_type = "None"; // "None", "HTTP", "SOCKS5"
	std::string proxy_host;
	int proxy_port = 0;
	std::string proxy_user;
	std::string proxy_pass;
	int timeout_sec = 60;
	int max_retries = 2;
};

/// 加载与保存方案列表
std::vector<AIProfile> AILoadProfiles(std::string* active_name = nullptr);
void AISaveProfiles(const std::vector<AIProfile>& profiles, const std::string& active_name);
AIProfile AIGetActiveProfile();

/// 调用 OpenAI 兼容 chat/completions 接口（内部支持重试与代理）
AIResult AIChatComplete(const AIProfile& profile, const std::string& body,
                        bool stream,
                        const std::shared_ptr<std::atomic<bool>>& cancel);

/// 主界面右下角常驻 AI 结果与控制面板
class AIResultPanel final : public wxPanel {
	agi::Context* c;
	std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>> alive_  = std::make_shared<std::atomic<bool>>(true);
	bool running_ = false;

	wxStaticText* status_ = nullptr;
	wxNotebook* notebook_ = nullptr;

	// 截图翻译页
	wxTextCtrl* frame_orig_ = nullptr;
	wxTextCtrl* frame_trans_ = nullptr;
	wxButton* frame_apply_ = nullptr;
	wxButton* frame_copy_ = nullptr;
	wxButton* frame_resnap_ = nullptr;
	wxButton* frame_retry_ = nullptr;

	// 音频翻译页
	wxListCtrl* audio_list_ = nullptr;
	wxButton* audio_apply_sel_ = nullptr;
	wxButton* audio_apply_all_ = nullptr;
	wxButton* audio_copy_btn_ = nullptr;
	wxButton* audio_vad_btn_ = nullptr;
	wxButton* audio_retry_ = nullptr;
	wxButton* audio_clear_ = nullptr;

	wxButton* cancel_btn_ = nullptr;
	wxButton* audio_cancel_btn_ = nullptr;
	wxButton* settings_btn_ = nullptr;
	wxButton* close_btn_ = nullptr;

	std::vector<AssDialogue*> audio_lines_;
	std::vector<AIAudioItem> audio_items_;

	// 样式快切页
	wxCheckBox* quick_style_chk_ = nullptr;
	wxListBox* style_list_ = nullptr;
	wxButton* btn_style_up_ = nullptr;
	wxButton* btn_style_down_ = nullptr;
	wxButton* btn_style_apply_ = nullptr;
	std::vector<std::string> ordered_styles_;
	std::vector<agi::signal::Connection> style_connections_;

	void OnMoveStyleUp(wxCommandEvent&);
	void OnMoveStyleDown(wxCommandEvent&);
	void OnStyleApply(wxCommandEvent&);
	void OnStyleListDClick(wxCommandEvent&);
	void OnQuickStyleToggle(wxCommandEvent&);
	void RefreshStyleList();
	void LoadStyleOrder();
	void SaveStyleOrder();

	void OnFrameApply(wxCommandEvent&);
	void OnFrameCopy(wxCommandEvent&);
	void OnFrameResnap(wxCommandEvent&);
	void OnFrameRetry(wxCommandEvent&);
	void OnAudioApplySelected(wxCommandEvent&);
	void OnAudioApplyAll(wxCommandEvent&);
	void OnAudioCopy(wxCommandEvent&);
	void OnAudioListKeyDown(wxKeyEvent&);
	void OnAudioListContextMenu(wxContextMenuEvent&);
	void OnAudioRetry(wxCommandEvent&);
	void OnAudioClear(wxCommandEvent&);
	void OnCancelClick(wxCommandEvent&);
	void OnSettingsClick(wxCommandEvent&);
	void OnCloseClick(wxCommandEvent&);

	enum CopyMode {
		COPY_BOTH_TAB = 0,
		COPY_TRANS_ONLY,
		COPY_ORIG_ONLY,
		COPY_DETAILED
	};
	void CopyItems(const std::vector<int>& indices, CopyMode mode);
	std::vector<int> GetSelectedOrCheckedAudioIndices() const;
	void ShowAudioContextMenu(const wxPoint& pos);
	int apply_items(const std::vector<int>& indices);

public:
	AIResultPanel(wxWindow* parent, agi::Context* ctx);
	~AIResultPanel() override;

	void SelectTab(int tab_idx);
	void SyncStyles();
	void ApplyStyle(int number_key_index);
	void ApplyStyleName(std::string const& style_name);
	void SetStatus(const std::string& s);
	void ShowSettings();

	void BeginJob(const std::string& doing);
	void EndJob();

	void SetFrameResult(const std::string& orig, const std::string& trans);
	void InitAudioItems(int total);
	void UpdateAudioItem(const AIAudioItem& item, int done, int total);
	void AudioJobFinished();

	std::shared_ptr<std::atomic<bool>> GetCancel() const { return cancel_; }
	std::shared_ptr<std::atomic<bool>> GetAlive() const { return alive_; }
	bool IsRunning() const { return running_; }
	void SetAudioLines(std::vector<AssDialogue*> lines) { audio_lines_ = std::move(lines); }
};

/// 视频画面框选截图视觉工具（类似 Snipaste / Frame Match 框选交互）
class VisualToolAIScreenshot final : public VisualToolBase {
	Vector2D start;
	Vector2D end;
	bool has_region = false;
	bool dragging = false;

	void ClearRegion();

public:
	VisualToolAIScreenshot(VideoDisplay *parent, agi::Context *context);
	~VisualToolAIScreenshot() override;
	void OnMouseEvent(wxMouseEvent &event) override;
	bool OnKeyDown(wxKeyEvent &event) override;
	void Draw() override;
};

/// 执行区域截图识别与翻译
void RunAIFrameTranslate(agi::Context* c, int x0 = -1, int y0 = -1, int x1 = -1, int y1 = -1);

/// 执行音频选中行识别与翻译
void RunAIAudioTranslate(agi::Context* c);

/// 智能人声切分与说话人识别对话框
void ShowVADDiarizeDialog(agi::Context* c);

/// 兼容接口：根据模式打开面板并执行对应翻译
void ShowAITranslationDialog(agi::Context *c, AIMode mode);
