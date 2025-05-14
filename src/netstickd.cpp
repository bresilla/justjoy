// Netstick - Copyright (c) 2021 Funkenstein Software Consulting.
// See LICENSE.txt for more details.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "joystick.h"
#include "server.h"
#include "slip.h"
#include "tlvc.h"

//---------------------------------------------------------------------------
// SERVER CODE
//---------------------------------------------------------------------------
typedef struct {
    slip_decode_message_t *slipDecode;
    bool configSet;
    js_context_t *joystickContext;
} jsproxy_client_context_t;

//---------------------------------------------------------------------------
void *jsproxy_connect(int clientFd_) {
    std::printf("enter:%s, %d\n", __func__, clientFd_);
    auto *newContext = static_cast<jsproxy_client_context_t *>(std::calloc(1, sizeof(jsproxy_client_context_t)));
    newContext->slipDecode = slip_decode_message_create(32768);
    slip_decode_begin(newContext->slipDecode);
    newContext->configSet = false;
    newContext->joystickContext = nullptr;
    return newContext;
}

//---------------------------------------------------------------------------
void jsproxy_disconnect(void *clientContext_) {
    auto *context = static_cast<jsproxy_client_context_t *>(clientContext_);
    slip_decode_message_destroy(context->slipDecode);
    std::printf("enter:%s, %d\n", __func__, context->joystickContext ? context->joystickContext->fd : -1);
    if (context->configSet && context->joystickContext) {
        joystick_destroy(context->joystickContext);
    }
}

//---------------------------------------------------------------------------
static bool emit(int fd, int type, int code, int val) {
    input_event ie{};
    ie.type = type;
    ie.code = code;
    ie.value = val;
    ie.time.tv_sec = 0;
    ie.time.tv_usec = 0;
    return write(fd, &ie, sizeof(ie)) == sizeof(ie);
}

//---------------------------------------------------------------------------
static void jsproxy_handle_message(jsproxy_client_context_t *context_, uint16_t eventType_, void *data_,
                                   size_t dataSize_) {
    switch (eventType_) {
    case 0: {
        if (context_->configSet) {
            std::printf("configuration already set - ignoring\n");
            return;
        }
        if (dataSize_ != sizeof(js_config_t)) {
            std::printf("expected configuration size %zu, got %zu\n", sizeof(js_config_t), dataSize_);
            return;
        }
        auto *config = static_cast<js_config_t *>(data_);
        context_->joystickContext = joystick_create(config);
        context_->configSet = true;
    } break;

    case 1: {
        if (!context_->configSet || !context_->joystickContext) {
            std::printf("joystick hasn't been configured.  Bailing\n");
            return;
        }

        js_config_t *config = &context_->joystickContext->config;
        int fd = context_->joystickContext->fd;

        // Cast the raw data buffer to bytes
        auto *rawReport = reinterpret_cast<uint8_t *>(data_);

        // Build a local js_report_t on the stack
        js_report_t report;
        report.absAxis = reinterpret_cast<int32_t *>(rawReport);
        report.relAxis = reinterpret_cast<int32_t *>(rawReport + sizeof(int32_t) * config->absAxisCount);
        report.buttons =
            reinterpret_cast<uint8_t *>(rawReport + sizeof(int32_t) * (config->absAxisCount + config->relAxisCount));

        // Emit absolute-axis events
        for (int i = 0; i < config->absAxisCount; ++i) {
            if (!emit(fd, EV_ABS, config->absAxis[i], report.absAxis[i])) {
                std::printf("error writing ABS event\n");
            }
        }
        // Emit relative-axis events
        for (int i = 0; i < config->relAxisCount; ++i) {
            if (!emit(fd, EV_REL, config->relAxis[i], report.relAxis[i])) {
                std::printf("error writing REL event\n");
            }
        }
        // Emit button events
        for (int i = 0; i < config->buttonCount; ++i) {
            if (!emit(fd, EV_KEY, config->buttons[i], report.buttons[i])) {
                std::printf("error writing KEY event\n");
            }
        }
        // Send a sync
        if (!emit(fd, EV_SYN, 0, 0)) {
            std::printf("error writing SYN event\n");
        }

    } break;

    default:
        std::printf("unknown message %u\n", static_cast<unsigned>(eventType_));
        break;
    }
}

//---------------------------------------------------------------------------
bool jsproxy_read(int clientFd_, void *clientContext_) {
    uint8_t buf[256];
    int nRead;
    auto *context = static_cast<jsproxy_client_context_t *>(clientContext_);

    do {
        nRead = ::read(clientFd_, buf, sizeof(buf));
        if (nRead <= 0) break;

        for (int i = 0; i < nRead; ++i) {
            auto rc = slip_decode_byte(context->slipDecode, buf[i]);
            if (rc == SlipDecodeEndOfFrame) {
                tlvc_data_t tlvc;
                if (tlvc_decode_data(&tlvc, context->slipDecode->raw, context->slipDecode->index)) {
                    jsproxy_handle_message(context, tlvc.header.tag, tlvc.data, tlvc.dataLen);
                }
                slip_decode_begin(context->slipDecode);
            } else if (rc != SlipDecodeOk) {
                slip_decode_begin(context->slipDecode);
            }
        }
    } while (nRead > 0);

    if (nRead == 0) return false;
    if (nRead < 0 && (errno == EINTR || errno == EAGAIN)) return true;
    return true;
}

//---------------------------------------------------------------------------
static void jsproxy_server(uint16_t port_) {
    client_handlers_t handlers = {
        .onConnect = jsproxy_connect, .onDisconnect = jsproxy_disconnect, .onReadData = jsproxy_read};
    auto *server = server_create(port_, 10, &handlers);
    server_run(server);
}

//---------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: netstickd [server port]\n");
        return -1;
    }
    jsproxy_server(static_cast<uint16_t>(std::atoi(argv[1])));
    return 0;
}
