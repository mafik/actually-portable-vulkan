#include "window_session_win32.hpp"

#include <libc/nt/dll.h>      // GetModuleHandle, GetProcAddress, LoadLibraryA
#include <libc/nt/enum/cs.h>  // kNtCsHredraw / kNtCsVredraw
#include <libc/nt/enum/cw.h>  // kNtCwUsedefault
#include <libc/nt/enum/idc.h> // kNtIdcArrow
#include <libc/nt/enum/sw.h>  // kNtSwShow
#include <libc/nt/enum/vk.h>  // kNtVkEscape
#include <libc/nt/enum/wm.h>  // kNtWmDestroy / Close / Keydown / Quit / Paint
#include <libc/nt/enum/ws.h>  // kNtWsOverlappedwindow / kNtWsVisible
#include <libc/nt/events.h> // GetMessage/TranslateMessage/DispatchMessage/PostQuitMessage
#include <libc/nt/paint.h>   // BeginPaint/EndPaint + NtPaintStruct
#include <libc/nt/runtime.h> // GetLastError
#include <libc/nt/struct/msg.h>
#include <libc/nt/struct/wndclass.h>
#include <libc/nt/windows.h> // CreateWindowEx, RegisterClass, ShowWindow, LoadCursor

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <uchar.h> // char16_t

namespace {

constexpr const char *kVkKhrWin32Surface = "VK_KHR_win32_surface";
constexpr VkFlags kVkWin32SurfaceSType =
    VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;

struct LocalVkWin32SurfaceCreateInfoKHR {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  void *hinstance; // HINSTANCE on Windows, opaque here
  void *hwnd;      // HWND on Windows, opaque here
};

using LocalPfnVkCreateWin32SurfaceKHR =
    VkResult(VKAPI_PTR *)(VkInstance, const LocalVkWin32SurfaceCreateInfoKHR *,
                          const VkAllocationCallbacks *, VkSurfaceKHR *);

// PeekMessageW isn't exported by cosmo's libc/nt/ headers.
using PeekMessageW_t = int32_t(__attribute__((__ms_abi__)) *)(
    struct NtMsg *lpMsg, int64_t hwnd, uint32_t wMsgFilterMin,
    uint32_t wMsgFilterMax, uint32_t wRemoveMsg);

constexpr uint32_t kPmRemove = 0x0001;

PeekMessageW_t g_peek_message = nullptr;

Win32WindowSession *g_active_session = nullptr;

constexpr const char16_t kClassName[] = u"ApvWindowClass";

void widen_ascii(const char *src, char16_t *dst, size_t dst_cap) {
  size_t i = 0;
  for (; src[i] && i + 1 < dst_cap; ++i) {
    dst[i] = static_cast<char16_t>(static_cast<unsigned char>(src[i]));
  }
  dst[i] = 0;
}

bool resolve_peek_message() {
  if (g_peek_message)
    return true;
  int64_t user32 = LoadLibraryA("user32.dll");
  if (!user32) {
    std::fprintf(stderr, "win32: LoadLibraryA(user32.dll) failed: %u\n",
                 GetLastError());
    return false;
  }
  void *p = GetProcAddress(user32, "PeekMessageW");
  if (!p) {
    std::fprintf(stderr, "win32: GetProcAddress(PeekMessageW) failed: %u\n",
                 GetLastError());
    return false;
  }
  g_peek_message = reinterpret_cast<PeekMessageW_t>(p);
  return true;
}

} // namespace

Win32WindowSession::Win32WindowSession() : WindowSession(kKind) {}

Win32WindowSession::~Win32WindowSession() {
  if (hwnd_)
    DestroyWindow(hwnd_);
  if (g_active_session == this)
    g_active_session = nullptr;
}

bool Win32WindowSession::create_window(unsigned width, unsigned height,
                                       const char *title) {
  if (!resolve_peek_message())
    return false;

  hinstance_ = GetModuleHandle(nullptr);

  struct NtWndClass wc{};
  wc.style = kNtCsHredraw | kNtCsVredraw;
  // The actual calling convention at runtime is what matters.
  wc.lpfnWndProc = reinterpret_cast<NtWndProc>(&Win32WindowSession::wndproc);
  wc.hInstance = hinstance_;
  wc.hCursor = LoadCursor(0, kNtIdcArrow);
  wc.lpszClassName = kClassName;

  if (!RegisterClass(&wc)) {
    uint32_t err = GetLastError();
    // 1410 == ERROR_CLASS_ALREADY_EXISTS
    if (err != 1410) {
      std::fprintf(stderr, "win32: RegisterClass failed: %u\n", err);
      return false;
    }
  }

  char16_t wtitle[256];
  widen_ascii(title, wtitle, sizeof(wtitle) / sizeof(wtitle[0]));

  g_active_session = this;

  hwnd_ = CreateWindowEx(0, kClassName, wtitle,
                         kNtWsOverlappedwindow | kNtWsVisible, kNtCwUsedefault,
                         kNtCwUsedefault, static_cast<int>(width),
                         static_cast<int>(height), 0, 0, hinstance_, 0);

  if (!hwnd_) {
    std::fprintf(stderr, "win32: CreateWindowEx failed: %u\n", GetLastError());
    return false;
  }

  ShowWindow(hwnd_, kNtSwShow);
  return true;
}

bool Win32WindowSession::poll_events(bool &should_close) {
  if (error_)
    return false;
  struct NtMsg msg{};
  while (g_peek_message(&msg, 0, 0, 0, kPmRemove)) {
    if (msg.dwMessage == kNtWmQuit) {
      should_close_ = true;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  if (should_close_)
    should_close = true;
  return !error_;
}

const char *Win32WindowSession::vk_surface_extension_name() const {
  return kVkKhrWin32Surface;
}

bool Win32WindowSession::create_vk_surface(const VkApi &vk, VkInstance instance,
                                           VkSurfaceKHR *out_surface) {
  auto pfn = reinterpret_cast<LocalPfnVkCreateWin32SurfaceKHR>(
      vk_trampoline(reinterpret_cast<void *>(
          vk.vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"))));
  if (!pfn) {
    std::fprintf(stderr, "vk: vkCreateWin32SurfaceKHR unavailable (is "
                         "VK_KHR_win32_surface enabled?)\n");
    return false;
  }

  LocalVkWin32SurfaceCreateInfoKHR ci{};
  ci.sType = static_cast<VkStructureType>(kVkWin32SurfaceSType);
  ci.hinstance = reinterpret_cast<void *>(hinstance_);
  ci.hwnd = reinterpret_cast<void *>(hwnd_);
  VkResult r = pfn(instance, &ci, nullptr, out_surface);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "vk: vkCreateWin32SurfaceKHR failed: %d\n", int(r));
    return false;
  }
  return true;
}

__attribute__((__ms_abi__)) int64_t Win32WindowSession::wndproc(
    int64_t hwnd, uint32_t msg, uint64_t wparam, int64_t lparam) {
  switch (msg) {
  case kNtWmClose:
    if (g_active_session)
      g_active_session->should_close_ = true;
    return 0;
  case kNtWmDestroy:
    PostQuitMessage(0);
    return 0;
  case kNtWmKeydown:
    if (wparam == kNtVkEscape && g_active_session) {
      g_active_session->should_close_ = true;
    }
    return 0;
  case kNtWmPaint: {
    // Minimal paint: begin/end so Windows doesn't re-queue WM_PAINT
    // forever while the Vulkan swapchain is doing the real drawing.
    struct NtPaintStruct ps{};
    int64_t hdc = BeginPaint(hwnd, &ps);
    (void)hdc;
    EndPaint(hwnd, &ps);
    return 0;
  }
  default:
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }
}
