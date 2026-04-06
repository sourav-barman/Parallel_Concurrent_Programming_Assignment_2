/**
 * CLQ.cpp  –  Coarse-Grained Locking Queue
 *
 * C++ translation of the Java LockBasedQueue<T> reference code:
 *
 *   class LockBasedQueue<T> {
 *       int head, tail;          // indices into circular array
 *       T[] items;               // fixed-capacity circular buffer
 *       Lock lock;               // single ReentrantLock
 *
 *       enq(T x): lock → if full throw FullException
 *                         items[tail % capacity] = x; tail++
 *                 unlock
 *
 *       deq():    lock → if empty throw EmptyException
 *                         x = items[head % capacity]; head++; return x
 *                 unlock
 *   }
 *
 * Design decisions for C++ port:
 *   - std::mutex  replaces ReentrantLock.
 *   - std::vector<T> replaces the Java array; capacity is a constructor arg.
 *   - FullException / EmptyException are lightweight structs derived from
 *     std::exception; the tester catches them gracefully and skips the op.
 *   - head and tail are plain ints (no atomics needed – the mutex protects them).
 *
 * Linearization Points:
 *   - enq():  items[tail % capacity] = x  while the lock is held.  (LP)
 *   - deq():  x = items[head % capacity]  while the lock is held.  (LP)
 *
 * Extra Credit:
 *   - front(): read items[head % capacity] under the lock (no structural change).
 *   - rear():  read items[(tail-1) % capacity] under the lock.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <iomanip>
#include <string>

// ──────────────────────────────────────────────────────────
//  Custom exceptions  (mirror Java FullException / EmptyException)
// ──────────────────────────────────────────────────────────
struct FullException : public std::exception {
    const char* what() const noexcept override { return "Queue is full"; }
};
struct EmptyException : public std::exception {
    const char* what() const noexcept override { return "Queue is empty"; }
};

// ──────────────────────────────────────────────────────────
//  Timing helpers
// ──────────────────────────────────────────────────────────
static std::string getCurrentTime()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

static long long getTimeMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(
               steady_clock::now().time_since_epoch()).count();
}

// ──────────────────────────────────────────────────────────
//  Thread-local log file
// ──────────────────────────────────────────────────────────
thread_local std::ofstream tlLogFile;

// ──────────────────────────────────────────────────────────
//  CLQ  –  direct C++ port of Java LockBasedQueue<T>
// ──────────────────────────────────────────────────────────
template <typename T>
class CLQ
{
    int            head;    // index of next item to dequeue
    int            tail;    // index of next free slot
    std::vector<T> items;   // circular buffer
    std::mutex     lock;    // single coarse-grained lock (= ReentrantLock)

public:
    // 'capacity' mirrors the Java constructor argument
    explicit CLQ(int capacity)
        : head(0), tail(0), items(capacity)
    {}

    // ── enq ──────────────────────────────────────────────
    // Java reference:
    //   lock.lock();
    //   if (tail - head == items.length) throw new FullException();
    //   items[tail % items.length] = x;
    //   tail++;
    //   lock.unlock();
    void enq(T x)
    {
        // Log "started" before acquiring the lock
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue started in enq method at time: "
                      << getCurrentTime() << "\n";

        {
            std::lock_guard<std::mutex> lk(lock);       // lock.lock()

            if (tail - head == static_cast<int>(items.size()))
                throw FullException();                  // queue full

            items[tail % static_cast<int>(items.size())] = x; // ← LP
            tail++;

        }                                               // lock.unlock()

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue completed in enq method at time: "
                      << getCurrentTime() << "\n";
    }

    // ── deq ──────────────────────────────────────────────
    // Java reference:
    //   lock.lock();
    //   if (tail == head) throw new EmptyException();
    //   T x = items[head % items.length];
    //   head++;
    //   return x;
    //   lock.unlock();
    T deq()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue started in deq method at time: "
                      << getCurrentTime() << "\n";

        T x;
        {
            std::lock_guard<std::mutex> lk(lock);       // lock.lock()

            if (tail == head)
                throw EmptyException();                 // queue empty

            x = items[head % static_cast<int>(items.size())]; // ← LP
            head++;

        }                                               // lock.unlock()

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue completed in deq method at time: "
                      << getCurrentTime() << "\n";

        return x;
    }

    // ── Extra Credit: front() ─────────────────────────────
    // Returns front element without removal.
    // LP: read of items[head % capacity] while lock is held.
    T front()
    {
        std::lock_guard<std::mutex> lk(lock);
        if (tail == head) throw EmptyException();
        return items[head % static_cast<int>(items.size())]; // ← LP
    }

    // ── Extra Credit: rear() ──────────────────────────────
    // Returns rear element without removal.
    // LP: read of items[(tail-1) % capacity] while lock is held.
    T rear()
    {
        std::lock_guard<std::mutex> lk(lock);
        if (tail == head) throw EmptyException();
        int cap = static_cast<int>(items.size());
        return items[(tail - 1 + cap) % cap];               // ← LP
    }
};

// ──────────────────────────────────────────────────────────
//  QTester globals
// ──────────────────────────────────────────────────────────
int    n        = 4;
int    numOps   = 20;
double rndLt    = 0.5;
double lambda   = 5.0;
int    CAPACITY = 1024;

std::ofstream           globLogFile;
std::mutex              globLogMtx;
std::vector<long long>  thrTimes;
std::vector<long long>  enqTimes;
std::vector<long long>  deqTimes;
std::vector<int>        enqCount;
std::vector<int>        deqCount;
std::vector<int>        enqFull;
std::vector<int>        deqEmpty;

CLQ<int>* qObj = nullptr;

static void globLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(globLogMtx);
    if (globLogFile.is_open()) globLogFile << msg << "\n";
}

// ──────────────────────────────────────────────────────────
//  Thread worker  (mirrors Java QTester.testThread)
// ──────────────────────────────────────────────────────────
void testThread(int tid)
{
    std::string tlName = "LogFile-" + std::to_string(tid) + ".log";
    tlLogFile.open(tlName);

    std::mt19937                     rng(std::random_device{}());
    std::uniform_real_distribution<> unifDist(0.0, 1.0);
    std::exponential_distribution<>  expDist(1.0 / lambda);

    for (int i = 0; i < numOps; ++i)
    {
        double    p     = unifDist(rng);
        long long start = getTimeMicros();

        if (p < rndLt)   // ── Enqueue path ──────────────────
        {
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue started in testThread at time: "
                          << getCurrentTime() << "\n";

            int v = static_cast<int>(rng() % 1000);
            try {
                qObj->enq(v);
            } catch (const FullException&) {
                enqFull[tid]++;
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": EnQueue skipped – queue full – at time: "
                              << getCurrentTime() << "\n";
            }

            long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";

            enqTimes[tid] += (end - start);
            enqCount[tid]++;
        }
        else              // ── Dequeue path ──────────────────
        {
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": DeQueue started in testThread at time: "
                          << getCurrentTime() << "\n";

            try {
                qObj->deq();
            } catch (const EmptyException&) {
                deqEmpty[tid]++;
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": DeQueue skipped – queue empty – at time: "
                              << getCurrentTime() << "\n";
            }

            long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": DeQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";

            deqTimes[tid] += (end - start);
            deqCount[tid]++;
        }

        thrTimes[tid] += (getTimeMicros() - start);

        // Simulate other work: exponential sleep
        double sleepMs = expDist(rng);
        std::this_thread::sleep_for(
            std::chrono::microseconds(
                static_cast<long long>(sleepMs * 1000.0)));
    }

    tlLogFile.close();
}

// ──────────────────────────────────────────────────────────
//  computeStats  →  CLQ-avgs.txt
// ──────────────────────────────────────────────────────────
void computeStats()
{
    long long totalEnqTime = 0, totalDeqTime = 0, totalTime = 0;
    int       totalEnq = 0,    totalDeq = 0;
    int       totalFull = 0,   totalEmpty = 0;

    for (int i = 0; i < n; ++i)
    {
        totalEnqTime += enqTimes[i];
        totalDeqTime += deqTimes[i];
        totalTime    += thrTimes[i];
        totalEnq     += enqCount[i];
        totalDeq     += deqCount[i];
        totalFull    += enqFull[i];
        totalEmpty   += deqEmpty[i];
    }

    int    ops        = totalEnq + totalDeq;
    double avgEnq     = (totalEnq > 0) ? (double)totalEnqTime / totalEnq : 0.0;
    double avgDeq     = (totalDeq > 0) ? (double)totalDeqTime / totalDeq : 0.0;
    double avgTotal   = (ops      > 0) ? (double)totalTime    / ops      : 0.0;
    long long wallUs  = *std::max_element(thrTimes.begin(), thrTimes.end());
    double throughput = (wallUs > 0) ? (double)ops / (wallUs / 1e6) : 0.0;

    std::ofstream avgs("CLQ-avgs.txt");
    avgs << "=== CLQ Statistics ===\n"
         << "Threads          : " << n          << "\n"
         << "Ops / thread     : " << numOps     << "\n"
         << "Buffer capacity  : " << CAPACITY   << "\n"
         << "Total Enqs tried : " << totalEnq   << "\n"
         << "Total Deqs tried : " << totalDeq   << "\n"
         << "Full exceptions  : " << totalFull  << "\n"
         << "Empty exceptions : " << totalEmpty << "\n"
         << std::fixed << std::setprecision(2)
         << "Avg Enq time     : " << avgEnq    << " us\n"
         << "Avg Deq time     : " << avgDeq    << " us\n"
         << "Avg Op  time     : " << avgTotal  << " us\n"
         << "Throughput       : " << throughput << " ops/sec\n";
    avgs.close();

    std::cout << "[CLQ] Avg Enq: " << avgEnq
              << " us | Avg Deq: " << avgDeq
              << " us | Throughput: " << throughput << " ops/s\n";
}

// ──────────────────────────────────────────────────────────
//  mergeLogs  →  CLQ-out.log  (time-sorted)
// ──────────────────────────────────────────────────────────
void mergeLogs()
{
    std::vector<std::pair<std::string, std::string>> lines;

    for (int i = 0; i < n; ++i)
    {
        std::ifstream in("LogFile-" + std::to_string(i) + ".log");
        std::string   line;
        while (std::getline(in, line))
        {
            auto pos = line.rfind("time: ");
            std::string ts = (pos != std::string::npos)
                             ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    globLogFile.close();
    {
        std::ifstream in("LogFile.log");
        std::string   line;
        while (std::getline(in, line))
        {
            auto pos = line.rfind("time: ");
            std::string ts = (pos != std::string::npos)
                             ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    std::stable_sort(lines.begin(), lines.end(),
        [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ofstream out("CLQ-out.log");
    for (auto& [ts, ln] : lines)
        out << ln << "\n";
    out.close();

    std::cout << "[CLQ] Merged log written to CLQ-out.log\n";
}

// ──────────────────────────────────────────────────────────
//  main
// ──────────────────────────────────────────────────────────
int main()
{
    std::ifstream params("inp-params.txt");
    if (params.is_open())
    {
        params >> n >> numOps >> rndLt >> lambda;
        // Optional 5th param: buffer capacity
        if (!(params >> CAPACITY)) {
            CAPACITY = 1024; // default
        }
    }
    else
    {
        std::cerr << "[CLQ] inp-params.txt not found – using defaults.\n";
    }

    // Ensure capacity >= n * numOps so FullException only occurs naturally
    CAPACITY = std::max(CAPACITY, n * numOps);

    qObj = new CLQ<int>(CAPACITY);

    thrTimes.assign(n, 0);
    enqTimes.assign(n, 0);
    deqTimes.assign(n, 0);
    enqCount.assign(n, 0);
    deqCount.assign(n, 0);
    enqFull .assign(n, 0);
    deqEmpty.assign(n, 0);

    globLogFile.open("LogFile.log");

    std::vector<std::thread> threads;
    threads.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        globLog("Thread " + std::to_string(i)
                + " created at time: " + getCurrentTime());
        threads.emplace_back(testThread, i);
    }

    for (auto& t : threads) t.join();

    computeStats();
    mergeLogs();

    delete qObj;
    return 0;
}
