// Linux and Steam Deck side of the app: an SDL3 window, SDL's renderer, native file dialogs through kdialog or zenity, processes.
// SDL3 is the game's own copy: beside the launcher (stub_linux.c puts it there) or in Game/, found through this program's run path.
#include <SDL3/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "platform.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;
extern char **environ;

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static std::mutex g_tools_m;
static std::set<pid_t> g_tools;

static std::string shell_quote(const std::string &s) {
	std::string o = "'";
	for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
	return o + "'";
}
static bool have(const char *tool) {
	return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0;
}
static std::string capture(const std::string &cmd) {
	// (Steam's own libraries must not reach a dialog program)
	std::string full = "env -u LD_LIBRARY_PATH -u LD_PRELOAD " + cmd + " 2>/dev/null";
	FILE *p = popen(full.c_str(), "r");
	if (!p) return "";
	std::string out;
	char b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof b, p)) > 0) out.append(b, n);
	pclose(p);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
	return out;
}
static std::string home() { const char *h = getenv("HOME"); return h ? h : "."; }
static std::string pattern_of(const char *spec) {   // "*.PUP" also matches "*.pup" here, where names have case
	std::string s = spec, lower = s;
	for (auto &c : lower) c = (char)tolower(c);
	return lower == s ? s : s + " " + lower;
}

namespace plat {
std::string exe_path() {
	// (the file the player runs: stub_linux.c says which, since this program itself was unpacked from it)
	if (const char *self = getenv("SHBOM_SELF")) return self;
	std::error_code ec;
	return fs::read_symlink("/proc/self/exe", ec).u8string();
}
std::string pick_file(const char *title, const char *filter_name, const char *filter_spec) {
	std::string pat = pattern_of(filter_spec);
	if (have("kdialog")) return capture("kdialog --title " + shell_quote(title) + " --getopenfilename " + shell_quote(home()) + " " + shell_quote(std::string(filter_name) + " (" + pat + ")"));
	if (have("zenity")) return capture("zenity --file-selection --title=" + shell_quote(title) + " --file-filter=" + shell_quote(std::string(filter_name) + " | " + pat));
	return "";
}
std::string pick_folder(const char *title) {
	if (have("kdialog")) return capture("kdialog --title " + shell_quote(title) + " --getexistingdirectory " + shell_quote(home()));
	if (have("zenity")) return capture("zenity --file-selection --directory --title=" + shell_quote(title));
	return "";
}
std::string pick_save(const char *title, const char *default_name, const char *filter_name, const char *filter_spec) {
	std::string start = home() + "/" + default_name, pat = pattern_of(filter_spec);
	if (have("kdialog")) return capture("kdialog --title " + shell_quote(title) + " --getsavefilename " + shell_quote(start) + " " + shell_quote(std::string(filter_name) + " (" + pat + ")"));
	if (have("zenity")) return capture("zenity --file-selection --save --confirm-overwrite --title=" + shell_quote(title) + " --filename=" + shell_quote(start));
	return "";
}
bool has_file_dialogs() { return have("kdialog") || have("zenity"); }
void open_folder(const std::string &path) { std::system(("env -u LD_LIBRARY_PATH -u LD_PRELOAD xdg-open " + shell_quote(path) + " >/dev/null 2>&1 &").c_str()); }
void open_url(const char *url) { open_folder(url); }
uint64_t free_bytes(const std::string &path) {
	fs::path p = fs::u8path(path);
	std::error_code ec;
	while (!fs::exists(p, ec) && p.has_parent_path() && p != p.parent_path()) p = p.parent_path();
	struct statvfs s;
	return statvfs(p.c_str(), &s) == 0 ? (uint64_t)s.f_bavail * s.f_frsize : 0;
}

// ---- processes
static void apply_env(const Env &env) { for (auto &kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1); }
int run_tool(const std::vector<std::string> &argv, const std::string &cwd, const Env &env, const std::function<void(const std::string &)> &on_line) {
	int fds[2];
	if (pipe(fds)) return -1;
	std::vector<char *> args;
	for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(0);
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
	if (pid == 0) {
		setpgid(0, 0);   // (a group of its own: kill_tools ends what it started too)
		nice(10);
		dup2(fds[1], 1); dup2(fds[1], 2);
		close(fds[0]); close(fds[1]);
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) dup2(nul, 0);
		if (chdir(cwd.c_str())) _exit(127);
		unsetenv("LD_PRELOAD");
		apply_env(env);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(fds[1]);
	setpgid(pid, pid);
	{ std::lock_guard<std::mutex> g(g_tools_m); g_tools.insert(pid); }
	std::string line;
	char buf[4096];
	ssize_t n;
	while ((n = read(fds[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') { if (!line.empty()) on_line(line); line.clear(); }
			else line += buf[i];
		}
	}
	if (!line.empty()) on_line(line);
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	{ std::lock_guard<std::mutex> g(g_tools_m); g_tools.erase(pid); }
	if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
	return 1;
}
void kill_tools() {
	std::lock_guard<std::mutex> g(g_tools_m);
	for (pid_t p : g_tools) kill(-p, SIGKILL);
}
ToolPaths tool_paths(const std::string &work) {
	fs::path w = fs::u8path(work);
	ToolPaths t;
	t.python = (w / "python" / "bin" / "python3").u8string();
	t.path = (w / "python" / "bin").u8string() + ":/usr/bin:/bin";
	t.cpu = "x86_64_v2";   // (zig's spelling)
	return t;
}

struct Game { pid_t pid; };
static void exec_game_now(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env) {
	std::vector<char *> a;
	a.push_back(const_cast<char *>(exe.c_str()));
	for (auto &x : args) a.push_back(const_cast<char *>(x.c_str()));
	a.push_back(0);
	if (chdir(cwd.c_str())) return;
	apply_env(env);
	setenv("LD_LIBRARY_PATH", ".", 1);   // (the game's SDL over anything Steam's environment points at)
	execv(exe.c_str(), a.data());
}
Game *start_game(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env, const std::string &log_path, std::string *error) {
	if (access(exe.c_str(), X_OK)) { *error = "The game could not start: " + exe + " is missing or not executable."; return 0; }
	pid_t pid = fork();
	if (pid < 0) { *error = "The game could not start."; return 0; }
	if (pid == 0) {
		int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (log >= 0) { dup2(log, 1); dup2(log, 2); }
		exec_game_now(exe, args, cwd, env);
		_exit(127);
	}
	return new Game{pid};
}
bool game_finished(Game *g, int *exit_code) {
	int status = 0;
	if (waitpid(g->pid, &status, WNOHANG) == 0) return false;
	*exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	delete g;
	return true;
}
bool replace_with_game(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env) {
	std::string log = cwd + "/session.log", prev = cwd + "/session.prev.log";
	rename(log.c_str(), prev.c_str());
	int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
	exec_game_now(exe, args, cwd, env);
	return false;
}

// ---- the launcher file: this very program, which was unpacked beside its SDL
bool install_launcher(const std::string &dest, uint64_t) {
	std::error_code ec;
	std::ifstream in("/proc/self/exe", std::ios::binary);
	std::ofstream out(fs::u8path(dest), std::ios::binary | std::ios::trunc);
	out << in.rdbuf();
	out.close();
	if (!in || !out) return false;
	return chmod(dest.c_str(), 0755) == 0;
}
void finish_file(const std::string &path, int mode) { if (mode == 1) chmod(path.c_str(), 0755); }

// ---- shortcut: an application entry and a desktop file
static std::string entry_text(const std::string &target) {
	fs::path t = fs::u8path(target);
	std::string icon = (t.parent_path() / "Game" / "fs" / "app0" / "sce_sys" / "icon0.png").u8string();
	return "[Desktop Entry]\nType=Application\nName=Silent Hill: Book of Memories\nExec=\"" + target + "\"\nPath=" + t.parent_path().u8string() + "\nIcon=" + icon + "\nTerminal=false\nCategories=Game;\n";
}
static std::string apps_entry() { return home() + "/.local/share/applications/silent-hill-book-of-memories.desktop"; }
static std::string desk_entry() { return home() + "/Desktop/Silent Hill Book of Memories.desktop"; }
bool desktop_shortcut_exists() { std::error_code ec; return fs::exists(fs::u8path(apps_entry()), ec); }
bool desktop_shortcut_make(const std::string &target) {
	std::error_code ec;
	fs::create_directories(fs::u8path(apps_entry()).parent_path(), ec);
	std::string text = entry_text(target);
	{ std::ofstream f(fs::u8path(apps_entry())); f << text; if (!f) return false; }
	if (fs::is_directory(fs::u8path(home() + "/Desktop"), ec)) {
		std::ofstream f(fs::u8path(desk_entry()));
		f << text;
		f.close();
		chmod(desk_entry().c_str(), 0755);
	}
	return true;
}
void desktop_shortcut_refresh(const std::string &target) { if (desktop_shortcut_exists()) desktop_shortcut_make(target); }
bool set_exe_icon(const std::string &, const std::string &) { return true; }   // (Linux files have no icon of their own: the shortcut names the picture)
bool can_add_to_steam() { return have("steamos-add-to-steam") || have("steam"); }
bool add_to_steam(const std::string &target) {
	std::string prefix = "env -u LD_LIBRARY_PATH -u LD_PRELOAD ";
	if (have("steamos-add-to-steam")) return std::system((prefix + "steamos-add-to-steam " + shell_quote(target) + " >/dev/null 2>&1").c_str()) == 0;
	return std::system((prefix + "steam " + shell_quote("steam://addnonsteamgame/" + target) + " >/dev/null 2>&1 &").c_str()) == 0;
}
std::vector<std::string> font_paths() {
	return {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
	        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
	        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
}

// ---- pictures
void *load_texture(const std::string &path, int *w, int *h) {
	std::ifstream in(fs::u8path(path), std::ios::binary);
	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.empty() || !g_ren) return 0;
	unsigned char *px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), w, h, 0, 4);
	if (!px) return 0;
	SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, *w, *h);
	if (t) { SDL_UpdateTexture(t, 0, px, *w * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
	stbi_image_free(px);
	return t;
}
// (hidden, not minimized: under the Deck's Gaming Mode a minimized window would stay in the way of the game)
void window_minimize() { SDL_HideWindow(g_win); }
void window_restore() { SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win); }
bool ask_yes_no(const char *title, const char *text) {
	SDL_MessageBoxButtonData b[2] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"}};
	SDL_MessageBoxData d = {SDL_MESSAGEBOX_WARNING, g_win, title, text, 2, b, 0};
	int hit = 0;
	return SDL_ShowMessageBox(&d, &hit) && hit == 1;
}
}   // namespace plat

int main(int argc, char **argv) {
	// (stub_linux.c unpacked this program and its SDL for this run; loaded now, they can go)
	if (const char *tmp = getenv("SHBOM_TMP")) {
		std::error_code ec;
		fs::remove_all(tmp, ec);
		unsetenv("SHBOM_TMP");
	}
	unsetenv("LD_LIBRARY_PATH");
	signal(SIGPIPE, SIG_IGN);
	int rc = app_command_line(argc, argv);
	if (rc >= 0) return rc;

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		fprintf(stderr, "Could not open a window: %s\n", SDL_GetError());
		return 1;
	}
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	float scale = SDL_GetDisplayContentScale(display);
	if (scale <= 0) scale = 1;
	const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(display);
	int ww = (int)(900 * scale), wh = (int)(780 * scale);
	if (dm && wh > dm->h - 40) wh = dm->h - 40;
	if (dm && ww > dm->w) ww = dm->w;
	g_win = SDL_CreateWindow(APP_TITLE, ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!g_win) { fprintf(stderr, "Could not open a window: %s\n", SDL_GetError()); return 1; }
	g_ren = SDL_CreateRenderer(g_win, 0);
	if (!g_ren) { fprintf(stderr, "Could not start drawing: %s\n", SDL_GetError()); return 1; }
	SDL_SetRenderVSync(g_ren, 1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = 0;
	ImGui::GetIO().LogFilename = 0;
	ImGui_ImplSDL3_InitForSDLRenderer(g_win, g_ren);
	ImGui_ImplSDLRenderer3_Init(g_ren);
	app_init();
	app_set_scale(scale);

	for (bool running = true; running;) {
		SDL_Event e;
		bool got = app_animating() ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 100);
		while (got) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { if (app_can_close()) running = false; }
			if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) app_set_scale(SDL_GetWindowDisplayScale(g_win));
			got = SDL_PollEvent(&e);
		}
		if (!running) break;
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		app_frame();
		ImGui::Render();
		ImGuiIO &io = ImGui::GetIO();
		SDL_SetRenderScale(g_ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
		SDL_SetRenderDrawColor(g_ren, 18, 18, 20, 255);
		SDL_RenderClear(g_ren);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_ren);
		// (SHBOM_SCREENSHOT=<file.bmp>: the window after a moment, then quit. Remote desktops can't always capture it from outside)
		static int frames;
		if (getenv("SHBOM_SCREENSHOT") && ++frames == 40) {
			if (SDL_Surface *shot = SDL_RenderReadPixels(g_ren, 0)) { SDL_SaveBMP(shot, getenv("SHBOM_SCREENSHOT")); SDL_DestroySurface(shot); }
			running = false;
		}
		SDL_RenderPresent(g_ren);
	}
	plat::kill_tools();
	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(g_ren);
	SDL_DestroyWindow(g_win);
	SDL_Quit();
	return 0;
}
