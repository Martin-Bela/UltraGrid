#include <unistd.h>


#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_C11_THREADS
#include <threads.h>
#else
#include <pthread.h>
#define mtx_t pthread_mutex_t
#define mtx_lock pthread_mutex_lock
#define mtx_unlock pthread_mutex_unlock
#define mtx_destroy pthread_mutex_destroy
#endif

#include <libug.h>

#include "src/vrgstream-fallback.h"

#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define FPS 30.0

struct sender_data {
        struct RenderPacket pkt;
        mtx_t lock;
};

static void render_packet_received_callback(void *udata, struct RenderPacket *pkt) {
        struct sender_data *s = udata;
        printf("Received RenderPacket: %p\n", pkt);
        mtx_lock(&s->lock);
        memcpy(&s->pkt, pkt, sizeof(struct RenderPacket));
        mtx_unlock(&s->lock);
}

static void usage(const char *progname) {
        printf("%s [options] [receiver[:port]]\n", progname);
        printf("options:\n");
        printf("\t-h - show this help\n");
        printf("\t-j - use JPEG\n");
        printf("\t-m - use specified MTU\n");
        printf("\t-n - disable strips\n");
        printf("\t-s - size (WxH)\n");
        printf("\t-v - increase verbosity (use twice for debug)\n");
}

#define MIN(a, b)      (((a) < (b))? (a): (b))
#define MAX(a, b)      (((a) > (b))? (a): (b))

static void fill(unsigned char **buf_p, int width, int height, libug_pixfmt_t pixfmt) {
        assert(width > 0 && height > 0);
        free(*buf_p);
        size_t len = (width + 768) * height * 4;
        char *data = malloc(len);
        *buf_p = data;
        assert(pixfmt == UG_RGBA);
        for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                        *data++ = MIN(256, y % (3 * 256)) % 256;
                        *data++ = MIN(256, MAX(0, y - 256) % (3 * 256)) % 256;
                        *data++ = MIN(256, MAX(0, y - 512) % (3 * 256)) % 256;
                        *data++ = 255;
                }
        }
}

int main(int argc, char *argv[]) {
        struct sender_data data = { 0 };
        struct ug_sender_parameters init_params = { 0 };
        init_params.receiver = "localhost";
        init_params.compression = UG_UNCOMPRESSED;
        init_params.rprc = render_packet_received_callback;
        init_params.rprc_udata = &data;
        bool disable_strips = false;
        int width = DEFAULT_WIDTH;
        int height = DEFAULT_HEIGHT;

        int ch = 0;
        while ((ch = getopt(argc, argv, "hjm:ns:v")) != -1) {
                switch (ch) {
                case 'h':
                        usage(argv[0]);
                        return 0;
                case 'j':
                        init_params.compression = UG_JPEG;
                        break;
                case 'm':
                        init_params.mtu = atoi(optarg);
                        break;
                case 'n':
                        init_params.disable_strips = 1;
                        break;
                case 's':
                        width = atoi(optarg);
                        assert(strchr(optarg, 'x') != NULL);
                        height = atoi(strchr(optarg, 'x') + 1);
                        break;
                case 'v':
                        init_params.verbose += 1;
                        break;
                default:
                        usage(argv[0]);
                        return -2;
                }
        }

        argc -= optind;
        argv += optind;

        if (argc > 0) {
                init_params.receiver = argv[0];
                if (strchr(argv[0], ':') != NULL) {
                        char *port_str = strchr(argv[0], ':') + 1;
                        *strchr(argv[0], ':') = '\0';
                        init_params.port = atoi(port_str);
                }
        }

#ifdef USE_C11_THREADS
        int ret = mtx_init(&data.lock, mtx_plain);
        if (ret != thrd_success) {
#else
        int ret = pthread_mutex_init(&data.lock, NULL);
        if (ret != 0) {
#endif
                perror("Mutex init failed");
                return 1;
        }
        struct ug_sender *s = ug_sender_init(&init_params);
        if (!s) {
                return 1;
        }

        time_t t0 = time(NULL);
        size_t len = width * height * 4;
        unsigned char *test = malloc(len + 768 * width * 4);
        fill(&test, width, height, UG_RGBA);
        uint32_t frames = 0;
        uint32_t frames_last = 0;
        while (1) {
                struct RenderPacket pkt;
                mtx_lock(&data.lock);
                memcpy(&pkt, &data.pkt, sizeof pkt);
                mtx_unlock(&data.lock);
                if (pkt.pix_width_eye != 0 && pkt.pix_height_eye != 0 && width != pkt.pix_width_eye && height != pkt.pix_height_eye) {
                        width = pkt.pix_width_eye;
                        height = pkt.pix_height_eye;
                        fill(&test, width, height, UG_RGBA);
                }

                ug_send_frame(s, (char *) test + width * 4 * (frames % 768), UG_RGBA, width, height, &pkt);
                frames += 1;
                time_t seconds = time(NULL) - t0;
                if (seconds > 0) {
                        printf("Sent %d frames in last %ld second%s.\n", frames - frames_last, seconds, seconds > 1 ? "s" : "");
                        t0 += seconds;
                        frames_last = frames;
                }
                usleep(1000 * 1000 / FPS);
        }

        free(test);
        ug_sender_done(s);
        mtx_destroy(&data.lock);
}

