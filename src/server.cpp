#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

static constexpr char PROTOCOL_VERSION[] = "2";
using json = nlohmann::json;

class InputClient {
  public:
    int run() {
        try {
            readProtocolVersion();
            parseDeviceMetadata();
            setupUInputDevices();
            std::cout << "Device(s) created" << std::endl;
            eventLoop();
            cleanup();
        } catch (const std::exception &ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
            return 1;
        }
        return 0;
    }

  private:
    struct DeviceMeta {
        std::string name;
        uint16_t vendor;
        uint16_t product;
        json capabilities;
    };

    std::vector<DeviceMeta> metas_;
    std::vector<int> fds_;

    void readProtocolVersion() {
        std::string version;
        if (!std::getline(std::cin, version)) throw std::runtime_error("Failed to read protocol version");
        if (version != PROTOCOL_VERSION)
            throw std::runtime_error("Invalid protocol version. Got " + version + ", expected " + PROTOCOL_VERSION);
    }

    void parseDeviceMetadata() {
        std::string line;
        if (!std::getline(std::cin, line)) throw std::runtime_error("Failed to read devices JSON");
        auto arr = json::parse(line);
        for (auto &dev : arr) {
            metas_.push_back({dev["name"].get<std::string>(), dev["vendor"].get<uint16_t>(),
                              dev["product"].get<uint16_t>(), dev["capabilities"]});
        }
    }

    void setupUInputDevices() {
        for (auto &meta : metas_) {
            int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
            if (fd < 0) throw std::runtime_error("Cannot open /dev/uinput");

            // Enable event types and codes
            for (auto &cap : meta.capabilities.items()) {
                int type = std::stoi(cap.key());
                if (ioctl(fd, UI_SET_EVBIT, type) < 0) perror("UI_SET_EVBIT");
                for (auto &code_entry : cap.value()) {
                    if (type == EV_ABS) {
                        int code = code_entry[0];
                        auto &info = code_entry[1];
                        struct uinput_abs_setup abs{};
                        abs.code = code;
                        abs.absinfo.minimum = info["minimum"];
                        abs.absinfo.maximum = info["maximum"];
                        abs.absinfo.fuzz = info["fuzz"];
                        abs.absinfo.flat = info["flat"];
                        abs.absinfo.resolution = info["resolution"];
                        if (ioctl(fd, UI_ABS_SETUP, &abs) < 0) perror("UI_ABS_SETUP");
                    } else {
                        int code = code_entry.get<int>();
                        unsigned long ioctl_cmd = (type == EV_KEY)   ? UI_SET_KEYBIT
                                                  : (type == EV_REL) ? UI_SET_RELBIT
                                                                     : UI_SET_MSCBIT;
                        if (ioctl(fd, ioctl_cmd, code) < 0) perror("UI_SET_*BIT");
                    }
                }
            }

            struct uinput_setup usetup;
            std::memset(&usetup, 0, sizeof(usetup));
            std::string name = meta.name + " (via input-over-ssh)";
            std::strncpy(usetup.name, name.c_str(), UINPUT_MAX_NAME_SIZE);
            usetup.id.bustype = BUS_USB;
            usetup.id.vendor = meta.vendor;
            usetup.id.product = meta.product;

            if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) perror("UI_DEV_SETUP");
            if (ioctl(fd, UI_DEV_CREATE) < 0) perror("UI_DEV_CREATE");

            fds_.push_back(fd);
        }
    }

    void eventLoop() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            auto evt = json::parse(line);
            int idx = evt[0];
            struct input_event ev{};
            ev.type = evt[1];
            ev.code = evt[2];
            ev.value = evt[3];
            if (write(fds_[idx], &ev, sizeof(ev)) < 0) perror("write event");
        }
    }

    void cleanup() {
        for (int fd : fds_) {
            if (ioctl(fd, UI_DEV_DESTROY) < 0) perror("UI_DEV_DESTROY");
            close(fd);
        }
    }
};

int main() {
    InputClient client;
    return client.run();
}
