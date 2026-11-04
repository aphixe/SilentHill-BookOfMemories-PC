/*
 * Silent Hill: Book of Memories (PCSE00011): its hooks, built into this game alone.
 *
 * The boot logos run on a thread of their own for a fixed time (fn_81196faa; 0x8178e214 set while they run), drawing
 * through the one GXM context, while the main thread loads the title screen. On a Vita the load outlasts the logos. Here
 * it finishes in a moment, and the world's draws (main thread, job queue) land inside and between the logo thread's
 * scenes: sceGxmDraw -> NOT_WITHIN_SCENE, the game's fatal error. So: anyone else entering the renderer waits for the
 * logos to end, as they would have on the console. A thread holding the render lock (0x81d214e0: its owner, 0x816aaa34: the depth) never waits,
 * since the logo thread needs that lock.
 */
#include "hooks.h"
#include <unistd.h>

extern uint32_t recomp_cur_tid(void);
static volatile uint32_t sh_logo_tid;

void recomp_hook_sh_logo_thread(Cpu *c) { sh_logo_tid = recomp_cur_tid(); }

void recomp_hook_sh_render_gate(Cpu *c) {
	uint32_t me = recomp_cur_tid();
	if (me == sh_logo_tid) return;
	while (RD8(0x8178e214u) && me != sh_logo_tid && !(RD32(0x816aaa34u) > 0 && RD32(0x81d214e0u) == me)) usleep(1000);
}

void recomp_hook_sh_t_stop(Cpu *c);
/*
 * The logo thread's last act (fn_81196faa, past its cleanup) is to start the loading screen, when the game is still
 * loading. fn_8119714e ends the loading screen and frees its textures: called in that window, it saw no loading screen yet
 * and freed them, and the loading thread then drew an uncached texture (a fatal error). On a Vita the load always outlasted
 * this. So ending the loading screen waits for the logo thread to have finished.
 */
static volatile int sh_logo_done;
void recomp_ihook_sh_logo_done(Cpu *c, int reg) { sh_logo_done = 1; }
void recomp_hook_sh_loading_end(Cpu *c) { while ((sh_logo_tid || RD8(0x8178e214u)) && !sh_logo_done) usleep(1000); }

/* tracing the loading screen's texture (a porting aid: SH_TRACE=1) */
#include <stdio.h>
#include <stdlib.h>
static void sh_trace(const char *what, Cpu *c) {
	static int on = -1; if (on < 0) on = getenv("SH_TRACE") != 0;
	if (on) fprintf(stderr, "sh: %-12s t%x r0 %x lr %x | e210 %d e214 %d e21c %d 9d28 %d tex+4 %d\n", what, recomp_cur_tid(), c->r[0], c->r[14], RD8(0x8178e210u), RD8(0x8178e214u), RD8(0x8178e21cu), RD8(0x816a9d28u), (int)RD32(0x81bea77cu + 4));
}
void recomp_hook_sh_t_cache(Cpu *c) { if (c->r[0] == 0x81bea77cu) sh_trace("cache", c); }
void recomp_hook_sh_t_uncache(Cpu *c) { if (c->r[0] == 0x81bea77cu) sh_trace("uncache", c); }
void recomp_hook_sh_t_prep(Cpu *c) { sh_trace("prep(c2c)", c); }
/* fn_811971ba also ends the loading screen: with the logos still up it saw none, freed its texture and left it marked ready */
void recomp_hook_sh_t_start(Cpu *c) { sh_trace("start(1ba)", c); recomp_hook_sh_loading_end(c); }
void recomp_hook_sh_t_stop(Cpu *c) { sh_trace("stop(14e)", c); }
void recomp_hook_sh_t_thread(Cpu *c) { sh_trace("thread(550)", c); }
