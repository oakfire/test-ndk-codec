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
#include <string>

#include "looper.h"
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaExtractor.h"

// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
#include <android/log.h>
#define TAG "NativeCodec"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// for native window JNI
#include <android/native_window_jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

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
    bool isRawH264;
    const void* buf;
    int buf_size;
    int buf_index;
    int64_t rawTimeUs;
} workerdata;

workerdata data = {-1, NULL, NULL, NULL, 0, false, false, false, false, false, NULL, 0, 0, 0};

enum {
    kMsgCodecBuffer,
    kMsgPause,
    kMsgResume,
    kMsgPauseAck,
    kMsgDecodeDone,
    kMsgSeek,
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


int FindStartCode(const uint8_t *Buf) {
    if(Buf[0]!=0 || Buf[1]!=0 || Buf[2] !=0 || Buf[3] !=1)
        return 0;//0x00000001?
    else
        return 1;
}

int GetOneNALU(const uint8_t *buf, int bufsize, int *end)
{
    int pos = 0;
    int StartCodeFound = 0;
    int info = 0;

    info = FindStartCode (buf);
    if(info != 1) {
        return -1;
    }

    while (!StartCodeFound){
        pos++;
        if (pos > bufsize){
            *end = 1;
            return bufsize;
        }
        StartCodeFound = FindStartCode(buf + pos);
    }
    *end = 0;
    return pos;
}

int InputRawData(uint8_t* buf, size_t bufsize, workerdata *d) {
    if (d->buf_index >= d->buf_size) {
        d->buf_index = 0;
        d->rawTimeUs = 0;
        return -1;
    }
    int end = 0;
    int nalu_len = GetOneNALU(((const uint8_t*)d->buf) + d->buf_index, d->buf_size - d->buf_index, &end);
    if (nalu_len > 0) {
        memcpy(buf, ((const uint8_t*)d->buf) + d->buf_index, nalu_len);
        d->buf_index += nalu_len;
        d->rawTimeUs += 33333;
        return nalu_len;
    } else {
        return -1;
    }

}


void doCodecWork(workerdata *d) {

    ssize_t bufidx = -1;
    if (!d->sawInputEOS) {
        bufidx = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
        LOGV("input buffer %zd", bufidx);
        if (bufidx >= 0) {
            size_t bufsize;
            auto buf = AMediaCodec_getInputBuffer(d->codec, bufidx, &bufsize);
            ssize_t  sampleSize = 0;
            if (d->isRawH264) {
                sampleSize = InputRawData(buf, bufsize, d);
                LOGV("input size: %zd", sampleSize);
            } else {
                sampleSize = AMediaExtractor_readSampleData(d->ex, buf, bufsize);
            }

            if (sampleSize < 0) {
                sampleSize = 0;
                d->sawInputEOS = true;
                LOGV("EOS");
            }
            auto presentationTimeUs = d->isRawH264 ? d->rawTimeUs : AMediaExtractor_getSampleTime(d->ex);

            AMediaCodec_queueInputBuffer(d->codec, bufidx, 0, sampleSize, presentationTimeUs,
                    d->sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);

            if (!d->isRawH264)
                AMediaExtractor_advance(d->ex);
        }
    }

    while (!d->sawOutputEOS) {
        AMediaCodecBufferInfo info;
        auto status = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 0);
        if (status >= 0) {
            LOGV("got a frame : flags: %d, size: %d, time: %lld", info.flags, info.size, info.presentationTimeUs);
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
            auto format = AMediaCodec_getOutputFormat(d->codec);
            LOGV("format changed to: %s", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            LOGV("no output buffer right now");
            break;
        } else {
            LOGV("unexpected info code: %zd", status);
            break;
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
            if (d->isRawH264) {
                d->buf_index = 0;
                d->buf = NULL;
                d->buf_size = 0;
            } else {
                AMediaExtractor_delete(d->ex);
            }

            d->sawInputEOS = true;
            d->sawOutputEOS = true;
        }
        break;

        case kMsgSeek:
        {
            workerdata *d = (workerdata*)obj;
            if (d->isRawH264) {
                d->buf_index = 0;
            } else {
                AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC);
            }

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
    }
}




extern "C" {

jboolean Java_com_example_nativecodec_NativeCodec_createStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jobject assetMgr, jstring filename)
{
    LOGV("@@@ create");

    // convert Java string to UTF-8
    const char *utf8 = env->GetStringUTFChars(filename, NULL);
    LOGV("opening %s", utf8);

    std::string name_bak = utf8;
    env->ReleaseStringUTFChars(filename, utf8);


    workerdata *d = &data;

    if (name_bak.find(".h264") != std::string::npos) {
        LOGV("h264 raw data file");

        auto asset_file = AAssetManager_open(AAssetManager_fromJava(env, assetMgr), name_bak.c_str(), 0);
        d->buf = AAsset_getBuffer(asset_file);
        d->buf_size = AAsset_getLength(asset_file);
        d->buf_index = 0;
        AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");
        AMediaFormat *format = AMediaFormat_new();
        AMediaFormat_setString(format, "mime", "video/avc");
        AMediaFormat_setInt32(format, "width", 1920);
        AMediaFormat_setInt32(format, "height", 1080);
        AMediaCodec_configure(codec, format, d->window, NULL, 0);

        d->codec = codec;
        d->renderstart = -1;
        d->sawInputEOS = false;
        d->sawOutputEOS = false;
        d->isPlaying = false;
        d->renderonce = true;
        d->isRawH264 = true;
        AMediaCodec_start(codec);
        AMediaFormat_delete(format);

    } else {

        off_t outStart, outLen;
        int fd = AAsset_openFileDescriptor(AAssetManager_open(AAssetManager_fromJava(env, assetMgr), name_bak.c_str(), 0),
                                           &outStart, &outLen);
        if (fd < 0) {
            LOGE("failed to open file: %s %d (%s)", utf8, fd, strerror(errno));
            return JNI_FALSE;
        }

        data.fd = fd;

        AMediaExtractor *ex = AMediaExtractor_new();
        media_status_t err = AMediaExtractor_setDataSourceFd(ex, d->fd,
                                                             static_cast<off64_t>(outStart),
                                                             static_cast<off64_t>(outLen));
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
                d->isRawH264 = false;
                AMediaCodec_start(codec);
            }
            AMediaFormat_delete(format);
        }
    }



    mlooper = new mylooper();
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
    if (mlooper) {
        mlooper->post(kMsgSeek, &data);
    }
}

}
