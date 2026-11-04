// What the app needs from the operating system. platform_win32.cpp is the Windows side; a Linux side comes with the Steam Deck build.
// All strings are UTF-8.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace plat {
using Env = std::vector<std::pair<std::string, std::string>>;

std::string exe_path();
std::string pick_file(const char *title, const char *filter_name, const char *filter_spec);
std::string pick_folder(const char *title);
std::string pick_save(const char *title, const char *default_name, const char *filter_name, const char *filter_spec);
void open_folder(const std::string &path);
void open_url(const char *url);
uint64_t free_bytes(const std::string &path);   // on the drive holding path

// A tool, its output line by line. Blocks until it ends. Returns its exit code, or -1 if it could not start.
int run_tool(const std::vector<std::string> &argv, const std::string &cwd, const Env &env, const std::function<void(const std::string &)> &on_line);
void kill_tools();   // ends every running tool and whatever it started

struct Game;
Game *start_game(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env, const std::string &log_path, std::string *error);
bool game_finished(Game *g, int *exit_code);   // frees g when it returns true

bool desktop_shortcut_exists();
bool desktop_shortcut_make(const std::string &target);
void desktop_shortcut_refresh(const std::string &target);   // follows a moved folder: only a shortcut already pointing at an exe of this name
bool set_exe_icon(const std::string &exe, const std::string &png_path);
bool can_add_to_steam();
bool add_to_steam(const std::string &target);

// Where the unpacked tools are, and what the tools' PATH should be
struct ToolPaths { std::string python, path, cpu; };   // cpu: the build's target, in this compiler's spelling
ToolPaths tool_paths(const std::string &work);
// The launcher in the portable folder: this program without its payload
bool install_launcher(const std::string &dest, uint64_t program_size);
void finish_file(const std::string &path, int mode);   // mode 1: a program
std::vector<std::string> font_paths();                 // regular, bold, and so on: the first pair that exists
bool has_file_dialogs();
// Become the game, with no window of ours: false if this platform doesn't, or it failed
bool replace_with_game(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const Env &env);

void *load_texture(const std::string &path, int *w, int *h);
void window_minimize();
void window_restore();
bool ask_yes_no(const char *title, const char *text);
}

// app.cpp
extern const char *const APP_TITLE;
int app_command_line(int argc, char **argv);   // >= 0: a command ran with no window, this is its exit code
void app_init();
void app_set_scale(float dpi_scale);
void app_frame();
bool app_animating();    // something on screen moves, so draw without waiting for input
bool app_can_close();    // asks first if work would be lost
