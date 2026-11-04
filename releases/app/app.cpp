// Silent Hill: Book of Memories for PC: the setup program and launcher, in one exe.
//
// The exe a player downloads carries the build tools and this port's source as a payload after the program (see releases/build_app.py).
// Setup unpacks them, builds the game from the player's own firmware and game files, and leaves a portable folder:
//     <folder>/Silent Hill Book of Memories.exe     this same program without the payload
//     <folder>/Game                                 the built game
//     <folder>/Saves                                saves only, so this folder alone can be synced
//     <folder>/Save Backups                         what Import replaced
// Nothing here is Windows-specific but platform_win32.cpp.
#include <algorithm>
#include <cfloat>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include <zlib.h>
#include "imgui.h"
#include "platform.h"

namespace fs = std::filesystem;
extern const char *const APP_TITLE = "Silent Hill: Book of Memories";
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
static const char *const TITLE_ID = "PCSE00011";
static const char *const LAUNCHER_NAME = "Silent Hill Book of Memories.exe";
static const char *const DEFAULT_FOLDER = "Silent Hill Book of Memories";
static const char *const REPO_URL = "https://github.com/aphixe/SilentHill-BookOfMemories-PC";
static const uint64_t NEEDED_BYTES = 5ull << 30;

static fs::path P(const std::string &s) { return fs::u8path(s); }
static std::string S(const fs::path &p) { return p.u8string(); }

// ---- payload: [program][deflated files][deflated index][footer]
struct Footer { char magic[8]; uint64_t data_off, index_off, index_csize, index_usize; };
struct Entry { std::string path; uint64_t off, csize, usize; };
static const char PAYLOAD_MAGIC[8] = {'S', 'H', 'B', 'M', 'P', 'A', 'K', '1'};

static bool inflate_raw(const void *in, size_t in_n, void *out, size_t out_n) {
	z_stream z = {};
	if (inflateInit2(&z, -15) != Z_OK) return false;
	z.next_in = (Bytef *)in; z.avail_in = (uInt)in_n;
	z.next_out = (Bytef *)out; z.avail_out = (uInt)out_n;
	int r = inflate(&z, Z_FINISH);
	bool ok = r == Z_STREAM_END && z.total_out == out_n;
	inflateEnd(&z);
	return ok;
}
static bool read_footer(const std::string &exe, Footer *f) {
	std::ifstream in(P(exe), std::ios::binary);
	in.seekg(-(std::streamoff)sizeof *f, std::ios::end);
	in.read((char *)f, sizeof *f);
	return in && !memcmp(f->magic, PAYLOAD_MAGIC, 8);
}
static bool read_index(const std::string &exe, const Footer &f, std::vector<Entry> *out) {
	std::ifstream in(P(exe), std::ios::binary);
	std::vector<char> c(f.index_csize), u(f.index_usize);
	in.seekg((std::streamoff)f.index_off);
	in.read(c.data(), c.size());
	if (!in || !inflate_raw(c.data(), c.size(), u.data(), u.size())) return false;
	for (size_t i = 0; i + 2 <= u.size();) {
		uint16_t n; memcpy(&n, &u[i], 2); i += 2;
		Entry e;
		e.path.assign(&u[i], n); i += n;
		memcpy(&e.off, &u[i], 8); memcpy(&e.csize, &u[i + 8], 8); memcpy(&e.usize, &u[i + 16], 8); i += 24;
		out->push_back(std::move(e));
	}
	return true;
}

// ---- zip, for saves
static bool read_all(const fs::path &p, std::vector<unsigned char> *out) {
	std::ifstream in(p, std::ios::binary);
	if (!in) return false;
	out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}
static void put16(std::vector<unsigned char> &v, unsigned x) { v.push_back(x & 255); v.push_back(x >> 8 & 255); }
static void put32(std::vector<unsigned char> &v, uint32_t x) { put16(v, x & 0xffff); put16(v, x >> 16); }
static uint32_t get16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static bool is_sync_or_note(const std::string &name) {   // what is the sync tool's or ours, never part of a save
	return name.rfind(".st", 0) == 0 || name.rfind(".syncthing.", 0) == 0 || name.find(".sync-conflict-") != std::string::npos || name == "ABOUT SAVES.txt";
}
static std::vector<fs::path> save_files(const fs::path &dir) {
	std::vector<fs::path> out;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		std::string name = S(it->path().filename());
		if (is_sync_or_note(name)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
		if (it->is_regular_file(ec)) out.push_back(it->path());
	}
	return out;
}
static bool zip_folder(const fs::path &dir, const fs::path &zip_path, std::string *err) {
	std::vector<unsigned char> body, central;
	unsigned count = 0;
	for (auto &file : save_files(dir)) {
		std::vector<unsigned char> raw;
		if (!read_all(file, &raw)) { *err = "Could not read " + S(file); return false; }
		std::string name = S(fs::relative(file, dir));
		std::replace(name.begin(), name.end(), '\\', '/');
		uLong crc = crc32(0, raw.data(), (uInt)raw.size());
		std::vector<unsigned char> c(compressBound((uLong)raw.size()) + 64);
		z_stream z = {};
		deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
		z.next_in = raw.data(); z.avail_in = (uInt)raw.size();
		z.next_out = c.data(); z.avail_out = (uInt)c.size();
		deflate(&z, Z_FINISH);
		uint32_t csize = (uint32_t)z.total_out;
		deflateEnd(&z);
		uint32_t off = (uint32_t)body.size();
		std::error_code ec;
		time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		tm *lt = localtime(&t);
		unsigned dtime = lt->tm_hour << 11 | lt->tm_min << 5 | lt->tm_sec / 2, ddate = (lt->tm_year - 80) << 9 | (lt->tm_mon + 1) << 5 | lt->tm_mday;
		auto common = [&](std::vector<unsigned char> &v) {
			put16(v, 20); put16(v, 0x0800); put16(v, 8); put16(v, dtime); put16(v, ddate);
			put32(v, (uint32_t)crc); put32(v, csize); put32(v, (uint32_t)raw.size()); put16(v, (unsigned)name.size()); put16(v, 0);
		};
		put32(body, 0x04034b50); common(body);
		body.insert(body.end(), name.begin(), name.end());
		body.insert(body.end(), c.begin(), c.begin() + csize);
		put32(central, 0x02014b50); put16(central, 20); common(central);
		put16(central, 0); put16(central, 0); put16(central, 0); put32(central, 0); put32(central, off);
		central.insert(central.end(), name.begin(), name.end());
		count++;
	}
	if (!count) { *err = "There are no saves to export yet."; return false; }
	std::vector<unsigned char> end;
	put32(end, 0x06054b50); put16(end, 0); put16(end, 0); put16(end, count); put16(end, count);
	put32(end, (uint32_t)central.size()); put32(end, (uint32_t)body.size()); put16(end, 0);
	std::ofstream out(zip_path, std::ios::binary);
	out.write((char *)body.data(), body.size());
	out.write((char *)central.data(), central.size());
	out.write((char *)end.data(), end.size());
	if (!out) { *err = "Could not write " + S(zip_path); return false; }
	return true;
}
// a name from a zip, only if it stays inside the folder
static bool safe_name(const std::string &n) {
	if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
	std::string part;
	for (size_t i = 0; i <= n.size(); i++) {
		if (i == n.size() || n[i] == '/' || n[i] == '\\') { if (part == "..") return false; part.clear(); }
		else part += n[i];
	}
	return true;
}
static bool unzip_to(const fs::path &zip_path, const fs::path &dir, std::string *err) {
	std::vector<unsigned char> z;
	if (!read_all(zip_path, &z) || z.size() < 22) { *err = "That file could not be read as a zip."; return false; }
	size_t e = z.size() - 22;
	while (e > 0 && get32(&z[e]) != 0x06054b50) e--;
	if (get32(&z[e]) != 0x06054b50) { *err = "That file could not be read as a zip."; return false; }
	unsigned count = get16(&z[e + 10]);
	size_t p = get32(&z[e + 16]);
	for (unsigned i = 0; i < count; i++) {
		if (p + 46 > z.size() || get32(&z[p]) != 0x02014b50) { *err = "That zip is damaged."; return false; }
		unsigned method = get16(&z[p + 10]), nlen = get16(&z[p + 28]), xlen = get16(&z[p + 30]), clen = get16(&z[p + 32]);
		uint32_t csize = get32(&z[p + 20]), usize = get32(&z[p + 24]), crc = get32(&z[p + 16]), lo = get32(&z[p + 42]);
		if (p + 46 + nlen > z.size()) { *err = "That zip is damaged."; return false; }
		std::string name((char *)&z[p + 46], nlen);
		p += 46 + nlen + xlen + clen;
		if (!name.empty() && (name.back() == '/' || name.back() == '\\')) continue;
		if (!safe_name(name)) { *err = "That zip holds an unsafe file name, so nothing was imported."; return false; }
		if (usize > (64u << 20) || lo + 30 > z.size()) { *err = "That zip is too large to be a save."; return false; }
		size_t data = (size_t)lo + 30 + get16(&z[lo + 26]) + get16(&z[lo + 28]);
		if (data + csize > z.size() || (method != 0 && method != 8)) { *err = "That zip is damaged or uses an unknown format."; return false; }
		std::vector<unsigned char> out(usize);
		if (method == 0) { if (csize != usize) { *err = "That zip is damaged."; return false; } if (usize) memcpy(out.data(), &z[data], usize); }
		else if (usize && !inflate_raw(&z[data], csize, out.data(), usize)) { *err = "That zip is damaged."; return false; }
		if (crc32(0, out.data(), (uInt)usize) != crc) { *err = "That zip is damaged."; return false; }
		fs::path target = dir / P(name);
		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		std::ofstream f(target, std::ios::binary);
		f.write((char *)out.data(), out.size());
		if (!f) { *err = "Could not write " + S(target); return false; }
	}
	return true;
}
static std::string stamp(const char *fmt) {
	time_t t = time(0);
	char b[64];
	strftime(b, sizeof b, fmt, localtime(&t));
	return b;
}
// is there a save in here? data0000.bin and .slot0 are what the game writes
static bool looks_like_saves(const fs::path &dir) {
	for (auto &f : save_files(dir)) {
		std::string n = S(f.filename());
		if (n.rfind("data", 0) == 0 || n.rfind(".slot", 0) == 0) return true;
	}
	return false;
}
// saves: <root>/Saves. source: a zip or a folder. The old saves go to Save Backups first, and the sync tool's own files stay.
static bool import_saves(const fs::path &root, const fs::path &source, std::string *msg) {
	std::error_code ec;
	fs::path saves = root / "Saves", tmp = root / "Saves.import";
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	bool ok = true;
	if (fs::is_directory(source, ec)) {
		// (a folder may be a Saves folder, or an older install's savedata0 under fs: take either)
		fs::path from = source;
		if (!looks_like_saves(from) && fs::is_directory(from / "savedata0", ec)) from = from / "savedata0";
		for (auto &f : save_files(from)) {
			fs::path to = tmp / fs::relative(f, from);
			fs::create_directories(to.parent_path(), ec);
			fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec);
			if (ec) { *msg = "Could not copy " + S(f); ok = false; break; }
		}
	} else ok = unzip_to(source, tmp, msg);
	if (ok && !looks_like_saves(tmp)) { *msg = "No saves were found there, so nothing changed."; ok = false; }
	if (!ok) { fs::remove_all(tmp, ec); return false; }

	auto old = save_files(saves);
	if (!old.empty()) {
		fs::path backup = root / "Save Backups" / stamp("%Y-%m-%d %H-%M-%S");
		for (auto &f : old) {
			fs::path to = backup / fs::relative(f, saves);
			fs::create_directories(to.parent_path(), ec);
			fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec);
			if (ec) { *msg = "Could not back up your current saves, so nothing changed."; fs::remove_all(tmp, ec); return false; }
		}
		for (auto &f : old) fs::remove(f, ec);
	}
	fs::create_directories(saves, ec);
	for (auto &f : save_files(tmp)) {
		fs::path to = saves / fs::relative(f, tmp);
		fs::create_directories(to.parent_path(), ec);
		fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec);
		if (ec) { *msg = "Could not write " + S(to) + ". Your old saves are in Save Backups."; fs::remove_all(tmp, ec); return false; }
	}
	fs::remove_all(tmp, ec);
	*msg = old.empty() ? "Saves imported." : "Saves imported. The old ones are in Save Backups.";
	return true;
}

// ---- what the player points at
struct Check { bool ok; std::string text; };
static Check check_firmware(const std::string &s) {
	if (s.empty()) return {false, "Choose Sony's firmware update file, or a Vita3K folder with the firmware installed."};
	std::error_code ec;
	fs::path p = P(s);
	if (fs::is_regular_file(p, ec)) {
		std::ifstream in(p, std::ios::binary);
		char magic[5] = {};
		in.read(magic, 5);
		if (memcmp(magic, "SCEUF", 5)) return {false, "That file is not a Vita firmware update file (PUP)."};
		return {true, "Sony firmware update file"};
	}
	if (fs::is_directory(p / "os0" / "us", ec) && fs::is_directory(p / "vs0" / "sys" / "external", ec)) return {true, "Vita3K folder with firmware"};
	return {false, "That folder has no os0 and vs0 folders. In Vita3K, install the firmware first, then choose the folder holding them."};
}
static std::string game_root(const std::string &s) {   // the folder holding eboot.bin: the one chosen, or the PCSE00011 inside it
	std::error_code ec;
	fs::path p = P(s);
	if (fs::is_regular_file(p / "eboot.bin", ec)) return s;
	if (fs::is_regular_file(p / TITLE_ID / "eboot.bin", ec)) return S(p / TITLE_ID);
	return "";
}
static Check check_game(const std::string &s) {
	if (s.empty()) return {false, "Choose your copy of the game: the PCSE00011 folder from ux0:app, which holds eboot.bin."};
	std::string r = game_root(s);
	if (r.empty()) return {false, "That folder has no eboot.bin. Choose the PCSE00011 folder from ux0:app."};
	std::vector<unsigned char> sfo;
	if (read_all(P(r) / "sce_sys" / "param.sfo", &sfo) && std::search(sfo.begin(), sfo.end(), TITLE_ID, TITLE_ID + strlen(TITLE_ID)) == sfo.end())
		return {false, "That is a different game. This port needs the US release, PCSE00011."};
	return {true, "Silent Hill: Book of Memories (PCSE00011)"};
}
static bool installed_at(const fs::path &root) { std::error_code ec; return fs::is_regular_file(root / "Game" / "launch.cfg", ec); }

// ---- setup, on its own thread
enum Step { S_UNPACK, S_FIRMWARE, S_DECRYPT, S_RECOMPILE, S_COMPILE, S_FINISH, S_COUNT };
static const char *const STEP_NAME[S_COUNT] = {"Unpacking the build tools", "Reading the firmware", "Decrypting your game", "Recompiling the game's code", "Compiling for this PC", "Making the portable folder"};
static const float STEP_WEIGHT[S_COUNT] = {0.03f, 0.01f, 0.03f, 0.38f, 0.53f, 0.02f};   // of the whole, as measured
static const int COMPILE_JOBS = 55;

struct Job {
	std::thread thread;
	std::atomic<int> step{0};
	std::atomic<float> part{-1};   // of this step; below 0: not known
	std::atomic<bool> done{false}, failed{false}, cancelled{false};
	std::mutex m;
	std::vector<std::string> log;
	std::string error;
	std::chrono::steady_clock::time_point started;
	fs::path target;
	std::string firmware, game;
	bool shortcut = true;
	void say(const std::string &line) {
		std::lock_guard<std::mutex> g(m);
		log.push_back(line);
		if (log.size() > 4000) log.erase(log.begin(), log.begin() + 1000);
		std::ofstream(target / "setup.log", std::ios::app) << line << "\n";
	}
	void fail(const std::string &text) {
		{ std::lock_guard<std::mutex> g(m); error = text; }
		say(text);
		failed = true;
	}
};
static Job *g_job;

static bool unpack(Job &j, const std::string &exe, const Footer &f, const fs::path &dir) {
	std::vector<Entry> entries;
	if (!read_index(exe, f, &entries)) { j.fail("The setup program is damaged. Download it again."); return false; }
	std::error_code ec;
	std::string version = std::to_string(f.index_off);   // (a different setup program: unpack again)
	std::vector<unsigned char> mark;
	if (read_all(dir / ".unpacked", &mark) && std::string(mark.begin(), mark.end()) == version) { j.part = 1; return true; }
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	if (ec) { j.fail("Could not create " + S(dir) + ": " + ec.message()); return false; }
	{	// folders first, so the threads don't race to make them
		std::vector<fs::path> dirs;
		for (auto &e : entries) dirs.push_back(P(e.path).parent_path());
		std::sort(dirs.begin(), dirs.end());
		dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
		for (auto &d : dirs) fs::create_directories(dir / d, ec);
	}
	std::atomic<size_t> next{0};
	std::atomic<bool> bad{false};
	std::string bad_file;
	std::mutex bad_m;
	unsigned n = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
	std::vector<std::thread> workers;
	for (unsigned w = 0; w < n; w++) workers.emplace_back([&] {
		std::ifstream in(P(exe), std::ios::binary);
		std::vector<char> c, u;
		for (size_t i; (i = next++) < entries.size() && !bad && !j.cancelled;) {
			const Entry &e = entries[i];
			c.resize(e.csize); u.resize(e.usize);
			in.seekg((std::streamoff)e.off);
			in.read(c.data(), c.size());
			bool ok = (bool)in && (e.usize == 0 || inflate_raw(c.data(), c.size(), u.data(), u.size()));
			if (ok) {
				std::ofstream o(dir / P(e.path), std::ios::binary);
				o.write(u.data(), u.size());
				ok = (bool)o;
			}
			if (!ok) { std::lock_guard<std::mutex> g(bad_m); bad_file = e.path; bad = true; }
			if (i % 64 == 0) j.part = (float)i / entries.size();
		}
	});
	for (auto &t : workers) t.join();
	if (j.cancelled) return false;
	if (bad) { j.fail("Could not unpack " + bad_file + ". Is the drive full, or is antivirus blocking the folder?"); return false; }
	std::ofstream(dir / ".unpacked") << version;
	j.say("Unpacked " + std::to_string(entries.size()) + " files.");
	return true;
}
static void install_thread(Job *jp, Footer f) {
	Job &j = *jp;
	std::error_code ec;
	fs::create_directories(j.target, ec);
	if (ec) { j.fail("Could not create " + S(j.target) + ": " + ec.message()); return; }
	std::string exe = plat::exe_path();
	fs::path work = j.target / "_setup", python = work / "python" / "python.exe";
	j.step = S_UNPACK;
	if (!unpack(j, exe, f, work)) return;
	fs::create_directories(work / "dump", ec);   // (the tools use <folder>/dump only if it exists)

	// (nothing of this PC's own Python or compilers can get in the way)
	std::string sys = getenv("SystemRoot") ? getenv("SystemRoot") : "C:\\Windows";
	plat::Env env = {{"PATH", S(work / "python") + ";" + sys + "\\System32;" + sys}, {"PYTHONHOME", ""}, {"PYTHONPATH", ""}, {"PYTHONUNBUFFERED", "1"}, {"PYTHONUTF8", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"},
	                 {"RECOMP_CPU", "x86-64-v2"}};   // (not this PC's own processor: the folder has to run on the next PC)
	std::string py = S(python), driver = S(work / "pipeline" / "vitarecomp.py");
	int compiled = 0;
	auto tool = [&](std::vector<std::string> argv, const char *what) {
		if (j.cancelled) return false;
		int rc = plat::run_tool(argv, S(work), env, [&](const std::string &l) {
			j.say(l);
			if (j.step == S_COMPILE && l.find("clang") != std::string::npos) j.part = std::min(1.0f, (float)++compiled / COMPILE_JOBS);
		});
		if (j.cancelled) return false;
		if (rc != 0) { j.fail(std::string(what) + " failed" + (rc == -1 ? ": the tool could not start." : " (exit code " + std::to_string(rc) + "). The log below says why.")); return false; }
		return true;
	};
	auto step = [&](Step s) { j.step = s; j.part = -1; };

	step(S_FIRMWARE);
	if (fs::is_regular_file(P(j.firmware), ec)) { if (!tool({py, driver, "firmware", j.firmware}, "Reading the firmware")) return; }
	else if (!tool({py, S(work / "releases" / "vita3k_firmware.py"), j.firmware, S(work / "dump")}, "Reading the firmware")) return;

	step(S_DECRYPT);
	if (!tool({py, driver, "add", game_root(j.game), "--no-build"}, "Decrypting your game")) return;
	if (!tool({py, driver, "import", TITLE_ID}, "Importing your game")) return;
	step(S_RECOMPILE);
	if (!tool({py, driver, "analyze", TITLE_ID}, "Analyzing the game's code")) return;
	if (!tool({py, driver, "recompile", TITLE_ID}, "Recompiling the game's code")) return;
	step(S_COMPILE);
	j.part = 0;
	if (!tool({py, driver, "build", TITLE_ID}, "Compiling the game")) return;
	step(S_FINISH);
	if (!tool({py, S(work / "releases" / "make_portable.py"), S(j.target)}, "Making the portable folder")) return;

	// the launcher: this program without the payload, with the icon from the player's own copy
	fs::path launcher = j.target / P(LAUNCHER_NAME);
	{
		std::ifstream in(P(exe), std::ios::binary);
		std::ofstream out(launcher, std::ios::binary);
		std::vector<char> buf(1 << 20);
		for (uint64_t left = f.data_off; left && in; ) {
			size_t n = (size_t)std::min<uint64_t>(left, buf.size());
			in.read(buf.data(), n);
			out.write(buf.data(), n);
			left -= n;
		}
		if (!in || !out) { j.fail("Could not write " + S(launcher)); return; }
	}
	fs::path icon = j.target / "Game" / "fs" / "app0" / "sce_sys" / "icon0.png";
	if (!plat::set_exe_icon(S(launcher), S(icon))) j.say("(the launcher keeps the plain icon)");
	if (j.shortcut && !plat::desktop_shortcut_make(S(launcher))) j.say("(could not make the desktop shortcut)");
	fs::remove_all(work, ec);
	if (ec) j.say("(could not remove " + S(work) + ": delete it by hand)");
	j.say("Setup finished.");
	j.done = true;
}
static void start_install(const fs::path &target, const std::string &firmware, const std::string &game, bool shortcut, const Footer &f) {
	std::error_code ec;
	g_job = new Job;
	g_job->target = target; g_job->firmware = firmware; g_job->game = game; g_job->shortcut = shortcut;
	g_job->started = std::chrono::steady_clock::now();
	fs::create_directories(target, ec);
	fs::remove(target / "setup.log", ec);
	g_job->thread = std::thread(install_thread, g_job, f);
}

// ---- launching
static plat::Game *g_game;
static bool play(const fs::path &root, std::string *err) {
	std::ifstream in(root / "Game" / "launch.cfg");
	std::string line, exe;
	std::vector<std::string> args;
	plat::Env env;
	while (std::getline(in, line)) {
		if (line.rfind("exe=", 0) == 0) exe = line.substr(4);
		else if (line.rfind("arg=", 0) == 0) args.push_back(line.substr(4));
		else if (line.rfind("env=", 0) == 0) {
			size_t eq = line.find('=', 4);
			if (eq != std::string::npos) env.push_back({line.substr(4, eq - 4), line.substr(eq + 1)});
		}
	}
	if (exe.empty()) { *err = "The Game folder is incomplete. Run setup again."; return false; }
	std::error_code ec;
	fs::create_directories(root / "Saves", ec);
	fs::path game = root / "Game";
	std::error_code e2;
	if (fs::exists(game / "session.log", e2)) fs::rename(game / "session.log", game / "session.prev.log", e2);
	g_game = plat::start_game(S(game / exe), args, S(game), env, S(game / "session.log"), err);
	return g_game != 0;
}

// ---- no window: for scripts
static std::string g_pre_target, g_pre_firmware, g_pre_game;   // (without --install: the fields filled in)
static bool g_auto_play;   // --play: straight into the game, for a shortcut or a Steam entry
int app_command_line(int argc, char **argv) {
	std::string root, target, firmware, game, export_to, import_from;
	g_auto_play = false;
	bool install = false, shortcut = true;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--root") root = next();
		else if (a == "--install") install = true;
		else if (a == "--play") g_auto_play = true;
		else if (a == "--target") target = next();
		else if (a == "--firmware") firmware = next();
		else if (a == "--game") game = next();
		else if (a == "--no-shortcut") shortcut = false;
		else if (a == "--export") export_to = next();
		else if (a == "--import") import_from = next();
	}
	std::string exe = plat::exe_path();
	if (root.empty()) root = S(P(exe).parent_path());
	std::string msg;
	if (!export_to.empty()) return zip_folder(P(root) / "Saves", P(export_to), &msg) ? 0 : 1;
	if (!import_from.empty()) return import_saves(P(root), P(import_from), &msg) ? 0 : 1;
	if (install) {
		Footer f;
		if (!read_footer(exe, &f) || !check_firmware(firmware).ok || !check_game(game).ok || target.empty()) return 2;
		start_install(P(target), firmware, game, shortcut, f);
		g_job->thread.join();
		return g_job->done ? 0 : 1;
	}
	g_pre_target = target; g_pre_firmware = firmware; g_pre_game = game;
	return -1;
}

// ---- the window
static ImFont *g_font_big, *g_font_bold;
static float g_scale = 1;
static const ImVec4 ACCENT(0.910f, 0.063f, 0.063f, 1), ACCENT_HI(1.000f, 0.180f, 0.160f, 1), ACCENT_LO(0.700f, 0.040f, 0.040f, 1);   // the red of the game's icon
static const ImVec4 TEXT_DIM(0.60f, 0.60f, 0.64f, 1), GOOD(1.0f, 0.55f, 0.26f, 1), BAD(1.0f, 0.20f, 0.18f, 1);
enum Page { P_SETUP, P_PROGRESS, P_PLAY, P_BROKEN };
static Page g_page;
static fs::path g_root;
static Footer g_footer;
static bool g_has_payload;
static char g_target[1024], g_firmware[1024], g_game_path[1024];
static bool g_shortcut = true;
static std::string g_status;
static bool g_status_bad;
static void *g_art;
static int g_art_w, g_art_h;
static bool g_art_tried;
static std::string g_saves_info;
static double g_saves_checked = -100;
static bool g_finished_banner;

static void set_status(const std::string &s, bool bad = false) { g_status = s; g_status_bad = bad; }

void app_set_scale(float s) {
	g_scale = s;
	ImGuiStyle &st = ImGui::GetStyle();
	st = ImGuiStyle();
	ImGui::StyleColorsDark(&st);
	st.WindowRounding = 0; st.WindowBorderSize = 0;
	st.ChildRounding = 8; st.FrameRounding = 6; st.GrabRounding = 6; st.ScrollbarRounding = 6;
	st.ChildBorderSize = 1; st.FrameBorderSize = 0;
	st.WindowPadding = ImVec2(22, 18); st.FramePadding = ImVec2(10, 7); st.ItemSpacing = ImVec2(10, 9);
	ImVec4 *c = st.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 1);
	c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.105f, 0.12f, 1);
	c[ImGuiCol_Border] = ImVec4(0.18f, 0.18f, 0.20f, 1);
	c[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.93f, 1);
	c[ImGuiCol_TextDisabled] = TEXT_DIM;
	c[ImGuiCol_FrameBg] = ImVec4(0.055f, 0.055f, 0.065f, 1);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.09f, 0.09f, 0.10f, 1);
	c[ImGuiCol_FrameBgActive] = ImVec4(0.11f, 0.11f, 0.12f, 1);
	c[ImGuiCol_Button] = ImVec4(0.17f, 0.17f, 0.19f, 1);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.27f, 1);
	c[ImGuiCol_ButtonActive] = ImVec4(0.13f, 0.13f, 0.15f, 1);
	c[ImGuiCol_CheckMark] = ACCENT_HI;
	c[ImGuiCol_CheckboxSelectedBg] = c[ImGuiCol_FrameBg];
	c[ImGuiCol_Header] = ImVec4(0.17f, 0.17f, 0.19f, 1);
	c[ImGuiCol_HeaderHovered] = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.55f);
	c[ImGuiCol_HeaderActive] = ACCENT;
	c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.14f, 1);
	c[ImGuiCol_PlotHistogram] = ACCENT;
	c[ImGuiCol_TextSelectedBg] = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.45f);
	c[ImGuiCol_NavCursor] = ACCENT_HI;
	c[ImGuiCol_InputTextCursor] = ACCENT_HI;
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	st.ScaleAllSizes(s);
	st.FontScaleDpi = s;
}
void app_init() {
	ImGuiIO &io = ImGui::GetIO();
	const char *regular = "C:\\Windows\\Fonts\\segoeui.ttf", *bold = "C:\\Windows\\Fonts\\seguisb.ttf";
	std::error_code ec;
	ImFontConfig cfg;
	if (fs::exists(regular, ec)) {
		io.Fonts->AddFontFromFileTTF(regular, 17, &cfg);
		g_font_bold = fs::exists(bold, ec) ? io.Fonts->AddFontFromFileTTF(bold, 17, &cfg) : 0;
	} else io.Fonts->AddFontDefault();
	ImGui::GetStyle().FontSizeBase = 17;
	g_font_big = g_font_bold;

	std::string exe = plat::exe_path();
	fs::path dir = P(exe).parent_path();
	g_has_payload = read_footer(exe, &g_footer);
	fs::path def = dir / P(DEFAULT_FOLDER);
	snprintf(g_target, sizeof g_target, "%s", S(installed_at(dir) ? dir : def).c_str());
	if (installed_at(dir)) { g_root = dir; g_page = P_PLAY; }
	else if (g_has_payload && installed_at(def)) { g_root = def; g_page = P_PLAY; }
	else g_page = g_has_payload ? P_SETUP : P_BROKEN;
	if (!g_pre_target.empty()) snprintf(g_target, sizeof g_target, "%s", g_pre_target.c_str());
	snprintf(g_firmware, sizeof g_firmware, "%s", g_pre_firmware.c_str());
	snprintf(g_game_path, sizeof g_game_path, "%s", g_pre_game.c_str());
	if (g_page == P_PLAY) plat::desktop_shortcut_refresh(S(g_root / P(LAUNCHER_NAME)));
}
bool app_animating() { return g_page == P_PROGRESS && g_job && !g_job->failed; }
bool app_can_close() {
	if (g_job && !g_job->done && !g_job->failed && g_page == P_PROGRESS) {
		if (!plat::ask_yes_no(APP_TITLE, "Setup is still running. Stop it and close?")) return false;
		g_job->cancelled = true;
		plat::kill_tools();
		g_job->thread.join();
	}
	return true;
}

// ---- pieces
static float px(float v) { return v * g_scale; }
static bool accent_button(const char *label, ImVec2 size, bool enabled = true) {
	ImGui::PushStyleColor(ImGuiCol_Button, enabled ? ACCENT : ImVec4(0.20f, 0.11f, 0.11f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ACCENT_HI);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACCENT_LO);
	ImGui::PushStyleColor(ImGuiCol_Text, enabled ? ImVec4(1, 1, 1, 1) : TEXT_DIM);
	if (g_font_bold) ImGui::PushFont(g_font_bold, 0);
	ImGui::BeginDisabled(!enabled);
	bool hit = ImGui::Button(label, size);
	ImGui::EndDisabled();
	if (g_font_bold) ImGui::PopFont();
	ImGui::PopStyleColor(4);
	return hit;
}
static void heading(const char *text) {
	if (g_font_bold) ImGui::PushFont(g_font_bold, 0);
	ImGui::TextUnformatted(text);
	if (g_font_bold) ImGui::PopFont();
}
static void dim(const char *text) {
	ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}
static void dot_text(ImVec4 color, const std::string &text) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetTextLineHeight();
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + px(5), p.y + h / 2), px(4), ImGui::ColorConvertFloat4ToU32(color));
	ImGui::SetCursorScreenPos(ImVec2(p.x + px(17), p.y));
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}
static bool begin_card(const char *id) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(13)));
	return ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
static void end_card() {
	ImGui::EndChild();
	ImGui::PopStyleVar();
}
static void header() {
	ImDrawList *d = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetWindowPos();
	float w = ImGui::GetWindowWidth(), h = g_art ? std::min(w * 0.24f, px(240)) : px(96);
	if (g_art) {
		// (the picture filling the band, centred, fading into the page)
		float scale = std::max(w / g_art_w, h / g_art_h), iw = g_art_w * scale, ih = g_art_h * scale;
		float u = (iw - w) / 2 / iw, v = (ih - h) / 2 / ih;
		d->AddImage((ImTextureID)(intptr_t)g_art, o, ImVec2(o.x + w, o.y + h), ImVec2(u, v), ImVec2(1 - u, 1 - v));
		d->AddRectFilledMultiColor(o, ImVec2(o.x + w, o.y + h), IM_COL32(18, 18, 20, 0), IM_COL32(18, 18, 20, 0), IM_COL32(18, 18, 20, 170), IM_COL32(18, 18, 20, 170));
	}
	d->AddRectFilled(ImVec2(o.x, o.y + h - px(2)), ImVec2(o.x + w, o.y + h), ImGui::ColorConvertFloat4ToU32(ACCENT));
	ImFont *f = g_font_big ? g_font_big : ImGui::GetFont();
	float size = px(30), line = px(18);
	const char *sub = "Unofficial PC port  |  " APP_VERSION;
	if (g_art) {   // (the picture says the title)
		ImVec2 t = ImGui::GetFont()->CalcTextSizeA(px(15), FLT_MAX, 0, sub);
		d->AddText(ImGui::GetFont(), px(15), ImVec2(o.x + w - t.x - px(14), o.y + h - t.y - px(10)), ImGui::ColorConvertFloat4ToU32(TEXT_DIM), sub);
	} else {
		float top = o.y + h - px(66);
		d->AddText(f, size, ImVec2(o.x + px(22), top), IM_COL32(255, 255, 255, 255), APP_TITLE);
		d->AddText(ImGui::GetFont(), line, ImVec2(o.x + px(23), top + px(35)), ImGui::ColorConvertFloat4ToU32(TEXT_DIM), sub);
	}
	ImGui::SetCursorScreenPos(ImVec2(o.x + px(22), o.y + h + px(16)));
}
static void path_row(const char *id, char *buf, size_t n, int buttons, float button_w) {
	float w = ImGui::GetContentRegionAvail().x - buttons * (button_w + ImGui::GetStyle().ItemSpacing.x);
	ImGui::SetNextItemWidth(w);
	ImGui::InputText(id, buf, n);
}

// ---- pages
static void page_setup() {
	if (g_has_payload && installed_at(P(g_target))) { /* an update: said below */ }
	float bw = px(150);
	{
		begin_card("folder");
		heading("1.  Where to install");
		path_row("##target", g_target, sizeof g_target, 1, bw);
		ImGui::SameLine();
		if (ImGui::Button("Browse...##t", ImVec2(bw, 0))) {
			std::string s = plat::pick_folder("Choose where to install the game");
			if (!s.empty()) {
				fs::path p = P(s);
				std::error_code ec;
				// (a new folder inside the one chosen, unless it is an install or already this game's)
				if (!installed_at(p) && S(p.filename()) != DEFAULT_FOLDER) p /= P(DEFAULT_FOLDER);
				snprintf(g_target, sizeof g_target, "%s", S(p).c_str());
			}
		}
		dim("Everything goes in this one folder. You can move or copy it later, to another drive or another PC.");
		end_card();
	}
	{
		begin_card("firmware");
		heading("2.  Vita firmware");
		float small = px(150);
		path_row("##firmware", g_firmware, sizeof g_firmware, 2, small);
		ImGui::SameLine();
		if (ImGui::Button("Firmware file...", ImVec2(small, 0))) {
			std::string s = plat::pick_file("Choose Sony's Vita firmware update file", "Vita firmware (*.PUP)", "*.PUP");
			if (!s.empty()) snprintf(g_firmware, sizeof g_firmware, "%s", s.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Vita3K folder...", ImVec2(small, 0))) {
			std::string s = plat::pick_folder("Choose the Vita3K folder holding os0 and vs0");
			if (!s.empty()) snprintf(g_firmware, sizeof g_firmware, "%s", s.c_str());
		}
		Check c = check_firmware(g_firmware);
		if (g_firmware[0]) dot_text(c.ok ? GOOD : BAD, c.text); else dim(c.text.c_str());
		end_card();
	}
	{
		begin_card("game");
		heading("3.  Your game");
		path_row("##game", g_game_path, sizeof g_game_path, 1, bw);
		ImGui::SameLine();
		if (ImGui::Button("Browse...##g", ImVec2(bw, 0))) {
			std::string s = plat::pick_folder("Choose your PCSE00011 folder, the one holding eboot.bin");
			if (!s.empty()) snprintf(g_game_path, sizeof g_game_path, "%s", s.c_str());
		}
		Check c = check_game(g_game_path);
		if (g_game_path[0]) dot_text(c.ok ? GOOD : BAD, c.text); else dim(c.text.c_str());
		end_card();
	}
	ImGui::Spacing();
	ImGui::Checkbox("Add a desktop shortcut", &g_shortcut);

	fs::path target = P(g_target);
	bool update = installed_at(target);
	uint64_t have = plat::free_bytes(g_target);
	bool space = have >= NEEDED_BYTES;
	bool ready = g_target[0] && space && check_firmware(g_firmware).ok && check_game(g_game_path).ok;
	if (!space && g_target[0]) {
		char b[160];
		snprintf(b, sizeof b, "That drive has %.1f GB free. Setup needs about 5 GB while it runs.", have / 1073741824.0);
		dot_text(BAD, b);
	} else if (update) dot_text(GOOD, "This folder already has the game. Installing updates it and keeps your saves and settings.");
	else dim("Needs about 5 GB while building, and about 1.6 GB after. It takes about 8 minutes and uses all your CPU cores.");
	ImGui::Spacing();
	if (accent_button(update ? "Update" : "Install", ImVec2(px(220), px(46)), ready)) {
		g_root = target;
		g_finished_banner = false;
		start_install(target, g_firmware, g_game_path, g_shortcut, g_footer);
		g_page = P_PROGRESS;
	}
	if (installed_at(target)) {
		ImGui::SameLine();
		if (ImGui::Button("Back", ImVec2(px(100), px(46)))) { g_root = target; g_page = P_PLAY; }
	}
	ImGui::Spacing();
	dim("The port is built on this PC from your own files. Nothing is uploaded, and no game or firmware files are included in this program.");
}

static void page_progress() {
	Job &j = *g_job;
	int step = j.step;
	bool failed = j.failed;
	float part = j.part;
	if (j.done) {
		j.thread.join();
		delete g_job; g_job = 0;
		g_finished_banner = true;
		g_art_tried = false;
		g_saves_checked = -100;
		g_page = P_PLAY;
		return;
	}
	float done = 0;
	for (int i = 0; i < step && i < S_COUNT; i++) done += STEP_WEIGHT[i];
	begin_card("steps");
	for (int i = 0; i < S_COUNT; i++) {
		ImVec4 c = i < step ? GOOD : i == step ? (failed ? BAD : ACCENT_HI) : TEXT_DIM;
		dot_text(c, STEP_NAME[i]);
	}
	end_card();
	ImGui::Spacing();
	if (failed) {
		std::string e;
		{ std::lock_guard<std::mutex> g(j.m); e = j.error; }
		dot_text(BAD, e);
	} else {
		auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - j.started).count();
		float whole = done + (part >= 0 ? part * STEP_WEIGHT[step] : 0.5f * STEP_WEIGHT[step]);
		char overlay[64];
		snprintf(overlay, sizeof overlay, "%d%%   %d:%02d", (int)(whole * 100), (int)(secs / 60), (int)(secs % 60));
		ImGui::ProgressBar(whole, ImVec2(-1, px(24)), overlay);
	}
	ImGui::Spacing();
	float bottom = px(46) + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1));
	ImGui::BeginChild("log", ImVec2(0, -bottom), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	{
		std::lock_guard<std::mutex> g(j.m);
		ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
		ImGuiListClipper clip;
		clip.Begin((int)j.log.size());
		while (clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(j.log[i].c_str());
		ImGui::PopStyleColor();
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(40)) ImGui::SetScrollHereY(1);
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::Spacing();
	if (failed) {
		if (ImGui::Button("Back", ImVec2(px(140), px(40)))) {
			j.thread.join();
			delete g_job; g_job = 0;
			g_page = P_SETUP;
		}
		ImGui::SameLine();
		if (ImGui::Button("Open folder", ImVec2(px(140), px(40)))) plat::open_folder(S(j.target));
	} else if (ImGui::Button("Cancel", ImVec2(px(140), px(40)))) {
		j.cancelled = true;
		plat::kill_tools();
		j.thread.join();
		std::error_code ec;
		fs::remove_all(j.target / "_setup", ec);
		if (!installed_at(j.target)) fs::remove(j.target / "setup.log", ec);
		delete g_job; g_job = 0;
		g_page = P_SETUP;
	}
}

static void refresh_saves_info() {
	double now = ImGui::GetTime();
	if (now - g_saves_checked < 2) return;
	g_saves_checked = now;
	auto files = save_files(g_root / "Saves");
	if (files.empty()) { g_saves_info = "No saves yet."; return; }
	std::error_code ec;
	fs::file_time_type newest{};
	uint64_t size = 0;
	for (auto &f : files) {
		auto t = fs::last_write_time(f, ec);
		if (!ec && t > newest) newest = t;
		size += fs::file_size(f, ec);
	}
	auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(newest - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
	time_t tt = std::chrono::system_clock::to_time_t(sys);
	char when[64];
	strftime(when, sizeof when, "%d %b %Y, %H:%M", localtime(&tt));
	char b[160];
	snprintf(b, sizeof b, "%d files, %.0f KB. Last saved %s.", (int)files.size(), size / 1024.0, when);
	g_saves_info = b;
}

static void page_play() {
	bool running = g_game != 0;
	if (g_finished_banner) {
		std::string name = g_has_payload ? "Setup finished. The folder is ready to use, and you can delete this setup program." : "Setup finished.";
		dot_text(GOOD, name);
		ImGui::Spacing();
	}
	if (accent_button(running ? "Running..." : "PLAY", ImVec2(-1, px(64)), !running)) {
		std::string err;
		if (play(g_root, &err)) { set_status(""); g_finished_banner = false; plat::window_minimize(); }
		else set_status(err, true);
	}
	ImGui::Spacing();
	begin_card("saves");
	heading("Saves");
	refresh_saves_info();
	dim(g_saves_info.c_str());
	ImGui::BeginDisabled(running);
	float bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
	if (ImGui::Button("Open save folder", ImVec2(bw, px(36)))) { std::error_code ec; fs::create_directories(g_root / "Saves", ec); plat::open_folder(S(g_root / "Saves")); }
	ImGui::SameLine();
	if (ImGui::Button("Export to zip...", ImVec2(bw, px(36)))) {
		std::string name = "Silent Hill BoM saves " + stamp("%Y-%m-%d") + ".zip";
		std::string to = plat::pick_save("Export saves", name.c_str(), "Zip file (*.zip)", "*.zip");
		if (!to.empty()) {
			std::string err;
			if (zip_folder(g_root / "Saves", P(to), &err)) set_status("Saves exported to " + to);
			else set_status(err, true);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Import...", ImVec2(bw, px(36)))) ImGui::OpenPopup("import");
	if (ImGui::BeginPopup("import")) {
		std::string from;
		if (ImGui::Selectable("From a zip file...")) from = plat::pick_file("Import saves", "Zip file (*.zip)", "*.zip");
		if (ImGui::Selectable("From a folder...")) from = plat::pick_folder("Choose a folder holding saves");
		if (!from.empty()) {
			if (plat::ask_yes_no("Import saves", "Replace your current saves with these? A copy of the current ones goes to Save Backups first.")) {
				std::string msg;
				bool ok = import_saves(g_root, P(from), &msg);
				set_status(msg, !ok);
				g_saves_checked = -100;
			}
		}
		ImGui::EndPopup();
	}
	ImGui::EndDisabled();
	dim("To keep saves in step between machines, point Syncthing at the save folder. It holds nothing but saves. Close the game and let the sync finish before you play on another machine.");
	end_card();
	ImGui::Spacing();
	begin_card("install");
	heading("This install");
	dim(S(g_root).c_str());
	float b2 = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
	if (ImGui::Button("Open folder", ImVec2(b2, px(36)))) plat::open_folder(S(g_root));
	ImGui::SameLine();
	if (ImGui::Button("Desktop shortcut", ImVec2(b2, px(36)))) {
		bool ok = plat::desktop_shortcut_make(S(g_root / P(LAUNCHER_NAME)));
		set_status(ok ? "Desktop shortcut made." : "Could not make the desktop shortcut.", !ok);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(running);
	if (g_has_payload) {
		if (ImGui::Button("Reinstall or update...", ImVec2(b2, px(36)))) { snprintf(g_target, sizeof g_target, "%s", S(g_root).c_str()); g_page = P_SETUP; }
	} else if (ImGui::Button("Project page", ImVec2(b2, px(36)))) plat::open_url(REPO_URL);
	ImGui::EndDisabled();
	dim("In the game: Alt+Enter for fullscreen, F10 for settings and key bindings, - and = for volume.");
	end_card();
	if (!g_status.empty()) {
		ImGui::Spacing();
		dot_text(g_status_bad ? BAD : GOOD, g_status);
	}
}
static void page_broken() {
	dot_text(BAD, "The Game folder is missing from this folder.");
	ImGui::Spacing();
	dim("This launcher needs the Game folder beside it. If you moved the launcher by itself, put it back. Otherwise run the setup program again: your Saves folder is kept.");
	ImGui::Spacing();
	if (ImGui::Button("Project page", ImVec2(px(160), px(36)))) plat::open_url(REPO_URL);
}

void app_frame() {
	if (g_auto_play && g_page == P_PLAY) {
		g_auto_play = false;
		std::string err;
		if (play(g_root, &err)) plat::window_minimize(); else set_status(err, true);
	}
	if (g_game) {
		int code = 0;
		if (plat::game_finished(g_game, &code)) {
			g_game = 0;
			g_saves_checked = -100;
			plat::window_restore();
			if (code != 0) set_status("The game stopped with error code " + std::to_string(code) + ". Its log is Game\\session.log.", true);
		}
	}
	if (!g_art_tried && g_page == P_PLAY) {
		g_art_tried = true;
		fs::path sys = g_root / "Game" / "fs" / "app0" / "sce_sys" / "livearea" / "contents";
		std::error_code ec;
		for (const fs::path &p : {sys / "bg0.png", sys / "bg.png", g_root / "Game" / "fs" / "app0" / "sce_sys" / "pic0.png"}) {
			if (!fs::exists(p, ec)) continue;
			g_art = plat::load_texture(S(p), &g_art_w, &g_art_h);
			if (g_art) break;
		}
	}
	ImGuiViewport *v = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(v->Pos);
	ImGui::SetNextWindowSize(v->Size);
	ImGui::Begin("##main", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
	header();
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None, 0);
	ImGui::PopStyleColor();
	switch (g_page) {
	case P_SETUP: page_setup(); break;
	case P_PROGRESS: page_progress(); break;
	case P_PLAY: page_play(); break;
	case P_BROKEN: page_broken(); break;
	}
	ImGui::EndChild();
	ImGui::End();
}
