// Copyright (c) 2020 Microsoft Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-CHROMIUM file.

#include "shell/browser/win/dark_mode.h"

// #include <climits>
// #include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

// Including SDKDDKVer.h defines the highest available Windows platform.
// If you wish to build your application for a previous Windows platform,
// include WinSDKVer.h and set the _WIN32_WINNT macro to the platform you wish
// to support before including SDKDDKVer.h.
#include <SDKDDKVer.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <Vssym32.h>
#include <Windows.h>
#include <WindowsX.h>
#include <dwmapi.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Uxtheme.lib")

// This namespace contains code from
// https://github.com/stevemk14ebr/PolyHook_2_0/blob/master/sources/IatHook.cpp
// which is licensed under the MIT License.
// See PolyHook_2_0-LICENSE for more information.
namespace {

template <typename T, typename T1, typename T2>
constexpr T RVA2VA(T1 base, T2 rva) {
  return reinterpret_cast<T>(reinterpret_cast<ULONG_PTR>(base) + rva);
}

template <typename T>
constexpr T DataDirectoryFromModuleBase(void* moduleBase, size_t entryID) {
  auto* dosHdr = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
  auto* ntHdr = RVA2VA<PIMAGE_NT_HEADERS>(moduleBase, dosHdr->e_lfanew);
  auto* dataDir = ntHdr->OptionalHeader.DataDirectory;
  return RVA2VA<T>(moduleBase, dataDir[entryID].VirtualAddress);
}

#if 0
PIMAGE_THUNK_DATA FindAddressByName(void *moduleBase, PIMAGE_THUNK_DATA impName, PIMAGE_THUNK_DATA impAddr, const char *funcName)
{
	for (; impName->u1.Ordinal; ++impName, ++impAddr)
	{
		if (IMAGE_SNAP_BY_ORDINAL(impName->u1.Ordinal))
			continue;

		auto* import = RVA2VA<PIMAGE_IMPORT_BY_NAME>(moduleBase, impName->u1.AddressOfData);
		if (strcmp(import->Name, funcName) != 0)
			continue;
		return impAddr;
	}
	return nullptr;
}
#endif

PIMAGE_THUNK_DATA FindAddressByOrdinal(void* moduleBase,
                                       PIMAGE_THUNK_DATA impName,
                                       PIMAGE_THUNK_DATA impAddr,
                                       uint16_t ordinal) {
  for (; impName->u1.Ordinal; ++impName, ++impAddr) {
    if (IMAGE_SNAP_BY_ORDINAL(impName->u1.Ordinal) &&
        IMAGE_ORDINAL(impName->u1.Ordinal) == ordinal)
      return impAddr;
  }
  return nullptr;
}

#if 0
PIMAGE_THUNK_DATA FindIatThunkInModule(void *moduleBase, const char *dllName, const char *funcName)
{
	auto* imports = DataDirectoryFromModuleBase<PIMAGE_IMPORT_DESCRIPTOR>(moduleBase, IMAGE_DIRECTORY_ENTRY_IMPORT);
	for (; imports->Name; ++imports)
	{
		if (_stricmp(RVA2VA<LPCSTR>(moduleBase, imports->Name), dllName) != 0)
			continue;

		auto* origThunk = RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->OriginalFirstThunk);
		auto* thunk = RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->FirstThunk);
		return FindAddressByName(moduleBase, origThunk, thunk, funcName);
	}
	return nullptr;
}

PIMAGE_THUNK_DATA FindDelayLoadThunkInModule(void *moduleBase, const char *dllName, const char *funcName)
{
	auto* imports = DataDirectoryFromModuleBase<PIMAGE_DELAYLOAD_DESCRIPTOR>(moduleBase, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
	for (; imports->DllNameRVA; ++imports)
	{
		if (_stricmp(RVA2VA<LPCSTR>(moduleBase, imports->DllNameRVA), dllName) != 0)
			continue;

		auto* impName = RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->ImportNameTableRVA);
		auto* impAddr = RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->ImportAddressTableRVA);
		return FindAddressByName(moduleBase, impName, impAddr, funcName);
	}
	return nullptr;
}
#endif

PIMAGE_THUNK_DATA FindDelayLoadThunkInModule(void* moduleBase,
                                             const char* dllName,
                                             uint16_t ordinal) {
  auto* imports = DataDirectoryFromModuleBase<PIMAGE_DELAYLOAD_DESCRIPTOR>(
      moduleBase, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
  for (; imports->DllNameRVA; ++imports) {
    if (_stricmp(RVA2VA<LPCSTR>(moduleBase, imports->DllNameRVA), dllName) != 0)
      continue;

    auto* impName =
        RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->ImportNameTableRVA);
    auto* impAddr =
        RVA2VA<PIMAGE_THUNK_DATA>(moduleBase, imports->ImportAddressTableRVA);
    return FindAddressByOrdinal(moduleBase, impName, impAddr, ordinal);
  }
  return nullptr;
}

}  // namespace

// Code in this namespace (c) 2019 Richard Yu.
// Use of this source code is governed by the MIT license.
// Source: https://github.com/ysc3839/win32-darkmode/
namespace {

enum IMMERSIVE_HC_CACHE_MODE { IHCM_USE_CACHED_VALUE, IHCM_REFRESH };

// 1903 18362
enum PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };

enum WINDOWCOMPOSITIONATTRIB {
  WCA_UNDEFINED = 0,
  WCA_NCRENDERING_ENABLED = 1,
  WCA_NCRENDERING_POLICY = 2,
  WCA_TRANSITIONS_FORCEDISABLED = 3,
  WCA_ALLOW_NCPAINT = 4,
  WCA_CAPTION_BUTTON_BOUNDS = 5,
  WCA_NONCLIENT_RTL_LAYOUT = 6,
  WCA_FORCE_ICONIC_REPRESENTATION = 7,
  WCA_EXTENDED_FRAME_BOUNDS = 8,
  WCA_HAS_ICONIC_BITMAP = 9,
  WCA_THEME_ATTRIBUTES = 10,
  WCA_NCRENDERING_EXILED = 11,
  WCA_NCADORNMENTINFO = 12,
  WCA_EXCLUDED_FROM_LIVEPREVIEW = 13,
  WCA_VIDEO_OVERLAY_ACTIVE = 14,
  WCA_FORCE_ACTIVEWINDOW_APPEARANCE = 15,
  WCA_DISALLOW_PEEK = 16,
  WCA_CLOAK = 17,
  WCA_CLOAKED = 18,
  WCA_ACCENT_POLICY = 19,
  WCA_FREEZE_REPRESENTATION = 20,
  WCA_EVER_UNCLOAKED = 21,
  WCA_VISUAL_OWNER = 22,
  WCA_HOLOGRAPHIC = 23,
  WCA_EXCLUDED_FROM_DDA = 24,
  WCA_PASSIVEUPDATEMODE = 25,
  WCA_USEDARKMODECOLORS = 26,  // build 18875+
  WCA_LAST = 27
};

struct WINDOWCOMPOSITIONATTRIBDATA {
  WINDOWCOMPOSITIONATTRIB Attrib;
  PVOID pvData;
  SIZE_T cbData;
};

using fnRtlGetNtVersionNumbers = void(WINAPI*)(LPDWORD major,
                                               LPDWORD minor,
                                               LPDWORD build);
using fnSetWindowCompositionAttribute =
    BOOL(WINAPI*)(HWND hWnd, WINDOWCOMPOSITIONATTRIBDATA*);
// 1809 17763
using fnShouldAppsUseDarkMode = bool(WINAPI*)();  // ordinal 132
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND hWnd,
                                               bool allow);  // ordinal 133
using fnAllowDarkModeForApp =
    bool(WINAPI*)(bool allow);              // ordinal 135, in 1809
using fnFlushMenuThemes = void(WINAPI*)();  // ordinal 136
using fnRefreshImmersiveColorPolicyState = void(WINAPI*)();     // ordinal 104
using fnIsDarkModeAllowedForWindow = bool(WINAPI*)(HWND hWnd);  // ordinal 137
using fnGetIsImmersiveColorUsingHighContrast =
    bool(WINAPI*)(IMMERSIVE_HC_CACHE_MODE mode);  // ordinal 106
using fnOpenNcThemeData = HTHEME(WINAPI*)(HWND hWnd,
                                          LPCWSTR pszClassList);  // ordinal 49
// 1903 18362
using fnShouldSystemUseDarkMode = bool(WINAPI*)();  // ordinal 138
using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(
    PreferredAppMode appMode);                      // ordinal 135, in 1903
using fnIsDarkModeAllowedForApp = bool(WINAPI*)();  // ordinal 139

fnSetWindowCompositionAttribute _SetWindowCompositionAttribute = nullptr;
fnShouldAppsUseDarkMode _ShouldAppsUseDarkMode = nullptr;
fnAllowDarkModeForWindow _AllowDarkModeForWindow = nullptr;
fnAllowDarkModeForApp _AllowDarkModeForApp = nullptr;
// fnFlushMenuThemes _FlushMenuThemes = nullptr;
fnRefreshImmersiveColorPolicyState _RefreshImmersiveColorPolicyState = nullptr;
fnIsDarkModeAllowedForWindow _IsDarkModeAllowedForWindow = nullptr;
fnGetIsImmersiveColorUsingHighContrast _GetIsImmersiveColorUsingHighContrast =
    nullptr;
fnOpenNcThemeData _OpenNcThemeData = nullptr;
// 1903 18362
// fnShouldSystemUseDarkMode _ShouldSystemUseDarkMode = nullptr;
fnSetPreferredAppMode _SetPreferredAppMode = nullptr;

using fnDwmSetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
fnDwmSetWindowAttribute _DwmSetWindowAttribute = {};

using fnDwmGetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
fnDwmGetWindowAttribute _DwmGetWindowAttribute = {};

bool g_darkModeSupported = false;
bool g_darkModeEnabled = false;
DWORD g_buildNumber = 0;

bool AllowDarkModeForWindow(HWND hWnd, bool allow) {
  if (g_darkModeSupported)
    return _AllowDarkModeForWindow(hWnd, allow);
  return false;
}

bool IsHighContrast() {
  HIGHCONTRASTW highContrast = {sizeof(highContrast)};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast),
                            &highContrast, FALSE))
    return highContrast.dwFlags & HCF_HIGHCONTRASTON;
  return false;
}

void RefreshTitleBarThemeColor(HWND hWnd, bool dark) {
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << " hWnd["
            << hWnd << "] dark[" << dark << ']' << std::endl;
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " IsDarkModeAllowedForWindow["
            << _IsDarkModeAllowedForWindow(hWnd) << ']' << std::endl;
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " IsHighContrast[" << IsHighContrast() << ']' << std::endl;
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " _ShouldAppsUseDarkMode[" << _ShouldAppsUseDarkMode() << ']'
            << std::endl;
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " buildNumber " << g_buildNumber << std::endl;

#if 1
  if (g_buildNumber < 18362) {
    std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
              << std::endl;
    SetPropW(hWnd, L"UseImmersiveDarkModeColors",
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(dark)));
  } else if (_SetWindowCompositionAttribute) {
    std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << ' '
              << dark << std::endl;
    auto data =
        WINDOWCOMPOSITIONATTRIBDATA{WCA_USEDARKMODECOLORS, &dark, sizeof dark};
    // WINDOWCOMPOSITIONATTRIBDATA data = {WCA_USEDARKMODECOLORS, &ldark, sizeof
    // ldark}; sizeof(dark)};
    _SetWindowCompositionAttribute(hWnd, &data);
  } else {
    std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
              << std::endl;
  }
#else
  LONG ldark = dark;
  if (g_buildNumber >= 20161) {
    std::cerr << "a" << std::endl;
    _DwmSetWindowAttribute(hWnd, 20, &ldark,
                           sizeof dark);  // DWMA_USE_IMMERSIVE_DARK_MODE = 20
  } else if (g_buildNumber >= 18363) {
    std::cerr << "b ldark " << ldark << std::endl;
    auto data = WINDOWCOMPOSITIONATTRIBDATA{WCA_USEDARKMODECOLORS, &ldark,
                                            sizeof ldark};
    _SetWindowCompositionAttribute(hWnd, &data);
  } else {
    std::cerr << "c" << std::endl;
    _DwmSetWindowAttribute(hWnd, 0x13, &ldark, sizeof ldark);
  }
#endif
}

void RefreshTitleBarThemeColor(HWND hWnd) {
  BOOL dark = FALSE;
  if (_IsDarkModeAllowedForWindow(hWnd) && _ShouldAppsUseDarkMode() &&
      !IsHighContrast()) {
    dark = TRUE;
  }
  RefreshTitleBarThemeColor(hWnd, dark);
}

bool IsColorSchemeChangeMessage(LPARAM lParam) {
  bool is = false;
  if (lParam &&
      CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1,
                           L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
    _RefreshImmersiveColorPolicyState();
    is = true;
  }
  _GetIsImmersiveColorUsingHighContrast(IHCM_REFRESH);
  return is;
}

#if 0
bool IsColorSchemeChangeMessage(UINT message, LPARAM lParam) {
  if (message == WM_SETTINGCHANGE)
    return IsColorSchemeChangeMessage(lParam);
  return false;
}
#endif

void AllowDarkModeForApp(bool allow) {
  if (_AllowDarkModeForApp)
    _AllowDarkModeForApp(allow);
  else if (_SetPreferredAppMode)
    _SetPreferredAppMode(allow ? AllowDark : Default);
}

void FixDarkScrollBar() {
  HMODULE hComctl =
      LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (hComctl) {
    auto* addr = FindDelayLoadThunkInModule(hComctl, "uxtheme.dll",
                                            49);  // OpenNcThemeData
    if (addr) {
      DWORD oldProtect;
      if (VirtualProtect(addr, sizeof(IMAGE_THUNK_DATA), PAGE_READWRITE,
                         &oldProtect)) {
        auto MyOpenThemeData = [](HWND hWnd, LPCWSTR classList) -> HTHEME {
          if (wcscmp(classList, L"ScrollBar") == 0) {
            hWnd = nullptr;
            classList = L"Explorer::ScrollBar";
          }
          return _OpenNcThemeData(hWnd, classList);
        };

        addr->u1.Function = reinterpret_cast<ULONG_PTR>(
            static_cast<fnOpenNcThemeData>(MyOpenThemeData));
        VirtualProtect(addr, sizeof(IMAGE_THUNK_DATA), oldProtect, &oldProtect);
      }
    }
  }
}

template <typename P>
bool Symbol(HMODULE h, P& pointer, const char* name) {
  if (P p = reinterpret_cast<P>(GetProcAddress(h, name))) {
    pointer = p;
    return true;
  } else {
    return false;
  }
}

bool CheckBuildNumber(DWORD buildNumber) {
  // constexpr bool CheckBuildNumber(DWORD buildNumber) {
  std::cerr << "build number " << buildNumber << std::endl;
  return (buildNumber == 17763 ||  // 1809
          buildNumber == 18362 ||  // 1903
          buildNumber == 18363 ||  // 1909
          buildNumber == 19041);   // 2004
}

void InitDarkMode() {
  HMODULE hDwmApi = LoadLibrary(L"DWMAPI");
  if (hDwmApi != 0) {
    Symbol(hDwmApi, _DwmGetWindowAttribute, "DwmGetWindowAttribute");
    Symbol(hDwmApi, _DwmSetWindowAttribute, "DwmSetWindowAttribute");
  }
  std::cerr << "_DwmGetWindowAttribute "
            << reinterpret_cast<void*>(_DwmGetWindowAttribute) << std::endl;

  auto RtlGetNtVersionNumbers = reinterpret_cast<fnRtlGetNtVersionNumbers>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers"));
  if (RtlGetNtVersionNumbers) {
    DWORD major, minor;
    RtlGetNtVersionNumbers(&major, &minor, &g_buildNumber);
    g_buildNumber &= ~0xF0000000;
    if (major == 10 && minor == 0 && CheckBuildNumber(g_buildNumber)) {
      HMODULE hUxtheme =
          LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (hUxtheme) {
        _OpenNcThemeData = reinterpret_cast<fnOpenNcThemeData>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(49)));
        _RefreshImmersiveColorPolicyState =
            reinterpret_cast<fnRefreshImmersiveColorPolicyState>(
                GetProcAddress(hUxtheme, MAKEINTRESOURCEA(104)));
        _GetIsImmersiveColorUsingHighContrast =
            reinterpret_cast<fnGetIsImmersiveColorUsingHighContrast>(
                GetProcAddress(hUxtheme, MAKEINTRESOURCEA(106)));
        _ShouldAppsUseDarkMode = reinterpret_cast<fnShouldAppsUseDarkMode>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(132)));
        _AllowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));

        auto ord135 = GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
        if (g_buildNumber < 18362)
          _AllowDarkModeForApp =
              reinterpret_cast<fnAllowDarkModeForApp>(ord135);
        else
          _SetPreferredAppMode =
              reinterpret_cast<fnSetPreferredAppMode>(ord135);

        //_FlushMenuThemes =
        // reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(hUxtheme,
        // MAKEINTRESOURCEA(136)));
        _IsDarkModeAllowedForWindow =
            reinterpret_cast<fnIsDarkModeAllowedForWindow>(
                GetProcAddress(hUxtheme, MAKEINTRESOURCEA(137)));

        _SetWindowCompositionAttribute =
            reinterpret_cast<fnSetWindowCompositionAttribute>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"),
                               "SetWindowCompositionAttribute"));

        if (_OpenNcThemeData && _RefreshImmersiveColorPolicyState &&
            _ShouldAppsUseDarkMode && _AllowDarkModeForWindow &&
            (_AllowDarkModeForApp || _SetPreferredAppMode) &&
            //_FlushMenuThemes &&
            _IsDarkModeAllowedForWindow) {
          g_darkModeSupported = true;

          AllowDarkModeForApp(true);
          _RefreshImmersiveColorPolicyState();

          g_darkModeEnabled = _ShouldAppsUseDarkMode() && !IsHighContrast();

          FixDarkScrollBar();
        }
      }
    }
  }
}

}  // namespace

namespace electron {

std::once_flag dark_mode_inited_;

void EnsureInitialized() {
  std::call_once(dark_mode_inited_, []() { ::InitDarkMode(); });
}

bool IsDarkPreferred(ui::NativeTheme::ThemeSource theme_source) {
  switch (theme_source) {
    case ui::NativeTheme::ThemeSource::kForcedLight:
      return false;
    case ui::NativeTheme::ThemeSource::kForcedDark:
      return win::IsDarkModeSupported();
    case ui::NativeTheme::ThemeSource::kSystem:
      return win::IsDarkModeEnabled();
  }
}

namespace win {

void AllowDarkModeForApp(bool allow) {
  EnsureInitialized();

  ::AllowDarkModeForApp(allow);
}

bool AllowDarkModeForWindow(HWND hWnd, bool allow) {
  EnsureInitialized();

  bool const ret = ::AllowDarkModeForWindow(hWnd, allow);
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << " hWnd["
            << hWnd << "] allow[" << allow << "] ret[" << ret << ']'
            << std::endl;
  return ret;
}

bool IsDarkModeEnabled() {
  EnsureInitialized();

  return g_darkModeEnabled;
}

bool IsDarkModeSupported() {
  EnsureInitialized();

  return g_darkModeSupported;
}

void HandleSettingChange(HWND hWnd,
                         UINT message,
                         WPARAM wParam,
                         LPARAM lParam) {
  EnsureInitialized();

  if (message == WM_SETTINGCHANGE) {
    if (::IsColorSchemeChangeMessage(lParam)) {
      g_darkModeEnabled = ::_ShouldAppsUseDarkMode() && !::IsHighContrast();
      ::RefreshTitleBarThemeColor(hWnd);
      // SendMessageW(g_hWndListView, WM_THEMECHANGED, 0, 0);
    }
  }
}

void HandleWindowThemeChanged(HWND hWnd) {
  EnsureInitialized();

  if (IsDarkModeSupported()) {
    AllowDarkModeForWindow(hWnd, IsDarkModeEnabled());
    ::RefreshTitleBarThemeColor(hWnd);
  }
}

void SetDarkModeForApp(ui::NativeTheme::ThemeSource theme_source) {
  auto const use_dark = IsDarkPreferred(theme_source);

  ::AllowDarkModeForApp(use_dark);
}

void SetDarkModeForWindow(HWND hWnd,
                          ui::NativeTheme::ThemeSource theme_source) {
  EnsureInitialized();

  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " theme_source[" << theme_source << ']' << std::endl;
  auto const use_dark = IsDarkPreferred(theme_source);
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__
            << " use_dark[" << use_dark << ']' << std::endl;

  // for (;;) {
  HWND up = GetParent(hWnd);
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << " hWnd["
            << hWnd << "] up [" << up << ']' << std::endl;

  // set the titlebar to confirm we have the right window
  char str[64];
  snprintf(str, sizeof(str), "%zu", size_t(hWnd));
  ::SetWindowTextA(hWnd, str);

  // set permissions
  if (_AllowDarkModeForApp != nullptr) {
    std::cerr << "AllowDarkModeForApp " << use_dark << std::endl;
    _AllowDarkModeForApp(use_dark);
  }
  if (_AllowDarkModeForWindow != nullptr) {
    std::cerr << "AllowDarkModeForWindow " << use_dark << std::endl;
    _AllowDarkModeForWindow(hWnd, use_dark);
  }

  // set the titlebar to dark
  // BOOL darkFlag = TRUE;
  // auto data = WINDOWCOMPOSITIONATTRIBDATA {WCA_USEDARKMODECOLORS, &darkFlag,
  // sizeof(darkFlag) }; _SetWindowCompositionAttribute(hWnd, &data);

  // tell it to repaint
  // STYLESTRUCT style_struct = {};
  // SendMessageW(hWnd, WM_STYLECHANGED, 0,
  // reinterpret_cast<LPARAM>(&style_struct)); SendNotifyMessageA(hWnd,
  // WM_STYLECHANGED, 0, 0); PostMessageW(hWnd, WM_STYLECHANGED, 0, 0);
  // PostMessageW(hWnd, WM_THEMECHANGED, 0, 0);
  // PostMessageW(hWnd, WM_STYLECHANGED, GWL_STYLE, 0);
  // SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);
  // SetWindowPos (hWnd, NULL, 0,0,0,0, SWP_FRAMECHANGED | SWP_DRAWFRAME |
  // SWP_NOREPOSITION | SWP_NOSIZE | SWP_NOMOVE); SendMessageW(hWnd,
  // WM_STYLECHANGED, 0, 0); SetWindowPos (hWnd, NULL, 0,0,0,0, SWP_FRAMECHANGED
  // | SWP_DRAWFRAME | SWP_NOREPOSITION | SWP_NOSIZE | SWP_NOMOVE);
  // RedrawWindow(hWnd, {}, {}, RDW_INVALIDATE|RDW_FRAME|RDW_NOINTERNALPAINT);
  // UpdateWindow(hWnd);
  // SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);

  if (g_buildNumber >= 18875) {
    BOOL darkFlag = use_dark != 0;
    auto attr = WINDOWCOMPOSITIONATTRIBDATA{WCA_USEDARKMODECOLORS, &darkFlag,
                                            sizeof(darkFlag)};
    _SetWindowCompositionAttribute(hWnd, &attr);
    //_RefreshImmersiveColorPolicyState();
    // SetWindowPos (hWnd, NULL, 0,0,0,0, SWP_SHOWWINDOW | SWP_FRAMECHANGED |
    // SWP_DRAWFRAME | SWP_NOREPOSITION | SWP_NOSIZE | SWP_NOMOVE);
    //::RedrawWindow(hWnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE |
    //RDW_ERASE | RDW_INTERNALPAINT | RDW_ALLCHILDREN | RDW_UPDATENOW);
    // PostMessage(hWnd, WM_DWMNCRENDERINGCHANGED, TRUE, 0);
    // PostMessage(hWnd, WM_DWMNCRENDERINGCHANGED, FALSE, 0);
    // PostMessage(hWnd, WM_DWMNCRENDERINGCHANGED, TRUE, 0);
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_DRAWFRAME | SWP_FRAMECHANGED);
  }
#if 0
	  else {
          static constexpr int DWMA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;
          static constexpr int DWMA_USE_IMMERSIVE_DARK_MODE = 20;
	  int attribute = -1;
          if (g_buildNumber >= 17763) {
	    attribute = DWMA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;
	  }
	  if (g_buildNumber >= 18985) {
            attribute = DWMA_USE_IMMERSIVE_DARK_MODE;
	  }
	  if (attribute >= 0) {
	    BOOL dark = use_dark != 0;
            _DwmSetWindowAttribute(hWnd, attribute, &dark, sizeof(dark));
	  }
#endif

  // SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
  // | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

#if 0
	  // walk up the parent tree
	  if (!up || up == hWnd)
		  break;
	  hWnd = up;
  }
#endif

#if 0
  auto* top = ::GetTopWindow(hWnd);
  std::cerr << "hWnd[" <<  hWnd << "] top[" << top << ']' << std::endl;
  // hWnd = top;

  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << " theme_source[" << theme_source << ']' << std::endl;
  auto const use_dark = IsDarkPreferred(theme_source);
  std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << " use_dark[" << use_dark << ']' << std::endl;

  if (use_dark)
  {
    if (_AllowDarkModeForApp != nullptr)
      _AllowDarkModeForApp(use_dark);
    if (_AllowDarkModeForWindow != nullptr)
      _AllowDarkModeForWindow(hWnd, use_dark);

    SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
    if (FAILED(::SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr))) {
      std::cerr << __FILE__ << ':' << __LINE__ << " failed darkmode" << std::endl;
      ::SetWindowTheme(hWnd, L"Explorer", nullptr);
    }
    BOOL darkFlag = TRUE;
    std::cerr << __FILE__ << ':' << __LINE__ << ':' << __FUNCTION__ << ' ' << darkFlag << std::endl;
    auto data = WINDOWCOMPOSITIONATTRIBDATA {WCA_USEDARKMODECOLORS, &darkFlag, sizeof(darkFlag) };
    _SetWindowCompositionAttribute(hWnd, &data);
    _RefreshImmersiveColorPolicyState();
  }
  else
  {
    std::cerr << __FILE__ << ':' << __LINE__ << "  FIXME use_dark false" << std::endl;
  }

  //AllowDarkModeForWindow(hWnd, use_dark);
  // RefreshTitleBarThemeColor(hWnd, use_dark);
  //_RefreshImmersiveColorPolicyState();
  ::RedrawWindow(hWnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_INTERNALPAINT | RDW_ALLCHILDREN | RDW_UPDATENOW);
  SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);
#endif
}

}  // namespace win
}  // namespace electron