#pragma once
#include <string>

#include "cr_proto.h"

// 注入远端控制事件。坐标为目标显示器像素坐标。
void InjectInput(const cr::InputEvent& ev);
void SetClipboardText(const std::string& utf8);
std::string GetClipboardText();
// 返回主显示器尺寸，用于 screen_info。
void PrimaryScreenSize(int* w, int* h);
