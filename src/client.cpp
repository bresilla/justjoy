#include <atomic>
#include <cstdlib>    // For EXIT_FAILURE, exit()
#include <filesystem> // Using for convenience, but glob is still used below
#include <iostream>
#include <memory>    // For std::unique_ptr
#include <mutex>     // For std::mutex and std::lock_guard
#include <stdexcept> // For std::runtime_error (optional for error handling)
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// External/System Headers
#include <fcntl.h>
#include <glob.h>   // Still using glob for listing devices
#include <unistd.h> // For close()

#include <CLI/CLI.hpp> // Include CLI11 header
#include <libevdev-1.0/libevdev/libevdev.h>
#include <nlohmann/json.hpp>

// --- Configuration ---
static constexpr char PROTOCOL_VERSION[] = "2";
using json = nlohmann::json;

// --- RAII Helpers ---

// Deleter for libevdev*
struct LibEvdevDeleter {
    void operator()(libevdev *dev) const {
        if (dev) {
            // Note: Does NOT automatically ungrab, must be done manually if grabbed
            libevdev_free(dev);
        }
    }
};
using unique_evdev = std::unique_ptr<libevdev, LibEvdevDeleter>;

// Simple RAII wrapper for file descriptors
class FdGuard {
    int fd_;

  public:
    explicit FdGuard(int fd = -1) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    // Move semantics
    FdGuard(FdGuard &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FdGuard &operator=(FdGuard &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    // Disallow copy
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;

    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }

    // Release ownership (useful if passing fd elsewhere)
    int release() {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }
};

// --- Options ---
struct Options {
    bool list_devices = false;
    bool exclusive = false;
    std::vector<std::string> by_path;
    std::vector<std::string> by_name;
};

// --- Argument Parsing ---
Options parseArgs(int argc, char **argv) {
    Options opts;
    CLI::App app{"Forwards Linux input device events over stdout as JSON"};
    app.set_version_flag("--version", std::string("Protocol version: ") + PROTOCOL_VERSION);

    app.add_flag("-L,--list-devices", opts.list_devices, "List available /dev/input/event* devices and exit");
    app.add_flag("-e,--exclusive", opts.exclusive, "Grab devices exclusively (prevents other apps from seeing events)");

    app.add_option("-p,--device-by-path", opts.by_path, "Forward device specified by its /dev/input/event* path");
    app.add_option("-n,--device-by-name", opts.by_name, "Forward device specified by its name");

    app.set_help_flag("-h,--help", "Show this help message and exit");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        // Exit with the code provided by CLI11 upon error
        // (prints message automatically)
        exit(app.exit(e));
    }

    // Validation: Ensure some action is requested if not listing
    if (!opts.list_devices && opts.by_path.empty() && opts.by_name.empty()) {
        std::cerr << "Error: No devices specified. Use -p, -n, or -L to list devices." << std::endl;
        std::cerr << app.help() << std::endl; // Show help message
        exit(EXIT_FAILURE);
    }

    return opts;
}

// --- Device Forwarder Logic ---
class DeviceForwarder {
  public:
    // Constructor takes options by value or const reference
    DeviceForwarder(Options opts) : opts_(std::move(opts)), done_(false) {
        if (!opts_.list_devices) {
            gatherNameMap();
            selectDevices();
        }
    }

    int run() {
        if (opts_.list_devices) {
            for (const auto &p : listDevicePaths()) {
                std::cout << p << "\n";
            }
            return 0;
        }

        if (devices_.empty()) {
            std::cerr << "No valid devices selected to forward." << std::endl;
            return EXIT_FAILURE;
        }

        // Protocol version
        std::cout << PROTOCOL_VERSION << std::endl;

        // Print device metadata
        if (!printDeviceInfo()) {
            std::cerr << "Error preparing device information." << std::endl;
            return EXIT_FAILURE;
        }

        // Launch forward threads
        launchForwardThreads();

        // Wait for threads to finish (e.g., on error or signal)
        for (auto &t : threads_) {
            if (t.joinable()) t.join();
        }

        std::cerr << "Exiting." << std::endl;
        return done_.load() ? EXIT_FAILURE : 0; // Return failure if done_ was set due to error
    }

  private:
    // Make opts_ const as it's not modified after construction
    const Options opts_;
    std::vector<std::string> devices_;
    std::unordered_map<std::string, std::string> name_to_path_;
    std::vector<std::thread> threads_;
    std::atomic<bool> done_;       // Flag to signal threads to stop
    static std::mutex cout_mutex_; // Mutex to protect std::cout

    std::vector<std::string> listDevicePaths() const {
        std::vector<std::string> paths;
        glob_t glob_result;
        // Use memset to ensure glob_result is zeroed out before use
        memset(&glob_result, 0, sizeof(glob_result));

        int ret = glob("/dev/input/event*", GLOB_TILDE, nullptr, &glob_result);
        if (ret != 0) {
            if (ret != GLOB_NOMATCH) {
                // Use strerror(errno) for better error reporting if glob fails unexpectedly
                std::cerr << "Warning: glob(/dev/input/event*) failed: " << strerror(errno) << std::endl;
            }
            // Ensure globfree is called even on error (except GLOB_NOMATCH where it's not needed)
            if (ret != GLOB_NOMATCH) globfree(&glob_result);
            return paths; // Return empty vector
        }

        for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            paths.emplace_back(glob_result.gl_pathv[i]);
        }

        globfree(&glob_result); // Free glob resources
        return paths;
    }

    void gatherNameMap() {
        for (const auto &p : listDevicePaths()) {
            FdGuard fd_guard(open(p.c_str(), O_RDONLY | O_NONBLOCK));
            if (!fd_guard.is_valid()) continue; // Silently skip inaccessible devices

            struct libevdev *raw_dev = nullptr;
            if (libevdev_new_from_fd(fd_guard.get(), &raw_dev) >= 0) {
                unique_evdev dev(raw_dev); // RAII takes ownership
                const char *name = libevdev_get_name(dev.get());
                if (name) {
                    name_to_path_[name] = p;
                }
                // dev automatically freed by unique_evdev destructor
            }
            // fd_guard automatically closes fd
        }
    }

    void selectDevices() {
        // Use a set to avoid duplicates easily, then copy to vector
        std::unordered_set<std::string> selected_paths;

        for (const auto &p : opts_.by_path) {
            // Optional: Check if path actually exists and is valid?
            // std::filesystem::path dev_path(p);
            // std::error_code ec;
            // if (std::filesystem::is_character_file(dev_path, ec) && p.rfind("/dev/input/event", 0) == 0) {
            //     selected_paths.insert(p);
            // } else {
            //     std::cerr << "Warning: Invalid or non-existent device path specified: " << p << std::endl;
            // }
            selected_paths.insert(p); // Keep it simple for now
        }

        for (const auto &name : opts_.by_name) {
            auto it = name_to_path_.find(name);
            if (it != name_to_path_.end()) {
                selected_paths.insert(it->second);
            } else {
                std::cerr << "Warning: No input device found with name: \"" << name << "\"\n";
            }
        }
        // Copy unique paths to the final vector
        devices_.assign(selected_paths.begin(), selected_paths.end());
    }

    // Make const as it doesn't modify the class state
    json encodeDevice(libevdev *dev) const {
        json j;
        j["name"] = libevdev_get_name(dev);
        j["vendor"] = libevdev_get_id_vendor(dev);
        j["product"] = libevdev_get_id_product(dev);
        // Optional: Add physical location, unique identifier if available/needed
        // j["phys"] = libevdev_get_phys(dev);
        // j["uniq"] = libevdev_get_uniq(dev);

        json caps = json::object();
        for (int type = 0; type < EV_MAX; ++type) { // Iterate up to EV_MAX
            if (!libevdev_has_event_type(dev, type)) continue;

            json evs = json::array();
            // Use libevdev_get_num_events() and libevdev_get_event_value() ?
            // The original loop iterating codes is generally correct for capabilities.
            int max_code = libevdev_event_type_get_max(type);
            if (max_code < 0) max_code = 0; // Handle cases where type might not have codes

            for (int code = 0; code <= max_code; ++code) {
                if (libevdev_has_event_code(dev, type, code)) {
                    evs.push_back(code);
                }
            }
            // Only add if there are codes for this type
            if (!evs.empty()) {
                caps[std::to_string(type)] = evs;
            }
        }
        j["capabilities"] = caps;
        return j;
    }

    // Make const
    bool printDeviceInfo() const {
        json info = json::array();
        bool success = true;
        for (const auto &path : devices_) {
            FdGuard fd_guard(open(path.c_str(), O_RDONLY | O_NONBLOCK));
            if (!fd_guard.is_valid()) {
                perror(("Error opening device for info: " + path).c_str());
                success = false;
                continue; // Skip this device
            }

            struct libevdev *raw_dev = nullptr;
            if (libevdev_new_from_fd(fd_guard.get(), &raw_dev) == 0) {
                unique_evdev dev(raw_dev);
                info.push_back(encodeDevice(dev.get()));
            } else {
                std::cerr << "Warning: Failed to initialize libevdev for info: " << path << "\n";
                success = false; // Mark as potentially problematic run
            }
            // fd_guard closes fd, unique_evdev frees dev
        }
        // Print even if some devices failed, but report overall success/failure
        std::cout << info.dump() << std::endl;
        return success;
    }

    void launchForwardThreads() {
        threads_.reserve(devices_.size());
        for (size_t i = 0; i < devices_.size(); ++i) {
            // Pass arguments needed by the static thread function
            threads_.emplace_back(&DeviceForwarder::forwardDeviceThread, i, devices_[i], opts_.exclusive,
                                  std::ref(done_));
        }
    }

    // Static thread function
    static void forwardDeviceThread(int device_index, const std::string device_path, bool grab_exclusive,
                                    std::atomic<bool> &done_flag) {
        FdGuard fd_guard(open(device_path.c_str(), O_RDONLY)); // Use blocking read for libevdev_next_event
        if (!fd_guard.is_valid()) {
            perror(("Error opening device in thread: " + device_path).c_str());
            done_flag = true; // Signal other threads to stop on critical error
            return;
        }

        struct libevdev *raw_dev = nullptr;
        int rc = libevdev_new_from_fd(fd_guard.get(), &raw_dev);
        if (rc < 0) {
            std::cerr << "Failed to init libevdev for " << device_path << ": " << strerror(-rc) << "\n";
            done_flag = true;
            return;
        }
        unique_evdev dev(raw_dev); // RAII for libevdev

        bool grabbed = false;
        if (grab_exclusive) {
            // Short delay before grab sometimes helps
            // std::this_thread::sleep_for(std::chrono::milliseconds(50));
            rc = libevdev_grab(dev.get(), LIBEVDEV_GRAB);
            if (rc != 0) {
                // Use strerror(-rc) for libevdev error codes
                std::cerr << "Warning: Could not exclusively grab " << device_path << ": " << strerror(-rc) << "\n";
                // Proceed without grab? Or exit? For now, just warn.
            } else {
                grabbed = true;
                std::cerr << "Successfully grabbed " << device_path << std::endl;
            }
        }

        struct input_event ev;
        std::cerr << "Forwarding events from: " << device_path << " (Index: " << device_index << ")" << std::endl;

        while (!done_flag) {
            // LIBEVDEV_READ_FLAG_NORMAL blocks until an event is available
            rc = libevdev_next_event(dev.get(), LIBEVDEV_READ_FLAG_NORMAL, &ev);

            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) { // Event successfully read
                json out = json::array({device_index, ev.type, ev.code, ev.value});
                // Lock stdout before writing
                std::lock_guard<std::mutex> lock(cout_mutex_);
                std::cout << out.dump() << std::endl;
            } else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                // Re-synchronizing, handle sync events if needed
                // You might want to log this or handle specific EV_SYN events
                // For simple forwarding, often ignored, but depends on receiver needs
                // json out = json::array({device_index, ev.type, ev.code, ev.value});
                // { std::lock_guard<std::mutex> lock(cout_mutex_); std::cout << out.dump() << std::endl;}
                std::cerr << "SYNC event received for " << device_path << std::endl;
                // Potentially clear state or re-send device info if protocol requires
            } else {
                // Error condition (-EAGAIN shouldn't happen with blocking read)
                if (rc != -EAGAIN) {
                    std::cerr << "Error reading event from " << device_path << ": " << strerror(-rc) << std::endl;
                    done_flag = true; // Signal exit on read error
                }
                break; // Exit loop on error or EAGAIN (though latter unexpected)
            }
        }

        std::cerr << "Stopping event forwarding for: " << device_path << std::endl;

        // Manually ungrab *before* unique_evdev destructor runs if it was grabbed
        if (grabbed) {
            libevdev_grab(dev.get(), LIBEVDEV_UNGRAB);
            std::cerr << "Ungrabbed " << device_path << std::endl;
        }
        // unique_evdev destructor calls libevdev_free
        // fd_guard destructor calls close
    }
};

// Define the static mutex
std::mutex DeviceForwarder::cout_mutex_;

// --- Main Function ---
int main(int argc, char **argv) {
    // std::ios_base::sync_with_stdio(false); // Optional: Can potentially speed up IO

    Options opts;
    try {
        opts = parseArgs(argc, argv); // Use the CLI11 parser
        DeviceForwarder forwarder(opts);
        return forwarder.run();
    } catch (const std::exception &e) {
        std::cerr << "Unhandled exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unknown unhandled exception." << std::endl;
        return EXIT_FAILURE;
    }
}
