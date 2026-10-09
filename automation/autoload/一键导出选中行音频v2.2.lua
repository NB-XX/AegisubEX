local tr = aegisub.gettext
script_name = tr("一键导出选中行音频")
script_description = tr("脱裤子放屁工具")
script_author = "拉姆0v0"
script_version = "v2.2"

dialog_config1=
{

	[1]={class="label",x=0,y=0,label="【前后延长设置】"},
	[2]={class="label",x=0,y=1,label="前面延长：(单位ms)"},
	[3]={class="intedit",name="前面延长",x=1,y=1,width=1,height=1,items={},value=300,hint="请输入整数"},
	[4]={class="label",x=0,y=2,label="后面延长：(单位ms)"},
	[5]={class="intedit",name="后面延长",x=1,y=2,width=1,height=1,items={},value=300,hint="请输入整数"},
}

function text_processing(subs,sel)
	properties = aegisub.project_properties()
	audio_file = properties.audio_file
	local startnum
	for i=1,#subs do
		if subs[i].class == "dialogue" then
			startnum = i
			break
		end
	end
	if audio_file ~= "" then
		output_filename = aegisub.dialog.save("【选择音频导出位置】", "", "截取音频", "音频文件 (.mp3)|*.mp3|音频文件 (.wav)|*.wav", false)
		if output_filename == nil then
			aegisub.debug.out("请重新选择正确的输出位置。")
		else
			buttons,results =aegisub.dialog.display(dialog_config1,{"OK","Cancel"})
			if buttons=="OK" then
				local addstart = results["前面延长"]
				local addend = results["后面延长"]
				local line_number = {}
				local j = 1
				local outputfilenum = 0
				while j<=#sel do
					local linenum = sel[j]-startnum+1
					local outputlinenum = "【"..tostring(linenum).."】"
					table.insert(line_number , linenum)
					local st_predo = subs[sel[j]].start_time - addstart
					if st_predo < 0 then
						st_predo = 0
					end
					local st_t = st_predo / 1000
					local k = j
					while k < #sel do
						if sel[k+1]-sel[k] == 1 then
							k=k+1
							linenum = sel[k]-startnum+1
							table.insert(line_number , linenum)
							outputlinenum = outputlinenum.."【"..tostring(linenum).."】"
						else
							break
						end
					end
					local duration_t = (subs[sel[k]].end_time - subs[sel[j]].start_time + (subs[sel[j]].start_time - st_predo) + addend) / 1000
					local output_name = string.sub(output_filename, 1,(string.len(output_filename)-4))
					local output_type = string.sub(output_filename, (string.len(output_filename)-2))
					local ts="ffmpeg -y -i \""..audio_file.."\" -ss "..st_t.." -t "..duration_t.." -f "..output_type.." \""..output_name.."_第"..outputlinenum.."行."..output_type.."\""
					os.execute(ts)
					j=k+1
					outputfilenum = outputfilenum+1
				end
				aegisub.debug.out("导出音频对应的字幕行是：\n")
				for k=1,#line_number do
					aegisub.debug.out("【第"..line_number[k].."行】\n")
				end
				aegisub.debug.out("共选择"..#line_number.."行字幕，共生成导出"..outputfilenum.."个音频文件。                            ———————————by拉姆0v0")
			else
				aegisub.debug.out("已取消操作")
			end
			
		end
	else
		aegisub.debug.out("请在aegisub中打开一个音频文件或者视频文件。")
	end
end


TLL_macros = {
	{
		script_name = "一键导出选中行音频",
		script_description = "一键导出选中行音频",
		entry = function(subs,sel) text_processing(subs,sel) end,
		validation = false
	},
}

for i = 1, #TLL_macros do
	aegisub.register_macro(script_name.." "..script_version.."/"..TLL_macros[i]["script_name"], TLL_macros[i]["script_description"], TLL_macros[i]["entry"], TLL_macros[i]["validation"])
end
