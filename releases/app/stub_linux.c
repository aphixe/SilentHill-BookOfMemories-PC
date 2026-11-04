// The first part of the single-file Linux setup program. Static, so it runs before anything is known about the machine.
// The app needs SDL3, a shared library, so this unpacks the app and SDL from the end of its own file to a temporary folder and
// starts them. The app deletes that folder as soon as it has loaded. See releases/build_app.py for the file's layout.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct Footer { char magic[8]; uint64_t data_off, index_off, index_csize, index_usize; };
struct Parts { char magic[8]; uint64_t app_off, app_len, sdl_off, sdl_len; };

static int copy_part(FILE *in, uint64_t off, uint64_t len, const char *path) {
	FILE *out = fopen(path, "wb");
	if (!out) return 0;
	static char buf[1 << 16];
	if (fseeko(in, (off_t)off, SEEK_SET)) { fclose(out); return 0; }
	while (len) {
		size_t want = len < sizeof buf ? (size_t)len : sizeof buf;
		size_t got = fread(buf, 1, want, in);
		if (!got || fwrite(buf, 1, got, out) != got) { fclose(out); return 0; }
		len -= got;
	}
	if (fclose(out)) return 0;
	return chmod(path, 0755) == 0;
}

int main(int argc, char **argv) {
	(void)argc;
	static char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0) { fprintf(stderr, "Could not find this program's own file.\n"); return 1; }
	self[n] = 0;
	FILE *in = fopen(self, "rb");
	if (!in) { fprintf(stderr, "Could not read %s\n", self); return 1; }
	struct Footer f;
	struct Parts p;
	if (fseeko(in, -(off_t)(sizeof f + sizeof p), SEEK_END) || fread(&p, sizeof p, 1, in) != 1 || fread(&f, sizeof f, 1, in) != 1
	    || memcmp(p.magic, "SHBMPART", 8) || memcmp(f.magic, "SHBMPAK1", 8)) {
		fprintf(stderr, "This setup program is damaged. Download it again.\n");
		return 1;
	}
	// (somewhere programs may run: /tmp can forbid it, so try the next place if starting fails)
	char beside[4096 + 8];
	snprintf(beside, sizeof beside, "%s", self);
	char *slash = strrchr(beside, '/');
	if (slash) *slash = 0;
	const char *places[] = {getenv("XDG_RUNTIME_DIR"), "/tmp", getenv("HOME"), beside};
	for (unsigned i = 0; i < sizeof places / sizeof *places; i++) {
		if (!places[i] || !*places[i]) continue;
		static char dir[4300], app[4400], sdl[4400];
		snprintf(dir, sizeof dir, "%s/shbom-app.XXXXXX", places[i]);
		if (!mkdtemp(dir)) continue;
		snprintf(app, sizeof app, "%s/shbom-app", dir);
		snprintf(sdl, sizeof sdl, "%s/libSDL3.so.0", dir);
		if (copy_part(in, p.app_off, p.app_len, app) && copy_part(in, p.sdl_off, p.sdl_len, sdl)) {
			setenv("SHBOM_SELF", self, 1);
			setenv("SHBOM_TMP", dir, 1);
			setenv("LD_LIBRARY_PATH", dir, 1);
			argv[0] = app;
			execv(app, argv);
		}
		unlink(app); unlink(sdl); rmdir(dir);
	}
	fprintf(stderr, "Could not start the app: %s\n", strerror(errno));
	return 1;
}
