// Copyright (c) 2026, Aegisub Project
// All rights reserved.
//
// Visual on-canvas interactive bounding box selection tool for Motion Tracking.

#pragma once

#include "visual_tool.h"
#include "visual_tool_cross.h"
#include "video_display.h"
#include "project.h"
#include "async_video_provider.h"
#include "include/aegisub/context.h"
#include <algorithm>
#include <cmath>
#include <wx/event.h>

class VisualToolMotionTrack final : public VisualToolBase {
private:
	Vector2D start;
	Vector2D end;
	bool has_box = false;
	bool dragging = false;

	static inline int s_box_x0 = -1;
	static inline int s_box_y0 = -1;
	static inline int s_box_x1 = -1;
	static inline int s_box_y1 = -1;
	static inline bool s_has_box = false;
	static inline std::function<void(int, int, int, int)> s_on_box_changed = nullptr;
	static inline std::function<void(agi::Context*)> s_on_box_finished = nullptr;

	void ClearBox() {
		has_box = false;
		dragging = false;
		if (parent->HasCapture()) parent->ReleaseMouse();
		parent->Render();
	}

public:
	VisualToolMotionTrack(VideoDisplay *parent, agi::Context *context)
	: VisualToolBase(parent, context)
	{
		if (s_has_box && context && context->project->VideoProvider() && script_res.X() > 0 && script_res.Y() > 0) {
			auto p = context->project->VideoProvider();
			double sx = static_cast<double>(script_res.X()) / p->GetWidth();
			double sy = static_cast<double>(script_res.Y()) / p->GetHeight();
			start = Vector2D(s_box_x0 * sx, s_box_y0 * sy);
			end = Vector2D(s_box_x1 * sx, s_box_y1 * sy);
			has_box = true;
		}
	}

	static bool HasBox() { return s_has_box; }

	static bool GetCurrentBox(int& out_x0, int& out_y0, int& out_x1, int& out_y1) {
		if (!s_has_box) return false;
		out_x0 = s_box_x0;
		out_y0 = s_box_y0;
		out_x1 = s_box_x1;
		out_y1 = s_box_y1;
		return true;
	}

	static void SetCurrentBox(int x0, int y0, int x1, int y1) {
		s_box_x0 = x0;
		s_box_y0 = y0;
		s_box_x1 = x1;
		s_box_y1 = y1;
		s_has_box = (x1 > x0 && y1 > y0);
		if (s_on_box_changed && s_has_box) {
			s_on_box_changed(s_box_x0, s_box_y0, s_box_x1, s_box_y1);
		}
	}

	static void SetBoxCallback(std::function<void(int, int, int, int)> cb) {
		s_on_box_changed = cb;
	}

	static void SetBoxFinishedCallback(std::function<void(agi::Context*)> cb) {
		s_on_box_finished = cb;
	}

	bool OnKeyDown(wxKeyEvent &event) override {
		if (event.GetKeyCode() == WXK_ESCAPE) {
			ClearBox();
			if (c && c->videoDisplay)
				c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
			return true;
		}
		return false;
	}

	void OnMouseEvent(wxMouseEvent &event) override {
		mouse_pos = Vector2D(event.GetX(), event.GetY());

		if (event.RightDown()) {
			ClearBox();
			if (c && c->videoDisplay)
				c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
		}
		else if (event.LeftDown()) {
			start = end = ToScriptCoords(mouse_pos);
			has_box = true;
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
				double vw = provider->GetWidth();
				double vh = provider->GetHeight();
				int x0 = std::clamp(static_cast<int>(std::floor(std::min(start.X(), end.X()) * vw / script_res.X())), 0, static_cast<int>(vw) - 1);
				int x1 = std::clamp(static_cast<int>(std::ceil(std::max(start.X(), end.X()) * vw / script_res.X())), 0, static_cast<int>(vw));
				int y0 = std::clamp(static_cast<int>(std::floor(std::min(start.Y(), end.Y()) * vh / script_res.Y())), 0, static_cast<int>(vh) - 1);
				int y1 = std::clamp(static_cast<int>(std::ceil(std::max(start.Y(), end.Y()) * vh / script_res.Y())), 0, static_cast<int>(vh));

				if (x1 - x0 >= 8 && y1 - y0 >= 8) {
					SetCurrentBox(x0, y0, x1, y1);
					c->videoDisplay->SetTool(std::make_unique<VisualToolCross>(c->videoDisplay, c));
					if (s_on_box_finished) {
						wxTheApp->CallAfter([context = c]() {
							if (s_on_box_finished) s_on_box_finished(context);
						});
					}
				} else {
					ClearBox();
				}
			}
			parent->Render();
		}
		else if (event.Leaving()) {
			mouse_pos = Vector2D(-1, -1);
		}
		event.Skip(false);
	}

	void Draw() override {
		if (!has_box && !dragging) return;

		Vector2D p1 = FromScriptCoords(start);
		Vector2D p2 = FromScriptCoords(end);
		double left = std::min(p1.X(), p2.X());
		double right = std::max(p1.X(), p2.X());
		double top = std::min(p1.Y(), p2.Y());
		double bottom = std::max(p1.Y(), p2.Y());

		// 经典 Mocha 风格：高亮青色框 + 半透明遮罩
		gl.SetLineColour(wxColour(0, 230, 255), 1.0f, 2);
		gl.SetFillColour(wxColour(0, 230, 255), 0.12f);
		gl.DrawRectangle(Vector2D(left, top), Vector2D(right, bottom));

		// 中心瞄准准星
		Vector2D mid((left + right) / 2.0, (top + bottom) / 2.0);
		gl.SetLineColour(wxColour(255, 230, 0), 1.0f, 1);
		gl.DrawLine(mid - Vector2D(10, 0), mid + Vector2D(10, 0));
		gl.DrawLine(mid - Vector2D(0, 10), mid + Vector2D(0, 10));

		// 四角强化标识
		double corner_len = std::min(12.0, std::min(right - left, bottom - top) / 3.0);
		gl.SetLineColour(wxColour(255, 255, 255), 1.0f, 3);
		// 左上
		gl.DrawLine(Vector2D(left, top), Vector2D(left + corner_len, top));
		gl.DrawLine(Vector2D(left, top), Vector2D(left, top + corner_len));
		// 右上
		gl.DrawLine(Vector2D(right, top), Vector2D(right - corner_len, top));
		gl.DrawLine(Vector2D(right, top), Vector2D(right, top + corner_len));
		// 左下
		gl.DrawLine(Vector2D(left, bottom), Vector2D(left + corner_len, bottom));
		gl.DrawLine(Vector2D(left, bottom), Vector2D(left, bottom - corner_len));
		// 右下
		gl.DrawLine(Vector2D(right, bottom), Vector2D(right - corner_len, bottom));
		gl.DrawLine(Vector2D(right, bottom), Vector2D(right, bottom - corner_len));
	}
};
