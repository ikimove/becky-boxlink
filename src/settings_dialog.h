#pragma once
#include <windows.h>
#include <string>

// 設定画面（BoxLink.ini とユーザー環境変数を編集する）。保存したら IDOK を返す
INT_PTR ShowSettingsDialog(HINSTANCE hInst, HWND hParent, const std::wstring& iniPath);
