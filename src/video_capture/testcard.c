/**
 * @file   video_capture/testcard.c
 * @author Colin Perkins <csp@csperkins.org
 * @author Alvaro Saurin <saurin@dcs.gla.ac.uk>
 * @author Martin Benes     <martinbenesh@gmail.com>
 * @author Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 * @author Petr Holub       <hopet@ics.muni.cz>
 * @author Milos Liska      <xliska@fi.muni.cz>
 * @author Jiri Matela      <matela@ics.muni.cz>
 * @author Dalibor Matura   <255899@mail.muni.cz>
 * @author Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 */
/*
 * Copyright (c) 2005-2006 University of Glasgow
 * Copyright (c) 2005-2023 CESNET z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
 *
 * 4. Neither the name of the University, Institute, CESNET nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file
 * @todo
 * * fix broken tiling (perhaps wrapper over pattern generator)
 */

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"

#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "tv.h"
#include "video.h"
#include "video_capture.h"
#include "utils/color_out.h"
#include "utils/misc.h"
#include "utils/string.h"
#include "utils/vf_split.h"
#include "utils/pam.h"
#include "utils/y4m.h"
#include <stdio.h>
#include <stdlib.h>
#include "audio/types.h"
#include "utils/video_pattern_generator.h"
#include "video_capture/testcard_common.h"

enum {
        AUDIO_SAMPLE_RATE = 48000,
        AUDIO_BPS = 2,
        BUFFER_SEC = 1,
        DEFAULT_AUIDIO_FREQUENCY = 1000,
};
#define MOD_NAME "[testcard] "
#define AUDIO_BUFFER_SIZE(ch_count) ( AUDIO_SAMPLE_RATE * AUDIO_BPS * (ch_count) * BUFFER_SEC )
#define DEFAULT_FORMAT ((struct video_desc) { 1920, 1080, UYVY, 25.0, INTERLACED_MERGED, 1 })
#define DEFAULT_PATTERN "bars"

struct audio_len_pattern {
        int count;
        int samples[5];
        int current_idx;
};
static const int alen_pattern_2997[] = { 1602, 1601, 1602, 1601, 1602 };
static const int alen_pattern_5994[] = { 801, 801, 800, 801, 801 };
static const int alen_pattern_11988[] = { 400, 401, 400, 401, 400 };
_Static_assert(sizeof alen_pattern_2997 <= sizeof ((struct audio_len_pattern *) 0)->samples && sizeof alen_pattern_5994 <= sizeof ((struct audio_len_pattern *) 0)->samples, "insufficient length");

struct testcard_state {
        time_ns_t last_frame_time;
        int pan;
        video_pattern_generator_t generator;
        struct video_frame *frame;
        struct video_frame *tiled;

        struct audio_frame audio;
        struct audio_len_pattern apattern;
        int audio_frequency;

        char **tiles_data;
        int tiles_cnt_horizontal;
        int tiles_cnt_vertical;

        char *audio_data;
        bool grab_audio;
        bool still_image;
        char pattern[128];
};

static void configure_fallback_audio(struct testcard_state *s) {
        static_assert(AUDIO_BPS == sizeof(int16_t), "Only 2-byte audio is supported for testcard audio at the moment");
        const double scale = 0.1;

        for (int i = 0; i < AUDIO_BUFFER_SIZE(s->audio.ch_count) / AUDIO_BPS; i += 1) {
                *((int16_t*)(void *)(&s->audio_data[i * AUDIO_BPS])) = round(sin(((double) i / ((double) AUDIO_SAMPLE_RATE / s->audio_frequency)) * M_PI * 2. ) * ((1U << (AUDIO_BPS * 8U - 1)) - 1) * scale);
        }
}

static bool configure_audio(struct testcard_state *s)
{
        s->audio.bps = AUDIO_BPS;
        s->audio.ch_count = audio_capture_channels > 0 ? audio_capture_channels : DEFAULT_AUDIO_CAPTURE_CHANNELS;
        s->audio.sample_rate = AUDIO_SAMPLE_RATE;
        s->audio.max_size = AUDIO_BUFFER_SIZE(s->audio.ch_count);
        s->audio.data = s->audio_data = (char *) realloc(s->audio.data, 2 * s->audio.max_size);
        const int vnum = get_framerate_n(s->frame->fps);
        const int vden = get_framerate_d(s->frame->fps);
        if ((AUDIO_SAMPLE_RATE * vden) % vnum == 0) {
                s->apattern.count = 1;
                s->apattern.samples[0] = (AUDIO_SAMPLE_RATE * vden) / vnum;
        } else if (vden == 1001 && vnum == 30000) {
                s->apattern.count = sizeof alen_pattern_2997 / sizeof alen_pattern_2997[0];
                memcpy(s->apattern.samples, alen_pattern_2997, sizeof alen_pattern_2997);
        } else if (vden == 1001 && vnum == 60000) {
                s->apattern.count = sizeof alen_pattern_5994 / sizeof alen_pattern_5994[0];
                memcpy(s->apattern.samples, alen_pattern_5994, sizeof alen_pattern_5994);
        } else if (vden == 1001 && vnum == 120000) {
                s->apattern.count = sizeof alen_pattern_11988 / sizeof alen_pattern_11988[0];
                memcpy(s->apattern.samples, alen_pattern_11988, sizeof alen_pattern_11988);
        } else {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Audio not implemented for %f FPS! Please report a bug if it is a common frame rate.\n", s->frame->fps);
                return false;
        }

        configure_fallback_audio(s);
        memcpy(s->audio.data + s->audio.max_size, s->audio.data, s->audio.max_size);
        s->grab_audio = true;

        return true;
}

#if 0
static int configure_tiling(struct testcard_state *s, const char *fmt)
{
        char *tmp, *token, *saveptr = NULL;
        int tile_cnt;
        int x;

        int grid_w, grid_h;

        if(fmt[1] != '=') return 1;

        tmp = strdup(&fmt[2]);
        token = strtok_r(tmp, "x", &saveptr);
        grid_w = atoi(token);
        token = strtok_r(NULL, "x", &saveptr);
        grid_h = atoi(token);
        free(tmp);

        s->tiled = vf_alloc(grid_w * grid_h);
        s->tiles_cnt_horizontal = grid_w;
        s->tiles_cnt_vertical = grid_h;
        s->tiled->color_spec = s->frame->color_spec;
        s->tiled->fps = s->frame->fps;
        s->tiled->interlacing = s->frame->interlacing;

        tile_cnt = grid_w *
                grid_h;
        assert(tile_cnt >= 1);

        s->tiles_data = (char **) malloc(tile_cnt *
                        sizeof(char *));
        /* split only horizontally!!!!!! */
        vf_split(s->tiled, s->frame, grid_w,
                        1, 1 /*prealloc*/);
        /* for each row, make the tile data correct.
         * .data pointers of same row point to same block,
         * but different row */
        for(x = 0; x < grid_w; ++x) {
                int y;

                s->tiles_data[x] = s->tiled->tiles[x].data;

                s->tiled->tiles[x].width = s->frame->tiles[0].width/ grid_w;
                s->tiled->tiles[x].height = s->frame->tiles[0].height / grid_h;
                s->tiled->tiles[x].data_len = s->frame->tiles[0].data_len / (grid_w * grid_h);

                s->tiled->tiles[x].data =
                        s->tiles_data[x] = (char *) realloc(s->tiled->tiles[x].data,
                                        s->tiled->tiles[x].data_len * grid_h * 2);


                memcpy(s->tiled->tiles[x].data + s->tiled->tiles[x].data_len  * grid_h,
                                s->tiled->tiles[x].data, s->tiled->tiles[x].data_len * grid_h);
                /* recopy tiles vertically */
                for(y = 1; y < grid_h; ++y) {
                        memcpy(&s->tiled->tiles[y * grid_w + x],
                                        &s->tiled->tiles[x], sizeof(struct tile));
                        /* make the pointers correct */
                        s->tiles_data[y * grid_w + x] =
                                s->tiles_data[x] +
                                y * s->tiled->tiles[x].height *
                                vc_get_linesize(s->tiled->tiles[x].width, s->tiled->color_spec);

                        s->tiled->tiles[y * grid_w + x].data =
                                s->tiles_data[x] +
                                y * s->tiled->tiles[x].height *
                                vc_get_linesize(s->tiled->tiles[x].width, s->tiled->color_spec);
                }
        }

        return 0;
}
#endif

static bool parse_fps(const char *fps, struct video_desc *desc) {
        char *endptr = NULL;
        desc->fps = strtod(fps, &endptr);
        desc->interlacing = PROGRESSIVE;
        if (strlen(endptr) != 0) { // optional interlacing suffix
                desc->interlacing = get_interlacing_from_suffix(endptr);
                if (desc->interlacing != PROGRESSIVE &&
                                desc->interlacing != SEGMENTED_FRAME &&
                                desc->interlacing != INTERLACED_MERGED) { // tff or bff
                        log_msg(LOG_LEVEL_ERROR, "Unsuppored interlacing format: %s!\n", endptr);
                        return false;
                }
                if (desc->interlacing == INTERLACED_MERGED) {
                        desc->fps /= 2;
                }
        }
        return true;
}

static struct video_desc parse_format(char **fmt, char **save_ptr) {
        struct video_desc desc = { 0 };
        desc.tile_count = 1;
        char *tmp = strtok_r(*fmt, ":", save_ptr);
        if (!tmp) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Missing width!\n");
                return (struct video_desc) { 0 };
        }
        desc.width = MAX(strtol(tmp, NULL, 0), 0);

        if ((tmp = strtok_r(NULL, ":", save_ptr)) == NULL) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Missing height!\n");
                return (struct video_desc) { 0 };
        }
        desc.height = MAX(strtol(tmp, NULL, 0), 0);

        if (desc.width * desc.height == 0) {
                fprintf(stderr, "Wrong dimensions for testcard.\n");
                return (struct video_desc) { 0 };
        }

        if ((tmp = strtok_r(NULL, ":", save_ptr)) == NULL) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Missing FPS!\n");
                return (struct video_desc) { 0 };
        }
        if (!parse_fps(tmp, &desc)) {
                return (struct video_desc) { 0 };
        }

        if ((tmp = strtok_r(NULL, ":", save_ptr)) == NULL) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Missing pixel format!\n");
                return (struct video_desc) { 0 };
        }
        desc.color_spec = get_codec_from_name(tmp);
        if (desc.color_spec == VIDEO_CODEC_NONE) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unknown codec '%s'\n", tmp);
                return (struct video_desc) { 0 };
        }
        if (!testcard_has_conversion(desc.color_spec)) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unsupported codec '%s'\n", tmp);
                return (struct video_desc) { 0 };
        }

        *fmt = NULL;
        return desc;
}

static size_t testcard_load_from_file_pam(const char *filename, struct video_desc *desc, char **in_file_contents) {
        struct pam_metadata info;
        unsigned char *data = NULL;
        if (pam_read(filename, &info, &data, malloc) == 0) {
                return false;
        }
        switch (info.depth) {
                case 3:
                        desc->color_spec = info.maxval == 255 ? RGB : RG48;
                        break;
                case 4:
                        desc->color_spec = RGBA;
                        break;
                default:
                        log_msg(LOG_LEVEL_ERROR, "Unsupported PAM/PNM channel count %d!\n", info.depth);
                        return 0;
        }
        desc->width = info.width;
        desc->height = info.height;
        size_t data_len = vc_get_datalen(desc->width, desc->height, desc->color_spec);
        *in_file_contents = (char  *) malloc(data_len);
        if (desc->color_spec == RG48) {
                uint16_t *in = (uint16_t *)(void *) data;
                uint16_t *out = (uint16_t *)(void *) *in_file_contents;
                for (size_t i = 0; i < (size_t) info.width * info.height * 3; ++i) {
                        *out++ = ntohs(*in++) * ((1<<16U) / (info.maxval + 1));
                }
        } else {
                memcpy(*in_file_contents, data, data_len);
        }
        free(data);
        return data_len;
}

static size_t testcard_load_from_file_y4m(const char *filename, struct video_desc *desc, char **in_file_contents) {
        struct y4m_metadata info;
        unsigned char *data = NULL;
        if (y4m_read(filename, &info, &data, malloc) == 0) {
                return 0;
        }
        if (!((info.subsampling == Y4M_SUBS_422 || info.subsampling == Y4M_SUBS_444) && info.bitdepth == 8) || (info.subsampling == Y4M_SUBS_444 && info.bitdepth > 8)) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Only 8-bit Y4M with subsampling 4:2:2 and 4:4:4 or higher bit depths with subsampling 4:4:4 are supported.\n");
                log_msg(LOG_LEVEL_INFO, MOD_NAME "Provided Y4M picture has subsampling %d and bit depth %d bits.\n", info.subsampling, info.bitdepth);
                return 0;
        }
        desc->width = info.width;
        desc->height = info.height;
        desc->color_spec = info.bitdepth == 8 ? UYVY : Y416;
        size_t data_len = vc_get_datalen(desc->width, desc->height, desc->color_spec);
        *in_file_contents = (char *) malloc(data_len);
        if (info.bitdepth == 8) {
                if (info.subsampling == Y4M_SUBS_422) {
                        i422_8_to_uyvy(desc->width, desc->height, (char *) data, *in_file_contents);
                } else {
                        i444_8_to_uyvy(desc->width, desc->height, (char *) data, *in_file_contents);
                }
        } else {
                i444_16_to_y416(desc->width, desc->height, (char *) data, *in_file_contents, info.bitdepth);
        }
        free(data);
        return data_len;
}

static size_t testcard_load_from_file(const char *filename, struct video_desc *desc, char **in_file_contents, bool deduce_pixfmt) {
        if (ends_with(filename, ".pam") || ends_with(filename, ".pnm") || ends_with(filename, ".ppm")) {
                return testcard_load_from_file_pam(filename, desc, in_file_contents);
        } else if (ends_with(filename, ".y4m")) {
                return testcard_load_from_file_y4m(filename, desc, in_file_contents);
        }

        if (deduce_pixfmt && strchr(filename, '.') != NULL && get_codec_from_file_extension(strrchr(filename, '.') + 1)) {
                desc->color_spec = get_codec_from_file_extension(strrchr(filename, '.') + 1);
        }
        long data_len = vc_get_datalen(desc->width, desc->height, desc->color_spec);
        *in_file_contents = (char *) malloc(data_len);
        FILE *in = fopen(filename, "r");
        if (in == NULL) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "%s fopen: %s\n", filename, ug_strerror(errno));
                return 0;
        }
        fseek(in, 0L, SEEK_END);
        long filesize = ftell(in);
        if (filesize == -1) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "ftell: %s\n", ug_strerror(errno));
                filesize = data_len;
        }
        fseek(in, 0L, SEEK_SET);

        do {
                if (data_len != filesize) {
                        int level = data_len < filesize ? LOG_LEVEL_WARNING : LOG_LEVEL_ERROR;
                        log_msg(level, MOD_NAME "Wrong file size for selected resolution"
                                "and codec. File size %ld, computed size %ld\n", filesize, data_len);
                        filesize = data_len;
                        if (level == LOG_LEVEL_ERROR) {
                                data_len = 0; break;
                        }
                }

                if (fread(*in_file_contents, filesize, 1, in) != 1) {
                        log_msg(LOG_LEVEL_ERROR, "Cannot read file %s\n", filename);
                        data_len = 0; break;
                }
        } while (false);

        fclose(in);
        if (data_len == 0) {
                free(*in_file_contents);
                *in_file_contents = NULL;
        }
        return data_len;
}

static void show_help(bool full) {
        printf("testcard options:\n");
        color_printf(TBOLD(TRED("\t-t testcard") "[:size=<width>x<height>][:fps=<fps>][:codec=<codec>]") "[:file=<filename>][:p][:s=<X>x<Y>][:i|:sf][:still][:pattern=<pattern>] " TBOLD("| -t testcard:[full]help\n"));
        color_printf("or\n");
        color_printf(TBOLD(TRED("\t-t testcard") ":<width>:<height>:<fps>:<codec>") "[:other_opts]\n");
        color_printf("where\n");
        color_printf(TBOLD("\t  file ") "      - use file for input data instead of predefined pattern\n");
        color_printf(TBOLD("\t  fps  ") "      - frames per second (with optional 'i' suffix for interlaced)\n");
        color_printf(TBOLD("\t  i|sf ") "      - send as interlaced or segmented frame\n");
        color_printf(TBOLD("\t  mode ") "      - use specified mode (use 'mode=help' for list)\n");
        color_printf(TBOLD("\t   p   ") "      - pan with frame\n");
        color_printf(TBOLD("\tpattern") "      - pattern to use, use \"" TBOLD("pattern=help") "\" for options\n");
        color_printf(TBOLD("\t   s   ") "      - split the frames into XxY separate tiles (currently defunct)\n");
        color_printf(TBOLD("\t still ") "      - send still image\n");
        if (full) {
                color_printf(TBOLD("       afrequency") "    - embedded audio frequency\n");
        }
        color_printf("\n");
        testcard_show_codec_help("testcard", false);
        color_printf("\n");
        color_printf("Examples:\n");
        color_printf(TBOLD("\t%s -t testcard:file=picture.pam\n"), uv_argv[0]);
        color_printf(TBOLD("\t%s -t testcard:mode=VGA\n"), uv_argv[0]);
        color_printf(TBOLD("\t%s -t testcard:size=1920x1080:fps=59.94i\n"), uv_argv[0]);
        color_printf("\n");
        color_printf("Default mode: %s\n", video_desc_to_string(DEFAULT_FORMAT));
        color_printf(TBOLD("Note:") " only certain codec and generator combinations produce full-depth samples (not up-sampled 8-bit), use " TBOLD("pattern=help") " for details.\n");
}

static int vidcap_testcard_init(struct vidcap_params *params, void **state)
{
        struct testcard_state *s = NULL;
        char *filename = NULL;
        const char *strip_fmt = NULL;
        char *save_ptr = NULL;
        int ret = VIDCAP_INIT_FAIL;
        char *tmp;
        char *in_file_contents = NULL;
        size_t in_file_contents_size = 0;

        if (strcmp(vidcap_params_get_fmt(params), "help") == 0 || strcmp(vidcap_params_get_fmt(params), "fullhelp") == 0) {
                show_help(strcmp(vidcap_params_get_fmt(params), "fullhelp") == 0);
                return VIDCAP_INIT_NOERR;
        }

        if ((s = calloc(1, sizeof *s)) == NULL) {
                return VIDCAP_INIT_FAIL;
        }
        strncat(s->pattern, DEFAULT_PATTERN, sizeof s->pattern - 1);
        s->audio_frequency = DEFAULT_AUIDIO_FREQUENCY;

        char *fmt = strdup(vidcap_params_get_fmt(params));
        char *ptr = fmt;

        bool pixfmt_default = true;
        struct video_desc desc = DEFAULT_FORMAT;
        if (strlen(ptr) > 0 && isdigit(ptr[0])) {
                pixfmt_default = false;
                desc = parse_format(&ptr, &save_ptr);
        }
        if (!desc.width) {
                goto error;
        }

        tmp = strtok_r(ptr, ":", &save_ptr);
        while (tmp) {
                if (strcmp(tmp, "p") == 0) {
                        s->pan = 48;
                } else if (strstr(tmp, "file=") == tmp || strstr(tmp, "filename=") == tmp) {
                        filename = strchr(tmp, '=') + 1;
                } else if (strncmp(tmp, "s=", 2) == 0) {
                        strip_fmt = tmp;
                } else if (strcmp(tmp, "i") == 0) {
                        desc.interlacing = INTERLACED_MERGED;
                        log_msg(LOG_LEVEL_WARNING, "[testcard] Deprecated 'i' option. Use format testcard:1920:1080:50i:UYVY instead!\n");
                } else if (strcmp(tmp, "sf") == 0) {
                        desc.interlacing = SEGMENTED_FRAME;
                        log_msg(LOG_LEVEL_WARNING, "[testcard] Deprecated 'sf' option. Use format testcard:1920:1080:25sf:UYVY instead!\n");
                } else if (strcmp(tmp, "still") == 0) {
                        s->still_image = true;
                } else if (strncmp(tmp, "pattern=", strlen("pattern=")) == 0) {
                        const char *pattern = tmp + strlen("pattern=");
                        strncpy(s->pattern, pattern, sizeof s->pattern - 1);
                } else if (strstr(tmp, "codec=") == tmp) {
                        desc.color_spec = get_codec_from_name(strchr(tmp, '=') + 1);
                        pixfmt_default = false;
                } else if (strstr(tmp, "mode=") == tmp) {
                        codec_t saved_codec = desc.color_spec;
                        desc = get_video_desc_from_string(strchr(tmp, '=') + 1);
                        desc.color_spec = saved_codec;
                } else if (strstr(tmp, "size=") == tmp && strchr(tmp, 'x') != NULL) {
                        desc.width = atoi(strchr(tmp, '=') + 1);
                        desc.height = atoi(strchr(tmp, 'x') + 1);
                } else if (strstr(tmp, "fps=") == tmp) {
                        if (!parse_fps(strchr(tmp, '=') + 1, &desc)) {
                                goto error;
                        }
                } else if (strstr(tmp, "afrequency=") == tmp) {
                        s->audio_frequency = atoi(strchr(tmp, '=') + 1);
                } else {
                        fprintf(stderr, "[testcard] Unknown option: %s\n", tmp);
                        goto error;
                }
                tmp = strtok_r(NULL, ":", &save_ptr);
        }

        if (desc.color_spec == VIDEO_CODEC_NONE || desc.width <= 0 || desc.height <= 0 || desc.fps <= 0.0) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Wrong video format: %s\n", video_desc_to_string(desc));;
                goto error;
        }

        if (filename) {
                if ((in_file_contents_size = testcard_load_from_file(filename, &desc, &in_file_contents, pixfmt_default)) == 0) {
                        goto error;
                }
        }

        if (!s->still_image && codec_is_planar(desc.color_spec)) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Planar pixel format '%s', using still picture.\n", get_codec_name(desc.color_spec));
                s->still_image = true;
        }

        s->frame = vf_alloc_desc(desc);

        s->generator = video_pattern_generator_create(s->pattern, s->frame->tiles[0].width, s->frame->tiles[0].height, s->frame->color_spec,
                        s->still_image ? 0 : vc_get_linesize(desc.width, desc.color_spec) + s->pan);
        if (!s->generator) {
                ret = strstr(s->pattern, "help") != NULL ? VIDCAP_INIT_NOERR : VIDCAP_INIT_FAIL;
                goto error;
        }
        if (in_file_contents_size > 0) {
                video_pattern_generator_fill_data(s->generator, in_file_contents);
        }

        s->last_frame_time = get_time_in_ns();

        log_msg(LOG_LEVEL_INFO, MOD_NAME "capture set to %s, bpc %d, pattern: %s, audio %s\n", video_desc_to_string(desc),
                get_bits_per_component(s->frame->color_spec), s->pattern, (s->grab_audio ? "on" : "off"));

        if (strip_fmt != NULL) {
                log_msg(LOG_LEVEL_ERROR, "Multi-tile testcard (stripping) is currently broken, you can use eg. \"-t aggregate -t testcard[args] -t testcard[args]\" instead!\n");
                goto error;
#if 0
                if(configure_tiling(s, strip_fmt) != 0) {
                        goto error;
                }
#endif
        }

        if ((vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_ANY) != 0) {
                if (!configure_audio(s)) {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Cannot initialize audio!\n");
                        goto error;
                }
        }

        free(fmt);
        free(in_file_contents);

        *state = s;
        return VIDCAP_INIT_OK;

error:
        free(fmt);
        vf_free(s->frame);
        free(in_file_contents);
        free(s);
        return ret;
}

static void vidcap_testcard_done(void *state)
{
        struct testcard_state *s = (struct testcard_state *) state;
        if (s->tiled) {
                int i;
                for (i = 0; i < s->tiles_cnt_horizontal; ++i) {
                        free(s->tiles_data[i]);
                }
                vf_free(s->tiled);
        }
        vf_free(s->frame);
        video_pattern_generator_destroy(s->generator);
        free(s->audio_data);
        free(s);
}

static struct video_frame *vidcap_testcard_grab(void *arg, struct audio_frame **audio)
{
        struct testcard_state *state;
        state = (struct testcard_state *)arg;

        time_ns_t curr_time = get_time_in_ns();
        if ((curr_time - state->last_frame_time) / NS_IN_SEC_DBL < 1.0 / state->frame->fps) {
                return NULL;
        }
        state->last_frame_time = curr_time;

        if (state->grab_audio) {
                state->audio.data += (ptrdiff_t) state->audio.ch_count * state->audio.bps * state->apattern.samples[state->apattern.current_idx];
                state->apattern.current_idx = (state->apattern.current_idx + 1) % state->apattern.count;
                state->audio.data_len = state->audio.ch_count * state->audio.bps * state->apattern.samples[state->apattern.current_idx];
                if (state->audio.data >= state->audio_data + AUDIO_BUFFER_SIZE(state->audio.ch_count)) {
                        state->audio.data -= AUDIO_BUFFER_SIZE(state->audio.ch_count);
                }
                *audio = &state->audio;
        } else {
                *audio = NULL;
        }

        vf_get_tile(state->frame, 0)->data = video_pattern_generator_next_frame(state->generator);

        if (state->tiled) {
                /* update tile data instead */
                int i;
                int count = state->tiled->tile_count;

                for (i = 0; i < count; ++i) {
                        /* shift - for semantics of vars refer to configure_tiling*/
                        state->tiled->tiles[i].data += vc_get_linesize(
                                        state->tiled->tiles[i].width, state->tiled->color_spec);
                        /* if out of data, move to beginning
                         * keep in mind that we have two "pictures" for
                         * every tile stored sequentially */
                        if(state->tiled->tiles[i].data >= state->tiles_data[i] +
                                        state->tiled->tiles[i].data_len * state->tiles_cnt_vertical) {
                                state->tiled->tiles[i].data = state->tiles_data[i];
                        }
                }

                return state->tiled;
        }
        return state->frame;
}

static void vidcap_testcard_probe(struct device_info **available_devices, int *count, void (**deleter)(void *))
{
        *deleter = free;

        *count = 1;
        *available_devices = (struct device_info *) calloc(*count, sizeof(struct device_info));
        struct device_info *card = *available_devices;
        snprintf(card->name, sizeof card->name, "Testing signal");

        struct size {
                int width;
                int height;
        } sizes[] = {
                {1280, 720},
                {1920, 1080},
                {3840, 2160},
        };
        int framerates[] = {24, 30, 60};
        const char * const pix_fmts[] = {"UYVY", "RGB"};

        snprintf(card->modes[0].name,
                        sizeof card->modes[0].name, "Default");
        snprintf(card->modes[0].id,
                        sizeof card->modes[0].id,
                        "{\"width\":\"\", "
                        "\"height\":\"\", "
                        "\"format\":\"\", "
                        "\"fps\":\"\"}");

        int i = 1;
        for (const char * const *pix_fmt = pix_fmts; pix_fmt != pix_fmts + sizeof pix_fmts / sizeof pix_fmts[0]; pix_fmt++) {
                for (const struct size *size = sizes; size != sizes + sizeof sizes / sizeof sizes[0]; size++) {
                        for (const int *fps = framerates; fps != framerates + sizeof framerates / sizeof framerates[0]; fps++) {
                                snprintf(card->modes[i].name,
                                                sizeof card->name,
                                                "%dx%d@%d %s",
                                                size->width, size->height,
                                                *fps, *pix_fmt);
                                snprintf(card->modes[i].id,
                                                sizeof card->modes[0].id,
                                                "{\"width\":\"%d\", "
                                                "\"height\":\"%d\", "
                                                "\"format\":\"%s\", "
                                                "\"fps\":\"%d\"}",
                                                size->width, size->height,
                                                *pix_fmt, *fps);
                                i++;
                        }
                }
        }
        dev_add_option(card, "Still", "Send still image", "still", ":still", true);
        dev_add_option(card, "Pattern", "Pattern to use", "pattern", ":pattern=", false);
}

static const struct video_capture_info vidcap_testcard_info = {
        vidcap_testcard_probe,
        vidcap_testcard_init,
        vidcap_testcard_done,
        vidcap_testcard_grab,
        MOD_NAME,
};

REGISTER_MODULE(testcard, &vidcap_testcard_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);

/* vim: set expandtab sw=8: */
