/*
 * rp6imu: stream the RP6 QMI8658 accelerometer + gyroscope from the Qualcomm SSC (via libssc)
 * to UDP 127.0.0.1:<port> as packed { u8 type; f32 x, y, z } little-endian records,
 * type 0 = gyroscope (rad/s), 1 = accelerometer (m/s^2), raw sensor axes.
 * Exits with status 2 when no samples arrive for 5 s so the supervisor restarts it.
 */
#include <arpa/inet.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>

#include "libssc-sensor-accelerometer.h"
#include "libssc-sensor-gyroscope.h"

static int sock = -1;
static struct sockaddr_in dst;
static gint64 last_sample;

static void send_sample(guint8 type, gfloat x, gfloat y, gfloat z)
{
	guint8 buf[13];

	buf[0] = type;
	memcpy(buf + 1, &x, 4);
	memcpy(buf + 5, &y, 4);
	memcpy(buf + 9, &z, 4);
	sendto(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *) &dst, sizeof(dst));
	last_sample = g_get_monotonic_time();
}

static void on_gyro(SSCSensorGyroscope *s, gfloat x, gfloat y, gfloat z, gpointer user_data)
{
	send_sample(0, x, y, z);
}

static void on_accel(SSCSensorAccelerometer *s, gfloat x, gfloat y, gfloat z, gpointer user_data)
{
	send_sample(1, x, y, z);
}

static gboolean watchdog(gpointer user_data)
{
	if (g_get_monotonic_time() - last_sample > 5 * G_USEC_PER_SEC) {
		g_printerr("rp6imu: no samples for 5 s, exiting\n");
		exit(2);
	}
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	SSCSensorAccelerometer *accel;
	SSCSensorGyroscope *gyro;
	GError *err = NULL;
	int port = argc > 1 ? atoi(argv[1]) : 47811;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(port);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	gyro = ssc_sensor_gyroscope_new_sync(NULL, &err);
	if (!gyro) {
		g_printerr("rp6imu: gyroscope: %s\n", err ? err->message : "unknown");
		return 1;
	}
	accel = ssc_sensor_accelerometer_new_sync(NULL, &err);
	if (!accel) {
		g_printerr("rp6imu: accelerometer: %s\n", err ? err->message : "unknown");
		return 1;
	}
	g_signal_connect(gyro, "measurement", G_CALLBACK(on_gyro), NULL);
	g_signal_connect(accel, "measurement", G_CALLBACK(on_accel), NULL);
	if (!ssc_sensor_gyroscope_open_sync(gyro, NULL, &err) || !ssc_sensor_accelerometer_open_sync(accel, NULL, &err)) {
		g_printerr("rp6imu: open: %s\n", err ? err->message : "unknown");
		return 1;
	}

	last_sample = g_get_monotonic_time();
	g_timeout_add_seconds(1, watchdog, NULL);
	g_print("rp6imu: streaming to 127.0.0.1:%d\n", port);
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
