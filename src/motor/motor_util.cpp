#include "motor_manager.h"
#include "motor.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include "rt_version.h"
#include "CLI11.hpp"
#include <queue>

class Statistics {
 public:
    Statistics(int size) : size_(size) {}
    void push(double value) {
        double old_value = 0;
        if (queue_.size() >= size_) {
            old_value = queue_.front();
            queue_.pop_front();
        }
        queue_.push_back(value);
        value_sum_ += value - old_value;
        value_squared_sum_ += pow(value, 2) - pow(old_value, 2);
    }
    double get_mean() const {
        return value_sum_/queue_.size();
        //return std::accumulate(std::begin(queue_), std::end(queue_), 0)/queue_.size();
    }
    double get_stddev() const {
        if (queue_.size() > 1) {
            double variance = value_squared_sum_ - 2*value_sum_*get_mean() + pow(get_mean(),2)*queue_.size();
            return sqrt(variance/(queue_.size()-1));
        } else {
            return 0;
        }
    }
    double get_min() const {
        return *std::min_element(std::begin(queue_), std::end(queue_));
    }
    double get_max() const {
        return *std::max_element(std::begin(queue_), std::end(queue_));
    }
 private:
    int size_;
    double value_sum_ = 0, value_squared_sum_ = 0;
    std::deque<double> queue_;
};

struct ReadOptions {
    bool poll;
    bool aread;
    double frequency_hz;
    bool statistics;
};

int main(int argc, char** argv) {
    CLI::App app{"Utility for communicating with motor drivers"};
    bool print = false, list = true, version = false, list_names=false, list_path=false;
    std::vector<std::string> names = {};
    Command command = {};
    ReadOptions read_opts = { .poll = false, .aread = false, .frequency_hz = 1000, .statistics = false };
    auto set = app.add_subcommand("set", "Send data to motor(s)");
    set->add_option("--host_time", command.host_timestamp, "Host time");
    set->add_option("--mode", command.mode_desired, "Mode desired");
    set->add_option("--current", command.current_desired, "Current desired");
    set->add_option("--position", command.position_desired, "Position desired");
    set->add_option("--reserved", command.reserved, "Reserved command");
    auto read_option = app.add_subcommand("read", "Print data received from motor(s)");
    read_option->add_flag("--poll", read_opts.poll, "Use poll before read");
    read_option->add_flag("--aread", read_opts.aread, "Use aread before poll");
    read_option->add_option("--frequency", read_opts.frequency_hz , "Read frequency in Hz");
    read_option->add_flag("--statistics", read_opts.statistics, "Print statistics rather than values");
    app.add_flag("-l,--list", list, "List connected motors");
    app.add_flag("-v,--version", version, "Print version information");
    app.add_flag("--list-names-only", list_names, "Print only connected motor names");
    app.add_flag("--list-path-only", list_path, "Print only connected motor paths");
    app.add_option("-n,--names", names, "Connect only to NAME(S)")->type_name("NAME")->expected(-1);
    CLI11_PARSE(app, argc, argv);

    MotorManager m;
    std::vector<std::shared_ptr<Motor>> motors;
    if (names.size()) {
        motors = m.get_motors_by_name(names);
        auto i = std::begin(motors);
        while (i != std::end(motors)) {
            if (!*i) {
                i = motors.erase(i);
            } else {
                ++i;
            }
        }
    } else {
        motors = m.get_connected_motors();
    }

    if (version) {
        std::cout << "motor_util version: " << RT_VERSION_STRING << std::endl;
    }

    if (list) {
        int name_width = 10;
        int serial_number_width = 15;
        int version_width = 60;
        int path_width = 15;
        int dev_path_width = 12;
        if (list_names || list_path) {
              if (motors.size() > 0) {
                    for (auto m : motors) {
                        if (list_names) {
                            std::cout << m->name();
                        } else if (list_path) {
                            std::cout << m->base_path();
                        }
                        std::cout << std::endl;
                    }
              }
        } else {
            std::cout << motors.size() << " connected motor" << (motors.size() == 1 ? "" : "s") << std::endl;
            if (motors.size() > 0) {
                std::cout << std::setw(dev_path_width) << "Dev" << std::setw(name_width) << "Name"
                            << std::setw(serial_number_width) << " Serial number"
                            << std::setw(version_width) << "Version" << std::setw(path_width) << "Path" << std::endl;
                std::cout << std::setw(dev_path_width + name_width + serial_number_width + version_width + path_width) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
                for (auto m : motors) {
                    std::cout << std::setw(dev_path_width) << m->dev_path()
                            << std::setw(name_width) << m->name()
                            << std::setw(serial_number_width) << m->serial_number()
                            << std::setw(version_width) << m->version()
                            << std::setw(path_width) << m->base_path() << std::endl;
                }
            }
        }
    }

    if (*set && motors.size()) {
        m.open();
        auto commands = std::vector<Command>(motors.size(), command);
        std::cout << "Writing commands: \n" << commands << std::endl;
        m.write(commands);
        m.close();
    }

    if (*read_option) {
        m.open();
        auto start_time = std::chrono::steady_clock::now();
        auto next_time = start_time;
        auto loop_start_time = start_time;
        int64_t period_ns = 1e9/read_opts.frequency_hz;
        Statistics exec(100), period(100);
        int i = 0;
        while (1) {
            auto last_loop_start_time = loop_start_time;
            loop_start_time = std::chrono::steady_clock::now();
            next_time += std::chrono::nanoseconds(period_ns);
            if (read_opts.aread) {
                m.aread();
            }
            if (read_opts.poll) {
                m.poll();
            }
            auto status = m.read();
            auto exec_time = std::chrono::steady_clock::now();

            if (read_opts.statistics) {
                i++;
                auto last_exec = std::chrono::duration_cast<std::chrono::nanoseconds>(exec_time - loop_start_time).count();
                auto last_start = std::chrono::duration_cast<std::chrono::nanoseconds>(loop_start_time - start_time).count();
                auto last_period = std::chrono::duration_cast<std::chrono::nanoseconds>(loop_start_time - last_loop_start_time).count();
                exec.push(last_exec);
                period.push(last_period);
                if (i > 100) {
                    i = 0;
                    auto width = 15;
                    std::cout << std::fixed << std::setprecision(0) << std::setw(width) << last_start << std::setw(width) << floor(period.get_mean()) << std::setw(width) << 
                    period.get_stddev() << std::setw(width) << period.get_min()  << std::setw(width) << period.get_max() << std::setw(width) <<
                    floor(exec.get_mean()) << std::setw(width) <<  exec.get_stddev() << std::setw(width) << exec.get_min() << std::setw(width) << exec.get_max()  << std::endl;
                }
            } else {
                std::cout << status << std::endl;
            }

            // option to not sleep
            // while (std::chrono::steady_clock::now() < next_time);
            std::this_thread::sleep_until(next_time);
        }
        m.close();
    }

    return 0;
}