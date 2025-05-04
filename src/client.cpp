#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <unistd.h>

#include <libevdev-1.0/libevdev/libevdev.h>
#include <nlohmann/json.hpp>

static constexpr char PROTOCOL_VERSION[] = "2";
using json = nlohmann::json;

struct Options {
    bool list_devices = false;
    bool exclusive = false;
    std::vector<std::string> by_path;
    std::vector<std::string> by_name;
};

class DeviceForwarder {
  public:
    DeviceForwarder(const Options &opts) : opts_(opts), done_(false) {
        if (!opts_.list_devices) {
            gatherNameMap();
            selectDevices();
        }
    }

    int run() {
        if (opts_.list_devices) {
            for (auto &p : listDevicePaths()) {
                std::cout << p << "\n";
            }
            return 0;
        }

        // Protocol version
        std::cout << PROTOCOL_VERSION << std::endl;

        // Print device metadata
        printDeviceInfo();

        // Launch forward threads
        launchForwardThreads();

        // Join threads
        for (auto &t : threads_) {
            if (t.joinable()) t.join();
        }
        return 0;
    }

  private:
    Options opts_;
    std::vector<std::string> devices_;
    std::unordered_map<std::string, std::string> name_to_path_;
    std::vector<std::thread> threads_;
    std::atomic<bool> done_;

    std::vector<std::string> listDevicePaths() {
        std::vector<std::string> paths;
        glob_t glob_result;
        int ret = glob("/dev/input/event*", 0, nullptr, &glob_result);
        if (ret != 0) {
            if (ret != GLOB_NOMATCH) std::perror("glob(/dev/input/event*)");
            globfree(&glob_result);
            return paths;
        }
        for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            paths.emplace_back(glob_result.gl_pathv[i]);
        }
        globfree(&glob_result);
        return paths;
    }

    void gatherNameMap() {
        for (auto &p : listDevicePaths()) {
            int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            struct libevdev *dev = nullptr;
            if (libevdev_new_from_fd(fd, &dev) >= 0) {
                name_to_path_[libevdev_get_name(dev)] = p;
                libevdev_free(dev);
            }
            close(fd);
        }
    }

    void selectDevices() {
        for (auto &p : opts_.by_path)
            devices_.push_back(p);

        for (auto &name : opts_.by_name) {
            auto it = name_to_path_.find(name);
            if (it != name_to_path_.end()) {
                devices_.push_back(it->second);
            } else {
                std::cerr << "Warning: no device named " << name << "\n";
            }
        }
    }

    json encodeDevice(libevdev *dev) {
        json j;
        j["name"] = libevdev_get_name(dev);
        j["vendor"] = libevdev_get_id_vendor(dev);
        j["product"] = libevdev_get_id_product(dev);

        json caps = json::object();
        for (int t = 0; t <= EV_MAX; ++t) {
            if (!libevdev_has_event_type(dev, t)) continue;
            json evs = json::array();
            for (int code = 0; code <= libevdev_event_type_get_max(t); ++code) {
                if (libevdev_has_event_code(dev, t, code)) {
                    evs.push_back(code);
                }
            }
            caps[std::to_string(t)] = evs;
        }
        j["capabilities"] = caps;
        return j;
    }

    void printDeviceInfo() {
        json info = json::array();
        for (auto &path : devices_) {
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            struct libevdev *dev = nullptr;
            if (fd >= 0 && libevdev_new_from_fd(fd, &dev) == 0) {
                info.push_back(encodeDevice(dev));
                libevdev_free(dev);
            }
            close(fd);
        }
        std::cout << info.dump() << std::endl;
    }

    void launchForwardThreads() {
        for (size_t i = 0; i < devices_.size(); ++i) {
            threads_.emplace_back(&DeviceForwarder::forwardDeviceThread, i, devices_[i], opts_.exclusive,
                                  std::ref(done_));
        }
    }

    static void forwardDeviceThread(int idx, const std::string &path, bool exclusive, std::atomic<bool> &done_flag) {
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            std::perror(("Opening " + path).c_str());
            done_flag = true;
            return;
        }
        struct libevdev *dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            std::cerr << "Failed to init libevdev for " << path << "\n";
            close(fd);
            done_flag = true;
            return;
        }
        if (exclusive) {
            if (libevdev_grab(dev, LIBEVDEV_GRAB) != 0) std::cerr << "Warning: could not grab " << path << "\n";
        }
        struct input_event ev;
        while (!done_flag) {
            int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == 0) {
                json out = json::array({idx, ev.type, ev.code, ev.value});
                std::cout << out.dump() << std::endl;
            }
        }
        if (exclusive) libevdev_grab(dev, LIBEVDEV_UNGRAB);
        libevdev_free(dev);
        close(fd);
    }
};

Options parseArgs(int argc, char **argv) {
    Options opts;
    const char *short_opts = "Lp:n:e";
    const struct option long_opts[] = {{"list-devices", no_argument, nullptr, 'L'},
                                       {"device-by-path", required_argument, nullptr, 'p'},
                                       {"device-by-name", required_argument, nullptr, 'n'},
                                       {"exclusive", no_argument, nullptr, 'e'},
                                       {nullptr, 0, nullptr, 0}};
    int c;
    while ((c = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        switch (c) {
        case 'L':
            opts.list_devices = true;
            break;
        case 'p':
            opts.by_path.emplace_back(optarg);
            break;
        case 'n':
            opts.by_name.emplace_back(optarg);
            break;
        case 'e':
            opts.exclusive = true;
            break;
        default:
            std::cerr << "Unknown option\n";
            exit(1);
        }
    }
    return opts;
}

int main(int argc, char **argv) {
    auto opts = parseArgs(argc, argv);
    DeviceForwarder forwarder(opts);
    return forwarder.run();
}
