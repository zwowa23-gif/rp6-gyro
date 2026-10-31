/*
 * rp6gyrod: Steam gyro for the Retroid Pocket 6.
 *
 * rp6imu (libssc, in a container) streams raw QMI8658 samples over UDP 127.0.0.1:47811 as
 * { u8 type; f32 x, y, z } (type 0 = gyro rad/s, 1 = accel m/s^2, raw sensor axes). This daemon
 * - turns them into Steam Deck IMU fields (orientation, gyro bias tracking, accel gain/low-pass,
 *   sensitivity, per-axis inversion),
 * - wraps InputPlumber's deck-uhid controller (28de:12f0) in an identical uhid copy that carries those
 *   fields and forwards every other report byte for byte; the original is hidden while this runs
 *   (udev rule 99-rp6-gyro-proxy.rules keyed on FLAG),
 * - feeds a uinput accelerometer for desktop auto-rotation.
 * Settings: proxy.conf (key=value, written by the Decky plugin), re-read when it changes.
 * Status: /run/rp6-gyro/status.json. Needs root (/dev/uhid, hidraw, uinput).
 *
 * Deck input report (SDL_hidapi_steamdeck): u16 version, u8 type=9, u8 len=64, u32 seq, u64 buttons,
 * s16 pads x4 @16, accel xyz @24, gyro xyz @30, quaternion @36, triggers @44/46, sticks @48..54.
 * Units (hid-steam): 16384 per g, 16 per deg/s.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONF "/var/lib/rp6-gyro/motion/proxy.conf"
#define STATUS_DIR "/run/rp6-gyro"
#define FLAG "/run/rp6-gyro-proxy.active"
#define PHYS "rp6-gyro-proxy"
#define PORT 47811
#define G 9.80665f
#define RAD2DEG 57.29578f
#define ACCEL_PER_G 16384.0f
#define GYRO_PER_DPS 16.0f
#define OFF_ACCEL 24
#define WIN 40

static const float ORIENT[3] = { -1.0f, -1.0f, 1.0f };   /* sensor -> Android frame (vendor qmi8658 json: x:-x y:-y z:+z) */
static volatile sig_atomic_t stop;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------------------------ config */
struct config {
	int enabled;
	int compensation;           /* 0 = raw sensor; 2 = fixed bias (measured once / on request, persisted), nothing else; 1 = adaptive bias + accel gain/filter */
	float sensitivity;
	float aid_dps, aid_gate;    /* anti-dead-zone for gyro-as-joystick: add aid_dps to the received speed once it exceeds aid_gate */
	int invert[3];              /* pitch (Deck gyro x), yaw (y), roll (z) */
	float smooth_threshold;     /* deg/s, soft tiered smoothing upper threshold (lower = half); 0 = off */
	float smooth_time;          /* s, rolling-average window of the smoothed part */
	float tighten_threshold;    /* deg/s, tightening: below it the speed is scaled by speed/threshold; 0 = off */
	float accel_gain[3];        /* per Android axis: |a| must read 1 g at rest */
	float accel_alpha;
	float m[3][3];              /* Android frame -> Deck frame */
	int rotation_accel;
	long calibrate_seq;
	int mouse_mode;             /* profile mode: 1 = gyro drives a mouse (no anti-dead-zone), 0 = gyro drives a stick */
};

static void conf_defaults(struct config *c)
{
	static const float m[3][3] = { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };   /* landscape: right=-y, up=x */

	memset(c, 0, sizeof(*c));
	c->enabled = 1;
	c->sensitivity = 1.0f;
	c->aid_gate = 0.5f;
	c->invert[0] = c->invert[1] = c->invert[2] = 1;
	c->smooth_time = 0.05f;
	c->accel_gain[0] = c->accel_gain[1] = 1.0f;
	c->accel_gain[2] = 1.0377f;
	c->accel_alpha = 0.08f;
	memcpy(c->m, m, sizeof(m));
	c->rotation_accel = 1;
}

static void conf_parse(struct config *c, const char *path)
{
	char line[256];
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *k = line, *v;

		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(k, "enabled"))
			c->enabled = atoi(v);
		else if (!strcmp(k, "compensation"))
			c->compensation = atoi(v);
		else if (!strcmp(k, "sensitivity"))
			c->sensitivity = strtof(v, NULL);
		else if (!strcmp(k, "antideadzone_dps"))
			c->aid_dps = strtof(v, NULL);
		else if (!strcmp(k, "antideadzone_gate"))
			c->aid_gate = strtof(v, NULL);
		else if (!strcmp(k, "invert_pitch"))
			c->invert[0] = atoi(v);
		else if (!strcmp(k, "invert_yaw"))
			c->invert[1] = atoi(v);
		else if (!strcmp(k, "invert_roll"))
			c->invert[2] = atoi(v);
		else if (!strcmp(k, "smooth_threshold"))
			c->smooth_threshold = strtof(v, NULL);
		else if (!strcmp(k, "smooth_time"))
			c->smooth_time = strtof(v, NULL);
		else if (!strcmp(k, "tighten_threshold"))
			c->tighten_threshold = strtof(v, NULL);
		else if (!strcmp(k, "accel_gain"))
			sscanf(v, "%f,%f,%f", &c->accel_gain[0], &c->accel_gain[1], &c->accel_gain[2]);
		else if (!strcmp(k, "accel_alpha"))
			c->accel_alpha = strtof(v, NULL);
		else if (!strcmp(k, "matrix"))
			sscanf(v, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &c->m[0][0], &c->m[0][1], &c->m[0][2],
			       &c->m[1][0], &c->m[1][1], &c->m[1][2], &c->m[2][0], &c->m[2][1], &c->m[2][2]);
		else if (!strcmp(k, "rotation_accel"))
			c->rotation_accel = atoi(v);
		else if (!strcmp(k, "calibrate_seq"))
			c->calibrate_seq = atol(v);
		else if (!strcmp(k, "mode"))
			c->mouse_mode = !strcmp(v, "mouse");
	}
	fclose(f);
}

/* Global settings first, then the active game's profile on top (profile keys win). */
static void conf_load(struct config *c, const char *path, const char *profile)
{
	conf_defaults(c);
	conf_parse(c, path);
	if (profile && *profile)
		conf_parse(c, profile);
	if (c->mouse_mode)
		c->aid_dps = 0;        /* the anti-dead-zone only makes sense for a stick */
	if (c->sensitivity < 0.05f || c->sensitivity > 10.0f)
		c->sensitivity = 1.0f;
	if (c->aid_dps < 0 || c->aid_dps > 200.0f)
		c->aid_dps = 0;
	if (c->aid_gate < 0.05f || c->aid_gate > 10.0f)
		c->aid_gate = 0.5f;
	if (c->smooth_time < 0.0025f || c->smooth_time > 0.5f)
		c->smooth_time = 0.05f;
}


/* ------------------------------------------------------------------------------------------ profiles */
/*
 * The running game is the "AppId=<n>" argument of Steam's "SteamLaunch" reaper process. Its profile lives in
 * <profile_dir>/<n>.conf; without one, <profile_dir>/default.conf is used if present.
 */
static long active_appid(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	long id = 0;

	if (!d)
		return 0;
	while (!id && (e = readdir(d))) {
		char path[64], buf[2048];
		ssize_t n;
		int fd, i;

		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		for (i = 0; i < n; i++)
			if (!buf[i])
				buf[i] = ' ';
		buf[n] = 0;
		if (strstr(buf, "SteamLaunch") && strstr(buf, "reaper")) {
			char *p = strstr(buf, "AppId=");

			if (p)
				id = atol(p + 6);
		}
	}
	closedir(d);
	return id;
}

static void profile_path(char *out, size_t len, const char *dir, long appid)
{
	struct stat st;

	out[0] = 0;
	if (appid) {
		snprintf(out, len, "%s/%ld.conf", dir, appid);
		if (stat(out, &st) == 0)
			return;
	}
	snprintf(out, len, "%s/default.conf", dir);
	if (stat(out, &st) != 0)
		out[0] = 0;
}

/* ------------------------------------------------------------------------------------------ motion */
/*
 * Measured on the RP6 (2026-10-31): gyro noise sigma 0.11-0.17 deg/s at 400 Hz, resolution 1/32 deg/s,
 * bias ~1-3 deg/s; accel noise 1-2.5 mg. White noise integrates to ~0.01 deg of aim error per second, so the
 * output is NOT filtered by default; a bias error integrates linearly, so the bias is only learned while the
 * console is truly still (GamepadMotionHelpers "stillness" approach): 2 s window whose gyro AND accel
 * peak-to-peak stay within an adaptive noise floor. Slow deliberate aiming must never be absorbed as drift.
 */
#define BLOCK 200                   /* stillness block: 0.5 s at 400 Hz */
#define NBLOCK 4                    /* stillness window: 2 s */
#define SMOOTH_MAX 200
#define DEG2RAD (1.0f / RAD2DEG)
#define STILL_FACTOR 1.6f           /* 2 s window p-p vs the 0.5 s block noise floor */
#define FLOOR_RISE_G (0.01f * DEG2RAD)    /* per block: lets the floor re-learn after temperature changes */
#define FLOOR_RISE_A 0.005f               /* m/s^2 per block */
#define BIAS_GUARD (0.6f * DEG2RAD) /* a bigger jump needs LONG_STILL of stillness (could be slow panning) */
#define LONG_STILL 6.0

struct block {
	float gmin[3], gmax[3], amin[3], amax[3];
	double gsum[3];
	int n, na;
};

struct motion {
	struct config *c;
	float gm[3][3];             /* matrix with inversion and sensitivity folded in */
	float bias[3];
	struct block cur, blk[NBLOCK];
	int nblk, bpos, still, calibrated;
	float floor_g[3], floor_a[3];
	double still_since, cal_time;
	unsigned long n_gyro, n_accel;
	float sbuf[SMOOTH_MAX][3], ssum[3];
	int spos, slen;
	float gsum[3];
	int gn;
	float gyro[3];              /* Deck frame, rad/s, averaged per HID frame */
	float accel[3];             /* Deck frame, m/s^2, low-passed */
	int accel_primed;
	float android_accel[3];
	float phys[3];              /* deg/s, Deck frame, bias removed, no inversion/sensitivity */
	double angle[3];            /* deg */
	double last_t;
	int calibrating;
	double calib_deadline;
	double csum[3];
	long cn;
	long calib_seq;
};

/*
 * Ground truth for the aim trainer (rp6aim): /run/rp6-gyro/motion.bin, seqlock (seq odd while writing).
 * Deck frame: x right, y up, z toward the player. phys = real rotation of the console (bias removed, no inversion,
 * no sensitivity); out = what is sent to Steam; angle = phys integrated since the daemon started (deg).
 */
struct shared_motion {
	uint32_t seq, magic;
	double t;                   /* CLOCK_MONOTONIC seconds */
	float phys_dps[3], out_dps[3];
	double angle_deg[3];
	uint32_t compensation, reports;
	float sensitivity, pad;
};
static struct shared_motion *shm;

static char bias_path[512];

static void bias_save(const struct motion *mo)
{
	FILE *f = fopen(bias_path, "w");

	if (!f)
		return;
	fprintf(f, "%.6f %.6f %.6f\n", mo->bias[0], mo->bias[1], mo->bias[2]);
	fclose(f);
}

static void bias_load(struct motion *mo)
{
	FILE *f = fopen(bias_path, "r");

	if (!f)
		return;
	if (fscanf(f, "%f %f %f", &mo->bias[0], &mo->bias[1], &mo->bias[2]) == 3)
		mo->calibrated = 1, mo->cal_time = now_s();
	fclose(f);
}

static void block_reset(struct block *b)
{
	for (int i = 0; i < 3; i++) {
		b->gmin[i] = b->amin[i] = 1e9f;
		b->gmax[i] = b->amax[i] = -1e9f;
		b->gsum[i] = 0;
	}
	b->n = b->na = 0;
}

static void motion_init(struct motion *mo, struct config *c)
{
	memset(mo, 0, sizeof(*mo));
	mo->c = c;
	block_reset(&mo->cur);
	for (int i = 0; i < 3; i++) {
		mo->floor_g[i] = 1.0f * DEG2RAD;   /* GamepadMotionHelpers starts at 1 deg/s and learns down */
		mo->floor_a[i] = 0.3f;             /* ~30 mg */
	}
}

static void motion_apply_config(struct motion *mo, int first)
{
	struct config *c = mo->c;
	int slen = (int) (c->smooth_time * 400.0f + 0.5f);

	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 3; k++)
			mo->gm[r][k] = c->m[r][k] * (c->invert[r] ? -1.0f : 1.0f) * c->sensitivity;
	slen = slen < 1 ? 1 : slen > SMOOTH_MAX ? SMOOTH_MAX : slen;
	if (slen != mo->slen) {
		mo->slen = slen;
		mo->spos = 0;
		memset(mo->sbuf, 0, sizeof(mo->sbuf));
		memset(mo->ssum, 0, sizeof(mo->ssum));
	}
	if (!first && c->calibrate_seq != mo->calib_seq) {
		mo->calibrating = 1;
		mo->calib_deadline = now_s() + 1.5;
		mo->csum[0] = mo->csum[1] = mo->csum[2] = 0;
		mo->cn = 0;
		fprintf(stderr, "bias calibration requested\n");
	}
	mo->calib_seq = c->calibrate_seq;
}

/* one 0.5 s block done: learn the noise floor and test the 2 s window for stillness */
static void stillness_block(struct motion *mo)
{
	struct block *b = &mo->cur;
	float wgmin[3], wgmax[3], wamin[3], wamax[3], mean[3], diff = 0;
	double now = now_s(), sum[3] = { 0, 0, 0 };
	int still = 1, n = 0;

	for (int i = 0; i < 3; i++) {
		mo->floor_g[i] += FLOOR_RISE_G;
		if (b->gmax[i] - b->gmin[i] < mo->floor_g[i])
			mo->floor_g[i] = b->gmax[i] - b->gmin[i];
		if (b->na) {
			mo->floor_a[i] += FLOOR_RISE_A;
			if (b->amax[i] - b->amin[i] < mo->floor_a[i])
				mo->floor_a[i] = b->amax[i] - b->amin[i];
		}
	}
	mo->blk[mo->bpos] = *b;
	mo->bpos = (mo->bpos + 1) % NBLOCK;
	if (mo->nblk < NBLOCK)
		mo->nblk++;
	block_reset(b);
	if (mo->nblk < NBLOCK)
		return;

	for (int i = 0; i < 3; i++) {
		wgmin[i] = wamin[i] = 1e9f;
		wgmax[i] = wamax[i] = -1e9f;
	}
	for (int k = 0; k < NBLOCK; k++) {
		struct block *w = &mo->blk[k];

		for (int i = 0; i < 3; i++) {
			wgmin[i] = w->gmin[i] < wgmin[i] ? w->gmin[i] : wgmin[i];
			wgmax[i] = w->gmax[i] > wgmax[i] ? w->gmax[i] : wgmax[i];
			wamin[i] = w->amin[i] < wamin[i] ? w->amin[i] : wamin[i];
			wamax[i] = w->amax[i] > wamax[i] ? w->amax[i] : wamax[i];
			sum[i] += w->gsum[i];
		}
		n += w->n;
	}
	for (int i = 0; i < 3; i++)
		if (wgmax[i] - wgmin[i] > mo->floor_g[i] * STILL_FACTOR ||
		    wamax[i] - wamin[i] > mo->floor_a[i] * STILL_FACTOR)
			still = 0;
	mo->still = still;
	if (!still) {
		mo->still_since = 0;
		return;
	}
	if (mo->still_since == 0)
		mo->still_since = now - NBLOCK * 0.5;
	for (int i = 0; i < 3; i++) {
		mean[i] = sum[i] / n;
		if (fabsf(mean[i] - mo->bias[i]) > diff)
			diff = fabsf(mean[i] - mo->bias[i]);
	}
	if (mo->c->compensation == 2 && mo->calibrated)
		return;                 /* fixed bias: measured once, only changed by an explicit calibration */
	if (!mo->calibrated) {
		memcpy(mo->bias, mean, sizeof(mean));
		mo->calibrated = 1;
		bias_save(mo);
	} else if (diff < BIAS_GUARD || now - mo->still_since >= LONG_STILL) {
		for (int i = 0; i < 3; i++)
			mo->bias[i] += 0.3f * (mean[i] - mo->bias[i]);
	} else {
		return;
	}
	mo->cal_time = now;
}

static void gyro_sample(struct motion *mo, const float raw[3])
{
	struct config *c = mo->c;
	struct block *b = &mo->cur;
	float v[3], g[3], d[3], speed;

	mo->n_gyro++;
	for (int i = 0; i < 3; i++) {
		v[i] = raw[i] * ORIENT[i];
		b->gmin[i] = v[i] < b->gmin[i] ? v[i] : b->gmin[i];
		b->gmax[i] = v[i] > b->gmax[i] ? v[i] : b->gmax[i];
		b->gsum[i] += v[i];
	}
	if (++b->n == BLOCK)
		stillness_block(mo);
	if (mo->calibrating) {
		for (int i = 0; i < 3; i++)
			mo->csum[i] += v[i];
		mo->cn++;
		if (now_s() > mo->calib_deadline) {
			for (int i = 0; i < 3; i++)
				mo->bias[i] = mo->csum[i] / mo->cn;
			mo->calibrating = 0;
			mo->calibrated = 1;
			mo->cal_time = now_s();
			bias_save(mo);
			fprintf(stderr, "bias calibrated: %.4f %.4f %.4f\n", mo->bias[0], mo->bias[1], mo->bias[2]);
		}
	}
	{
		double tn = now_s(), dt = mo->last_t ? tn - mo->last_t : 0;

		mo->last_t = tn;
		dt = dt > 0.05 ? 0.05 : dt;
		for (int r = 0; r < 3; r++) {
			float p = 0;

			for (int k = 0; k < 3; k++)
				p += c->m[r][k] * (v[k] - mo->bias[k]);
			mo->phys[r] = p * RAD2DEG;
			mo->angle[r] += mo->phys[r] * dt;
		}
	}
	for (int i = 0; i < 3; i++)
		g[i] = c->compensation ? v[i] - mo->bias[i] : v[i];
	for (int r = 0; r < 3; r++)
		d[r] = mo->gm[r][0] * g[0] + mo->gm[r][1] * g[1] + mo->gm[r][2] * g[2];
	if (c->compensation != 1) {     /* raw (0) and fixed bias (2): no filters, no dead zones */
		for (int i = 0; i < 3; i++)
			mo->gsum[i] += d[i];
		mo->gn++;
		return;
	}
	speed = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) * RAD2DEG;   /* real deg/s, before sensitivity */

	/* GyroWiki "tightening": scale small speeds by speed/threshold, no hard cutoff */
	if (c->tighten_threshold > 0 && speed < c->tighten_threshold)
		for (int i = 0; i < 3; i++)
			d[i] *= speed / c->tighten_threshold;

	/* GyroWiki "soft tiered smoothing": only the slow part goes through a rolling average */
	if (c->smooth_threshold > 0) {
		float t2 = c->smooth_threshold, t1 = t2 * 0.5f, w = (speed - t1) / (t2 - t1);
		float *slot = mo->sbuf[mo->spos];

		w = w < 0 ? 0 : w > 1 ? 1 : w;
		for (int i = 0; i < 3; i++) {
			float slow = d[i] * (1.0f - w);

			mo->ssum[i] += slow - slot[i];
			slot[i] = slow;
			d[i] = d[i] * w + mo->ssum[i] / mo->slen;
		}
		mo->spos = (mo->spos + 1) % mo->slen;
	}
	for (int i = 0; i < 3; i++)
		mo->gsum[i] += d[i];
	mo->gn++;
}

static void accel_sample(struct motion *mo, const float raw[3])
{
	struct config *c = mo->c;
	struct block *b = &mo->cur;
	float v[3], d[3];

	mo->n_accel++;
	for (int i = 0; i < 3; i++) {
		v[i] = raw[i] * ORIENT[i] * (c->compensation == 1 ? c->accel_gain[i] : 1.0f);
		b->amin[i] = v[i] < b->amin[i] ? v[i] : b->amin[i];
		b->amax[i] = v[i] > b->amax[i] ? v[i] : b->amax[i];
	}
	b->na++;
	memcpy(mo->android_accel, v, sizeof(v));
	for (int r = 0; r < 3; r++)
		d[r] = c->m[r][0] * v[0] + c->m[r][1] * v[1] + c->m[r][2] * v[2];
	if (!mo->accel_primed || c->compensation != 1) {
		memcpy(mo->accel, d, sizeof(d));
		mo->accel_primed = 1;
	} else {
		for (int i = 0; i < 3; i++)
			mo->accel[i] += c->accel_alpha * (d[i] - mo->accel[i]);
	}
}

/* gyro averaged over the samples since the previous HID report (anti-aliasing 400 Hz -> 250 Hz) */
static void motion_frame(struct motion *mo)
{
	if (mo->gn) {
		for (int i = 0; i < 3; i++) {
			mo->gyro[i] = mo->gsum[i] / mo->gn;
			mo->gsum[i] = 0;
		}
		mo->gn = 0;
	}
	if (mo->c->aid_dps > 0) {       /* yaw (gyro[1]) and pitch (gyro[0]) as one vector, direction kept */
		float m = hypotf(mo->gyro[0], mo->gyro[1]) * RAD2DEG;

		if (m > mo->c->aid_gate) {
			float k = (m + mo->c->aid_dps) / m;

			mo->gyro[0] *= k;
			mo->gyro[1] *= k;
		}
	}
}

/* ------------------------------------------------------------------------------------------ rotation accel */
static int rotation_accel_create(void)
{
	struct uinput_setup us;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER);
	for (int code = ABS_X; code <= ABS_Z; code++) {
		struct uinput_abs_setup a = { .code = code,
			.absinfo = { .minimum = -32768, .maximum = 32768, .resolution = 8192 } };

		ioctl(fd, UI_SET_ABSBIT, code);
		ioctl(fd, UI_ABS_SETUP, &a);
	}
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = 0x1209;
	us.id.product = 0x0606;
	us.id.version = 1;
	strcpy(us.name, "RP6 Accelerometer");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Android frame with X and Y inverted (KDE auto-rotation turned the wrong way otherwise) */
static void rotation_accel_emit(int fd, const float v[3])
{
	struct input_event ev[4];
	float s[3] = { -v[0], -v[1], v[2] };

	memset(ev, 0, sizeof(ev));
	for (int i = 0; i < 3; i++) {
		float x = s[i] / G * 8192.0f;

		ev[i].type = EV_ABS;
		ev[i].code = ABS_X + i;
		ev[i].value = x < -32768 ? -32768 : x > 32768 ? 32768 : (int) x;
	}
	ev[3].type = EV_SYN;
	ev[3].code = SYN_REPORT;
	if (write(fd, ev, sizeof(ev)) < 0) {
		/* nobody listening is fine */
	}
}

/* ------------------------------------------------------------------------------------------ hid helpers */
static int find_original(char *dev, size_t devlen, char *name, size_t namelen, char *desc, size_t desclen)
{
	DIR *d = opendir("/sys/class/hidraw");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char path[512], line[256], id[64] = "", nm[128] = "", phys[64] = "";
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "HID_ID=", 7))
				snprintf(id, sizeof(id), "%s", line + 7);
			else if (!strncmp(line, "HID_NAME=", 9))
				snprintf(nm, sizeof(nm), "%s", line + 9);
			else if (!strncmp(line, "HID_PHYS=", 9))
				snprintf(phys, sizeof(phys), "%s", line + 9);
		}
		fclose(f);
		if (strlen(id) >= 17 && !strcasecmp(id + strlen(id) - 17, "000028DE:000012F0") && strcmp(phys, PHYS)) {
			snprintf(dev, devlen, "/dev/%s", e->d_name);
			snprintf(name, namelen, "%s", nm);
			snprintf(desc, desclen, "/sys/class/hidraw/%s/device/report_descriptor", e->d_name);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

static void udev_refresh(void)
{
	if (system("udevadm trigger --action=change --subsystem-match=hidraw; udevadm settle --timeout=5") != 0) {
		/* best effort */
	}
}

static void shm_open_motion(void)
{
	int fd = open(STATUS_DIR "/motion.bin", O_RDWR | O_CREAT, 0644);

	if (fd < 0)
		return;
	fchmod(fd, 0644);
	if (ftruncate(fd, 4096) == 0) {
		void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

		if (p != MAP_FAILED) {
			shm = p;
			memset(shm, 0, sizeof(*shm));
			shm->magic = 0x52503647;   /* "RP6G" */
		}
	}
	close(fd);
}

static void shm_publish(const struct motion *mo, unsigned long reports)
{
	if (!shm)
		return;
	__atomic_store_n(&shm->seq, shm->seq | 1, __ATOMIC_RELEASE);
	shm->t = now_s();
	for (int i = 0; i < 3; i++) {
		shm->phys_dps[i] = mo->phys[i];
		shm->out_dps[i] = mo->gyro[i] * RAD2DEG;
		shm->angle_deg[i] = mo->angle[i];
	}
	shm->compensation = mo->c->compensation;
	shm->sensitivity = mo->c->sensitivity;
	shm->reports = (uint32_t) reports;
	__atomic_store_n(&shm->seq, (shm->seq | 1) + 1, __ATOMIC_RELEASE);
}

static long cur_appid;
static char cur_profile[512];

static void write_status(struct motion *mo, unsigned long reports, double dt)
{
	FILE *f = fopen(STATUS_DIR "/.status.json", "w");

	if (!f)
		return;
	fprintf(f, "{\"imu_hz\": %.1f, \"accel_hz\": %.1f, \"reports_hz\": %.1f, "
		   "\"bias\": [%.4f, %.4f, %.4f], \"still\": %s, \"gyro_dps\": [%.2f, %.2f, %.2f], "
		   "\"accel_g\": [%.3f, %.3f, %.3f], \"enabled\": %s, \"calibrating\": %s, \"calibrated\": %s, "
		   "\"cal_age\": %.0f, \"noise_dps\": [%.2f, %.2f, %.2f], \"appid\": %ld, \"profile\": %s, \"mode\": \"%s\", \"time\": %ld}\n",
		mo->n_gyro / dt, mo->n_accel / dt, reports / dt,
		mo->bias[0], mo->bias[1], mo->bias[2], mo->still ? "true" : "false",
		mo->gyro[0] * RAD2DEG, mo->gyro[1] * RAD2DEG, mo->gyro[2] * RAD2DEG,
		mo->accel[0] / G, mo->accel[1] / G, mo->accel[2] / G,
		mo->c->enabled ? "true" : "false", mo->calibrating ? "true" : "false", mo->calibrated ? "true" : "false",
		mo->calibrated ? now_s() - mo->cal_time : -1.0,
		mo->floor_g[0] * RAD2DEG, mo->floor_g[1] * RAD2DEG, mo->floor_g[2] * RAD2DEG,
		cur_appid, cur_profile[0] ? "true" : "false", mo->c->mouse_mode ? "mouse" : "stick", (long) time(NULL));
	fchmod(fileno(f), 0644);
	fclose(f);
	rename(STATUS_DIR "/.status.json", STATUS_DIR "/status.json");
}

static int16_t clip16(float x)
{
	return x < -32768.0f ? -32768 : x > 32767.0f ? 32767 : (int16_t) x;
}

static void on_signal(int sig)
{
	stop = 1;
}

/* ------------------------------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
	const char *conf_path = argc > 1 ? argv[1] : DEFAULT_CONF;
	char dev[64], name[128], desc_path[512];
	unsigned char desc[HID_MAX_DESCRIPTOR_SIZE];
	struct config cfg;
	struct motion mo;
	struct uhid_event ev;
	struct sockaddr_in addr;
	struct pollfd pfd[3];
	struct sigaction sa;
	const char *profile_dir = argc > 2 ? argv[2] : NULL;
	time_t conf_mtime = -1, prof_mtime = -1;
	double last_app = 0;
	double last_conf = 0, last_status, last_rot = 0;
	unsigned long reports = 0;
	int rfd, ufd, sock, rot = -1, dlen, i;
	FILE *f;

	setvbuf(stderr, NULL, _IOLBF, 0);
	for (i = 0; i < 120 && !find_original(dev, sizeof(dev), name, sizeof(name), desc_path, sizeof(desc_path)); i++)
		sleep(1);              /* InputPlumber creates the device a bit after boot */
	if (i == 120) {
		fprintf(stderr, "original Deck-style controller (28de:12f0) not found\n");
		return 1;
	}
	f = fopen(desc_path, "rb");
	dlen = f ? (int) fread(desc, 1, sizeof(desc), f) : 0;
	if (f)
		fclose(f);
	rfd = open(dev, O_RDWR | O_NONBLOCK);
	if (rfd < 0 || dlen <= 0) {
		fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	fprintf(stderr, "proxying %s \"%s\" (%d byte descriptor)\n", dev, name, dlen);
	mkdir(STATUS_DIR, 0755);
	shm_open_motion();

	conf_load(&cfg, conf_path, NULL);
	motion_init(&mo, &cfg);
	snprintf(bias_path, sizeof(bias_path), "%s.bias", conf_path);
	bias_load(&mo);
	motion_apply_config(&mo, 1);
	if (cfg.rotation_accel && (rot = rotation_accel_create()) < 0)
		fprintf(stderr, "uinput accelerometer unavailable\n");

	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "bind 127.0.0.1:%d: %s\n", PORT, strerror(errno));
		return 1;
	}

	ufd = open("/dev/uhid", O_RDWR | O_NONBLOCK);
	if (ufd < 0) {
		fprintf(stderr, "/dev/uhid: %s\n", strerror(errno));
		return 1;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *) ev.u.create2.name, sizeof(ev.u.create2.name), "%s", name);
	snprintf((char *) ev.u.create2.phys, sizeof(ev.u.create2.phys), "%s", PHYS);
	ev.u.create2.rd_size = dlen;
	ev.u.create2.bus = BUS_USB;
	ev.u.create2.vendor = 0x28de;
	ev.u.create2.product = 0x12f0;
	ev.u.create2.version = 0x100;
	memcpy(ev.u.create2.rd_data, desc, dlen);
	if (write(ufd, &ev, sizeof(ev)) < 0) {
		fprintf(stderr, "UHID_CREATE2: %s\n", strerror(errno));
		return 1;
	}
	close(open(FLAG, O_WRONLY | O_CREAT, 0644));
	udev_refresh();            /* hide the original now that the copy exists */

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	pfd[0].fd = rfd;
	pfd[1].fd = sock;
	pfd[2].fd = ufd;
	for (i = 0; i < 3; i++)
		pfd[i].events = POLLIN;
	last_status = now_s();

	while (!stop) {
		double now;

		if (poll(pfd, 3, 1000) < 0 && errno != EINTR)
			break;
		now = now_s();

		if (profile_dir && now - last_app > 2.0) {
			long id = active_appid();

			last_app = now;
			if (id != cur_appid) {
				cur_appid = id;
				prof_mtime = -1;       /* force a reload with the new game's profile */
				fprintf(stderr, "active game: %ld\n", id);
			}
		}

		if (now - last_conf > 1.0) {
			struct stat st;
			char prof[512] = "";
			time_t mt = stat(conf_path, &st) == 0 ? st.st_mtime : 0, pm = 0;

			last_conf = now;
			if (profile_dir) {
				profile_path(prof, sizeof(prof), profile_dir, cur_appid);
				pm = prof[0] && stat(prof, &st) == 0 ? st.st_mtime : 0;
			}
			if (mt != conf_mtime || pm != prof_mtime || strcmp(prof, cur_profile)) {
				conf_mtime = mt;
				prof_mtime = pm;
				snprintf(cur_profile, sizeof(cur_profile), "%s", prof);
				conf_load(&cfg, conf_path, prof);
				motion_apply_config(&mo, 0);
				fprintf(stderr, "config: game=%ld profile=%s mode=%s enabled=%d compensation=%d sensitivity=%.2f aid=%.1f invert=%d/%d/%d smooth=%.1f tighten=%.1f\n",
					cur_appid, prof[0] ? prof : "-", cfg.mouse_mode ? "mouse" : "stick", cfg.enabled, cfg.compensation,
					cfg.sensitivity, cfg.aid_dps, cfg.invert[0], cfg.invert[1], cfg.invert[2],
					cfg.smooth_threshold, cfg.tighten_threshold);
			}
		}

		if (pfd[1].revents & POLLIN) {
			unsigned char pkt[64];
			ssize_t n;

			while ((n = recv(sock, pkt, sizeof(pkt), 0)) > 0) {
				float v[3];

				if (n != 13)
					continue;
				memcpy(v, pkt + 1, sizeof(v));
				if (pkt[0] == 0)
					gyro_sample(&mo, v);
				else
					accel_sample(&mo, v);
			}
		}

		if (pfd[0].revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "original gone\n");
			break;
		}
		if (pfd[0].revents & POLLIN) {
			unsigned char rep[256];
			ssize_t n;

			while ((n = read(rfd, rep, sizeof(rep))) > 0) {
				if (n == 64 && rep[2] == 9) {
					motion_frame(&mo);
					shm_publish(&mo, reports);
					if (cfg.enabled) {
						const float ka = ACCEL_PER_G / G, kg = RAD2DEG * GYRO_PER_DPS;
						int16_t imu[6] = {
							clip16(mo.accel[0] * ka), clip16(mo.accel[1] * ka), clip16(mo.accel[2] * ka),
							clip16(mo.gyro[0] * kg), clip16(mo.gyro[1] * kg), clip16(mo.gyro[2] * kg),
						};

						memcpy(rep + OFF_ACCEL, imu, sizeof(imu));   /* little-endian host */
					}
				}
				memset(&ev, 0, offsetof(struct uhid_event, u.input2.data));
				ev.type = UHID_INPUT2;
				ev.u.input2.size = n;
				memcpy(ev.u.input2.data, rep, n);
				if (write(ufd, &ev, offsetof(struct uhid_event, u.input2.data) + n) < 0)
					fprintf(stderr, "UHID_INPUT2: %s\n", strerror(errno));
				reports++;
			}
			if (n < 0 && errno != EAGAIN) {
				fprintf(stderr, "original gone: %s\n", strerror(errno));
				break;
			}
		}

		if (pfd[2].revents & POLLIN && read(ufd, &ev, sizeof(ev)) > 0) {
			struct uhid_event r;

			memset(&r, 0, sizeof(r));
			switch (ev.type) {
			case UHID_GET_REPORT: {
				unsigned char buf[65] = { ev.u.get_report.rnum };  /* relayed byte for byte */
				int n = ioctl(rfd, HIDIOCGFEATURE(sizeof(buf)), buf);

				r.type = UHID_GET_REPORT_REPLY;
				r.u.get_report_reply.id = ev.u.get_report.id;
				if (n < 0) {
					r.u.get_report_reply.err = EIO;
				} else {
					r.u.get_report_reply.size = n;
					memcpy(r.u.get_report_reply.data, buf, n);
				}
				if (write(ufd, &r, sizeof(r)) < 0)
					fprintf(stderr, "GET_REPORT_REPLY: %s\n", strerror(errno));
				break;
			}
			case UHID_SET_REPORT: {
				int ret;

				/* the data already starts with the (zero) report id byte */
				if (ev.u.set_report.rtype == UHID_FEATURE_REPORT)
					ret = ioctl(rfd, HIDIOCSFEATURE(ev.u.set_report.size), ev.u.set_report.data);
				else
					ret = write(rfd, ev.u.set_report.data, ev.u.set_report.size);
				r.type = UHID_SET_REPORT_REPLY;
				r.u.set_report_reply.id = ev.u.set_report.id;
				r.u.set_report_reply.err = ret < 0 ? EIO : 0;
				if (write(ufd, &r, sizeof(r)) < 0)
					fprintf(stderr, "SET_REPORT_REPLY: %s\n", strerror(errno));
				break;
			}
			case UHID_OUTPUT:
				if (write(rfd, ev.u.output.data, ev.u.output.size) < 0) {
					/* the original may reject it; nothing to report back */
				}
				break;
			case UHID_START:
				fprintf(stderr, "proxy device started\n");
				break;
			default:
				break;
			}
		}

		if (rot >= 0 && now - last_rot > 0.04) {
			last_rot = now;
			rotation_accel_emit(rot, mo.android_accel);
		}
		if (now - last_status > 2.0) {
			write_status(&mo, reports, now - last_status);
			mo.n_gyro = mo.n_accel = 0;
			reports = 0;
			last_status = now;
		}
	}

	unlink(FLAG);
	udev_refresh();            /* make the original visible again */
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;
	if (write(ufd, &ev, sizeof(ev)) < 0) {
		/* exiting anyway */
	}
	return stop ? 0 : 1;
}
