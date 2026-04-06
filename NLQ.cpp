/**
 * NLQ.cpp - Non-Locking Queue (Modified Herlihy–Wing Queue)
 *
 * A lock-free queue based on the modified HW Queue described in the textbook
 * (Figure 3.14, p.73).  The Java AtomicReference<T>[] array is modelled here
 * with a fixed-capacity array of std::atomic<T*> pointers.
 *
 * Design:
 *   - A fixed-size array 'items' of std::atomic<int*> (using pointers so that
 *     nullptr can serve as the "slot is empty" sentinel).
 *   - An atomic counter 'tail' is fetch-and-incremented by each enq(), giving
 *     each enqueueing thread an exclusive slot index.
 *   - deq() snapshots the current tail, then linearly scans [0, range) for a
 *     non-null slot, swapping it to null atomically (getAndSet in the Java code
 *     corresponds to exchange() in C++).
 *
 * Linearization Points:
 *   - enq(): the atomic store items[i].store(x) – once the value is visible,
 *            it is logically in the queue.
 *   - deq(): the successful atomic exchange (items[i].exchange(nullptr)) that
 *            returns a non-null pointer – at that moment the item is removed.
 *
 * Limitations of HW Queue:
 *   - The tail index only ever grows; there is no memory reclamation of
 *     consumed slots.  For this assignment we use a generous CAPACITY.
 *   - deq() may return null even if items were concurrently enqueued but not
 *     yet stored (the brief window between getAndIncrement and the store).
 *     This is consistent with the original HW Queue specification.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
#include <iomanip>

// ─────────────────────────────────────────────
//  Timing helpers  (identical to CLQ.cpp)
// ─────────────────────────────────────────────
static std::string getCurrentTime()
{
    using namespace std::chrono;
    auto now      = system_clock::now();
    auto ms       = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

static long long getTimeMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// ─────────────────────────────────────────────
//  Thread-local log file
// ─────────────────────────────────────────────
thread_local std::ofstream tlLogFile;

// ─────────────────────────────────────────────
//  NLQ  –  Non-Locking (Modified HW) Queue
// ─────────────────────────────────────────────
template <typename T>
class NLQ
{
    // Capacity large enough for the experiment; in production, use a
    // circular / resizable design.
    static constexpr int CAPACITY = 1 << 20; // 1 M slots

    // We store pointers so nullptr == empty
    std::vector<std::atomic<T*>> items;
    std::atomic<int>             tail;

public:
    NLQ() : items(CAPACITY), tail(0)
    {
        for (auto& slot : items)
            slot.store(nullptr, std::memory_order_relaxed);
    }

    ~NLQ()
    {
        // Free any remaining heap-allocated values
        int t = tail.load(std::memory_order_acquire);
        for (int i = 0; i < t; ++i)
        {
            T* p = items[i].exchange(nullptr, std::memory_order_relaxed);
            delete p;
        }
    }

    // Enqueue a value (copy it onto the heap so we can use the pointer as a flag)
    void enq(T x)
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue started in enq method at time: "
                      << getCurrentTime() << "\n";

        int   i    = tail.fetch_add(1, std::memory_order_relaxed); // ← reserve slot
        T*    val  = new T(x);
        items[i].store(val, std::memory_order_release);             // ← LP: visible to deq()

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue completed in enq method at time: "
                      << getCurrentTime() << "\n";
    }

    // Dequeue a value; returns nullptr if queue appears empty
    T* deq()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue started in deq method at time: "
                      << getCurrentTime() << "\n";

        int range = tail.load(std::memory_order_acquire); // snapshot current tail
        for (int i = 0; i < range; ++i)
        {
            T* val = items[i].exchange(nullptr, std::memory_order_acq_rel); // ← LP if non-null
            if (val != nullptr)
            {
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << std::this_thread::get_id()
                              << ": Dequeue completed in deq method at time: "
                              << getCurrentTime() << "\n";
                return val; // caller must delete
            }
        }

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue completed (empty) in deq method at time: "
                      << getCurrentTime() << "\n";
        return nullptr;
    }

    // Extra Credit – front(): atomically peek at the first non-null slot
    // LP: the load of items[i] that returns a non-null pointer.
    T* front()
    {
        int range = tail.load(std::memory_order_acquire);
        for (int i = 0; i < range; ++i)
        {
            T* val = items[i].load(std::memory_order_acquire);
            if (val != nullptr) return val; // do NOT exchange – just peek
        }
        return nullptr;
    }

    // Extra Credit – rear(): peek at the slot just before the current tail
    // LP: the load of items[t-1] while tail >= 1.
    T* rear()
    {
        int t = tail.load(std::memory_order_acquire);
        // Walk backwards to find the last non-null slot
        for (int i = t - 1; i >= 0; --i)
        {
            T* val = items[i].load(std::memory_order_acquire);
            if (val != nullptr) return val;
        }
        return nullptr;
    }
};

// ─────────────────────────────────────────────
//  QTester  (mirrors CLQ.cpp structure)
// ─────────────────────────────────────────────
int    n       = 4;
int    numOps  = 20;
double rndLt   = 0.5;
double lambda  = 5.0;

std::ofstream           globLogFile;
std::mutex              globLogMtx;
std::vector<long long>  thrTimes;
std::vector<long long>  enqTimes;
std::vector<long long>  deqTimes;
std::vector<int>        enqCount;
std::vector<int>        deqCount;
NLQ<int>                qObj;

static void globLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(globLogMtx);
    if (globLogFile.is_open()) globLogFile << msg << "\n";
}

void testThread(int tid)
{
    std::string tlName = "LogFile-NLQ-" + std::to_string(tid) + ".log";
    tlLogFile.open(tlName);

    std::mt19937                     rng(std::random_device{}());
    std::uniform_real_distribution<> unifDist(0.0, 1.0);
    std::exponential_distribution<>  expDist(1.0 / lambda);

    for (int i = 0; i < numOps; ++i)
    {
        double   p     = unifDist(rng);
        long long start = getTimeMicros();

        if (p < rndLt) // Enqueue
        {
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue started in testThread at time: "
                          << getCurrentTime() << "\n";

            int v = rng() % 1000;
            qObj.enq(v);

            long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";

            enqTimes[tid] += (end - start);
            enqCount[tid]++;
        }
        else // Dequeue
        {
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": DeQueue started in testThread at time: "
                          << getCurrentTime() << "\n";

            int* val = qObj.deq();
            delete val; // safe even if nullptr

            long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": DeQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";

            deqTimes[tid] += (end - start);
            deqCount[tid]++;
        }

        thrTimes[tid] += (getTimeMicros() - start);

        double sleepMs = expDist(rng);
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long long>(sleepMs * 1000)));
    }

    tlLogFile.close();
}

void computeStats()
{
    long long totalEnqTime = 0, totalDeqTime = 0, totalTime = 0;
    int       totalEnq = 0, totalDeq = 0;

    for (int i = 0; i < n; ++i)
    {
        totalEnqTime += enqTimes[i];
        totalDeqTime += deqTimes[i];
        totalTime    += thrTimes[i];
        totalEnq     += enqCount[i];
        totalDeq     += deqCount[i];
    }

    double avgEnq   = (totalEnq  > 0) ? (double)totalEnqTime / totalEnq  : 0.0;
    double avgDeq   = (totalDeq  > 0) ? (double)totalDeqTime / totalDeq  : 0.0;
    double avgTotal = (double)totalTime / (totalEnq + totalDeq);

    long long wallUs   = *std::max_element(thrTimes.begin(), thrTimes.end());
    double throughput  = (wallUs > 0) ? (double)(totalEnq + totalDeq) / (wallUs / 1e6) : 0.0;

    std::ofstream avgs("NLQ-avgs.txt");
    avgs << "=== NLQ Statistics ===\n"
         << "Threads       : " << n       << "\n"
         << "Ops/thread    : " << numOps  << "\n"
         << "Total Enqs    : " << totalEnq  << "\n"
         << "Total Deqs    : " << totalDeq  << "\n"
         << "Avg Enq time  : " << std::fixed << std::setprecision(2) << avgEnq   << " µs\n"
         << "Avg Deq time  : " << avgDeq   << " µs\n"
         << "Avg Op  time  : " << avgTotal << " µs\n"
         << "Throughput    : " << throughput << " ops/sec\n";
    avgs.close();

    std::cout << "[NLQ] Avg Enq: " << avgEnq << " µs | Avg Deq: " << avgDeq
              << " µs | Throughput: " << throughput << " ops/s\n";
}

void mergeLogs()
{
    std::vector<std::pair<std::string,std::string>> lines;

    for (int i = 0; i < n; ++i)
    {
        std::string fname = "LogFile-NLQ-" + std::to_string(i) + ".log";
        std::ifstream in(fname);
        std::string line;
        while (std::getline(in, line))
        {
            auto pos = line.rfind("time: ");
            std::string ts = (pos != std::string::npos) ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    globLogFile.close();
    {
        std::ifstream in("LogFile-NLQ.log");
        std::string line;
        while (std::getline(in, line))
        {
            auto pos = line.rfind("time: ");
            std::string ts = (pos != std::string::npos) ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    std::stable_sort(lines.begin(), lines.end(),
                     [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ofstream out("NLQ-out.log");
    for (auto& [ts, ln] : lines)
        out << ln << "\n";
    out.close();
    std::cout << "[NLQ] Merged log written to NLQ-out.log\n";
}

int main()
{
    std::ifstream params("inp-params.txt");
    if (params.is_open())
        params >> n >> numOps >> rndLt >> lambda;
    else
        std::cerr << "[NLQ] inp-params.txt not found – using defaults.\n";

    thrTimes.assign(n, 0);
    enqTimes.assign(n, 0);
    deqTimes.assign(n, 0);
    enqCount.assign(n, 0);
    deqCount.assign(n, 0);

    globLogFile.open("LogFile-NLQ.log");

    std::vector<std::thread> threads;
    threads.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        std::string msg = "Thread " + std::to_string(i)
                          + " created at time: " + getCurrentTime();
        globLog(msg);
        threads.emplace_back(testThread, i);
    }

    for (auto& t : threads) t.join();

    computeStats();
    mergeLogs();

    return 0;
}
