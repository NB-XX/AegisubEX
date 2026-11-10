// AI 辅助翻译命令入口

#include "command.h"

#include "../compat.h"
#include "../ai_assistant.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../video_display.h"
#include "../visual_tool_cross.h"

namespace {
	using cmd::Command;

	struct ai_frame_translate final : public Command {
		CMD_NAME("ai/frame_translate")
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)
		wxString StrMenu(const agi::Context *) const override { return to_wx("视频截图翻译"); }
		wxString StrDisplay(const agi::Context *) const override { return to_wx("截图翻译"); }
		wxString StrHelp() const override { return to_wx("视频截图翻译：在视频画面上框选字幕区域，直接截图并调用 AI 大模型识别与翻译"); }

		wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
			static const char svg[] =
				"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
				"<path d=\"M3 8 V5 A 2 2 0 0 1 5 3 H8\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<path d=\"M16 3 H19 A 2 2 0 0 1 21 5 V8\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<path d=\"M21 16 V19 A 2 2 0 0 1 19 21 H16\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<path d=\"M8 21 H5 A 2 2 0 0 1 3 19 V16\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<path d=\"M12 7 C12 9.8 10.2 12 7.5 12 C10.2 12 12 14.2 12 17 C12 14.2 13.8 12 16.5 12 C13.8 12 12 9.8 12 7 Z\" fill=\"#8E44AD\"/>"
				"<circle cx=\"16\" cy=\"8\" r=\"1.2\" fill=\"#E67E22\"/>"
				"</svg>";
			return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
		}

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
		CMD_TYPE(COMMAND_VALIDATE)
		wxString StrMenu(const agi::Context *) const override { return to_wx("音频翻译选中行"); }
		wxString StrDisplay(const agi::Context *) const override { return to_wx("音频翻译"); }
		wxString StrHelp() const override { return to_wx("音频翻译选中行：按选中行时间轴切取音频，调用百炼 omni 模型转写并翻译，可一键替换选中行"); }

		wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
			static const char svg[] =
				"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
				"<line x1=\"2\" y1=\"12\" x2=\"2\" y2=\"12\" stroke=\"#27AE60\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<line x1=\"5\" y1=\"8\" x2=\"5\" y2=\"16\" stroke=\"#27AE60\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<line x1=\"8\" y1=\"5\" x2=\"8\" y2=\"19\" stroke=\"#27AE60\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<line x1=\"11\" y1=\"9\" x2=\"11\" y2=\"15\" stroke=\"#27AE60\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
				"<path d=\"M13 6 C13 3.5 15.5 2 18.5 2 C21.5 2 23.5 3.5 23.5 6 C23.5 8 22 9.5 20 9.8 L20.5 12 L18 10 C15 10 13 8.5 13 6 Z\" fill=\"#8E44AD\"/>"
				"<text x=\"18.2\" y=\"7.5\" font-size=\"5\" font-family=\"sans-serif\" font-weight=\"bold\" fill=\"#FFFFFF\" text-anchor=\"middle\">AI</text>"
				"</svg>";
			return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
		}

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

	struct ai_style_quick final : public Command {
		CMD_NAME("ai/style_quick")
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
		wxString StrMenu(const agi::Context *) const override { return to_wx("样式快切面板"); }
		wxString StrDisplay(const agi::Context *) const override { return to_wx("样式快切"); }
		wxString StrHelp() const override { return to_wx("样式快切面板：显示或隐藏右侧样式快速切换面板及数字快捷键"); }

		wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
			static const char svg[] =
				"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
				"<path d=\"M12 3 C6.5 3 2 7 2 12 C2 16 5 19.5 9 20.5 C9.7 20.7 10.3 20.1 10.3 19.4 C10.3 18.8 10.1 18.2 10.1 17.6 C10.1 16.5 11 15.6 12.1 15.6 L14 15.6 C17.9 15.6 21 12.5 21 8.6 C21 5.5 17 3 12 3 Z\" fill=\"none\" stroke=\"#8E44AD\" stroke-width=\"2\"/>"
				"<circle cx=\"6.5\" cy=\"11.5\" r=\"1.5\" fill=\"#E74C3C\"/>"
				"<circle cx=\"9.5\" cy=\"7.5\" r=\"1.5\" fill=\"#F1C40F\"/>"
				"<circle cx=\"14.5\" cy=\"7.5\" r=\"1.5\" fill=\"#2ECC71\"/>"
				"<path d=\"M16 15 L20 19 M20 15 L16 19\" stroke=\"#8E44AD\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
				"</svg>";
			return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
		}

		bool IsActive(const agi::Context *) override {
			return OPT_GET("Tool/Playback/Review Quick Style")->GetBool();
		}

		void operator()(agi::Context *c) override {
			bool cur = OPT_GET("Tool/Playback/Review Quick Style")->GetBool();
			bool next = !cur;
			OPT_SET("Tool/Playback/Review Quick Style")->SetBool(next);
			if (c->aiPanel) {
				c->aiPanel->Show(next);
				if (next) {
					c->aiPanel->SelectTab(2);
					c->aiPanel->SyncStyles();
				}
				c->aiPanel->GetParent()->Layout();
			}
		}
	};

	struct ai_vad_diarize final : public Command {
		CMD_NAME("ai/vad_diarize")
		CMD_TYPE(COMMAND_VALIDATE)
		wxString StrMenu(const agi::Context *) const override { return to_wx("智能人声切分与说话人识别..."); }
		wxString StrDisplay(const agi::Context *) const override { return to_wx("语音切分"); }
		wxString StrHelp() const override { return to_wx("智能人声切分与说话人识别：精准检测说话发声瞬间、结束延长200ms，支持过滤背景乐与按音色匹配角色"); }

		wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
			static const char svg[] =
				"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
				"<path d=\"M12 2a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3z\" fill=\"#2980B9\"/>"
				"<path d=\"M19 10v1a7 7 0 0 1-14 0v-1M12 18v4M8 22h8\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
				"<path d=\"M2 8v4M22 8v4M5 5v10M19 5v10\" stroke=\"#27AE60\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>"
				"</svg>";
			return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
		}

		bool Validate(const agi::Context *c) override {
			return !!c->project->AudioProvider();
		}

		void operator()(agi::Context *c) override {
			ShowVADDiarizeDialog(c);
		}
	};
}

namespace cmd {
void init_ai() {
	reg(std::make_unique<ai_frame_translate>());
	reg(std::make_unique<ai_audio_translate>());
	reg(std::make_unique<ai_style_quick>());
	reg(std::make_unique<ai_vad_diarize>());
}
}
