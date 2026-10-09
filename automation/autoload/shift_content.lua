script_name = "智能内容平移 (修复版)"
script_description = "批量修改字幕内容向上或向下移动指定轴数，空轴覆盖，无空轴新建。"
script_author = "Gemini"
script_version = "3.0"

-- 核心处理函数：N为移动的轴数（正数向下，负数向上）
local function shift_lines(subs, sel, N)
    if N == 0 then return end
    
    -- 1. 提取所有对话轴，建立索引映射
    local dialogues = {}
    local dialogue_to_sub = {}
    for i = 1, #subs do
        if subs[i].class == "dialogue" then
            table.insert(dialogues, subs[i])
            dialogue_to_sub[#dialogues] = i
        end
    end
    
    -- 2. 标记哪些轴是被选中的
    local in_S = {}
    for _, i in ipairs(sel) do
        for d_idx, sub_idx in ipairs(dialogue_to_sub) do
            if sub_idx == i then
                in_S[d_idx] = true
                break
            end
        end
    end
    
    -- 3. 准备行动清单
    local modifications = {}       -- 记录需要被覆盖的轴的内容
    local insertions_before = {}   -- 记录需要在某轴前插入的新轴
    local insertions_after = {}    -- 记录需要在某轴后插入的新轴
    
    for i = 1, #dialogues do
        insertions_before[i] = {}
        insertions_after[i] = {}
    end
    
    -- 选中的轴默认会被“腾空”（除非有其他轴移入覆盖）
    for d_idx in pairs(in_S) do
        modifications[d_idx] = { 
            text = "", 
            style = dialogues[d_idx].style, 
            actor = dialogues[d_idx].actor, 
            effect = dialogues[d_idx].effect, 
            layer = dialogues[d_idx].layer 
        }
    end
    
    -- 保证从小到大遍历，让插入顺序正确
    local S_list = {}
    for d_idx in pairs(in_S) do table.insert(S_list, d_idx) end
    table.sort(S_list)
    
    -- 4. 模拟平移并生成策略
    for _, i in ipairs(S_list) do
        local tgt = i + N
        local content = {
            text = dialogues[i].text,
            style = dialogues[i].style,
            actor = dialogues[i].actor,
            effect = dialogues[i].effect,
            layer = dialogues[i].layer
        }
        
        -- 判断目标位置
        if tgt < 1 then
            -- 向上超出边界：在首行之前新建
            table.insert(insertions_before[1], content)
        elseif tgt > #dialogues then
            -- 向下超出边界：在尾行之后新建
            table.insert(insertions_after[#dialogues], content)
        else
            -- 剔除特效标签后，判断目标轴是否为空
            local clean_text = dialogues[tgt].text:gsub("{.-}", "")
            local is_empty = (clean_text:match("^%s*$") ~= nil)
            
            -- 如果目标轴本身就被选中(会腾空) 或者 目标轴本来就是空轴 -> 直接覆盖
            if in_S[tgt] or is_empty then
                modifications[tgt] = content
            else
                -- 目标轴不空，且未被选中 -> 新建轴
                if N > 0 then
                    -- 向下移：插在目标轴上方 (时间继承目标轴的起始时间)
                    table.insert(insertions_before[tgt], content)
                else
                    -- 向上移：插在目标轴下方 (时间继承目标轴的结束时间)
                    table.insert(insertions_after[tgt], content)
                end
            end
        end
    end
    
    -- 辅助拷贝函数
    local function clone_line(orig)
        local nl = {}
        for k, v in pairs(orig) do nl[k] = v end
        return nl
    end
    
    -- 5. 从底向上修改字幕数据 (倒序修改，防止影响已记录的物理索引)
    for d_idx = #dialogues, 1, -1 do
        local sub_i = dialogue_to_sub[d_idx]
        local orig_line = dialogues[d_idx]

        -- A. 处理目标轴之后插入
        for k = #insertions_after[d_idx], 1, -1 do
            local c = insertions_after[d_idx][k]
            local nl = clone_line(orig_line)
            nl.text, nl.style, nl.actor = c.text, c.style, c.actor
            nl.effect, nl.layer = c.effect, c.layer
            nl.start_time = orig_line.end_time -- 继承结束时间（瞬间轴）
            nl.end_time = orig_line.end_time
            subs.insert(sub_i + 1, nl)
        end

        -- B. 处理目标轴覆盖/腾空
        if modifications[d_idx] then
            local m = modifications[d_idx]
            local ml = clone_line(orig_line)
            ml.text, ml.style, ml.actor = m.text, m.style, m.actor
            ml.effect, ml.layer = m.effect, m.layer
            subs[sub_i] = ml
        end

        -- C. 处理目标轴之前插入
        for k = #insertions_before[d_idx], 1, -1 do
            local c = insertions_before[d_idx][k]
            local nl = clone_line(orig_line)
            nl.text, nl.style, nl.actor = c.text, c.style, c.actor
            nl.effect, nl.layer = c.effect, c.layer
            nl.start_time = orig_line.start_time -- 继承开始时间（瞬间轴）
            nl.end_time = orig_line.start_time
            subs.insert(sub_i, nl)
        end
    end
end

-- ================== 接口宏包装 ==================

function shift_up_1(subs, sel)
    shift_lines(subs, sel, -1)
    aegisub.set_undo_point("向上移动1个轴")
end

function shift_down_1(subs, sel)
    shift_lines(subs, sel, 1)
    aegisub.set_undo_point("向下移动1个轴")
end

function shift_custom(subs, sel)
    local config = {
        {class = "label", x = 0, y = 0, width = 2, height = 1, label = "请输入移动的轴数\n(正数向下移动，负数向上移动)："},
        {class = "intedit", name = "shift_amount", x = 0, y = 1, width = 2, height = 1, value = 1}
    }
    local btn, res = aegisub.dialog.display(config, {"确定", "取消"})
    if btn == "确定" then
        local N = res.shift_amount
        if N ~= 0 then
            shift_lines(subs, sel, N)
            aegisub.set_undo_point("自定义内容平移")
        end
    end
end

aegisub.register_macro("内容平移/向上移动1个轴 (上移)", "选中的轴批量向上移动1个轴", shift_up_1)
aegisub.register_macro("内容平移/向下移动1个轴 (下移)", "选中的轴批量向下移动1个轴", shift_down_1)
aegisub.register_macro("内容平移/自定义移动轴数", "弹窗输入需要移动几个轴", shift_custom)