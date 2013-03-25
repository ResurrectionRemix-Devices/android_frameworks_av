/*
 * Copyright 2013, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NEBUG 0
#define LOG_TAG "rtptest"
#include <utils/Log.h>

#include "ANetworkSession.h"
#include "rtp/RTPSender.h"
#include "rtp/RTPReceiver.h"

#include <binder/ProcessState.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NuMediaExtractor.h>

namespace android {

struct TestHandler : public AHandler {
    TestHandler(const sp<ANetworkSession> &netSession);

    void listen();
    void connect(const char *host, int32_t port);

protected:
    virtual ~TestHandler();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatListen,
        kWhatConnect,
        kWhatReceiverNotify,
        kWhatSenderNotify,
        kWhatSendMore,
        kWhatStop,
    };

    sp<ANetworkSession> mNetSession;
    sp<NuMediaExtractor> mExtractor;
    sp<RTPSender> mSender;
    sp<RTPReceiver> mReceiver;

    size_t mMaxSampleSize;

    int64_t mFirstTimeRealUs;
    int64_t mFirstTimeMediaUs;

    status_t readMore();

    DISALLOW_EVIL_CONSTRUCTORS(TestHandler);
};

TestHandler::TestHandler(const sp<ANetworkSession> &netSession)
    : mNetSession(netSession),
      mMaxSampleSize(1024 * 1024),
      mFirstTimeRealUs(-1ll),
      mFirstTimeMediaUs(-1ll) {
}

TestHandler::~TestHandler() {
}

void TestHandler::listen() {
    sp<AMessage> msg = new AMessage(kWhatListen, id());
    msg->post();
}

void TestHandler::connect(const char *host, int32_t port) {
    sp<AMessage> msg = new AMessage(kWhatConnect, id());
    msg->setString("host", host);
    msg->setInt32("port", port);
    msg->post();
}

void TestHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatListen:
        {
            sp<AMessage> notify = new AMessage(kWhatReceiverNotify, id());
            mReceiver = new RTPReceiver(mNetSession, notify);
            looper()->registerHandler(mReceiver);

            CHECK_EQ((status_t)OK,
                     mReceiver->registerPacketType(
                         33, RTPReceiver::PACKETIZATION_H264));

            int32_t receiverRTPPort;
            CHECK_EQ((status_t)OK,
                     mReceiver->initAsync(
                         RTPReceiver::TRANSPORT_UDP,  // rtpMode
                         RTPReceiver::TRANSPORT_UDP,  // rtcpMode
                         &receiverRTPPort));

            printf("picked receiverRTPPort %d\n", receiverRTPPort);

#if 0
            CHECK_EQ((status_t)OK,
                     mReceiver->connect(
                         "127.0.0.1", senderRTPPort, senderRTPPort + 1));
#endif
            break;
        }

        case kWhatConnect:
        {
            AString host;
            CHECK(msg->findString("host", &host));

            int32_t receiverRTPPort;
            CHECK(msg->findInt32("port", &receiverRTPPort));

            mExtractor = new NuMediaExtractor;
            CHECK_EQ((status_t)OK,
                     mExtractor->setDataSource(
                             "/sdcard/Frame Counter HD 30FPS_1080p.mp4"));

            bool haveVideo = false;
            for (size_t i = 0; i < mExtractor->countTracks(); ++i) {
                sp<AMessage> format;
                CHECK_EQ((status_t)OK, mExtractor->getTrackFormat(i, &format));

                AString mime;
                CHECK(format->findString("mime", &mime));

                if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime.c_str())) {
                    mExtractor->selectTrack(i);
                    haveVideo = true;
                    break;
                }
            }

            CHECK(haveVideo);

            sp<AMessage> notify = new AMessage(kWhatSenderNotify, id());
            mSender = new RTPSender(mNetSession, notify);
            looper()->registerHandler(mSender);

            int32_t senderRTPPort;
            CHECK_EQ((status_t)OK,
                     mSender->initAsync(
                         host.c_str(),
                         receiverRTPPort,
                         RTPSender::TRANSPORT_UDP,  // rtpMode
                         receiverRTPPort + 1,
                         RTPSender::TRANSPORT_UDP,  // rtcpMode
                         &senderRTPPort));

            printf("picked senderRTPPort %d\n", senderRTPPort);
            break;
        }

        case kWhatSenderNotify:
        {
            ALOGI("kWhatSenderNotify");

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case RTPSender::kWhatInitDone:
                {
                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGI("RTPSender::initAsync completed w/ err %d", err);

                    if (err == OK) {
                        err = readMore();

                        if (err != OK) {
                            (new AMessage(kWhatStop, id()))->post();
                        }
                    }
                    break;
                }

                case RTPSender::kWhatError:
                    break;
            }
            break;
        }

        case kWhatReceiverNotify:
        {
            ALOGI("kWhatReceiverNotify");

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case RTPReceiver::kWhatInitDone:
                {
                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGI("RTPReceiver::initAsync completed w/ err %d", err);
                    break;
                }

                case RTPSender::kWhatError:
                    break;
            }
            break;
        }

        case kWhatSendMore:
        {
            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

            CHECK_EQ((status_t)OK,
                     mSender->queueBuffer(
                         accessUnit,
                         33,
                         RTPSender::PACKETIZATION_H264));

            status_t err = readMore();

            if (err != OK) {
                (new AMessage(kWhatStop, id()))->post();
            }
            break;
        }

        case kWhatStop:
        {
            if (mReceiver != NULL) {
                looper()->unregisterHandler(mReceiver->id());
                mReceiver.clear();
            }

            if (mSender != NULL) {
                looper()->unregisterHandler(mSender->id());
                mSender.clear();
            }

            mExtractor.clear();

            looper()->stop();
            break;
        }

        default:
            TRESPASS();
    }
}

status_t TestHandler::readMore() {
    int64_t timeUs;
    status_t err = mExtractor->getSampleTime(&timeUs);

    if (err != OK) {
        return err;
    }

    sp<ABuffer> accessUnit = new ABuffer(mMaxSampleSize);
    CHECK_EQ((status_t)OK, mExtractor->readSampleData(accessUnit));

    accessUnit->meta()->setInt64("timeUs", timeUs);

    CHECK_EQ((status_t)OK, mExtractor->advance());

    int64_t nowUs = ALooper::GetNowUs();
    int64_t whenUs;

    if (mFirstTimeRealUs < 0ll) {
        mFirstTimeRealUs = whenUs = nowUs;
        mFirstTimeMediaUs = timeUs;
    } else {
        whenUs = mFirstTimeRealUs + timeUs - mFirstTimeMediaUs;
    }

    sp<AMessage> msg = new AMessage(kWhatSendMore, id());
    msg->setBuffer("accessUnit", accessUnit);
    msg->post(whenUs - nowUs);

    return OK;
}

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s -c host:port\tconnect to remote host\n"
            "               -l       \tlisten\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    // srand(time(NULL));

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    bool listen = false;
    int32_t connectToPort = -1;
    AString connectToHost;

    int res;
    while ((res = getopt(argc, argv, "hc:l")) >= 0) {
        switch (res) {
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    usage(argv[0]);
                    exit(1);
                }

                connectToHost.setTo(optarg, colonPos - optarg);

                char *end;
                connectToPort = strtol(colonPos + 1, &end, 10);

                if (*end != '\0' || end == colonPos + 1
                        || connectToPort < 1 || connectToPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case 'l':
            {
                listen = true;
                break;
            }

            case '?':
            case 'h':
                usage(argv[0]);
                exit(1);
        }
    }

    if (!listen && connectToPort < 0) {
        fprintf(stderr,
                "You need to select either client or server mode.\n");
        exit(1);
    }

    sp<ANetworkSession> netSession = new ANetworkSession;
    netSession->start();

    sp<ALooper> looper = new ALooper;

    sp<TestHandler> handler = new TestHandler(netSession);
    looper->registerHandler(handler);

    if (listen) {
        handler->listen();
    }

    if (connectToPort >= 0) {
        handler->connect(connectToHost.c_str(), connectToPort);
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}

