// AI 辅助翻译（阿里百炼 OpenAI 兼容接口）
// 区域截图翻译 + 音频翻译选中行，内嵌版实现。

#pragma once

#include "visual_tool.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>

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

/// 调用百炼 chat/completions 接口。
/// 必须在非 UI 线程调用（内部使用 libcurl 阻塞请求）。
/// stream 为 true 时按 SSE 流式格式解析响应。
/// cancel 置位后请求会被中断（通过进度回调检查）。
AIResult AIChatComplete(const std::string& api_key, const std::string& body,
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
	wxButton* frame_resnap_ = nullptr;
	wxButton* frame_retry_ = nullptr;

	// 音频翻译页
	wxTextCtrl* audio_combined_ = nullptr;
	wxButton* audio_apply_ = nullptr;
	wxButton* audio_retry_ = nullptr;
	wxButton* audio_clear_ = nullptr;

	wxButton* cancel_btn_ = nullptr;
	wxButton* audio_cancel_btn_ = nullptr;
	wxButton* settings_btn_ = nullptr;
	wxButton* close_btn_ = nullptr;

	std::vector<AssDialogue*> audio_lines_;

	void OnFrameApply(wxCommandEvent&);
	void OnFrameResnap(wxCommandEvent&);
	void OnFrameRetry(wxCommandEvent&);
	void OnAudioApply(wxCommandEvent&);
	void OnAudioRetry(wxCommandEvent&);
	void OnAudioClear(wxCommandEvent&);
	void OnCancelClick(wxCommandEvent&);
	void OnSettingsClick(wxCommandEvent&);
	void OnCloseClick(wxCommandEvent&);

	int apply_combined(const std::string& combined);

public:
	AIResultPanel(wxWindow* parent, agi::Context* ctx);
	~AIResultPanel() override;

	void SelectTab(int tab_idx);
	void SetStatus(const std::string& s);
	void ShowSettings();

	void BeginJob(const std::string& doing);
	void EndJob();

	void SetFrameResult(const std::string& orig, const std::string& trans);
	void AppendAudioResult(const std::string& block, int done, int total);
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
	void OnMouseEvent(wxMouseEvent &event) override;
	bool OnKeyDown(wxKeyEvent &event) override;
	void Draw() override;
};

/// 执行区域截图识别与翻译
void RunAIFrameTranslate(agi::Context* c, int x0 = -1, int y0 = -1, int x1 = -1, int y1 = -1);

/// 执行音频选中行识别与翻译
void RunAIAudioTranslate(agi::Context* c);

/// 兼容接口：根据模式打开面板并执行对应翻译
void ShowAITranslationDialog(agi::Context *c, AIMode mode);
