// Windows side of the app: the window, Direct3D 11, dialogs, processes, shortcuts. No DLLs beyond Windows' own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "platform.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static HWND g_hwnd;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain *g_swap;
static ID3D11RenderTargetView *g_rtv;
static HANDLE g_job;

static std::wstring W(const std::string &s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}
static std::string U(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
	if (n <= 1) return "";
	std::string s(n - 1, 0);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, 0, 0);
	return s;
}

// ---- dialogs
static std::string run_dialog(IFileDialog *d) {
	std::string out;
	if (SUCCEEDED(d->Show(g_hwnd))) {
		IShellItem *item = 0;
		if (SUCCEEDED(d->GetResult(&item))) {
			PWSTR p = 0;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U(p); CoTaskMemFree(p); }
			item->Release();
		}
	}
	d->Release();
	return out;
}
static void set_filter(IFileDialog *d, const char *name, const char *spec) {
	std::wstring n = W(name), s = W(spec), an = L"All files", as = L"*.*";
	COMDLG_FILTERSPEC f[2] = {{n.c_str(), s.c_str()}, {an.c_str(), as.c_str()}};
	d->SetFileTypes(2, f);
}

namespace plat {
std::string exe_path() {
	static wchar_t b[32768];
	GetModuleFileNameW(0, b, 32768);
	return U(b);
}
std::string pick_file(const char *title, const char *filter_name, const char *filter_spec) {
	IFileOpenDialog *d = 0;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return "";
	d->SetTitle(W(title).c_str());
	set_filter(d, filter_name, filter_spec);
	return run_dialog(d);
}
std::string pick_folder(const char *title) {
	IFileOpenDialog *d = 0;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return "";
	DWORD o = 0;
	d->GetOptions(&o);
	d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	d->SetTitle(W(title).c_str());
	return run_dialog(d);
}
std::string pick_save(const char *title, const char *default_name, const char *filter_name, const char *filter_spec) {
	IFileSaveDialog *d = 0;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return "";
	d->SetTitle(W(title).c_str());
	d->SetFileName(W(default_name).c_str());
	d->SetDefaultExtension(L"zip");
	set_filter(d, filter_name, filter_spec);
	return run_dialog(d);
}
void open_folder(const std::string &path) { ShellExecuteW(0, L"open", W(path).c_str(), 0, 0, SW_SHOWNORMAL); }
void open_url(const char *url) { ShellExecuteW(0, L"open", W(url).c_str(), 0, 0, SW_SHOWNORMAL); }
uint64_t free_bytes(const std::string &path) {
	fs::path p = fs::u8path(path);
	std::error_code ec;
	while (!fs::exists(p, ec) && p.has_parent_path() && p != p.parent_path()) p = p.parent_path();
	ULARGE_INTEGER f;
	return GetDiskFreeSpaceExW(p.c_str(), &f, 0, 0) ? f.QuadPart : 0;
}

// ---- processes
static std::wstring quote(const std::string &arg) {
	std::wstring a = W(arg), o = L"\"";
	size_t slashes = 0;
	for (wchar_t c : a) {
		if (c == L'\\') { slashes++; continue; }
		if (c == L'"') o.append(slashes * 2 + 1, L'\\');
		else o.append(slashes, L'\\');
		slashes = 0;
		o += c;
	}
	o.append(slashes * 2, L'\\');
	return o + L"\"";
}
struct NoCase { bool operator()(const std::wstring &a, const std::wstring &b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
static std::wstring env_block(const Env &add) {
	std::map<std::wstring, std::wstring, NoCase> m;
	wchar_t *cur = GetEnvironmentStringsW();
	for (wchar_t *e = cur; *e; e += wcslen(e) + 1) {
		const wchar_t *eq = wcschr(e + 1, L'=');   // (names like "=C:" start with the sign)
		if (eq) m[std::wstring(e, eq - e)] = eq + 1;
	}
	FreeEnvironmentStringsW(cur);
	for (auto &kv : add) m[W(kv.first)] = W(kv.second);
	std::wstring b;
	for (auto &kv : m) { b += kv.first + L"=" + kv.second; b.push_back(0); }
	b.push_back(0);
	return b;
}
int run_tool(const std::vector<std::string> &argv, const std::string &cwd, const Env &env, const std::function<void(const std::string &)> &on_line) {
	if (!g_job) {
		g_job = CreateJobObjectW(0, 0);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // the app closing ends the tools
		SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}
	SECURITY_ATTRIBUTES sa = {sizeof sa, 0, TRUE};
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
	std::wstring cmd, blk = env_block(env), dir = W(cwd);
	for (auto &a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote(a); }
	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessW(0, &cmd[0], 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, &blk[0], dir.c_str(), &si, &pi);
	CloseHandle(wr); CloseHandle(nul);
	if (!ok) { CloseHandle(rd); return -1; }
	AssignProcessToJobObject(g_job, pi.hProcess);
	ResumeThread(pi.hThread);
	std::string line;
	char buf[4096];
	DWORD n;
	while (ReadFile(rd, buf, sizeof buf, &n, 0) && n) {
		for (DWORD i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') { if (!line.empty()) on_line(line); line.clear(); }
			else line += buf[i];
		}
	}
	if (!line.empty()) on_line(line);
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	return (int)code;
}
void kill_tools() { if (g_job) TerminateJobObject(g_job, 1); }

struct Game { HANDLE process; };
Game *start_game(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env, const std::string &log_path, std::string *error) {
	SECURITY_ATTRIBUTES sa = {sizeof sa, 0, TRUE};
	HANDLE log = CreateFileW(W(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, 0);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul; si.hStdOutput = log; si.hStdError = log;
	std::wstring cmd = quote(exe), blk = env_block(env), dir = W(cwd), app = W(exe);
	for (auto &a : args) { cmd += L' '; cmd += quote(a); }
	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessW(app.c_str(), &cmd[0], 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, &blk[0], dir.c_str(), &si, &pi);
	DWORD err = GetLastError();
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	CloseHandle(nul);
	if (!ok) {
		if (err == 4551) *error = "Windows blocked the game: Smart App Control refused this build. Turn it off in Windows Security > App & browser control, or reinstall to build a new copy.";
		else if (err == 1260) *error = "Windows blocked the game: a policy on this PC refuses to run it.";
		else *error = "The game could not start (Windows error " + std::to_string(err) + ").";
		return 0;
	}
	CloseHandle(pi.hThread);
	return new Game{pi.hProcess};
}
bool game_finished(Game *g, int *exit_code) {
	if (WaitForSingleObject(g->process, 0) != WAIT_OBJECT_0) return false;
	DWORD c = 0;
	GetExitCodeProcess(g->process, &c);
	*exit_code = (int)c;
	CloseHandle(g->process);
	delete g;
	return true;
}

// ---- shortcut
static std::wstring link_path() {
	PWSTR d = 0;
	std::wstring out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, 0, &d))) { out = std::wstring(d) + L"\\Silent Hill Book of Memories.lnk"; CoTaskMemFree(d); }
	return out;
}
bool desktop_shortcut_exists() { std::error_code ec; return fs::exists(link_path(), ec); }
bool desktop_shortcut_make(const std::string &target) {
	IShellLinkW *l = 0;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) return false;
	fs::path t = fs::u8path(target);
	l->SetPath(t.c_str());
	l->SetWorkingDirectory(t.parent_path().c_str());
	l->SetIconLocation(t.c_str(), 0);
	IPersistFile *f = 0;
	bool ok = false;
	if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f)))) { ok = SUCCEEDED(f->Save(link_path().c_str(), TRUE)); f->Release(); }
	l->Release();
	return ok;
}
void desktop_shortcut_refresh(const std::string &target) {
	if (!desktop_shortcut_exists()) return;
	IShellLinkW *l = 0;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) return;
	IPersistFile *f = 0;
	static wchar_t cur[32768];
	cur[0] = 0;
	if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f)))) {
		if (SUCCEEDED(f->Load(link_path().c_str(), STGM_READ))) l->GetPath(cur, 32768, 0, 0);
		f->Release();
	}
	l->Release();
	fs::path now = fs::u8path(target), old = cur;
	if (old.filename() == now.filename() && old != now) desktop_shortcut_make(target);
}

// ---- the player's own icon, written into their copy of the launcher: game art never ships in a release
bool can_add_to_steam() { return false; }
bool add_to_steam(const std::string &) { return false; }
ToolPaths tool_paths(const std::string &work) {
	fs::path w = fs::u8path(work);
	const char *root = getenv("SystemRoot");
	std::string sys = root ? root : "C:\\Windows";
	return {(w / "python" / "python.exe").u8string(), (w / "python").u8string() + ";" + sys + "\\System32;" + sys, "x86-64-v2"};
}
bool install_launcher(const std::string &dest, uint64_t program_size) {
	std::ifstream in(fs::u8path(exe_path()), std::ios::binary);
	std::ofstream out(fs::u8path(dest), std::ios::binary);
	std::vector<char> buf(1 << 20);
	for (uint64_t left = program_size; left && in;) {
		size_t n = (size_t)std::min<uint64_t>(left, buf.size());
		in.read(buf.data(), n);
		out.write(buf.data(), n);
		left -= n;
	}
	return in && out;
}
void finish_file(const std::string &, int) {}
std::vector<std::string> font_paths() { return {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\seguisb.ttf"}; }
bool has_file_dialogs() { return true; }
bool replace_with_game(const std::string &, const std::vector<std::string> &, const std::string &, const Env &) { return false; }
bool set_exe_icon(const std::string &exe, const std::string &png_path) {
	std::ifstream in(fs::u8path(png_path), std::ios::binary);
	std::vector<char> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (png.size() < 24 || memcmp(png.data() + 1, "PNG", 3)) return false;
	unsigned w = (uint8_t)png[18] << 8 | (uint8_t)png[19], h = (uint8_t)png[22] << 8 | (uint8_t)png[23];
#pragma pack(push, 2)
	struct { WORD reserved, type, count; BYTE width, height, colors, zero; WORD planes, bits; DWORD size; WORD id; } group = {0, 1, 1, (BYTE)(w >= 256 ? 0 : w), (BYTE)(h >= 256 ? 0 : h), 0, 0, 1, 32, (DWORD)png.size(), 1};
#pragma pack(pop)
	HANDLE u = BeginUpdateResourceW(W(exe).c_str(), FALSE);
	if (!u) return false;
	bool ok = UpdateResourceW(u, RT_ICON, MAKEINTRESOURCEW(1), 0, png.data(), (DWORD)png.size())
	       && UpdateResourceW(u, RT_GROUP_ICON, MAKEINTRESOURCEW(1), 0, &group, sizeof group);
	return EndUpdateResourceW(u, !ok) && ok;
}

// ---- pictures
void *load_texture(const std::string &path, int *w, int *h) {
	std::ifstream in(fs::u8path(path), std::ios::binary);
	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.empty() || !g_dev) return 0;
	unsigned char *px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), w, h, 0, 4);
	if (!px) return 0;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = *w; d.Height = *h; d.MipLevels = 1; d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA s = {px, (UINT)(*w * 4), 0};
	ID3D11Texture2D *t = 0;
	ID3D11ShaderResourceView *v = 0;
	if (SUCCEEDED(g_dev->CreateTexture2D(&d, &s, &t))) { g_dev->CreateShaderResourceView(t, 0, &v); t->Release(); }
	stbi_image_free(px);
	return v;
}
void window_minimize() { ShowWindow(g_hwnd, SW_MINIMIZE); }
void window_restore() { ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); }
bool ask_yes_no(const char *title, const char *text) { return MessageBoxW(g_hwnd, W(text).c_str(), W(title).c_str(), MB_YESNO | MB_ICONWARNING) == IDYES; }
}   // namespace plat

// ---- the window
static void make_target() {
	ID3D11Texture2D *b = 0;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&b));
	g_dev->CreateRenderTargetView(b, 0, &g_rtv);
	b->Release();
}
static bool make_device(HWND h) {
	DXGI_SWAP_CHAIN_DESC d = {};
	d.BufferCount = 2;
	d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	d.OutputWindow = h;
	d.SampleDesc.Count = 1;
	d.Windowed = TRUE;
	d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
		if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(0, type, 0, 0, 0, 0, D3D11_SDK_VERSION, &d, &g_swap, &g_dev, 0, &g_ctx))) { make_target(); return true; }
	return false;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
	switch (m) {
	case WM_SIZE:
		if (g_swap && w != SIZE_MINIMIZED) {
			if (g_rtv) { g_rtv->Release(); g_rtv = 0; }
			g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
			make_target();
		}
		return 0;
	case WM_GETMINMAXINFO: {
		float s = GetDpiForWindow(h) / 96.0f;
		((MINMAXINFO *)l)->ptMinTrackSize = {(LONG)(720 * s), (LONG)(560 * s)};
		return 0;
	}
	case WM_DPICHANGED: {
		RECT *r = (RECT *)l;
		SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		app_set_scale(HIWORD(w) / 96.0f);
		return 0;
	}
	case WM_CLOSE:
		if (!app_can_close()) return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
	CoInitializeEx(0, COINIT_APARTMENTTHREADED);
	int wargc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	std::vector<std::string> args;
	std::vector<char *> argp;
	for (int i = 0; i < wargc; i++) args.push_back(U(wargv[i]));
	for (auto &a : args) argp.push_back(&a[0]);
	int rc = app_command_line(wargc, argp.data());
	if (rc >= 0) return rc;

	std::wstring title = W(APP_TITLE);
	WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, wndproc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursor(0, IDC_ARROW), 0, 0, L"SHBoMApp", 0};
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);
	float scale = GetDpiForSystem() / 96.0f;
	int ww = (int)(900 * scale), wh = (int)(780 * scale);
	int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
	if (wh > sy - 80) wh = sy - 80;
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, (sx - ww) / 2, (sy - wh) / 2, ww, wh, 0, 0, inst, 0);
	scale = GetDpiForWindow(g_hwnd) / 96.0f;
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
	COLORREF caption = RGB(18, 18, 20);
	DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR, Windows 11 */, &caption, sizeof caption);
	if (!make_device(g_hwnd)) {
		MessageBoxW(0, L"Could not start Direct3D 11 on this PC.", title.c_str(), MB_ICONERROR);
		return 1;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = 0;
	ImGui::GetIO().LogFilename = 0;
	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_dev, g_ctx);
	app_init();
	app_set_scale(scale);
	ShowWindow(g_hwnd, show);
	UpdateWindow(g_hwnd);

	for (bool running = true; running;) {
		// (idle: wait for input, so the launcher costs nothing while the game runs beside it)
		if (!app_animating()) MsgWaitForMultipleObjects(0, 0, FALSE, 100, QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) running = false;
		}
		if (!running) break;
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		app_frame();
		ImGui::Render();
		const float clear[4] = {0.07f, 0.07f, 0.08f, 1};
		g_ctx->OMSetRenderTargets(1, &g_rtv, 0);
		g_ctx->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swap->Present(IsIconic(g_hwnd) ? 0 : 1, 0);
	}
	plat::kill_tools();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
