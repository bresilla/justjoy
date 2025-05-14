// src/warpout.cpp

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <linux/uinput.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include <CLI/CLI.hpp>

#include "warpout/joystick.hpp"
#include "warpout/server.hpp"
#include "warpout/slip.hpp"
#include "warpout/tlvc.hpp"

//---------------------------------------------------------------------------
// Shared types for both client & server

typedef struct __attribute__((packed)) {
    uint16_t bus;
    uint16_t vid;
    uint16_t pid;
    uint16_t version;
} input_dev_info_t;

typedef struct __attribute__((packed)) {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t flat;
    int32_t fuzz;
} abs_axis_info_t;

typedef struct {
    int absAxis[KEY_MAX];
    int relAxis[KEY_MAX];
    int buttons[KEY_MAX];
} js_index_map_t;

//---------------------------------------------------------------------------
// SLIP + TLVC encode & transmit helper

static bool encode_and_transmit(int sockFd, uint16_t tag, void *data, size_t len) {
    tlvc_data_t tlvc = {};
    tlvc_encode_data(&tlvc, tag, len, data);

    auto *enc = slip_encode_message_create(len);
    slip_encode_begin(enc);

    auto *raw = reinterpret_cast<uint8_t *>(&tlvc.header);
    for (size_t i = 0; i < sizeof(tlvc.header); ++i)
        slip_encode_byte(enc, raw[i]);
    raw = reinterpret_cast<uint8_t *>(tlvc.data);
    for (size_t i = 0; i < tlvc.dataLen; ++i)
        slip_encode_byte(enc, raw[i]);
    raw = reinterpret_cast<uint8_t *>(&tlvc.footer);
    for (size_t i = 0; i < sizeof(tlvc.footer); ++i)
        slip_encode_byte(enc, raw[i]);

    slip_encode_finish(enc);

    int remaining = enc->index;
    raw = enc->encoded;
    while (remaining > 0) {
        int written = write(sockFd, raw, remaining);
        if (written <= 0 && errno != EINTR && errno != EAGAIN) {
            std::perror("socket write");
            slip_encode_message_destroy(enc);
            return false;
        }
        if (written > 0) {
            remaining -= written;
            raw += written;
        }
    }

    slip_encode_message_destroy(enc);
    return true;
}

//---------------------------------------------------------------------------
// js_index_map utilities

static void js_index_map_init(js_index_map_t *m) {
    for (int i = 0; i < KEY_MAX; ++i)
        m->absAxis[i] = m->relAxis[i] = m->buttons[i] = -1;
}

static void js_index_map_set(js_index_map_t *m, int type, int code, int idx) {
    if (type == EV_ABS)
        m->absAxis[code] = idx;
    else if (type == EV_REL)
        m->relAxis[code] = idx;
    else if (type == EV_KEY)
        m->buttons[code] = idx;
}

static int js_index_map_get(const js_index_map_t *m, int type, int code) {
    if (type == EV_ABS)
        return m->absAxis[code];
    else if (type == EV_REL)
        return m->relAxis[code];
    else if (type == EV_KEY)
        return m->buttons[code];
    return -1;
}

static inline bool is_bit_set(const uint8_t *buf, int bit) { return buf[bit / 8] & (1 << (bit % 8)); }

//---------------------------------------------------------------------------
// Client mode

static void run_client(const std::string &device, const std::string &server_addr, uint16_t server_port) {
    // 1) Open device
    int fd = open(device.c_str(), O_RDONLY);
    if (fd < 0) {
        std::perror(("open " + device).c_str());
        return;
    }

    // 2) Build index map + config
    auto indexMap = std::make_unique<js_index_map_t>();
    js_index_map_init(indexMap.get());
    js_config_t config = {};

    // 2a) Get device info
    input_dev_info_t info = {};
    ioctl(fd, EVIOCGID, &info);
    config.pid = info.pid;
    config.vid = info.vid;

    // 2b) Get device name
    char name[256] = {};
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    strncpy(config.name, name, sizeof(config.name));

    // 2c) Query supported events
    uint8_t bits[EV_MAX][(KEY_MAX + 7) / 8] = {};
    ioctl(fd, EVIOCGBIT(0, EV_MAX), bits[0]);
    for (int t = 0; t < EV_MAX; ++t) {
        if (t == EV_SYN || !is_bit_set(bits[0], t)) continue;
        ioctl(fd, EVIOCGBIT(t, KEY_MAX), bits[t]);
        for (int c = 0; c < KEY_MAX; ++c) {
            if (!is_bit_set(bits[t], c)) continue;
            if (t == EV_ABS) {
                abs_axis_info_t ai = {};
                ioctl(fd, EVIOCGABS(c), &ai);
                js_index_map_set(indexMap.get(), t, c, config.absAxisCount);
                config.absAxis[config.absAxisCount] = c;
                config.absAxisMin[config.absAxisCount] = ai.minimum;
                config.absAxisMax[config.absAxisCount] = ai.maximum;
                config.absAxisFuzz[config.absAxisCount] = ai.fuzz;
                config.absAxisFlat[config.absAxisCount] = ai.flat;
                config.absAxisResolution[config.absAxisCount] = 0;
                ++config.absAxisCount;
            } else if (t == EV_REL) {
                js_index_map_set(indexMap.get(), t, c, config.relAxisCount);
                config.relAxis[config.relAxisCount++] = c;
            } else if (t == EV_KEY) {
                js_index_map_set(indexMap.get(), t, c, config.buttonCount);
                config.buttons[config.buttonCount++] = c;
            }
        }
    }

    // 3) Connect to server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        close(fd);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_addr.c_str(), &addr.sin_addr);
    addr.sin_port = htons(server_port);
    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        std::perror("connect");
        close(sock);
        close(fd);
        return;
    }

    // 4) Send configuration
    if (!encode_and_transmit(sock, 0, &config, sizeof(config))) {
        close(sock);
        close(fd);
        return;
    }

    // 5) Prepare report buffer
    size_t reportSize = joystick_get_report_size(&config);
    std::vector<uint8_t> rawReport(reportSize);
    js_report_t report;
    report.absAxis = reinterpret_cast<int32_t *>(rawReport.data());
    report.relAxis = reinterpret_cast<int32_t *>(rawReport.data() + sizeof(int32_t) * config.absAxisCount);
    report.buttons = rawReport.data() + sizeof(int32_t) * (config.absAxisCount + config.relAxisCount);

    // 6) Event loop
    while (true) {
        input_event evbuf[128];
        ssize_t rd = read(fd, evbuf, sizeof(evbuf));
        if (rd <= 0) break;

        size_t cnt = rd / sizeof(input_event);
        for (size_t i = 0; i < cnt; ++i) {
            const auto &e = evbuf[i];
            if (e.type == EV_SYN) {
                if (!encode_and_transmit(sock, 1, rawReport.data(), reportSize)) goto cleanup;
            } else {
                int idx = js_index_map_get(indexMap.get(), e.type, e.code);
                if (idx < 0) continue;
                if (e.type == EV_KEY)
                    report.buttons[idx] = (e.value != 0);
                else if (e.type == EV_ABS)
                    report.absAxis[idx] = e.value;
                else if (e.type == EV_REL)
                    report.relAxis[idx] = e.value;
            }
        }
    }

cleanup:
    close(sock);
    close(fd);
}

//---------------------------------------------------------------------------
// Server mode

struct client_ctx {
    slip_decode_message_t *dec;
    bool configSet;
    js_context_t *jsctx;
};

static void *on_connect(int fd) {
    auto *c = (client_ctx *)std::calloc(1, sizeof(client_ctx));
    c->dec = slip_decode_message_create(32768);
    slip_decode_begin(c->dec);
    c->configSet = false;
    c->jsctx = nullptr;
    std::printf("Client %d connected\n", fd);
    return c;
}

static void on_disconnect(void *vc) {
    auto *c = (client_ctx *)vc;
    slip_decode_message_destroy(c->dec);
    if (c->configSet && c->jsctx) joystick_destroy(c->jsctx);
    std::printf("Client disconnected\n");
    std::free(c);
}

static bool emit_event(int fd, int type, int code, int val) {
    input_event ie = {};
    ie.type = type;
    ie.code = code;
    ie.value = val;
    return write(fd, &ie, sizeof(ie)) == sizeof(ie);
}

static void handle_msg(client_ctx *c, uint16_t tag, void *data, size_t len) {
    if (tag == 0) {
        if (c->configSet) {
            std::puts("config already set");
            return;
        }
        if (len != sizeof(js_config_t)) {
            std::printf("bad config size %zu\n", len);
            return;
        }
        c->jsctx = joystick_create((js_config_t *)data);
        c->configSet = true;
    } else if (tag == 1) {
        if (!c->configSet) {
            std::puts("no config yet");
            return;
        }
        auto *cfg = &c->jsctx->config;
        auto *raw = (uint8_t *)data;
        js_report_t r;
        r.absAxis = (int32_t *)raw;
        r.relAxis = (int32_t *)(raw + sizeof(int32_t) * cfg->absAxisCount);
        r.buttons = raw + sizeof(int32_t) * (cfg->absAxisCount + cfg->relAxisCount);

        for (int i = 0; i < cfg->absAxisCount; ++i)
            if (!emit_event(c->jsctx->fd, EV_ABS, cfg->absAxis[i], r.absAxis[i])) std::puts("ABS emit failed");

        for (int i = 0; i < cfg->relAxisCount; ++i)
            if (!emit_event(c->jsctx->fd, EV_REL, cfg->relAxis[i], r.relAxis[i])) std::puts("REL emit failed");

        for (int i = 0; i < cfg->buttonCount; ++i)
            if (!emit_event(c->jsctx->fd, EV_KEY, cfg->buttons[i], r.buttons[i])) std::puts("KEY emit failed");

        emit_event(c->jsctx->fd, EV_SYN, 0, 0);
    } else {
        std::printf("unknown tag %u\n", tag);
    }
}

static bool on_read(int fd, void *vc) {
    auto *c = (client_ctx *)vc;
    uint8_t buf[256];
    ssize_t rd;
    while ((rd = ::read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < rd; ++i) {
            auto rc = slip_decode_byte(c->dec, buf[i]);
            if (rc == SlipDecodeEndOfFrame) {
                tlvc_data_t tlvc;
                if (tlvc_decode_data(&tlvc, c->dec->raw, c->dec->index))
                    handle_msg(c, tlvc.header.tag, tlvc.data, tlvc.dataLen);
                slip_decode_begin(c->dec);
            } else if (rc != SlipDecodeOk) {
                slip_decode_begin(c->dec);
            }
        }
    }
    return rd != 0 || (rd < 0 && (errno == EINTR || errno == EAGAIN));
}

//---------------------------------------------------------------------------
// Modified run_server to take a bind address

static void run_server(const std::string &bind_addr, uint16_t port) {
    client_handlers_t handlers = {.onConnect = on_connect, .onDisconnect = on_disconnect, .onReadData = on_read};
    auto *srv = server_create(bind_addr.c_str(), port, 10, &handlers);
    if (!srv) {
        std::fprintf(stderr, "Failed to create server on %s:%u\n", bind_addr.c_str(), port);
        std::exit(1);
    }
    server_run(srv);
}

//---------------------------------------------------------------------------
// main()

int main(int argc, char **argv) {
    CLI::App app{"warpout — joystick/uinput proxy (client or server)"};

    // Server subcommand
    auto srv = app.add_subcommand("server", "Run as server");
    std::string bind_addr;
    uint16_t sPort;
    srv->add_option("-b,--bind", bind_addr, "Bind address/interface")->default_val("0.0.0.0");
    srv->add_option("-p,--port", sPort, "Listen port")->required();

    // Client subcommand
    auto cli = app.add_subcommand("client", "Run as client");
    std::string dev, addr;
    uint16_t cPort;
    cli->add_option("-d,--device", dev, "Input device path")->required();
    cli->add_option("-a,--address", addr, "Server address")->required();
    cli->add_option("-p,--port", cPort, "Server port")->required();

    CLI11_PARSE(app, argc, argv);

    if (srv->parsed()) {
        run_server(bind_addr, sPort);
    } else if (cli->parsed()) {
        while (true) {
            run_client(dev, addr, cPort);
            sleep(4);
        }
    } else {
        std::cout << app.help() << std::endl;
    }

    return 0;
}
