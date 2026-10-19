#include "input.h"

#include <windows.h>

#include <vector>

namespace {

int MouseFlag(int button, bool down) {
  switch (button) {
    case 1: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    case 2: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    default: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
  }
}

// 绝对坐标：以虚拟桌面为基准，避免多显示器时偏移。
void MoveAbsolute(int x, int y) {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (vw <= 1 || vh <= 1) return;
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = static_cast<LONG>((static_cast<long long>(x - vx) * 65535) / (vw - 1));
  in.mi.dy = static_cast<LONG>((static_cast<long long>(y - vy) * 65535) / (vh - 1));
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  SendInput(1, &in, sizeof(in));
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

void InjectInput(const cr::InputEvent& ev) {
  switch (ev.kind) {
    case cr::InputEvent::kMove:
      MoveAbsolute(ev.x, ev.y);
      break;
    case cr::InputEvent::kButton: {
      MoveAbsolute(ev.x, ev.y);
      INPUT in{};
      in.type = INPUT_MOUSE;
      in.mi.dwFlags = static_cast<DWORD>(MouseFlag(ev.button, ev.pressed));
      SendInput(1, &in, sizeof(in));
      break;
    }
    case cr::InputEvent::kWheel: {
      INPUT in{};
      in.type = INPUT_MOUSE;
      in.mi.dwFlags = MOUSEEVENTF_WHEEL;
      in.mi.mouseData = static_cast<DWORD>(ev.delta_y);
      SendInput(1, &in, sizeof(in));
      break;
    }
    case cr::InputEvent::kKey: {
      INPUT in{};
      in.type = INPUT_KEYBOARD;
      in.ki.wVk = static_cast<WORD>(ev.keycode);
      in.ki.dwFlags = ev.pressed ? 0 : KEYEVENTF_KEYUP;
      // 方向键/Insert/Delete/Home/End/PageUp/PageDown 需扩展键标志
      switch (ev.keycode) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU:
          in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
          break;
        default: break;
      }
      SendInput(1, &in, sizeof(in));
      break;
    }
    default:
      break;
  }
}

void SetClipboardText(const std::string& utf8) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  std::wstring w = Utf8ToWide(utf8);
  size_t bytes = (w.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (h) {
    void* p = GlobalLock(h);
    memcpy(p, w.c_str(), bytes);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
}

std::string GetClipboardText() {
  if (!OpenClipboard(nullptr)) return "";
  std::string out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    auto* p = static_cast<const wchar_t*>(GlobalLock(h));
    if (p) {
      out = WideToUtf8(p);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

void PrimaryScreenSize(int* w, int* h) {
  if (w) *w = GetSystemMetrics(SM_CXSCREEN);
  if (h) *h = GetSystemMetrics(SM_CYSCREEN);
}
