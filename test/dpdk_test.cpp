#include <thread>
#include <vector>
#include <chrono>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <pthread.h>

using std::vector, std::thread, std::stoi, std::atomic;

#define qsize 32768
//boost::lockfree::queue<uint64_t, boost::lockfree::capacity<qsize>> queue;
//boost::lockfree::queue<uint64_t, boost::lockfree::fixed_sized<true>> queue(qsize);
boost::lockfree::spsc_queue<uint64_t, boost::lockfree::capacity<qsize> > queue;// must with arguments: 1 1
//boost::lockfree::spsc_queue<uint64_t, boost::lockfree::fixed_sized<true>> queue(qsize);

atomic<bool> force_quit;
uint64_t iters;

void push()
{
    for(uint64_t i = 0; i < iters; i++) {
        while(!queue.push(i));
    }
}

void pull()
{
    uint64_t res, sum = 0, cnt = 0;
    while(!force_quit.load(std::memory_order_relaxed)) {
        while(!queue.pop(res) && !force_quit.load(std::memory_order_relaxed));
        sum += res;
        cnt ++;
    }
    printf("sum %lu, cnt %lu\n", sum, cnt);
}


int main(int argc, char **argv)
{
    int prod = stoi(argv[1]);
    int cons = stoi(argv[2]);
    iters = std::stoll(argv[3]);
    vector<thread> pool;
    auto start = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < prod; i++) {
        pool.push_back(thread(push));
    }
    for(int i = 0; i < cons; i++) {
        pool.push_back(thread(pull));
    }
    for(int i = 0; i < prod + cons; i++) {
        auto t = &pool[i];
        pthread_t native_handle = t->native_handle();
        // 创建 CPU 集合，设置绑定到第 0 号 CPU 核心
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);

        // 将线程绑定到 CPU 集合
        pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    }
    for(int i = 0; i < prod; i++) {
        pool[i].join();
    }
    fprintf(stderr, "1\n");
    force_quit.store(true, std::memory_order_relaxed);
    for(int i = 0; i < cons; i++) {
        pool[prod + i].join();
    }
    fprintf(stderr, "2\n");
    std::chrono::nanoseconds sec = std::chrono::high_resolution_clock::now() - start;
    printf("%lf\n", sec.count() * 1e-9);
    return 0;
}