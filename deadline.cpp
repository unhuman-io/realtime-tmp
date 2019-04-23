#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <sys/syscall.h>

#include <thread>
#include <iostream>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h> 
#include <netinet/in.h> 
#include <arpa/inet.h>

#include <string>
#include <vector>
#include <algorithm>
#include "motor.h"
#include "motor_manager.h"

#define PORT 8080 

#define gettid() syscall(__NR_gettid)

#define SCHED_DEADLINE	6

/* XXX use the proper syscall numbers */
#ifdef __x86_64__
#define __NR_sched_setattr		314
#define __NR_sched_getattr		315
#endif

#ifdef __i386__
#define __NR_sched_setattr		351
#define __NR_sched_getattr		352
#endif

#ifdef __arm__
#define __NR_sched_setattr		380
#define __NR_sched_getattr		381
#endif

static volatile int done;

struct sched_attr {
	__u32 size;

	__u32 sched_policy;
	__u64 sched_flags;

	/* SCHED_NORMAL, SCHED_BATCH */
	__s32 sched_nice;

	/* SCHED_FIFO, SCHED_RR */
	__u32 sched_priority;

	/* SCHED_DEADLINE (nsec) */
	__u64 sched_runtime;
	__u64 sched_deadline;
	__u64 sched_period;
};

int sched_setattr(pid_t pid,
		const struct sched_attr *attr,
		unsigned int flags)
{
return syscall(__NR_sched_setattr, pid, attr, flags);
}

int sched_getattr(pid_t pid,
		struct sched_attr *attr,
		unsigned int size,
		unsigned int flags)
{
return syscall(__NR_sched_getattr, pid, attr, size, flags);
}

struct Data {
	struct D {
		int32_t count;
		int32_t count_received;
		float res[3];
	} buffer;
	struct C {
		int32_t count;
	} command;
	int32_t delay;
	std::chrono::steady_clock::time_point time_start, last_time_start, last_time_end, aread_time, read_time, write_time;
};

template <class T>
class CStack {
 public:
    void push(T &t) {
		int future_pos = pos_ + 1;
		if (future_pos >= 100) {
			future_pos = 0;
		}
		data_[future_pos] = t;
		pos_ = future_pos;
	}
	T top() {
		return data_[pos_];
	}
 private:
	T data_[100];
	int pos_ = 0;
};

int sock;
bool send_tcp = false;
std::vector<std::string> motor_names = {"J1", "J2", "J3", "J4", "J5"};


class Task {
 public:
    Task(CStack<Data> &cstack, std::vector<std::shared_ptr<Motor>> motors) : cstack_(cstack), motors_(motors) {

		for (auto motor : motors_) {
			motor->open();
			std::cout << "open: " << motor->name() << std::endl;
		}
	}
	~Task() {

		close(fid_);
	}
	void run() { done_ = 0;
		start_time_ = std::chrono::steady_clock::now();
		next_time_ = start_time_;
		thread_ = new std::thread([=]{run_deadline();}); }
	void done() { done_ = 1; }
	void join() { thread_->join(); }
 private:
	void run_deadline()
	{
		struct sched_attr attr;
		int32_t x = 0;
		int ret;
		unsigned int flags = 0;

		printf("deadline thread started [%ld]\n", gettid());

		attr.size = sizeof(attr);
		attr.sched_flags = 0;
		attr.sched_nice = 0;
		attr.sched_priority = 0;

		attr.sched_policy = SCHED_DEADLINE;
		attr.sched_runtime =  300 * 1000;
		attr.sched_deadline = period_ns_*3/5;
		attr.sched_period =  period_ns_;

		int not_root = 0;
		ret = sched_setattr(0, &attr, flags);
		if (ret < 0) {
			perror("sched_setattr");
			not_root = 1;
		}

		while (!done_) {
			x++;
			next_time_ += std::chrono::nanoseconds(period_ns_);
			data_.last_time_start = data_.time_start;
			data_.time_start = std::chrono::steady_clock::now();

			// start a read on all motors
			for(auto m : motors_) {
				m->aread(&data_.buffer, sizeof(data_.buffer));
			}
			data_.aread_time = std::chrono::steady_clock::now();

			// blocking io to get the data alread set up and wait if not ready yet
			for(auto m : motors_) {
				m->read(&data_.buffer, sizeof(data_.buffer));
			}
			data_.read_time = std::chrono::steady_clock::now();


		// 	if (read_error < 0) {
		// 		std::cout << "read error: " << errno << std::endl;
		// 	} else {
		// //		std::cout << "read success: " << read_error << std::endl;
		// 	}

			data_.command.count = x;
			data_.delay = x - data_.buffer.count_received;
			if (data_.delay > 1) {
				std::cout << "Delay > 1: " << data_.delay << std::endl;
			}
		//	ssize_t write_error = write(fid_, &data_.command, 4);
			for(auto m : motors_) {
				m->write(&data_.command, sizeof(data_.command));
			}
			data_.write_time = std::chrono::steady_clock::now();

			// if (write_error < 0) {
			// 	std::cout << "write error: " << strerror(-write_error) << std::endl;
			// }

			if (send_tcp) {
				send(sock , &data_ , 20 , 0 ); 
			}

			cstack_.push(data_);
			data_.last_time_end = std::chrono::steady_clock::now();

			if(not_root) {
				std::this_thread::sleep_until(next_time_);
			} else {
				sched_yield();
			}
		}

		printf("deadline thread dies [%ld]\n", gettid());
	}
	std::thread *thread_;
	int done_;
	CStack<Data> &cstack_;
	Data data_;
	std::chrono::steady_clock::time_point start_time_, next_time_;
	long period_ns_ =   500 * 1000;
	int fid_;
	int fid_flags_;
	std::vector<std::shared_ptr<Motor>> motors_;
};



int setup_socket() {
    struct sockaddr_in address; 
    int valread; 
    struct sockaddr_in serv_addr; 
    char hello[] = "Hello from client"; 
    char buffer[1024] = {0}; 
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    { 
        printf("\n Socket creation error \n"); 
        return -1; 
    } 
   
    memset(&serv_addr, '0', sizeof(serv_addr)); 
   
    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_port = htons(PORT); 
       
    // Convert IPv4 and IPv6 addresses from text to binary form 
    if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)  
    { 
        printf("\nInvalid address/ Address not supported \n"); 
        return -1; 
    } 
   
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) 
    { 
        printf("\nConnection Failed a\n"); 
		send_tcp = false;
    } else {
		send_tcp = true;
	}

    return 0; 
}

int main (int argc, char **argv)
{
	MotorManager motor_manager;
	auto motors = motor_manager.get_connected_motors();
	setup_socket();
	printf("main thread [%ld]\n", gettid());
	CStack<Data> cstack;
	Task task(cstack, motors);
	task.run();
	std::chrono::steady_clock::time_point system_start = std::chrono::steady_clock::now();

	for(int i=0; i<100; i++) {
		Data data = cstack.top();
		auto last_exec = std::chrono::duration_cast<std::chrono::nanoseconds>(data.last_time_end - data.last_time_start).count();
		auto last_period =  std::chrono::duration_cast<std::chrono::nanoseconds>(data.time_start - data.last_time_start).count();
		auto start = std::chrono::duration_cast<std::chrono::nanoseconds>(data.time_start - system_start).count();
		std::cout << "last_period: " << last_period << " last_exec: " << last_exec 
				<< " count_received: " << data.buffer.count_received << "current_count: " << data.command.count 
				<< " aread_time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(data.aread_time - data.time_start).count()
				<< " read_time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(data.read_time - data.time_start).count()
				<< " write_time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(data.write_time - data.time_start).count()
				<< std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
	task.done();
	task.join();


	printf("main dies [%ld]\n", gettid());
	return 0;
}