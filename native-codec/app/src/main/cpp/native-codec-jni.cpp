/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This is a JNI example where we use native methods to play video
 * using the native AMedia* APIs.
 * See the corresponding Java source file located at:
 *
 *   src/com/example/nativecodec/NativeMedia.java
 *
 * In this example we use assert() for "impossible" error conditions,
 * and explicit handling and recovery for more likely error conditions.
 */

#include <assert.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "looper.h"
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaExtractor.h"



// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
#include <android/log.h>
#define TAG "NativeCodec"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

// for native window JNI
#include <android/native_window_jni.h>
extern "C" {
#include "libavformat/avformat.h"
}
typedef struct {
    int fd;
    ANativeWindow* window;
    AMediaExtractor* ex;
    AMediaCodec *codec;
    int64_t renderstart;
    bool sawInputEOS;
    bool sawOutputEOS;
    bool isPlaying;
    bool renderonce;
    AVFormatContext *ic;
    int video_stream_index;
} workerdata;

workerdata data = {-1, NULL, NULL, NULL, 0, false, false, false, false};

enum {
    kMsgCodecBuffer,
    kMsgPause,
    kMsgResume,
    kMsgPauseAck,
    kMsgDecodeDone,
    kMsgSeek,
    kMsgTest,
};



class mylooper: public looper {
    virtual void handle(int what, void* obj);
};

static mylooper *mlooper = NULL;

int64_t systemnanotime() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}

void doCodecWork(workerdata *d) {

    ssize_t bufidx = -1;
    int err;
    if (!d->sawInputEOS) {
        bufidx = AMediaCodec_dequeueInputBuffer(d->codec, 20000);
        LOGV("input buffer %zd", bufidx);
        if (bufidx >= 0) {
            size_t bufsize;
            uint8_t *buf = AMediaCodec_getInputBuffer(d->codec, bufidx, &bufsize);
            ssize_t sampleSize;
//            ssize_t sampleSize = AMediaExtractor_readSampleData(d->ex, buf, bufsize);
//            if (sampleSize < 0) {
//                sampleSize = 0;
//                d->sawInputEOS = true;
//                LOGV("EOS");
//            }
            //int64_t presentationTimeUs = AMediaExtractor_getSampleTime(d->ex);

            AVPacket pkt1, *pkt = &pkt1;
            do
            {
                err = av_read_frame(d->ic, pkt);
                if (err < 0) {
                    LOGV("@@@ ---------read frame err %d",err);
                    usleep(1000*10);
                }
                else {
                    LOGV("@@@ --------- %d %d %u %x", pkt->stream_index, pkt->size, bufsize,
                         pkt->flags);
                    for (uint8_t *p = pkt->data; p < pkt->data + pkt->size;) {
                        int len = p[1] * 256 * 16 + p[2] * 256 + p[3];
                        printf("len = %d\n", len);
                        p[0] = p[1] = p[2] = 0;
                        p[3] = 1;
                        //dump("/tmp/arb.264",p, len + 4);
                        p += len + 4;
                    }
                }
            } while (pkt->stream_index != d->video_stream_index || err < 0);


            memcpy(buf, pkt->data, pkt->size);

            sampleSize = pkt->size;
            int64_t presentationTimeUs = pkt->pts;

            AMediaCodec_queueInputBuffer(d->codec, bufidx, 0, sampleSize, presentationTimeUs,
                    d->sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
//            AMediaExtractor_advance(d->ex);
        }
    }

    if (!d->sawOutputEOS) {
        AMediaCodecBufferInfo info;
        ssize_t status = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 0);
        if (status >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                LOGV("output EOS");
                d->sawOutputEOS = true;
            }
            int64_t presentationNano = info.presentationTimeUs * 1000;
            if (d->renderstart < 0) {
                d->renderstart = systemnanotime() - presentationNano;
            }
            int64_t delay = (d->renderstart + presentationNano) - systemnanotime();
            if (delay > 0) {
                usleep(delay / 1000);
            }
            AMediaCodec_releaseOutputBuffer(d->codec, status, info.size != 0);
            if (d->renderonce) {
                d->renderonce = false;
                return;
            }
        } else if (status == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGV("output buffers changed");
        } else if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = NULL;
            format = AMediaCodec_getOutputFormat(d->codec);
            LOGV("format changed to: %s", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            LOGV("no output buffer right now");
        } else {
            LOGV("unexpected info code: %zd", status);
        }
    }

    if (!d->sawInputEOS || !d->sawOutputEOS) {
        mlooper->post(kMsgCodecBuffer, d);
    }
}

void mylooper::handle(int what, void* obj) {
    switch (what) {
        case kMsgCodecBuffer:
            doCodecWork((workerdata*)obj);
            break;

        case kMsgDecodeDone:
        {
            workerdata *d = (workerdata*)obj;
            AMediaCodec_stop(d->codec);
            AMediaCodec_delete(d->codec);
            AMediaExtractor_delete(d->ex);
            d->sawInputEOS = true;
            d->sawOutputEOS = true;
        }
        break;

        case kMsgSeek:
        {
            workerdata *d = (workerdata*)obj;
            AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC);
            AMediaCodec_flush(d->codec);
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            if (!d->isPlaying) {
                d->renderonce = true;
                post(kMsgCodecBuffer, d);
            }
            LOGV("seeked");
        }
        break;

        case kMsgPause:
        {
            workerdata *d = (workerdata*)obj;
            if (d->isPlaying) {
                // flush all outstanding codecbuffer messages with a no-op message
                d->isPlaying = false;
                post(kMsgPauseAck, NULL, true);
            }
        }
        break;

        case kMsgResume:
        {
            workerdata *d = (workerdata*)obj;
            if (!d->isPlaying) {
                d->renderstart = -1;
                d->isPlaying = true;
                post(kMsgCodecBuffer, d);
            }
        }
        break;
        case kMsgTest:
        {
            workerdata *d = (workerdata*)obj;
            int err, i;
            LOGV("@@@ --------------------------test");
            const char *url = "rtmp://live.hkstv.hk.lxdns.com/live/hks";
            avformat_network_init();
            av_register_all();
            d->ic = NULL;
            err = avformat_open_input(&d->ic, url, NULL, NULL);
            if (err < 0) {
                LOGV("@@@ --------------------------open error %d",err);
                return;
            }
            AVFormatContext *ic = d->ic;
            err = avformat_find_stream_info(d->ic, NULL);
            if (err < 0) {
                LOGV("@@@ --------------------------avformat_find_stream_info error %d", err);
                return;
            }

            for (i = 0; i < ic->nb_streams; i++) {
                AVStream *st = ic->streams[i];
                if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                    d->video_stream_index = i;
                }
            }
//            av_dump_format(d->ic, 0, url, 0);

            //avformat_close_input(&ic);
            //avformat_network_deinit();
            LOGV("@@@ --------------------------test");
        }
        break;
    }
}




extern "C" {

JNIEXPORT void JNICALL
Java_to_mu_tomato_TomatoImpl_test(JNIEnv *env, jobject instance) {
    // TODO

}

jboolean Java_com_example_nativecodec_NativeCodec_createStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jstring filename)
{
    LOGV("@@@ create");

    // convert Java string to UTF-8
    const char *utf8 = env->GetStringUTFChars(filename, NULL);
    LOGV("opening %s", utf8);
    int fd = open(utf8, O_RDONLY);
    env->ReleaseStringUTFChars(filename, utf8);
    if (fd < 0) {
        LOGV("failed: %d (%s)", fd, strerror(errno));
        return JNI_FALSE;
    }

    data.fd = fd;

    workerdata *d = &data;

    AMediaExtractor *ex = AMediaExtractor_new();
    media_status_t err = AMediaExtractor_setDataSourceFd(ex, d->fd, 0 , LONG_MAX);
    close(d->fd);
    if (err != AMEDIA_OK) {
        LOGV("setDataSource error: %d", err);
        return JNI_FALSE;
    }

    int numtracks = AMediaExtractor_getTrackCount(ex);

    AMediaCodec *codec = NULL;

    LOGV("input has %d tracks", numtracks);
    for (int i = 0; i < numtracks; i++) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(ex, i);
        const char *s = AMediaFormat_toString(format);
        LOGV("track %d format: %s", i, s);
        const char *mime;
        if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
            LOGV("no mime type");
            return JNI_FALSE;
        } else if (!strncmp(mime, "video/", 6)) {
            // Omitting most error handling for clarity.
            // Production code should check for errors.
            AMediaExtractor_selectTrack(ex, i);
            codec = AMediaCodec_createDecoderByType(mime);
            AMediaCodec_configure(codec, format, d->window, NULL, 0);
            d->ex = ex;
            d->codec = codec;
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            d->isPlaying = false;
            d->renderonce = true;
            AMediaCodec_start(codec);
        }
        AMediaFormat_delete(format);
    }

    mlooper = new mylooper();
    mlooper->post(kMsgTest, d);
    mlooper->post(kMsgCodecBuffer, d);

    return JNI_TRUE;
}

// set the playing state for the streaming media player
void Java_com_example_nativecodec_NativeCodec_setPlayingStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jboolean isPlaying)
{
    LOGV("@@@ playpause: %d", isPlaying);
    if (mlooper) {
        if (isPlaying) {
            mlooper->post(kMsgResume, &data);
        } else {
            mlooper->post(kMsgPause, &data);
        }
    }
}


// shut down the native media system
void Java_com_example_nativecodec_NativeCodec_shutdown(JNIEnv* env, jclass clazz)
{
    LOGV("@@@ shutdown");
    if (mlooper) {
        mlooper->post(kMsgDecodeDone, &data, true /* flush */);
        mlooper->quit();
        delete mlooper;
        mlooper = NULL;
    }
    if (data.window) {
        ANativeWindow_release(data.window);
        data.window = NULL;
    }
}


// set the surface
void Java_com_example_nativecodec_NativeCodec_setSurface(JNIEnv *env, jclass clazz, jobject surface)
{
    // obtain a native window from a Java surface
    if (data.window) {
        ANativeWindow_release(data.window);
        data.window = NULL;
    }
    data.window = ANativeWindow_fromSurface(env, surface);
    LOGV("@@@ setsurface %p", data.window);
}


// rewind the streaming media player
void Java_com_example_nativecodec_NativeCodec_rewindStreamingMediaPlayer(JNIEnv *env, jclass clazz)
{
    LOGV("@@@ rewind");
    mlooper->post(kMsgSeek, &data);
}

}
