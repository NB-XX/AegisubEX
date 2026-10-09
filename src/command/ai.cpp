// AI 辅助翻译命令入口

#include "command.h"

#include "../ai_assistant.h"
#include "../include/aegisub/context.h"
#include "../project.h"
#include "../selection_controller.h"

namespace {
	using cmd::Command;

	struct ai_frame_translate final : public Command {
		CMD_NAME("ai/frame_translate")
		CMD_TYPE(COMMAND_VALIDATE)
		STR_MENU("区域截图翻译")
		STR_DISP("区域截图翻译")
		STR_HELP("对视频当前画面指定区域截图，调用百炼大模型识别并翻译，可一键填入当前行")

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		void operator()(agi::Context *c) override {
			ShowAITranslationDialog(c, AIMode::Frame);
		}
	};

	struct ai_audio_translate final : public Command {
		CMD_NAME("ai/audio_translate")
		CMD_TYPE(COMMAND_VALIDATE)
		STR_MENU("音频翻译选中行")
		STR_DISP("音频翻译选中行")
		STR_HELP("按选中行时间轴切取音频，调用百炼 omni 模型转写并翻译，可一键替换选中行")

		bool Validate(const agi::Context *c) override {
			return !!c->project->AudioProvider()
				&& !c->selectionController->GetSortedSelection().empty();
		}

		void operator()(agi::Context *c) override {
			ShowAITranslationDialog(c, AIMode::Audio);
		}
	};
}

namespace cmd {
void init_ai() {
	reg(std::make_unique<ai_frame_translate>());
	reg(std::make_unique<ai_audio_translate>());
}
}
