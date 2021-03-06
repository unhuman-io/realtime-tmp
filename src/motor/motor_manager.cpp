#include "motor_manager.h"
#include "motor.h"

#include <libudev.h>

#include <cstring>
#include <algorithm>
#include <poll.h>
#include <sstream>

// Returns a vector of strings that contain the dev file locations,
// e.g. /dev/skel0
static std::vector<std::string> udev (bool user_space_driver=false)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
    
	/* Create the udev object */
	struct udev *udev = udev_new();
	if (!udev) {
		printf("Can't create udev\n");
		exit(1);
	}
	
	/* Create a list of the devices in the 'hidraw' subsystem. */
	enumerate = udev_enumerate_new(udev);
    if (user_space_driver) {
        udev_enumerate_add_match_sysattr(enumerate, "idVendor", "3293");
        udev_enumerate_add_match_sysattr(enumerate, "idProduct", "0100");
    } else {
        udev_enumerate_add_match_sysname(enumerate, "usbrt*");
        udev_enumerate_add_match_sysname(enumerate, "mtr*");
    }
    
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

    std::vector<std::string> dev_paths;
	udev_list_entry_foreach(dev_list_entry, devices) {
		// path in /sys/devices/pci*
		const char *path = udev_list_entry_get_name(dev_list_entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
        const char * sysname = udev_device_get_sysname(dev);
        const char * subsystem = udev_device_get_subsystem(dev);
        const char * devpath = udev_device_get_devpath(dev);

        // TODO better way of identifying other than interface number 0
		//if (std::string("00") == udev_device_get_sysattr_value(dev, "device/bInterfaceNumber")) {
			devpath = udev_device_get_devnode(dev);
            if (devpath) {
                dev_paths.push_back(devpath);
            }
		//}
		

		udev_device_unref(dev);
	}
	/* Free the enumerator object */
	udev_enumerate_unref(enumerate);

	udev_unref(udev);

	return dev_paths;       
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_connected_motors(bool connect) {
    auto dev_paths = udev(user_space_driver_);
    std::vector<std::shared_ptr<Motor>> m;
    for (auto dev_path : dev_paths) {
        try {
            if (user_space_driver_ == true) {
                m.push_back(std::make_shared<UserSpaceMotor>(dev_path));
            } else {
                m.push_back(std::make_shared<Motor>(dev_path));
            }
        } catch (std::runtime_error &e) {
            // There is a runtime_error if the motor is disconnected during this function
        }
    }
    if (connect) {
        motors_ = m;
        commands_.resize(m.size());
    }
    return m;
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_motors_by_name_function(std::vector<std::string> names, std::string (Motor::*name_fun)() const, bool connect, bool allow_simulated) {
    auto connected_motors = get_connected_motors(connect);
    std::vector<std::shared_ptr<Motor>> m(names.size());
    for (int i=0; i<names.size(); i++) {
        std::vector<std::shared_ptr<Motor>> found_motors;
        std::copy_if(connected_motors.begin(), connected_motors.end(), std::back_inserter(found_motors), [&names, &i, &name_fun](std::shared_ptr<Motor> m){ return names[i] == (m.get()->*name_fun)(); });
        if (found_motors.size() == 1) {
            m[i] = found_motors[0];
        } else {
            if (found_motors.size() > 1) {
                throw std::runtime_error("Found too many motors matching: " + names[i]);
            }
            if (allow_simulated) {
                std::cout << "Warning: found no motors matching \"" << names[i] << "\", using simulated motor" << std::endl;
                m[i] = std::make_shared<SimulatedMotor>(names[i]);
            } else {
                throw std::runtime_error("Found no motors matching: " + names[i]);
            }
        }
    }
    if (connect) {
        motors_ = m;
        commands_.resize(m.size());
    }
    return m;
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_motors_by_name(std::vector<std::string> names, bool connect, bool allow_simulated) {
    return get_motors_by_name_function(names, &Motor::name, connect, allow_simulated);
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_motors_by_serial_number(std::vector<std::string> serial_numbers, bool connect, bool allow_simulated) {
    return get_motors_by_name_function(serial_numbers, &Motor::serial_number, connect, allow_simulated);
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_motors_by_path(std::vector<std::string> paths, bool connect, bool allow_simulated) {
    return get_motors_by_name_function(paths, &Motor::base_path, connect, allow_simulated);
}

std::vector<std::shared_ptr<Motor>> MotorManager::get_motors_by_devpath(std::vector<std::string> devpaths, bool connect, bool allow_simulated) {
    return get_motors_by_name_function(devpaths, &Motor::dev_path, connect, allow_simulated);
}

std::vector<Status> MotorManager::read() {
    std::vector<Status> statuses(motors_.size());
    for (int i=0; i<motors_.size(); i++) {
        auto size = motors_[i]->read();
        if (size == -1) {
            // no data, error is in errno
            std::string err = "No data read from: " + motors_[i]->name() + ": " + std::to_string(errno) + ": " + strerror(errno);
            if (!reconnect_) {
                throw std::runtime_error(err);
            } else {
                if (reconnect_rate_.run()) {
                    std::cerr << err << std::endl;
                    std::cerr << "trying to reconnect " << motors_[i]->base_path() << std::endl;
                    try {
                        auto motors = get_motors_by_path({motors_[i]->base_path()}, false);
                        if (motors[0]) {
                            std::cerr << "found motor " << motors_[i]->base_path() << ": " << motors[0]->name() << std::endl;
                            motors_[i] = motors[0];
                        }
                    } catch (std::runtime_error &e) {
                        std::cerr << e.what() << std::endl;
                    }
                }
            }
        }
        statuses[i] = *motors_[i]->status();
    }
    return statuses;
}

void MotorManager::write(std::vector<Command> commands) {
    count_++;
    if (auto_count_) {
        set_command_count(count_);
        for (int i=0; i<commands.size(); i++) {
            commands[i].host_timestamp = count_;
        }
    }
    for (int i=0; i<motors_.size(); i++) {
        *motors_[i]->command() = commands[i];
        motors_[i]->write();
    }
}

void MotorManager::aread() {
    for (int i=0; i<motors_.size(); i++) {
        motors_[i]->aread();
    }
}

void MotorManager::set_commands(std::vector<Command> commands) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i] = commands[i];
    }
}

void MotorManager::set_command_count(int32_t count) {
    for (auto &c : commands_) {
        c.host_timestamp = count;
    }
}

void MotorManager::set_command_mode(uint8_t mode) {
    for (auto &c : commands_) {
        c.mode_desired = mode;
    }
}

void MotorManager::set_command_mode(std::vector<uint8_t> mode) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].mode_desired = mode[i];
    }
}
    
void MotorManager::set_command_current(std::vector<float> current) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].current_desired = current[i];
    }
}

void MotorManager::set_command_position(std::vector<float> position) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].position_desired = position[i];
    }
}

void MotorManager::set_command_velocity(std::vector<float> velocity) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].velocity_desired = velocity[i];
    }
}

void MotorManager::set_command_torque(std::vector<float> torque) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].torque_desired = torque[i];
    }
}

void MotorManager::set_command_reserved(std::vector<float> reserved) {
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].reserved = reserved[i];
    }
}

void MotorManager::set_command_stepper_tuning(TuningMode mode, double amplitude, double frequency, 
    double bias, double kv) {
    set_command_mode(ModeDesired::STEPPER_TUNING);
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].stepper_tuning.amplitude = amplitude;
        commands_[i].stepper_tuning.mode = mode;
        commands_[i].stepper_tuning.bias = bias;
        commands_[i].stepper_tuning.frequency = frequency;
        commands_[i].stepper_tuning.kv = kv;
    }
}

void MotorManager::set_command_stepper_velocity(double voltage, double velocity) {
    set_command_mode(ModeDesired::STEPPER_VELOCITY);
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].stepper_velocity.voltage = voltage;
        commands_[i].stepper_velocity.velocity = velocity;
    }
}

void MotorManager::set_command_position_tuning(TuningMode mode, double amplitude, double frequency, 
    double bias) {
    set_command_mode(ModeDesired::POSITION_TUNING);
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].position_tuning.amplitude = amplitude;
        commands_[i].position_tuning.mode = mode;
        commands_[i].position_tuning.bias = bias;
        commands_[i].position_tuning.frequency = frequency;
    }
}

void MotorManager::set_command_current_tuning(TuningMode mode, double amplitude, double frequency, 
    double bias) {
    set_command_mode(ModeDesired::CURRENT_TUNING);
    for (int i=0; i<commands_.size(); i++) {
        commands_[i].current_tuning.amplitude = amplitude;
        commands_[i].current_tuning.mode = mode;
        commands_[i].current_tuning.bias = bias;
        commands_[i].current_tuning.frequency = frequency;
    }
}

void MotorManager::write_saved_commands() {
    write(commands_);
}

int MotorManager::serialize_command_size() const {
    return sizeof(commands_.size()) + commands_.size() * sizeof(commands_[0]);
}

int MotorManager::serialize_saved_commands(char *data) const {
    char *datastart = data;
    auto size = commands_.size();
    std::memcpy(data, &size, sizeof(size));
    data += sizeof(size);
    for (int i=0; i<commands_.size(); i++) {
        std::memcpy(data, &commands_[i], sizeof(commands_[0]));
        data += sizeof(commands_[0]);
    }
    return data - datastart;
}

bool MotorManager::deserialize_saved_commands(char *data) {
   auto size = commands_.size();
   decltype(commands_.size()) size_in;
   std::memcpy(&size_in, data, sizeof(size_in));
   if (size_in == size) {
        data += sizeof(size_in);
        for (int i=0; i<commands_.size(); i++) {
            std::memcpy(&commands_[i], data, sizeof(commands_[0]));
            data += sizeof(commands_[0]);
        }
        return true;
   }
   return false;
}

int MotorManager::poll() {
    auto pollfds = new pollfd[motors_.size()];
    for (int i=0; i<motors_.size(); i++) {
        pollfds[i].fd = motors_[i]->fd();
        pollfds[i].events = POLLIN;
    }
    int retval = ::poll(pollfds, motors_.size(), 1);
    delete [] pollfds;
    return retval;
}

std::string MotorManager::command_headers() const {
    std::stringstream ss;
    int length = motors_.size();
    for (int i=0;i<length;i++) {
        ss << "host_timestamp" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "mode_desired" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "current_desired" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "position_desired" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "velocity_desired" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "reserved" << i << ", ";
    }
    return ss.str();
}

std::string MotorManager::status_headers() const {
    std::stringstream ss;
    int length = motors_.size();
    for (int i=0;i<length;i++) {
        ss << "mcu_timestamp" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "host_timestamp_received" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "motor_position" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "joint_position" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "iq" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "torque" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "motor_encoder" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "reserved0" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "reserved1" << i << ", ";
    }
    for (int i=0;i<length;i++) {
        ss << "reserved2" << i << ", ";
    }
    return ss.str();
}
