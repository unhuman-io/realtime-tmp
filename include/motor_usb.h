#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include <libgen.h>
#include <libudev.h>

// user space driver
#include <linux/usb/ch9.h> // todo why not usb.h
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdexcept>
#include <cstring>
#include "motor_messages.h"

class TextFile {
 public:
    virtual ~TextFile() {}
    virtual void flush() {}
    virtual ssize_t read(char *data, unsigned int length) = 0;
    virtual ssize_t write(const char *data, unsigned int length) = 0;
};

class SysfsFile : public TextFile {
 public:
    SysfsFile (std::string path) {
        path_ = path;
    }
    int open() {
        int fd = ::open(path_.c_str(), O_RDWR);
        if (fd >= 0) {
            return fd;
        } else {
            throw std::runtime_error("Sysfs open error " + std::to_string(errno) + ": " + strerror(errno) + ", " + path_.c_str());
        }
    }
    void close(int fd) {
        int retval = ::close(fd);
        if (retval) {
            throw std::runtime_error("Sysfs close error " + std::to_string(errno) + ": " + strerror(errno));
        }
    }
    virtual void flush() {
        char c[64];
        while(read(c, 64));
    }
    ssize_t read(char *data, unsigned int length) {
        // sysfs file needs to be opened and closed to read new values
        int fd = open();
        auto retval = ::read(fd, data, length);
        close(fd);
        if (retval < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            } else {
                throw std::runtime_error("Sysfs read error " + std::to_string(errno) + ": " + strerror(errno));
            }
        }
        return retval;
    }
    ssize_t write(const char *data, unsigned int length) {
        int fd = open();
        auto retval = ::write(fd, data, length);
        close(fd);
        if (retval < 0) {
            throw std::runtime_error("Sysfs write error " + std::to_string(errno) + ": " + strerror(errno));
        }
        return retval;
    }
    ~SysfsFile() {}
 private:
    std::string path_;
};

class USBFile : public TextFile {
 public:
    // file fd should be opened already - it can only have one open reference
    USBFile (int fd, uint8_t ep_num = 1) { 
        ep_num_ = ep_num;
        fd_ = fd;
    }
    ssize_t read(char *data, unsigned int length) { 
        struct usbdevfs_bulktransfer transfer = {
            .ep = ep_num_ | USB_DIR_IN,
            .len = length,
            .timeout = 100,
            .data = data
        };

        int retval = ::ioctl(fd_, USBDEVFS_BULK, &transfer);
        if (retval < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            } else {
                throw std::runtime_error("USB read error " + std::to_string(errno) + ": " + strerror(errno));
            }
        }
        return retval;
    }
    ssize_t write(const char *data, unsigned int length) { 
        char buf[64];
        std::memcpy(buf, data, length);
        struct usbdevfs_bulktransfer transfer = {
            .ep = ep_num_ | USB_DIR_OUT,
            .len = length,
            .timeout = 100,
            .data = buf
        };

        int retval = ::ioctl(fd_, USBDEVFS_BULK, &transfer);
        if (retval < 0) {
            throw std::runtime_error("USB write error " + std::to_string(errno) + ": " + strerror(errno));
        }
        return retval;
    }
 private:
    unsigned int ep_num_;
    int fd_;
};

class TextAPIItem {
 public:
    TextAPIItem(TextFile *motor_txt, std::string name) : motor_txt_(motor_txt), name_(name) { 
        motor_txt_->flush();
    }

    void set(std::string s) {
        std::string s2 = name_ + "=" + s;
        motor_txt_->write(s2.c_str(), s2.size());
        char c[64];
        motor_txt_->read(c, 64);
    }
    std::string get() const {
        motor_txt_->write(name_.c_str(), name_.size());
        char c[64] = {};
        motor_txt_->read(c, 64);
        return c;
    }
    void operator=(const std::string s) {
        set(s);
    }
 private:
    TextFile *motor_txt_;
    std::string name_;
};

inline std::ostream& operator<<(std::ostream& os, TextAPIItem const& item) {
    os << item.get();
    return os;
}
inline double& operator<<(double& d, TextAPIItem const& item) {
    d = std::stod(item.get());
    return d;
}
class Motor {
 public:
    Motor() {}
    Motor(std::string dev_path);
    virtual ~Motor();
    virtual ssize_t read() { return ::read(fd_, &status_, sizeof(status_)); };
    virtual ssize_t write() { return ::write(fd_, &command_, sizeof(command_)); };
    ssize_t aread() { int fcntl_error = fcntl(fd_, F_SETFL, fd_flags_ | O_NONBLOCK);
			ssize_t read_error = read(); 
            fcntl_error = fcntl(fd_, F_SETFL, fd_flags_);
            if (read_error != -1) {
                std::cout << "Nonzero aread" << std::endl;
            } else {
                if (errno != EAGAIN) {
                    std::cout << "Aread error " << errno << std::endl;
                }
            }
            return read_error; }
    std::string name() const { return name_; }
    std::string serial_number() const { return serial_number_; }
    std::string base_path() const {return base_path_; }
    std::string dev_path() const { return dev_path_; }
    std::string version() const { return version_; }
    std::string short_version() const {
        std::string s = version();
        auto pos = s.find("-g");
        return s.substr(0,pos);
    }
    std::string status_string() const;
    // note will probably not be the final interface
    TextAPIItem operator[](const std::string s) { TextAPIItem t(motor_txt_, s); return t; };
    int fd() const { return fd_; }
    const Status *const status() const { return &status_; }
    Command *const command() { return &command_; }
    TextFile* motor_text() { return motor_txt_; }
 protected:
    int open() { fd_ = ::open(dev_path_.c_str(), O_RDWR); fd_flags_ = fcntl(fd_, F_GETFL); return fd_; }
    int close() { return ::close(fd_); }
    int fd_ = 0;
    int fd_flags_;
    std::string serial_number_, name_, dev_path_, base_path_, version_;
    Status status_ = {};
    Command command_ = {};
    TextFile *motor_txt_;
};

class UserSpaceMotor : public Motor {
 public:
    UserSpaceMotor(std::string dev_path, uint8_t ep_num = 2) { 
        ep_num_ = ep_num;
        dev_path_ = dev_path; 
        struct udev *udev = udev_new();
        struct stat st;
        if (stat(dev_path.c_str(), &st) < 0) {
            throw std::runtime_error("Motor stat error " + std::to_string(errno) + ": " + strerror(errno));
        }
        struct udev_device *dev = udev_device_new_from_devnum(udev, 'c', st.st_rdev);
                const char * sysname = udev_device_get_sysname(dev);
        const char * subsystem = udev_device_get_subsystem(dev);
        const char * devpath = udev_device_get_devpath(dev);
        //struct udev_device *dev = udev_device_new_from_syspath(udev, syspath;
       // struct udev_device *dev = udev_device_new_from_subsystem_sysname(udev, "usb", sysname);
        std::string interface_name = sysname;
        interface_name += ":1.0/interface";
        const char * name = udev_device_get_sysattr_value(dev, interface_name.c_str());
        if (name != NULL) {
            name_ = name;
        } else {
            name_ = "";
        }

        // dev = udev_device_get_parent_with_subsystem_devtype(
		//        dev,
		//        "usb",
		//        "usb_device");
        serial_number_ = udev_device_get_sysattr_value(dev, "serial"); 
        base_path_ = basename(const_cast<char *>(udev_device_get_syspath(dev)));
        const char * version = udev_device_get_sysattr_value(dev, "configuration");
        if (version != NULL) {
            version_ = version;
        } else {
            version_ = "";
        }
        
        udev_device_unref(dev);
        udev_unref(udev);  
        open();
        motor_txt_ = new USBFile(fd_, 1);
    }
    virtual ~UserSpaceMotor() { close(); }
    virtual ssize_t read() { 
        char data[64];
        struct usbdevfs_bulktransfer transfer = {
            .ep = ep_num_ | USB_DIR_IN,
            .len = sizeof(status_),
            .timeout = 100,
            .data = &status_
        };

        int retval = ::ioctl(fd_, USBDEVFS_BULK, &transfer);
        if (retval < 0) {
            throw std::runtime_error("Motor read error " + std::to_string(errno) + ": " + strerror(errno));
        }
        return retval;
    }
    virtual ssize_t write() { 
        char data[64];
        struct usbdevfs_bulktransfer transfer = {
            .ep = ep_num_ | USB_DIR_OUT,
            .len = sizeof(command_),
            .timeout = 100,
            .data = &command_
        };

        int retval = ::ioctl(fd_, USBDEVFS_BULK, &transfer);
        if (retval < 0) {
            throw std::runtime_error("Motor write error " + std::to_string(errno) + ": " + strerror(errno));
        }
        return retval;
    }
 private:
    int open() {
        int retval = Motor::open();
        struct usbdevfs_disconnect_claim claim = { 0, USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER, "usb_rt" };
        int ioval = ::ioctl(fd_, USBDEVFS_DISCONNECT_CLAIM, &claim); // will take control from driver if one is installed
        if (ioval < 0) {
            throw std::runtime_error("Motor open error " + std::to_string(errno) + ": " + strerror(errno));
        }
        return retval;
    }
    int close() {
        int interface_num = 0;
        int ioval = ::ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &interface_num); 
        if (ioval < 0) {
            throw std::runtime_error("Motor release interface error " + std::to_string(errno) + ": " + strerror(errno));
        }
        struct usbdevfs_ioctl connect = { .ifno = 0, .ioctl_code=USBDEVFS_CONNECT };
        ioval = ::ioctl(fd_, USBDEVFS_IOCTL, &connect); // allow kernel driver to reconnect
        if (ioval < 0) {
            throw std::runtime_error("Motor close error " + std::to_string(errno) + ": " + strerror(errno));
        }
        // fd_ closed by base
        return 0;
    }
    unsigned int ep_num_;
};

inline int geti() { 
    static int i = std::ios_base::xalloc();
    return i;
}

inline std::ostream& reserved_uint32(std::ostream &os) {
    os.iword(geti()) = 1; 
    return os;
}


inline std::ostream& operator<<(std::ostream& os, const Status & status) {
    os << status.mcu_timestamp << ", ";
    os << status.host_timestamp_received << ", ";
    os << status.motor_position << ", ";
    os << status.joint_position << ", ";
    os << status.iq << ", ";
    os << status.torque << ", ";
    os << status.motor_encoder << ", ";
    if (os.iword(geti()) == 1) {
        os << s.reserved_uint[0] << ", ";
    } else {
        os << s.reserved[0] << ", ";
        os << s.reserved[1] << ", ";
    }
}
