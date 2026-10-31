/*
 * rp6aim: gyro aim trainer and calibrator for the Retroid Pocket 6 (Steam Input gyro -> mouse).
 *
 * It measures the whole path sensor -> rp6gyrod -> Steam Input -> mouse by comparing the mouse counts it receives with the
 * real rotation of the console published by rp6gyrod (/run/rp6-gyro/motion.bin), then lets you shoot static and moving
 * targets. At the end it suggests (and can apply) the sensitivity and axis inversion for rp6gyrod (the active profile).
 *
 * Stages: 1 rest (mouse creep), 2 rotation (counts per degree, dead zone by speed, axis signs), 3 targets, 4 tracking.
 * Controls (gamepad through Steam Input, keyboard/mouse also work): A / left click / RT = confirm or fire,
 * B = back/skip, X = recentre cursor, Start = quit.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DATA_DIR
#define DATA_DIR "/var/lib/rp6-gyro/aimlab"
#endif
#define PROXY_CONF conf_file()
#define SHM_PATH "/run/rp6-gyro/motion.bin"
#define FOV_DEG 90.0        /* horizontal field of view for "1:1" (the screen is a window onto a 90 deg view) */
#define WIN_S 0.2           /* analysis window (s) */
#define MAXWIN 3000
#define NBIN 8

/* ------------------------------------------------------------------------------------------ shared motion */
struct shared_motion {
	uint32_t seq, magic;
	double t;
	float phys_dps[3], out_dps[3];
	double angle_deg[3];
	uint32_t compensation, reports;
	float sensitivity, pad;
};

struct snap {
	double t, angle[3];
	float phys[3], out[3], sens;
	uint32_t comp;
	int ok;
};

static const struct shared_motion *shm;

static void shm_attach(void)
{
	int fd = open(SHM_PATH, O_RDONLY);
	void *p;

	if (fd < 0)
		return;
	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p != MAP_FAILED && ((const struct shared_motion *) p)->magic == 0x52503647)
		shm = p;
}

static struct snap snap_read(void)
{
	struct snap s;

	memset(&s, 0, sizeof(s));
	if (!shm)
		return s;
	for (int tries = 0; tries < 20; tries++) {
		uint32_t a = __atomic_load_n(&shm->seq, __ATOMIC_ACQUIRE);

		if (a & 1)
			continue;
		s.t = shm->t;
		for (int i = 0; i < 3; i++) {
			s.angle[i] = shm->angle_deg[i];
			s.phys[i] = shm->phys_dps[i];
			s.out[i] = shm->out_dps[i];
		}
		s.sens = shm->sensitivity;
		s.comp = shm->compensation;
		if (a == __atomic_load_n(&shm->seq, __ATOMIC_ACQUIRE)) {
			s.ok = 1;
			break;
		}
	}
	return s;
}

/* ------------------------------------------------------------------------------------------ text */
#define NGLYPH 512
struct glyph { float u0, v0, u1, v1; int w, h, bx, by, adv; int ok; };
struct font { SDL_Texture *tex; struct glyph g[NGLYPH]; int px, line; };

static SDL_Renderer *rend;
static struct font fonts[3];
static int win_w = 1920, win_h = 1080;
static float ui;            /* ui scale = win_h / 1080 */

static int gidx(uint32_t cp)
{
	if (cp >= 0x20 && cp <= 0x7E) return cp - 0x20;                    /* 0..94 */
	if (cp >= 0xA0 && cp <= 0xFF) return 95 + (cp - 0xA0);             /* 95..222 */
	if (cp >= 0x400 && cp <= 0x45F) return 223 + (cp - 0x400);         /* 223..318 */
	if (cp == 0x2013) return 320;
	if (cp == 0x2014) return 321;
	if (cp == 0x2192) return 322;
	if (cp == 0x2022) return 323;
	return -1;
}

static uint32_t cp_of(int i)
{
	if (i < 95) return 0x20 + i;
	if (i < 223) return 0xA0 + (i - 95);
	if (i < 319) return 0x400 + (i - 223);
	return i == 320 ? 0x2013 : i == 321 ? 0x2014 : i == 322 ? 0x2192 : i == 323 ? 0x2022 : 0;
}

static int font_build(struct font *f, FT_Face face, int px)
{
	const int AW = 1024, AH = 1024;
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, AW, AH, 32, SDL_PIXELFORMAT_RGBA32);
	int x = 1, y = 1, rowh = 0;

	if (!surf)
		return -1;
	SDL_memset(surf->pixels, 0, (size_t) surf->pitch * AH);
	FT_Set_Pixel_Sizes(face, 0, px);
	f->px = px;
	f->line = (int) ((face->size->metrics.height + 63) >> 6);
	for (int i = 0; i < NGLYPH; i++) {
		uint32_t cp = cp_of(i);
		FT_Bitmap *bm;
		struct glyph *g = &f->g[i];

		if (!cp || !FT_Get_Char_Index(face, cp) || FT_Load_Char(face, cp, FT_LOAD_RENDER))
			continue;
		bm = &face->glyph->bitmap;
		if (x + (int) bm->width + 1 >= AW) {
			x = 1;
			y += rowh + 1;
			rowh = 0;
		}
		if (y + (int) bm->rows + 1 >= AH)
			break;
		for (unsigned r = 0; r < bm->rows; r++)
			for (unsigned c = 0; c < bm->width; c++) {
				uint8_t a = bm->buffer[r * bm->pitch + c];
				uint32_t *dst = (uint32_t *) ((uint8_t *) surf->pixels + (y + r) * surf->pitch) + x + c;

				*dst = 0x00FFFFFFu | ((uint32_t) a << 24);
			}
		g->u0 = (float) x / AW;
		g->v0 = (float) y / AH;
		g->u1 = (float) (x + bm->width) / AW;
		g->v1 = (float) (y + bm->rows) / AH;
		g->w = bm->width;
		g->h = bm->rows;
		g->bx = face->glyph->bitmap_left;
		g->by = face->glyph->bitmap_top;
		g->adv = (int) (face->glyph->advance.x >> 6);
		g->ok = 1;
		x += bm->width + 1;
		if ((int) bm->rows > rowh)
			rowh = bm->rows;
	}
	f->tex = SDL_CreateTextureFromSurface(rend, surf);
	SDL_FreeSurface(surf);
	if (!f->tex)
		return -1;
	SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
	return 0;
}

static int fonts_init(void)
{
	static const char *paths[] = {
		DATA_DIR "/font.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto-vf/NotoSans[wdth,wght].ttf",
		"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
	static const int sizes[3] = { 22, 34, 64 };
	FT_Library lib;
	FT_Face face = NULL;

	if (FT_Init_FreeType(&lib))
		return -1;
	for (int i = 0; paths[i] && !face; i++)
		if (access(paths[i], R_OK) == 0 && FT_New_Face(lib, paths[i], 0, &face))
			face = NULL;
	if (!face) {
		fprintf(stderr, "no usable font found\n");
		return -1;
	}
	for (int i = 0; i < 3; i++)
		if (font_build(&fonts[i], face, (int) (sizes[i] * ui + 0.5f)))
			return -1;
	return 0;
}

static int text_width(int fs, const char *s)
{
	struct font *f = &fonts[fs];
	int w = 0;

	for (const unsigned char *p = (const unsigned char *) s; *p;) {
		uint32_t cp;
		int n = 1, gi;

		if (*p < 0x80) cp = *p;
		else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
		else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
		else { cp = '?'; }
		p += n;
		gi = gidx(cp);
		if (gi >= 0 && f->g[gi].ok)
			w += f->g[gi].adv;
	}
	return w;
}

static void text_at(int fs, int x, int y, SDL_Color c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	struct font *f = &fonts[fs];
	int pen = x, base = y + (int) (f->px * 0.9f);

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
	SDL_SetTextureAlphaMod(f->tex, c.a);
	for (const unsigned char *p = (const unsigned char *) buf; *p;) {
		uint32_t cp;
		int n = 1, gi;
		struct glyph *g;
		SDL_Rect src, dst;

		if (*p == '\n') {
			pen = x;
			base += f->line;
			p++;
			continue;
		}
		if (*p < 0x80) cp = *p;
		else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
		else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
		else { cp = '?'; }
		p += n;
		gi = gidx(cp);
		if (gi < 0 || !f->g[gi].ok)
			continue;
		g = &f->g[gi];
		src.x = (int) (g->u0 * 1024); src.y = (int) (g->v0 * 1024); src.w = g->w; src.h = g->h;
		dst.x = pen + g->bx; dst.y = base - g->by; dst.w = g->w; dst.h = g->h;
		SDL_RenderCopy(rend, f->tex, &src, &dst);
		pen += g->adv;
	}
}

static void text_center(int fs, int cx, int y, SDL_Color c, const char *s)
{
	text_at(fs, cx - text_width(fs, s) / 2, y, c, "%s", s);
}

/* ------------------------------------------------------------------------------------------ drawing */
static const SDL_Color WHITE = { 255, 255, 255, 255 }, GREY = { 170, 175, 185, 255 }, GREEN = { 90, 220, 120, 255 },
	RED = { 240, 90, 90, 255 }, YELLOW = { 240, 210, 80, 255 }, CYAN = { 90, 200, 240, 255 };

static void set_color(SDL_Color c) { SDL_SetRenderDrawColor(rend, c.r, c.g, c.b, c.a); }

static void fill_circle(int cx, int cy, int r, SDL_Color c)
{
	set_color(c);
	for (int dy = -r; dy <= r; dy++) {
		int dx = (int) sqrtf((float) (r * r - dy * dy));

		SDL_RenderDrawLine(rend, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

static void ring(int cx, int cy, int r, SDL_Color c)
{
	set_color(c);
	for (int i = 0; i < 96; i++) {
		float a0 = i * 6.2831853f / 96, a1 = (i + 1) * 6.2831853f / 96;

		SDL_RenderDrawLine(rend, cx + (int) (cosf(a0) * r), cy + (int) (sinf(a0) * r),
				   cx + (int) (cosf(a1) * r), cy + (int) (sinf(a1) * r));
	}
}

static void rect(int x, int y, int w, int h, SDL_Color c)
{
	SDL_Rect r = { x, y, w, h };

	set_color(c);
	SDL_RenderFillRect(rend, &r);
}

/* ------------------------------------------------------------------------------------------ analysis */
struct win { float dx, dy, dyaw, dpitch, speed, ryaw, rpitch, sx, sy, camx, camy; };
static struct win wins[MAXWIN];
static double game_dz = 0.15, game_cmax = 150.0;   /* the virtual game: inner stick dead zone and camera speed at full deflection */
static float g_sx, g_sy, g_camx, g_camy;           /* right stick (-1..1) and the virtual camera rate (deg/s) this frame */
static SDL_GameController *pad0;
static int stick_mode;
struct stick_fit { double a, b, rmin, rmax, dead_true, act, corr_yaw, corr_pitch; int n; };
static struct stick_fit sfit;
static double sug_aid, cur_aid, cur_sens_conf;
static int pref_yaw = 1, pref_pitch = 1;   /* direction preference: +1 look-where-you-point, -1 opposite */

static void prefs_load(void)
{
	char line[128];
	FILE *f = fopen(DATA_DIR "/prefs.conf", "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "yaw_sign=", 9)) pref_yaw = atoi(line + 9) < 0 ? -1 : 1;
		if (!strncmp(line, "pitch_sign=", 11)) pref_pitch = atoi(line + 11) < 0 ? -1 : 1;
		if (!strncmp(line, "game_deadzone=", 14)) game_dz = atof(line + 14);
		if (!strncmp(line, "game_max_dps=", 13)) game_cmax = atof(line + 13);
	}
	fclose(f);
}
static int nwin;

static const float bin_edge[NBIN + 1] = { 0.5f, 1, 2, 4, 8, 16, 32, 64, 1e9f };

struct bin_result { int n; double num, den; double k; };

struct rot_result {
	double k_yaw, k_pitch, k_all, k_ref;
	double den_yaw, den_pitch;
	struct bin_result bin[NBIN];
	int n_used;
	double deadzone_dps;        /* highest speed bin edge whose ratio is clearly below 1, 0 if none */
};

/* mouse counts expected = -k * rotation for both axes (yaw: turning right moves the cursor right; pitch: top edge toward you
 * = look up = cursor up). k > 0 means the directions are right. */
static void analyse(struct rot_result *r)
{
	double ny = 0, dy_ = 0, np = 0, dp = 0;

	memset(r, 0, sizeof(*r));
	for (int i = 0; i < nwin; i++) {
		struct win *w = &wins[i];
		int b = -1;

		for (int k = 0; k < NBIN; k++)
			if (w->speed >= bin_edge[k] && w->speed < bin_edge[k + 1]) b = k;
		if (b < 0)
			continue;
		if (fabsf(w->dyaw) > 0.02f) { ny += -pref_yaw * w->dx * w->dyaw; dy_ += w->dyaw * w->dyaw; }
		if (fabsf(w->dpitch) > 0.02f) { np += -pref_pitch * w->dy * w->dpitch; dp += w->dpitch * w->dpitch; }
		r->bin[b].n++;
		r->bin[b].num += -pref_yaw * w->dx * w->dyaw - pref_pitch * w->dy * w->dpitch;
		r->bin[b].den += w->dyaw * w->dyaw + w->dpitch * w->dpitch;
		r->n_used++;
	}
	r->den_yaw = dy_;
	r->den_pitch = dp;
	r->k_yaw = dy_ > 1e-6 ? ny / dy_ : 0;
	r->k_pitch = dp > 1e-6 ? np / dp : 0;
	r->k_all = (dy_ + dp) > 1e-6 ? (ny + np) / (dy_ + dp) : 0;
	{
		double num = 0, den = 0;

		for (int k = 5; k < NBIN; k++) { num += r->bin[k].num; den += r->bin[k].den; }
		r->k_ref = den > 1e-6 ? num / den : r->k_all;
	}
	for (int k = 0; k < NBIN; k++)
		r->bin[k].k = r->bin[k].den > 1e-6 ? r->bin[k].num / r->bin[k].den : 0;
	for (int k = 0; k < 5; k++)
		if (r->bin[k].n >= 4 && fabs(r->k_ref) > 0.01 && r->bin[k].k / r->k_ref < 0.8)
			r->deadzone_dps = bin_edge[k + 1];
}

static double cam_curve(double d)
{
	double a = fabs(d);

	if (a <= game_dz)
		return 0;
	a = (a - game_dz) / (1.0 - game_dz);
	return d < 0 ? -a : a;
}

/* Steam maps the received gyro speed linearly onto the stick: |stick| = a + b |speed| between 0 and 1. Fit it. */
static void analyse_stick(void)
{
	double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, cy = 0, cp = 0;

	memset(&sfit, 0, sizeof(sfit));
	for (int i = 0; i < nwin; i++) {
		struct win *w = &wins[i];
		double pt[2][2] = { { fabs(w->ryaw), fabs(w->sx) }, { fabs(w->rpitch), fabs(w->sy) } };

		sfit.act += fabs(w->sx) + fabs(w->sy);
		cy += -pref_yaw * w->sx * w->dyaw;
		cp += -pref_pitch * w->sy * w->dpitch;
		for (int k = 0; k < 2; k++)
			if (pt[k][0] > 0.5 && pt[k][1] > 0.03 && pt[k][1] < 0.95) {
				n++; sx += pt[k][0]; sy += pt[k][1]; sxx += pt[k][0] * pt[k][0]; sxy += pt[k][0] * pt[k][1];
			}
	}
	sfit.n = (int) n;
	sfit.corr_yaw = cy;
	sfit.corr_pitch = cp;
	if (n >= 8 && n * sxx - sx * sx > 1e-6) {
		sfit.b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
		sfit.a = (sy - sfit.b * sx) / n;
		if (sfit.b > 1e-4) {
			sfit.rmin = -sfit.a / sfit.b;
			sfit.rmax = (1.0 - sfit.a) / sfit.b;
		}
	}
}

/* ------------------------------------------------------------------------------------------ session state */
enum stage { ST_MENU, ST_REST, ST_ROTATE, ST_TARGETS, ST_TRACK, ST_RESULT };
static enum stage stage = ST_MENU;
static double stage_t0, now_t;
static double cur_x, cur_y;             /* cursor position (px) */
static double rest_dx, rest_dy;         /* mouse counts while resting */
static double rest_phys_max;
static struct rot_result rot;
static int rot_done;
static FILE *sessf;

struct tgt_stats { int n, hits, misses; double t_sum, eff_sum; };
static struct tgt_stats tstats[3];
static const int tgt_r[3] = { 70, 36, 18 };
static int tgt_idx, tgt_class_i;
static double tgt_x, tgt_y, tgt_spawn, path_len, tgt_start_x, tgt_start_y;
static int tgt_total;

static double trk_err_sum[3];
static long trk_frames[3], trk_lock[3];
static double trk_x, trk_y, trk_speed;

static int w_open;
static double suggestion_sens, current_sens;
static int flip_yaw, flip_pitch;
static char msg[256];

static double frand(double a, double b) { return a + (b - a) * (rand() / (double) RAND_MAX); }

static void go(enum stage s)
{
	stage = s;
	stage_t0 = now_t;
	if (s == ST_REST) { rest_dx = rest_dy = 0; rest_phys_max = 0; }
	if (s == ST_ROTATE) { nwin = 0; rot_done = 0; w_open = 0; }
	if (s == ST_TARGETS) {
		memset(tstats, 0, sizeof(tstats));
		tgt_idx = 0;
		tgt_total = 18;
		tgt_spawn = -1;
	}
	if (s == ST_TRACK) {
		memset(trk_err_sum, 0, sizeof(trk_err_sum)); memset(trk_frames, 0, sizeof(trk_frames)); memset(trk_lock, 0, sizeof(trk_lock));
	}
	cur_x = win_w / 2.0; cur_y = win_h / 2.0;
}

static void spawn_target(void)
{
	tgt_class_i = (tgt_idx / 6) % 3;
	tgt_x = frand(0.12 * win_w, 0.88 * win_w);
	tgt_y = frand(0.18 * win_h, 0.85 * win_h);
	tgt_spawn = now_t;
	path_len = 0;
	tgt_start_x = cur_x; tgt_start_y = cur_y;
}


/* The settings file Aim Lab reads and writes: the profile named in <data>/target.conf (set by the Decky plugin
 * before launching), otherwise the global proxy.conf. */
static const char *conf_file(void)
{
	static char path[600];
	char line[128];
	FILE *f;

	if (path[0])
		return path;
	snprintf(path, sizeof(path), DATA_DIR "/../motion/proxy.conf");
	f = fopen(DATA_DIR "/target.conf", "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "profile=", 8)) {
				line[strcspn(line, "\r\n")] = 0;
				if (line[8] && !strchr(line + 8, '/'))
					snprintf(path, sizeof(path), DATA_DIR "/../motion/profiles/%s.conf", line + 8);
			}
		fclose(f);
	}
	return path;
}

static void conf_update(const char *key, const char *val)
{
	char lines[64][160];
	int n = 0, found = 0;
	FILE *f = fopen(PROXY_CONF, "r");

	if (f) {
		while (n < 63 && fgets(lines[n], sizeof(lines[n]), f))
			n++;
		fclose(f);
	}
	for (int i = 0; i < n; i++)
		if (!strncmp(lines[i], key, strlen(key)) && lines[i][strlen(key)] == '=') {
			snprintf(lines[i], sizeof(lines[i]), "%s=%s\n", key, val);
			found = 1;
		}
	if (!found && n < 64)
		snprintf(lines[n++], sizeof(lines[0]), "%s=%s\n", key, val);
	char tmp[640];

	snprintf(tmp, sizeof(tmp), "%s.tmp", PROXY_CONF);
	f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++)
		fputs(lines[i], f);
	fclose(f);
	rename(tmp, PROXY_CONF);
}

static int conf_get_invert(const char *key)
{
	char line[160];
	int v = 1;
	FILE *f = fopen(PROXY_CONF, "r");

	if (!f)
		return v;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == '=')
			v = atoi(line + strlen(key) + 1);
	fclose(f);
	return v;
}

static double conf_get_num(const char *key, double def)
{
	char line[160];
	double v = def;
	FILE *f = fopen(PROXY_CONF, "r");

	if (!f)
		return v;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == '=')
			v = atof(line + strlen(key) + 1);
	fclose(f);
	return v;
}

static void compute_suggestion(void)
{
	double target_k = win_w / FOV_DEG;      /* counts (= px) per degree for a 1:1 view */

	current_sens = shm ? snap_read().sens : 1.0;
	if (current_sens < 0.05) current_sens = 1.0;
	flip_yaw = rot.k_yaw < -0.5;
	flip_pitch = rot.k_pitch < -0.5;
	suggestion_sens = rot.k_all > 0.5 || rot.k_all < -0.5 ? current_sens * target_k / fabs(rot.k_all) : current_sens;
	cur_aid = conf_get_num("antideadzone_dps", 0);
	sug_aid = 0;
	{
		double mouse_total = 0;

		for (int i = 0; i < nwin; i++) mouse_total += fabs(wins[i].dx) + fabs(wins[i].dy);
		analyse_stick();
		stick_mode = mouse_total < 50.0 && sfit.act > 3.0 && sfit.rmax > sfit.rmin + 1.0;
	}
	if (stick_mode) {
		double vfull = game_cmax, rr = sfit.rmax - sfit.rmin;

		flip_yaw = sfit.corr_yaw < 0;
		flip_pitch = sfit.corr_pitch < 0;
		suggestion_sens = (1.0 - game_dz) * rr / vfull;              /* slope: camera rate = true rate */
		sug_aid = sfit.rmin + game_dz * rr;                           /* offset: stick sits at the game dead zone edge */
		if (sug_aid < 0) sug_aid = 0;
		if (sug_aid > 60) sug_aid = 60;
		sfit.dead_true = ((sfit.rmin + game_dz * rr) - (cur_aid > 0 ? cur_aid : 0)) / current_sens;
		if (sfit.dead_true < 0) sfit.dead_true = 0;
	}
	if (suggestion_sens < 0.1) suggestion_sens = 0.1;
	if (suggestion_sens > 4.0) suggestion_sens = 4.0;
}

static void apply_suggestion(void)
{
	char b[32];

	snprintf(b, sizeof(b), "%.2f", suggestion_sens);
	conf_update("sensitivity", b);
	snprintf(b, sizeof(b), "%.1f", stick_mode ? sug_aid : 0.0);
	conf_update("antideadzone_dps", b);
	if (flip_yaw) { snprintf(b, sizeof(b), "%d", !conf_get_invert("invert_yaw")); conf_update("invert_yaw", b); }
	if (flip_pitch) { snprintf(b, sizeof(b), "%d", !conf_get_invert("invert_pitch")); conf_update("invert_pitch", b); }
	snprintf(msg, sizeof(msg), "Applied: sensitivity %.2f, anti-dead-zone %.1f deg/s%s%s. Repeat the rotation test to check.", suggestion_sens, stick_mode ? sug_aid : 0.0,
		 flip_yaw ? ", yaw axis inverted" : "", flip_pitch ? ", pitch axis inverted" : "");
}

static void save_result(void)
{
	char path[256];
	FILE *f;

	mkdir(DATA_DIR, 0755);
	snprintf(path, sizeof(path), DATA_DIR "/last_result.json");
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "{\n  \"time\": %ld,\n  \"rest_creep_counts_per_s\": %.3f,\n  \"k_yaw\": %.3f, \"k_pitch\": %.3f, \"k_all\": %.3f, \"k_ref\": %.3f,\n",
		(long) time(NULL), hypot(rest_dx, rest_dy) / 4.0, rot.k_yaw, rot.k_pitch, rot.k_all, rot.k_ref);
	fprintf(f, "  \"deadzone_dps\": %.2f,\n  \"bins\": [", rot.deadzone_dps);
	for (int k = 0; k < NBIN; k++)
		fprintf(f, "%s{\"from\": %.1f, \"n\": %d, \"k\": %.3f}", k ? ", " : "", bin_edge[k], rot.bin[k].n, rot.bin[k].k);
	fprintf(f, "],\n  \"targets\": [");
	for (int c = 0; c < 3; c++)
		fprintf(f, "%s{\"radius\": %d, \"hits\": %d, \"misses\": %d, \"mean_time\": %.3f, \"path_eff\": %.3f}", c ? ", " : "", tgt_r[c],
			tstats[c].hits, tstats[c].misses, tstats[c].hits ? tstats[c].t_sum / tstats[c].hits : 0.0,
			tstats[c].hits ? tstats[c].eff_sum / tstats[c].hits : 0.0);
	fprintf(f, "],\n  \"tracking\": [");
	for (int c = 0; c < 3; c++)
		fprintf(f, "%s{\"mean_err_px\": %.1f, \"lock_pct\": %.1f}", c ? ", " : "", trk_frames[c] ? trk_err_sum[c] / trk_frames[c] : 0.0,
			trk_frames[c] ? 100.0 * trk_lock[c] / trk_frames[c] : 0.0);
	fprintf(f, "],\n  \"suggested_sensitivity\": %.2f, \"flip_yaw\": %d, \"flip_pitch\": %d\n}\n", suggestion_sens, flip_yaw, flip_pitch);
	fclose(f);
}

/* ------------------------------------------------------------------------------------------ frame loop */
static int sdl_fire_edge, sdl_back_edge, sdl_quit, sdl_center_edge;
static double mdx, mdy;                 /* mouse counts since last frame */
static double w_dx, w_dy, w_t0, w_a_yaw, w_a_pitch;

static double a_ryaw, a_rpitch, a_sx, a_sy, a_camx, a_camy;
static int a_n;

static void win_feed(const struct snap *s)
{
	if (!w_open) {
		w_t0 = now_t; w_dx = w_dy = 0; w_a_yaw = s->angle[1]; w_a_pitch = s->angle[0]; w_open = 1;
		a_ryaw = a_rpitch = a_sx = a_sy = a_camx = a_camy = 0; a_n = 0;
		return;
	}
	w_dx += mdx; w_dy += mdy;
	a_ryaw += s->out[1]; a_rpitch += s->out[0]; a_sx += g_sx; a_sy += g_sy; a_camx += g_camx; a_camy += g_camy; a_n++;
	if (now_t - w_t0 >= WIN_S && s->ok) {
		struct win *w = nwin < MAXWIN ? &wins[nwin++] : NULL;
		double dyaw = s->angle[1] - w_a_yaw, dpitch = s->angle[0] - w_a_pitch, dt = now_t - w_t0;

		if (w) {
			w->dx = (float) w_dx; w->dy = (float) w_dy; w->dyaw = (float) dyaw; w->dpitch = (float) dpitch;
			w->speed = (float) (hypot(dyaw, dpitch) / dt);
			if (a_n > 0) {
				w->ryaw = (float) (a_ryaw / a_n); w->rpitch = (float) (a_rpitch / a_n);
				w->sx = (float) (a_sx / a_n); w->sy = (float) (a_sy / a_n);
				w->camx = (float) (a_camx / a_n); w->camy = (float) (a_camy / a_n);
			}
			if (sessf)
				fprintf(sessf, "win,%.3f,%.1f,%.1f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f\n", now_t, w_dx, w_dy, dyaw, dpitch, w->speed, w->ryaw, w->rpitch, w->sx, w->sy);
		}
		w_t0 = now_t; w_dx = w_dy = 0; w_a_yaw = s->angle[1]; w_a_pitch = s->angle[0];
		a_ryaw = a_rpitch = a_sx = a_sy = a_camx = a_camy = 0; a_n = 0;
	}
}

static void draw_cursor(void)
{
	int x = (int) cur_x, y = (int) cur_y, a = (int) (18 * ui), b = (int) (6 * ui);

	set_color(WHITE);
	SDL_RenderDrawLine(rend, x - a, y, x - b, y); SDL_RenderDrawLine(rend, x + b, y, x + a, y);
	SDL_RenderDrawLine(rend, x, y - a, x, y - b); SDL_RenderDrawLine(rend, x, y + b, x, y + a);
	fill_circle(x, y, (int) (2 * ui) + 1, RED);
}

static void hud(const struct snap *s)
{
	int pad = (int) (24 * ui);

	if (!shm) {
		text_at(0, pad, win_h - (int) (44 * ui), RED, "No data from rp6gyrod (/run/rp6-gyro/motion.bin): is rp6-deckproxy running?");
		return;
	}
	text_at(0, pad, win_h - (int) (44 * ui), GREY, "true speed: yaw %+6.1f  pitch %+6.1f deg/s    in Steam: %+6.1f %+6.1f deg/s   compensation %u   sens %.2f",
		-s->phys[1], s->phys[0], s->out[1], s->out[0], s->comp, s->sens);
}

static void frame_logic(const struct snap *s)
{
	int cx = win_w / 2, W = win_w, H = win_h;
	double t = now_t - stage_t0;

	{
		static double last_t;
		double dt = last_t > 0 ? now_t - last_t : 0.0, ppd = W / FOV_DEG;

		last_t = now_t;
		if (dt > 0.1) dt = 0.1;
		g_camx = (float) (cam_curve(g_sx) * game_cmax);              /* deg/s of the virtual game camera */
		g_camy = (float) (cam_curve(g_sy) * game_cmax);
		cur_x += g_camx * dt * ppd; cur_y += g_camy * dt * ppd;
	}
	cur_x += mdx; cur_y += mdy;
	if (cur_x < 0) cur_x = 0; if (cur_x > W) cur_x = W;
	if (cur_y < 0) cur_y = 0; if (cur_y > H) cur_y = H;
	if (sdl_center_edge) { cur_x = W / 2.0; cur_y = H / 2.0; }

	switch (stage) {
	case ST_MENU:
		text_center(2, cx, (int) (120 * ui), WHITE, "RP6 Aim Lab");
		text_center(1, cx, (int) (230 * ui), GREY, "Gyro calibration and aim training");
		text_at(1, cx - (int) (560 * ui), (int) (340 * ui), WHITE,
			"1. Rest: console still on a table, checks cursor drift\n"
			"2. Rotation: turn the console slowly and quickly; measures sensitivity\n    and speed dead zone and checks the axis directions\n"
			"3. Targets: hit circles of three sizes\n"
			"4. Tracking: keep the cursor on a moving target\n"
			"5. Result: recommendation, applied to the active profile");
		text_center(1, cx, (int) (800 * ui), GREEN, "A / LMB / RT - start     X - center cursor     Start - quit");
		{
			char tb[700];

			snprintf(tb, sizeof(tb), "Results are written to: %s", conf_file());
			text_center(0, cx, (int) (850 * ui), GREY, tb);
		}
		if (msg[0]) text_center(0, cx, (int) (900 * ui), YELLOW, msg);
		if (sdl_fire_edge) go(ST_REST);
		break;
	case ST_REST:
		rest_dx += mdx; rest_dy += mdy;
		if (s->ok && fabs(s->phys[1]) + fabs(s->phys[0]) > rest_phys_max) rest_phys_max = fabs(s->phys[1]) + fabs(s->phys[0]);
		text_center(2, cx, (int) (140 * ui), WHITE, "Rest");
		text_center(1, cx, (int) (260 * ui), GREY, "Put the console on a table and do not touch it");
		{
			char b[64];

			snprintf(b, sizeof(b), "%.1f s", 4.0 - t > 0 ? 4.0 - t : 0.0);
			text_center(2, cx, (int) (420 * ui), CYAN, b);
		}
		text_center(0, cx, (int) (560 * ui), GREY, "The cursor must not creep while the console is still");
		if (t >= 4.0) go(ST_ROTATE);
		break;
	case ST_ROTATE: {
		double sp = s->ok ? hypot(s->phys[1], s->phys[0]) : 0;
		char b[128];

		win_feed(s);
		text_center(2, cx, (int) (90 * ui), WHITE, "Rotation");
		text_center(1, cx, (int) (200 * ui), GREY, "Turn the console left-right and up-down.");
		text_center(1, cx, (int) (250 * ui), GREY, "First very slowly (5-15 deg/s), then medium, then fast. At least 25 s.");
		snprintf(b, sizeof(b), "speed %.1f deg/s   stick %+.2f %+.2f   windows %d   time %.0f s", sp, g_sx, g_sy, nwin, t);
		text_center(1, cx, (int) (360 * ui), CYAN, b);
		{
			int bx = cx - (int) (500 * ui), bw = (int) (1000 * ui), h = (int) (30 * ui);
			float f = (float) (log10(sp + 1.0) / 2.5);

			rect(bx, (int) (450 * ui), bw, h, (SDL_Color) { 40, 45, 55, 255 });
			rect(bx, (int) (450 * ui), (int) (bw * (f > 1 ? 1 : f)), h, sp < 16 ? YELLOW : GREEN);
		}
		{
			struct rot_result tmp;

			analyse(&tmp);
			snprintf(b, sizeof(b), "so far: yaw %+.1f  pitch %+.1f  total %+.1f  counts/deg", tmp.k_yaw, tmp.k_pitch, tmp.k_all);
			text_center(1, cx, (int) (560 * ui), WHITE, b);
		}
		text_center(0, cx, (int) (680 * ui), GREY, "A - finish (after 25 s)     B - skip");
		if ((sdl_fire_edge && t >= 25.0) || sdl_back_edge) {
			analyse(&rot);
			rot_done = 1;
			compute_suggestion();
			go(ST_TARGETS);
		}
		break;
	}
	case ST_TARGETS: {
		struct tgt_stats *st;

		if (tgt_spawn < 0) spawn_target();
		st = &tstats[tgt_class_i];
		path_len += hypot(mdx, mdy);
		fill_circle((int) tgt_x, (int) tgt_y, tgt_r[tgt_class_i], (SDL_Color) { 230, 80, 80, 255 });
		fill_circle((int) tgt_x, (int) tgt_y, tgt_r[tgt_class_i] * 2 / 3, WHITE);
		fill_circle((int) tgt_x, (int) tgt_y, tgt_r[tgt_class_i] / 3 + 1, (SDL_Color) { 230, 80, 80, 255 });
		text_at(0, (int) (24 * ui), (int) (20 * ui), GREY, "Targets %d/%d   A / RT / LMB - fire   B - skip stage", tgt_idx + 1, tgt_total);
		if (sdl_fire_edge) {
			double d = hypot(cur_x - tgt_x, cur_y - tgt_y), straight = hypot(tgt_x - tgt_start_x, tgt_y - tgt_start_y);

			st->n++;
			if (d <= tgt_r[tgt_class_i]) {
				st->hits++;
				st->t_sum += now_t - tgt_spawn;
				st->eff_sum += path_len > 1 ? straight / path_len : 1.0;
				if (sessf) fprintf(sessf, "hit,%.3f,%d,%.2f,%.2f\n", now_t, tgt_r[tgt_class_i], now_t - tgt_spawn, d);
			} else {
				st->misses++;
				if (sessf) fprintf(sessf, "miss,%.3f,%d,%.2f\n", now_t, tgt_r[tgt_class_i], d);
			}
			if (++tgt_idx >= tgt_total) go(ST_TRACK); else tgt_spawn = -1;
		} else if (now_t - tgt_spawn > 6.0) {
			st->n++; st->misses++;
			if (++tgt_idx >= tgt_total) go(ST_TRACK); else tgt_spawn = -1;
		}
		if (sdl_back_edge) go(ST_TRACK);
		break;
	}
	case ST_TRACK: {
		double ph = t, sp = 0.35 + 0.9 * (t / 24.0), a;
		int cls;
		double err;

		trk_x = cx + 0.33 * W * sin(0.9 * sp * ph);
		trk_y = H * 0.5 + 0.27 * H * sin(1.3 * sp * ph + 1.0);
		a = 0.33 * W * 0.9 * sp;
		trk_speed = a;                     /* rough px/s of the dominant component */
		cls = trk_speed < 250 ? 0 : trk_speed < 480 ? 1 : 2;
		fill_circle((int) trk_x, (int) trk_y, 40, (SDL_Color) { 230, 160, 60, 255 });
		fill_circle((int) trk_x, (int) trk_y, 22, WHITE);
		err = hypot(cur_x - trk_x, cur_y - trk_y);
		trk_err_sum[cls] += err; trk_frames[cls]++; if (err < 40) trk_lock[cls]++;
		if (sessf) fprintf(sessf, "trk,%.3f,%.1f,%.1f,%.1f,%.1f\n", now_t, trk_x, trk_y, cur_x, cur_y);
		text_at(0, (int) (24 * ui), (int) (20 * ui), GREY, "Tracking: keep the cursor on the target   %.0f/24 s   B - skip", t);
		if (t >= 24.0 || sdl_back_edge) { compute_suggestion(); save_result(); go(ST_RESULT); }
		break;
	}
	case ST_RESULT: {
		int y = (int) (50 * ui), x = (int) (60 * ui), dz = rot.deadzone_dps > 0;
		double k1 = win_w / FOV_DEG;

		text_at(2, x, y, WHITE, "Result");
		y += (int) (90 * ui);
		text_at(1, x, y, GREY, "Rest: creeps %.2f counts/s (normal < 0.5)   max true-speed noise %.2f deg/s", hypot(rest_dx, rest_dy) / 4.0, rest_phys_max);
		y += (int) (50 * ui);
		text_at(1, x, y, (rot.k_all > 0.5) ? GREEN : RED, "Rotation: %+.1f counts/deg (yaw %+.1f, pitch %+.1f); 1:1 at FOV %.0f deg needs %.1f",
			rot.k_all, rot.k_yaw, rot.k_pitch, FOV_DEG, k1);
		y += (int) (50 * ui);
		text_at(1, x, y, flip_yaw || flip_pitch ? RED : GREEN, "Axes: yaw %s, pitch %s", rot.k_yaw > 0.5 ? "correct" : rot.k_yaw < -0.5 ? "INVERTED" : "no data",
			rot.k_pitch > 0.5 ? "correct" : rot.k_pitch < -0.5 ? "INVERTED" : "no data");
		y += (int) (50 * ui);
		if (dz) text_at(1, x, y, RED, "Speed dead zone: up to %.1f deg/s (response below 80%% of normal)", rot.deadzone_dps);
		else text_at(1, x, y, GREEN, "No speed dead zone visible");
		y += (int) (60 * ui);
		if (stick_mode) {
			double rr = sfit.rmax - sfit.rmin;

			text_at(1, x, y, CYAN, "Camera-stick mode. Steam: stick = 0 at %.1f deg/s, full at %.1f deg/s (fit over %d windows)", sfit.rmin, sfit.rmax, sfit.n);
			y += (int) (44 * ui);
			text_at(0, x, y, GREY, "Virtual game: dead zone %.0f%%, camera %.0f deg/s at full deflection (prefs.conf: game_deadzone, game_max_dps).  The camera starts moving at a true speed of %.1f deg/s (current anti-dead-zone %.1f)",
				game_dz * 100, game_cmax, sfit.dead_true, cur_aid);
			y += (int) (40 * ui);
			text_at(0, x, y, (sfit.corr_yaw > 0 && sfit.corr_pitch > 0) ? GREEN : RED, "Directions: yaw %s, pitch %s", sfit.corr_yaw > 0 ? "correct" : "INVERTED", sfit.corr_pitch > 0 ? "correct" : "INVERTED");
			y += (int) (40 * ui);
			{
				static const float e[7] = { 1, 2, 4, 8, 16, 32, 1e9f };
				double sc[6] = { 0 }, st[6] = { 0 };
				int cn[6] = { 0 };

				for (int i = 0; i < nwin; i++)
					for (int k = 0; k < 6; k++)
						if (wins[i].speed >= e[k] && wins[i].speed < e[k + 1]) {
							sc[k] += hypot(wins[i].camx, wins[i].camy); st[k] += wins[i].speed; cn[k]++;
						}
				for (int k = 0; k < 6; k++) {
					double r = st[k] > 0 ? sc[k] / st[k] : 0;
					char lab[48];

					if (k < 5) snprintf(lab, sizeof(lab), "%.0f-%.0f deg/s", e[k], e[k + 1]); else snprintf(lab, sizeof(lab), "%.0f+ deg/s", e[k]);
					text_at(0, x, y, GREY, "camera/true %s (windows %d): %.2f", lab, cn[k], r);
					rect(x + (int) (560 * ui), y + 4, (int) (300 * ui * (r > 1.5 ? 1.5 : r) / 1.5), (int) (18 * ui), cn[k] < 3 ? (SDL_Color) { 70, 75, 85, 255 } : (r < 0.8 ? RED : (r > 1.25 ? YELLOW : GREEN)));
					y += (int) (28 * ui);
				}
			}
			text_at(1, x, y, YELLOW, "Recommendation: sensitivity %.2f, anti-dead-zone %.1f deg/s", suggestion_sens, sug_aid);
			y += (int) (60 * ui);
			(void) rr;
		}
		for (int k = 0; k < NBIN && !stick_mode; k++) {
			float ratio = fabs(rot.k_ref) > 0.01 ? (float) (rot.bin[k].k / rot.k_ref) : 0;
			int bw = (int) (500 * ui * (ratio > 1.5f ? 1.5f : ratio < 0 ? 0 : ratio) / 1.5f);
			char b[64];

			if (k < NBIN - 1) snprintf(b, sizeof(b), "%.1f-%.0f deg/s", bin_edge[k], bin_edge[k + 1]); else snprintf(b, sizeof(b), "%.0f+ deg/s", bin_edge[k]);
			text_at(0, x, y, GREY, "%s  (windows %d)", b, rot.bin[k].n);
			rect(x + (int) (330 * ui), y + 4, bw, (int) (18 * ui), rot.bin[k].n < 4 ? (SDL_Color) { 70, 75, 85, 255 } : (ratio < 0.8f ? RED : GREEN));
			y += (int) (30 * ui);
		}
		y += (int) (10 * ui);
		for (int c = 0; c < 3; c++)
			text_at(0, x, y + c * (int) (30 * ui), GREY, "Target r=%d: hits %d/%d, mean time %.2f s, path straightness %.0f%%   Tracking: error %.0f px, lock %.0f%%",
				tgt_r[c], tstats[c].hits, tstats[c].n, tstats[c].hits ? tstats[c].t_sum / tstats[c].hits : 0.0,
				tstats[c].hits ? 100.0 * tstats[c].eff_sum / tstats[c].hits : 0.0, trk_frames[c] ? trk_err_sum[c] / trk_frames[c] : 0.0,
				trk_frames[c] ? 100.0 * trk_lock[c] / trk_frames[c] : 0.0);
		y += (int) (110 * ui);
		text_at(1, x, y, YELLOW, "Recommendation: sensitivity %.2f (now %.2f)%s%s", suggestion_sens, current_sens,
			flip_yaw ? ", invert yaw" : "", flip_pitch ? ", invert pitch" : "");
		y += (int) (60 * ui);
		text_at(1, x, y, GREEN, "A - apply to the profile     X - repeat rotation     B - start over     Start - quit");
		if (msg[0]) text_at(0, x, y + (int) (50 * ui), YELLOW, "%s", msg);
		if (sdl_fire_edge) apply_suggestion();
		if (sdl_center_edge) go(ST_ROTATE);
		if (sdl_back_edge) { msg[0] = 0; go(ST_MENU); }
		break;
	}
	}
}

/* ------------------------------------------------------------------------------------------ self test */
static int selftest(void)
{
	/* synthetic: true k = 21.3 counts/deg, but a dead zone below 3 deg/s (output 0) and a yaw inversion */
	struct rot_result r;

	srand(1);
	nwin = 0;
	for (int i = 0; i < 1500; i++) {
		double sp = pow(10.0, frand(-0.2, 2.0)), ang = frand(-1, 1) > 0 ? 1 : -1, dyaw = ang * sp * WIN_S, dpitch = 0.3 * dyaw;
		double k = sp < 3.0 ? 0.0 : 21.3;

		wins[nwin].dyaw = (float) dyaw; wins[nwin].dpitch = (float) dpitch;
		wins[nwin].dx = (float) (-k * dyaw * (-1)); /* inverted yaw: moves the wrong way */
		wins[nwin].dy = (float) (-k * dpitch);
		wins[nwin].speed = (float) hypot(dyaw, dpitch) / WIN_S;
		nwin++;
	}
	analyse(&r);
	printf("k_yaw %.2f (expect -21.3) k_pitch %.2f (expect ~+21.3) deadzone_dps %.1f (expect 4.0 edge above 3)\n", r.k_yaw, r.k_pitch, r.deadzone_dps);
	for (int k = 0; k < NBIN; k++) printf("  bin %d n=%d k=%.2f\n", k, r.bin[k].n, r.bin[k].k);
	/* stick model: Steam stick = clamp((received - 3) / 40); all rotations are real, so pref signs are right */
	nwin = 0;
	for (int i = 0; i < 1500; i++) {
		double sp = frand(0.3, 70), sg = frand(-1, 1) > 0 ? 1 : -1, rv = sg * sp, st_ = fabs(rv) - 3.0;

		st_ = st_ < 0 ? 0 : st_ / 40.0;
		if (st_ > 1) st_ = 1;
		memset(&wins[nwin], 0, sizeof(wins[nwin]));
		wins[nwin].ryaw = (float) (-rv);        /* received yaw: turning right = negative */
		wins[nwin].sx = (float) (sg * st_ + frand(-0.01, 0.01));
		wins[nwin].dyaw = (float) (-sg * sp * WIN_S);  /* true rotation consistent with the stick */
		wins[nwin].speed = (float) sp;
		nwin++;
	}
	analyse_stick();
	printf("stick fit: rmin %.2f (expect 3), rmax %.2f (expect 43), n %d, corr_yaw %+.1f (positive = right direction)\n", sfit.rmin, sfit.rmax, sfit.n, sfit.corr_yaw);
	return 0;
}

int main(int argc, char **argv)
{
	SDL_Window *win;
	SDL_GameController *pads[4] = { 0 };
	int frames = 0, max_frames = 0;
	char path[256];
	int windowed = getenv("RP6AIM_WINDOWED") != NULL;
	Uint64 freq;

	if (argc > 1 && !strcmp(argv[1], "--selftest"))
		return selftest();
	if (argc > 2 && !strcmp(argv[1], "--frames"))
		max_frames = atoi(argv[2]);
	srand((unsigned) time(NULL));
	mkdir(DATA_DIR, 0755);
	snprintf(path, sizeof(path), DATA_DIR "/session-%ld.csv", (long) time(NULL));
	sessf = fopen(path, "w");
	shm_attach();
	prefs_load();

	SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");      /* touches must not look like gyro mouse motion */
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("RP6 Aim Lab", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
			       windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP);
	if (!win) {
		fprintf(stderr, "window: %s\n", SDL_GetError());
		return 1;
	}
	rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!rend)
		rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!rend) {
		fprintf(stderr, "renderer: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GetRendererOutputSize(rend, &win_w, &win_h);
	ui = win_h / 1080.0f;
	if (fonts_init())
		return 1;
	SDL_SetRelativeMouseMode(SDL_TRUE);
	SDL_ShowCursor(SDL_DISABLE);
	for (int i = 0, p = 0; i < SDL_NumJoysticks() && p < 4; i++)
		if (SDL_IsGameController(i))
			pads[p++] = SDL_GameControllerOpen(i);
	freq = SDL_GetPerformanceFrequency();
	cur_x = win_w / 2.0; cur_y = win_h / 2.0;
	go(ST_MENU);
	if (sessf)
		fprintf(sessf, "# rp6aim session; win=%dx%d\n", win_w, win_h);
	{
		Uint64 t0 = SDL_GetPerformanceCounter();
		int rt_prev = 0;

		while (!sdl_quit && !(max_frames && frames >= max_frames)) {
			SDL_Event e;
			struct snap s;

			mdx = mdy = 0;
			sdl_fire_edge = sdl_back_edge = sdl_center_edge = 0;
			while (SDL_PollEvent(&e)) {
				switch (e.type) {
				case SDL_QUIT: sdl_quit = 1; break;
				case SDL_MOUSEMOTION:
					if (e.motion.which == SDL_TOUCH_MOUSEID) break;
					mdx += e.motion.xrel; mdy += e.motion.yrel;
					break;
				case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) sdl_fire_edge = 1; if (e.button.button == SDL_BUTTON_RIGHT) sdl_back_edge = 1; break;
				case SDL_KEYDOWN:
					if (e.key.repeat) break;
					if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE) sdl_fire_edge = 1;
					if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_BACKSPACE) sdl_back_edge = 1;
					if (e.key.keysym.sym == SDLK_c) sdl_center_edge = 1;
					if (e.key.keysym.sym == SDLK_q) sdl_quit = 1;
					break;
				case SDL_CONTROLLERBUTTONDOWN:
					if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) sdl_fire_edge = 1;
					if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) sdl_back_edge = 1;
					if (e.cbutton.button == SDL_CONTROLLER_BUTTON_X) sdl_center_edge = 1;
					if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) sdl_quit = 1;
					break;
				case SDL_CONTROLLERDEVICEADDED:
					for (int p = 0; p < 4; p++) if (!pads[p]) { pads[p] = SDL_GameControllerOpen(e.cdevice.which); break; }
					break;
				case SDL_CONTROLLERAXISMOTION:
					if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
						int down = e.caxis.value > 16000;

						if (down && !rt_prev) sdl_fire_edge = 1;
						rt_prev = down;
					}
					break;
				}
			}
			pad0 = NULL;
			for (int p = 0; p < 4; p++) if (pads[p]) { pad0 = pads[p]; break; }
			g_sx = pad0 ? SDL_GameControllerGetAxis(pad0, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f : 0.0f;
			g_sy = pad0 ? SDL_GameControllerGetAxis(pad0, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f : 0.0f;
			now_t = (SDL_GetPerformanceCounter() - t0) / (double) freq;
			s = snap_read();
			SDL_SetRenderDrawColor(rend, 14, 17, 24, 255);
			SDL_RenderClear(rend);
			frame_logic(&s);
			if (stage == ST_TARGETS || stage == ST_TRACK || stage == ST_MENU || stage == ST_RESULT)
				draw_cursor();
			hud(&s);
			SDL_RenderPresent(rend);
			if (sessf && (frames % 6 == 0) && s.ok)
				fprintf(sessf, "f,%.3f,%d,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", now_t, stage, mdx, mdy, s.phys[0], s.phys[1], s.out[0], s.out[1], g_sx, g_sy);
			frames++;
		}
	}
	if (sessf)
		fclose(sessf);
	SDL_Quit();
	return 0;
}
