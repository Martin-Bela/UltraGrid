#include <chrono>
#include <string_view>
#include<iostream>

namespace chrono = std::chrono;

struct Timer{
	chrono::steady_clock::time_point start_time = chrono::steady_clock::now();
	chrono::nanoseconds duration {};

    void start(){
        start_time = chrono::steady_clock::now();
    }
	chrono::nanoseconds stop(){
		duration = chrono::steady_clock::now() - start_time;
		return duration;
	}
        
	template<typename Unit = chrono::microseconds>
	void stop_and_print_result(std::string_view msg = "Timer", unsigned parts = 1){
		duration = chrono::steady_clock::now() - start_time;
		std::cout << msg << ": " << chrono::duration_cast<Unit> (duration / parts).count() << '\n';
	}
};

struct AverageTimer{
        Timer timer{};
        chrono::nanoseconds duration {};
        uint64_t count = 0;
        std::string_view msg;
        
        AverageTimer(std::string_view msg)
        :msg(msg) { }

        ~AverageTimer(){
                printf("%s: %f\n", msg.data(), duration.count() / double(count));
        }

        void start(){
                timer.start();
        }

        void stop(){
                duration += timer.stop();
                count++;
        }
};

struct AddTimer{
        Timer timer{};
        chrono::nanoseconds duration {};
        std::string_view msg;
        bool active = false;

        AddTimer(std::string_view msg)
        :msg(msg) { }

        ~AddTimer(){
                if (active){
                        printf("%s: %lld\n", msg.data(), duration.count());
                }
        }

        void start(){
                timer.start();
                active = true;
        }

        void stop(){
                duration += timer.stop();
        }
};
