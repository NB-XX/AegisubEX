// AI 辅助翻译命令入口

#include "command.h"

#include "../ai_assistant.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../video_display.h"
#include "../visual_tool_cross.h"

namespace {
	using cmd::Command;

	struct ai_frame_translate final : public Command {
		CMD_NAME("ai/frame_translate")
		CMD_ICON(visual_clip)
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)
		STR_MENU("视频截图翻译")
		STR_DISP("截图翻译")
		STR_HELP("在视频画面上框选字幕区域，直接截图并调用 AI 大模型识别与翻译")

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		bool IsActive(const agi::Context *c) override {
			return c->videoDisplay && c->videoDisplay->ToolIsType(typeid(VisualToolAIScreenshot));
		}

		void operator()(agi::Context *c) override {
			if (c->videoDisplay->ToolIsType(typeid(VisualToolAIScreenshot))) {
				c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
			} else {
				c->videoDisplay->SetTool(std::make_unique<VisualToolAIScreenshot>(c->videoDisplay, c));
				if (c->aiPanel) {
					c->aiPanel->Show(true);
					c->aiPanel->GetParent()->Layout();
					c->aiPanel->SelectTab(0);
					c->aiPanel->SetStatus("请在画面上按住左键拖拽框选字幕区域（或点击右侧【重试】）");
				}
			}
		}
	};

	struct ai_audio_translate final : public Command {
		CMD_NAME("ai/audio_translate")
		CMD_ICON(toggle_audio_spectrum)
		CMD_TYPE(COMMAND_VALIDATE)
		STR_MENU("音频翻译选中行")
		STR_DISP("音频翻译")
		STR_HELP("按选中行时间轴切取音频，调用百炼 omni 模型转写并翻译，可一键替换选中行")

		bool Validate(const agi::Context *c) override {
			return !!c->project->AudioProvider()
				&& !c->selectionController->GetSortedSelection().empty();
		}

		void operator()(agi::Context *c) override {
			if (c->aiPanel) {
				c->aiPanel->Show(true);
				c->aiPanel->GetParent()->Layout();
				c->aiPanel->SelectTab(1);
			}
			RunAIAudioTranslate(c);
		}
	};
}

namespace cmd {
void init_ai() {
	reg(std::make_unique<ai_frame_translate>());
	reg(std::make_unique<ai_audio_translate>());
}
}
