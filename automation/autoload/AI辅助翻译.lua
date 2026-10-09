-- AI辅助翻译：调用阿里云百炼多模态模型，区域截图翻译 + 时间轴音频分析 + 文本直译
-- 依赖：系统 PATH 中的 ffmpeg（截帧/切音频）与 python（HTTPS 中转，仅用标准库）
-- 配置与 API 密钥持久化在 %APPDATA%\Aegisub\ai_translate.conf（不进代码仓库）

local tr = aegisub.gettext
script_name = tr("AI辅助翻译")
script_description = tr("百炼 qwen 多模态辅助：区域截图翻译 / 选中行音频翻译 / 文本翻译")
script_author = "Aegisub审校版"
script_version = "1.0"

local APPDATA = os.getenv("APPDATA") or "."
local TEMPDIR = os.getenv("TEMP") or APPDATA
CONF_PATH = APPDATA .. "\\ai_translate.conf"
RELAY_PATH = APPDATA .. "\\bailian_relay.py"
WORK_DIR = TEMPDIR .. "\\aegisub_ai\\"
ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"

-- 默认配置（对话框里可改，执行后写回配置文件）
DEFAULTS = {
	api_key = "",
	vision_model = "qwen3.8-flash",
	audio_model = "qwen3.8-omni-flash",
	text_model = "qwen3.8-flash",
	target_lang = "简体中文",
	x0 = 0, y0 = 70, x1 = 100, y1 = 100,   -- 截图区域百分比（默认画面底部条带）
	frame_offset = 1000,                   -- 取帧时刻 = 行开始 + 该毫秒数
	pad_start = 300, pad_end = 300,        -- 音频前后延长
}

------------------------------------------------------------------ 配置文件

function load_conf()
	local conf = {}
	for k, v in pairs(DEFAULTS) do conf[k] = v end
	local f = io.open(CONF_PATH, "rb")
	if f then
		for line in f:lines() do
			local k, v = line:match("^([%w_]+)=(.*)$")
			if k and conf[k] ~= nil then
				if type(conf[k]) == "number" then conf[k] = tonumber(v) or conf[k]
				else conf[k] = v end
			end
		end
		f:close()
	end
	return conf
end

function save_conf(conf)
	local f = io.open(CONF_PATH, "wb")
	if not f then return end
	for k, v in pairs(conf) do
		if type(v) == "number" then v = string.format("%g", v) end
		f:write(k .. "=" .. tostring(v) .. "\n")
	end
	f:close()
end

------------------------------------------------------------------ 工具函数

-- base64（LuaJIT 无现成库，逐字节实现）
local B64CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
function base64_lua(data)
	local out = {}
	local len = #data
	for i = 1, len, 3 do
		local a = data:byte(i)
		local b = data:byte(i + 1)
		local c = data:byte(i + 2)
		local n = a * 65536 + (b or 0) * 256 + (c or 0)
		local s = B64CH:sub(math.floor(n / 262144) + 1, math.floor(n / 262144) + 1)
			.. B64CH:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
			.. (b and B64CH:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1) or "=")
			.. (c and B64CH:sub(n % 64 + 1, n % 64 + 1) or "=")
		out[#out + 1] = s
	end
	return table.concat(out)
end

-- JSON 组装（对象手工拼、数组用 jarr；字符串转义只处理 ASCII 控制字符，UTF-8 汉字原样传输）
function jstr(s)
	s = tostring(s):gsub('[%c"\\]', function(c)
		if c == '"' then return '\\"' end
		if c == "\\" then return "\\\\" end
		if c == "\n" then return "\\n" end
		if c == "\r" then return "\\r" end
		if c == "\t" then return "\\t" end
		return string.format("\\u%04x", c:byte())
	end)
	return '"' .. s .. '"'
end
function jarr(list)
	local parts = {}
	for i, v in ipairs(list) do parts[i] = json_value(v) end
	return "[" .. table.concat(parts, ",") .. "]"
end
function jobj(map)
	local parts = {}
	for k, v in pairs(map) do parts[#parts + 1] = jstr(k) .. ":" .. json_value(v) end
	return "{" .. table.concat(parts, ",") .. "}"
end
function json_value(v)
	local t = type(v)
	if t == "table" then
		if #v > 0 then return jarr(v) end
		return jobj(v)
	elseif t == "number" then
		if v == math.floor(v) then return string.format("%d", v) end
		return string.format("%.6g", v)
	elseif t == "boolean" then
		return v and "true" or "false"
	else
		return jstr(v)
	end
end

function ensure_dirs()
	os.execute('if not exist "' .. WORK_DIR .. '" mkdir "' .. WORK_DIR .. '" >nul 2>&1')
end

-- 首次运行把 Python 中转脚本写入 APPDATA（只依赖标准库）
local RELAY_SRC = [==[
# Aegisub AI 翻译 HTTP 中转（由 AI辅助翻译.lua 自动生成，勿手工修改）
# 用法: python bailian_relay.py req.json resp.txt
# req.json: {"endpoint":..., "api_key":..., "body":{...openai格式...}, "timeout":180}
import sys, json, urllib.request
from urllib.error import HTTPError

def flatten(c):
    if isinstance(c, list):
        return "".join(x.get("text", "") if isinstance(x, dict) else str(x) for x in c)
    return "" if c is None else str(c)

def stream_parse(fp):
    parts = []
    for raw in fp:
        line = raw.decode("utf-8", "ignore").strip()
        if not line.startswith("data:"):
            continue
        p = line[5:].strip()
        if p == "[DONE]":
            break
        try:
            j = json.loads(p)
            ch = j.get("choices") or []
            if ch:
                parts.append(flatten((ch[0].get("delta") or {}).get("content")))
        except ValueError:
            pass
    return "".join(parts)

try:
    conf = json.load(open(sys.argv[1], "r", encoding="utf-8"))
    body = conf["body"]
    req = urllib.request.Request(
        conf["endpoint"], data=json.dumps(body).encode("utf-8"),
        headers={"Authorization": "Bearer " + conf["api_key"],
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=conf.get("timeout", 180)) as r:
        if body.get("stream"):
            text = stream_parse(r)
        else:
            resp = json.load(r)
            ch = resp.get("choices") or []
            text = flatten((ch[0].get("message") or {}).get("content")) if ch \
                else "ERROR: bad response " + json.dumps(resp)[:400]
    open(sys.argv[2], "w", encoding="utf-8").write(text)
except HTTPError as e:
    try:
        m = e.read().decode("utf-8", "ignore")[:600]
    except Exception:
        m = str(e)
    open(sys.argv[2], "w", encoding="utf-8").write("ERROR: HTTP %d %s" % (e.code, m))
except Exception as e:
    open(sys.argv[2], "w", encoding="utf-8").write("ERROR: " + repr(e))
]==]

function ensure_relay()
	local f = io.open(RELAY_PATH, "rb")
	if f then f:close() return true end
	f = io.open(RELAY_PATH, "wb")
	if not f then return false end
	f:write(RELAY_SRC)
	f:close()
	return true
end

-- 调 API：写请求 -> python 中转 -> 读结果。返回 text 或 (nil, 错误信息)
function api_call(conf, body)
	ensure_dirs()
	if not ensure_relay() then return nil, "无法写入中转脚本 " .. RELAY_PATH end
	local req = { endpoint = ENDPOINT, api_key = conf.api_key, body = body, timeout = 180 }
	local f = io.open(WORK_DIR .. "req.json", "wb")
	f:write(json_value(req))
	f:close()
	os.remove(WORK_DIR .. "resp.txt")
	os.execute('python "' .. RELAY_PATH .. '" "' .. WORK_DIR .. 'req.json" "' .. WORK_DIR .. 'resp.txt" >nul 2>&1')
	local rf = io.open(WORK_DIR .. "resp.txt", "rb")
	if not rf then
		return nil, "Python 中转未能执行（请确认系统 PATH 里有 python）"
	end
	local txt = rf:read("*a")
	rf:close()
	if txt:sub(1, 6) == "ERROR:" then
		local hint = ""
		if txt:find("AccessDenied") then
			hint = "\n该模型未在当前账号开通：请前往百炼控制台「模型广场」开通对应模型后重试。"
		elseif txt:find("api_key") or txt:find("API key") then
			hint = "\nAPI 密钥无效，请在对话框重新填写。"
		end
		return nil, txt .. hint
	end
	return txt
end

function read_file_b64(path)
	local f = io.open(path, "rb")
	if not f then return nil end
	local b64 = base64_lua(f:read("*a"))
	f:close()
	return b64
end

-- 从"原文=.../译文=..."风格回答里提取译文（找不到就原样返回）
function extract_translation(text)
	local i = text:find("译文")
	if i then
		local rest = text:sub(i + 6)
		rest = rest:gsub("^[%s=:：]*", "")
		rest = rest:gsub("%s+$", "")
		if rest ~= "" then return rest end
	end
	i = text:find("[Tt]ranslation")
	if i then
		local rest = text:sub(i):gsub("^[Tt]ranslation%s*[:=]?%s*", ""):gsub("%s+$", "")
		if rest ~= "" then return rest end
	end
	return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function ms_to_hms(ms)
	local s = math.floor(ms / 1000)
	return string.format("%d:%02d:%02d", math.floor(s / 3600), math.floor(s / 60) % 60, s % 60)
end

local function notice(msg)
	aegisub.dialog.display({ { class = "label", x = 0, y = 0, label = msg } }, { "知道了" })
end

------------------------------------------------------------------ ffmpeg 取媒体

-- 从视频按时间取一帧并裁剪区域；成功返回 base64，失败返回 nil
function grab_frame(video_file, time_ms, x0, y0, x1, y1)
	local out = WORK_DIR .. "frame.png"
	os.remove(out)
	local w = math.max(1, x1 - x0)
	local h = math.max(1, y1 - y0)
	local filter = string.format(
		"crop=iw*%d/100:ih*%d/100:iw*%d/100:ih*%d/100,scale='if(gt(iw,1280),1280,-2)':-2",
		w, h, x0, y0)
	local cmd = string.format('ffmpeg -y -ss %.3f -i "%s" -frames:v 1 -vf "%s" "%s"',
		math.max(0, time_ms) / 1000, video_file, filter, out)
	os.execute(cmd .. " >nul 2>&1")
	local f = io.open(out, "rb")
	if not f then return nil end
	local b64 = base64_lua(f:read("*a"))
	f:close()
	return b64
end

-- 切取一段音频转 16k 单声道 wav；成功返回 base64
function slice_audio(audio_file, start_ms, end_ms, pad_start, pad_end)
	local out = WORK_DIR .. "slice.wav"
	os.remove(out)
	local st = math.max(0, start_ms - pad_start)
	local dur = math.max(200, end_ms + pad_end - start_ms + pad_start)
	local cmd = string.format('ffmpeg -y -ss %.3f -t %.3f -i "%s" -ac 1 -ar 16000 -c:a pcm_s16le "%s"',
		st / 1000, dur / 1000, audio_file, out)
	os.execute(cmd .. " >nul 2>&1")
	local b64 = read_file_b64(out)
	if not b64 then return nil end
	return b64
end

------------------------------------------------------------------ 公共对话框辅助

local function build_result_dialog(parts)
	return table.concat(parts, "\n\n")
end

-- 按"【第n条】"分块，把每块译文写回对应选中行
function apply_translations(subs, sel, combined)
	local heads = {}
	local init = 1
	while true do
		local s, e, n = combined:find("【第(%d+)条[^】]-】", init)
		if not s then break end
		heads[#heads + 1] = { hstart = s, start = e + 1, num = tonumber(n) }
		init = e + 1
	end
	local applied = 0
	for i = 1, #heads do
		local stopx = (heads[i + 1] and heads[i + 1].hstart - 1) or #combined
		local block = combined:sub(heads[i].start, stopx)
		local li = sel[heads[i].num]
		if li and subs[li] then
			local line = subs[li]
			line.text = extract_translation(block)
			subs[li] = line
			applied = applied + 1
		end
	end
	return applied
end

------------------------------------------------------------------ 功能1：区域截图翻译

function macro_screenshot(subs, sel)
	if #sel == 0 then notice("请先在字幕网格中选中至少一行") return end
	local props = aegisub.project_properties()
	if not props or props.video_file == "" then
		notice("请先通过 视频→打开视频 打开视频文件") return
	end
	local conf = load_conf()
	ensure_dirs()
	local config = {
		{ class = "label", x = 0, y = 0, label = "API密钥:" },
		{ class = "lineedit", x = 1, y = 0, name = "api_key", width = 5, value = conf.api_key },

		{ class = "label", x = 0, y = 1, label = "截图区域 %(左,上,右,下):" },
		{ class = "intedit", x = 1, y = 1, name = "x0", min = 0, max = 100, value = conf.x0 },
		{ class = "intedit", x = 2, y = 1, name = "y0", min = 0, max = 100, value = conf.y0 },
		{ class = "intedit", x = 3, y = 1, name = "x1", min = 1, max = 100, value = conf.x1 },
		{ class = "intedit", x = 4, y = 1, name = "y1", min = 1, max = 100, value = conf.y1 },

		{ class = "label", x = 0, y = 2, label = "取帧偏移 ms(相对行首):" },
		{ class = "intedit", x = 1, y = 2, name = "frame_offset", min = 0, max = 60000, value = conf.frame_offset },

		{ class = "label", x = 0, y = 3, label = "视觉模型:" },
		{ class = "dropdown", x = 1, y = 3, name = "vision_model",
		  items = { "qwen3.8-flash", "qwen3.7-flash", "qwen3.6-flash", "qwen3.5-flash", "qwen-vl-max", "qwen-vl-ocr-latest" },
		  value = conf.vision_model, width = 2 },

		{ class = "label", x = 0, y = 4, label = "目标语言:" },
		{ class = "lineedit", x = 1, y = 4, name = "target_lang", width = 3, value = conf.target_lang },

		{ class = "checkbox", x = 0, y = 5, name = "replace", label = "完成时把各块译文写回对应选中行", value = false, span = 5 },
		{ class = "label", x = 0, y = 6, label = "共 " .. #sel .. " 行，每行约等待 5~15 秒（期间界面会短暂无响应）" },
	}
	local btn, res = aegisub.dialog.display(config, { "执行", "取消" })
	if btn ~= "执行" then return end
	if not res.api_key or res.api_key == "" then notice("API密钥不能为空") return end
	conf.api_key = res.api_key
	conf.vision_model = res.vision_model
	conf.target_lang = res.target_lang
	conf.x0, conf.y0, conf.x1, conf.y1 = res.x0, res.y0, res.x1, res.y1
	conf.frame_offset = res.frame_offset
	save_conf(conf)

	local prompt = "这是动画/影视视频的画面截图。识别图中的字幕或对白文字（忽略水印和台标），"
		.. "并翻译成" .. res.target_lang .. "，用词自然、符合字幕阅读习惯。"
		.. "严格按两行输出：\n原文=识别出的文字\n译文=翻译结果"

	local parts = {}
	for n = 1, #sel do
		local line = subs[sel[n]]
		local t = line.start_time + res.frame_offset
		aegisub.debug.out(string.format("截图翻译第 %d/%d 条（%s）...\n", n, #sel, ms_to_hms(t)))
		local b64 = grab_frame(props.video_file, t, res.x0, res.y0, res.x1, res.y1)
		if not b64 then
			parts[#parts + 1] = "【第" .. n .. "条】 取帧失败（时间点 " .. ms_to_hms(t) .. "）"
		else
			local body = {
				model = res.vision_model,
				messages = { { role = "user", content = {
					{ type = "image_url", image_url = { url = "data:image/png;base64," .. b64 } },
					{ type = "text", text = prompt },
				} } },
			}
			local text, err = api_call(conf, body)
			parts[#parts + 1] = "【第" .. n .. "条 " .. ms_to_hms(line.start_time) .. "】\n"
				.. (text or ("错误: " .. err))
		end
	end

	local combined = build_result_dialog(parts)
	local show = {
		{ class = "label", x = 0, y = 0, label = "识别与翻译结果（可修改后再写回）:" },
		{ class = "textedit", x = 0, y = 1, name = "result", width = 6, height = 10, value = combined },
	}
	local btn2, res2 = aegisub.dialog.display(show, { "完成", "丢弃" })
	if btn2 == "完成" and res.replace then
		local applied = apply_translations(subs, sel, res2.result)
		if applied > 0 then aegisub.set_undo_point(script_name .. "/截图翻译写回") end
	end
end

------------------------------------------------------------------ 功能2：选中行音频翻译

function macro_audio(subs, sel)
	if #sel == 0 then notice("请先在字幕网格中选中至少一行") return end
	local props = aegisub.project_properties()
	if not props or props.audio_file == "" then
		notice("请先打开音频文件（视频会自动提取，若未提取请先用 视频→打开视频）") return
	end
	local conf = load_conf()
	ensure_dirs()
	local config = {
		{ class = "label", x = 0, y = 0, label = "API密钥:" },
		{ class = "lineedit", x = 1, y = 0, name = "api_key", width = 5, value = conf.api_key },

		{ class = "label", x = 0, y = 1, label = "音频模型:" },
		{ class = "dropdown", x = 1, y = 1, name = "audio_model",
		  items = { "qwen3.8-omni-flash", "qwen3-livetranslate-flash", "qwen3-asr-flash", "qwen-audio-3.0-asr-flash", "qwen-omni-turbo" },
		  value = conf.audio_model, width = 3 },

		{ class = "label", x = 0, y = 2, label = "前面延长 ms:" },
		{ class = "intedit", x = 1, y = 2, name = "pad_start", min = 0, max = 5000, value = conf.pad_start },
		{ class = "label", x = 2, y = 2, label = "后面延长 ms:" },
		{ class = "intedit", x = 3, y = 2, name = "pad_end", min = 0, max = 5000, value = conf.pad_end },

		{ class = "label", x = 0, y = 3, label = "目标语言:" },
		{ class = "lineedit", x = 1, y = 3, name = "target_lang", width = 3, value = conf.target_lang },

		{ class = "checkbox", x = 0, y = 4, name = "replace", label = "完成时把各块译文写回对应选中行", value = false, span = 5 },
		{ class = "label", x = 0, y = 5, label = "共 " .. #sel .. " 行，每行约等待 8~20 秒" },
	}
	local btn, res = aegisub.dialog.display(config, { "执行", "取消" })
	if btn ~= "执行" then return end
	if not res.api_key or res.api_key == "" then notice("API密钥不能为空") return end

	conf.api_key = res.api_key
	conf.audio_model = res.audio_model
	conf.target_lang = res.target_lang
	conf.pad_start = res.pad_start
	conf.pad_end = res.pad_end
	save_conf(conf)

	local is_omni = res.audio_model:find("omni") ~= nil
	local prompt
	if is_omni then
		prompt = "听这段音频：先转写说话内容原文，再翻译成" .. res.target_lang
			.. "（字幕风格）。严格按两行输出：\n原文=...\n译文=..."
	else
		prompt = "转写这段音频中的语音内容。"
	end

	local parts = {}
	for n = 1, #sel do
		local line = subs[sel[n]]
		aegisub.debug.out(string.format("音频翻译第 %d/%d 条（%s - %s）...\n",
			n, #sel, ms_to_hms(line.start_time), ms_to_hms(line.end_time)))
		local b64 = slice_audio(props.audio_file, line.start_time, line.end_time, res.pad_start, res.pad_end)
		if not b64 then
			parts[#parts + 1] = "【第" .. n .. "条】 切分音频失败（检查音频文件是否存在）"
		else
			local body = {
				model = res.audio_model,
				messages = { { role = "user", content = {
					{ type = "input_audio", input_audio = { data = "data:;base64," .. b64, format = "wav" } },
					{ type = "text", text = prompt },
				} } },
				stream = is_omni,   -- omni 系列要求流式输出，中转脚本会自动拼接
			}
			local text, err = api_call(conf, body)
			parts[#parts + 1] = "【第" .. n .. "条 " .. ms_to_hms(line.start_time) .. "】\n"
				.. (text or ("错误: " .. err))
		end
	end

	local combined = build_result_dialog(parts)
	local btn2, res2 = aegisub.dialog.display({
		{ class = "label", x = 0, y = 0, label = "转写与翻译结果（可修改后再写回）:" },
		{ class = "textedit", x = 0, y = 1, name = "result", width = 6, height = 10, value = combined },
	}, { "完成", "丢弃" })
	if btn2 == "完成" and res.replace then
		local applied = apply_translations(subs, sel, res2.result)
		if applied > 0 then aegisub.set_undo_point(script_name .. "/音频翻译写回") end
	end
end

------------------------------------------------------------------ 功能3：选中行文本直译

function macro_text(subs, sel)
	if #sel == 0 then notice("请先在字幕网格中选中至少一行") return end
	local conf = load_conf()
	ensure_dirs()
	local lines = {}
	for n = 1, #sel do
		local text = subs[sel[n]].text
		text = text:gsub("{[^}]*}", "")
		text = text:gsub("\\N", " / ")
		text = text:gsub("^%s+", ""):gsub("%s+$", "")
		if text ~= "" then lines[#lines + 1] = "【第" .. n .. "条】" .. text end
	end
	if #lines == 0 then notice("选中行没有可翻译的文本") return end

	local btn, res = aegisub.dialog.display({
		{ class = "label", x = 0, y = 0, label = "API密钥:" },
		{ class = "lineedit", x = 1, y = 0, name = "api_key", width = 5, value = conf.api_key },
		{ class = "label", x = 0, y = 1, label = "模型:" },
		{ class = "dropdown", x = 1, y = 1, name = "text_model",
		  items = { "qwen3.8-flash", "qwen3.7-flash", "qwen-mt-flash" },
		  value = conf.text_model, width = 2 },
		{ class = "label", x = 0, y = 2, label = "目标语言:" },
		{ class = "lineedit", x = 1, y = 2, name = "target_lang", width = 3, value = conf.target_lang },
		{ class = "checkbox", x = 0, y = 3, name = "replace", label = "完成时把各块译文写回对应选中行", value = false, span = 5 },
		{ class = "label", x = 0, y = 4, label = "将一次请求翻译 " .. #lines .. " 行" },
	}, { "执行", "取消" })
	if btn ~= "执行" then return end
	if not res.api_key or res.api_key == "" then notice("API密钥不能为空") return end

	conf.api_key = res.api_key
	conf.text_model = res.text_model
	conf.target_lang = res.target_lang
	save_conf(conf)

	local prompt = "你是字幕译者。把下面每一行【第n条】后的日文/外文字幕翻译成" .. res.target_lang
		.. "，保持行编号，每行格式为：【第n条】译文=...。不要输出其他内容。\n\n"
		.. table.concat(lines, "\n")

	local text, err = api_call(conf, {
		model = res.text_model,
		messages = { { role = "user", content = prompt } },
	})
	local combined = text or ("错误: " .. err)

	local btn2, res2 = aegisub.dialog.display({
		{ class = "label", x = 0, y = 0, label = "翻译结果（可修改后再写回）:" },
		{ class = "textedit", x = 0, y = 1, name = "result", width = 6, height = 10, value = combined },
	}, { "完成", "丢弃" })
	if btn2 == "完成" and res.replace then
		local applied = apply_translations(subs, sel, res2.result)
		if applied > 0 then aegisub.set_undo_point(script_name .. "/文本翻译写回") end
	end
end

------------------------------------------------------------------ 注册菜单

aegisub.register_macro(script_name .. "/区域截图翻译", "按选中行时间点从视频取帧并裁剪区域，发视觉模型识别+翻译", macro_screenshot)
aegisub.register_macro(script_name .. "/音频翻译选中行", "用ffmpeg切分选中行时间轴音频，发多模态模型转写+翻译", macro_audio)
aegisub.register_macro(script_name .. "/文本直接翻译", "把选中行文本发模型翻译", macro_text)
