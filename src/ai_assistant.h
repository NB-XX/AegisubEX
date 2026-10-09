// AI 辅助翻译（阿里百炼 OpenAI 兼容接口）
// 区域截图翻译 + 音频翻译选中行，内嵌版实现。

#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace agi { struct Context; }

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

/// 构造并弹出 AI 翻译对话框（模态）。命令入口调用。
void ShowAITranslationDialog(agi::Context *c, AIMode mode);
