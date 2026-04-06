/**
 * =============================================================================
 * This file extends CLQ.cpp with two additional non-destructive accessor
 * methods required for extra credit:
 *
 *   front() – Returns the front element (next to be dequeued) without
 *             removing it.  Returns default T{} / throws EmptyException
 *             if the queue is empty.
 *
 *   rear()  – Returns the rear element (most recently enqueued) without
 *             removing it.  Returns default T{} / throws EmptyException
 *             if the queue is empty.
 *
 * Base implementation: direct C++ port of the Java LockBasedQueue<T>
 * reference (textbook Figure 3.1, p.50):
 *
 *   class LockBasedQueue<T> {
 *       int  head, tail;        // circular-buffer indices
 *       T[]  items;             // fixed-capacity circular buffer
 *       Lock lock;              // single ReentrantLock
 *   }
 *
 * Design decisions:
 *   - std::mutex           replaces ReentrantLock
 *   - std::vector<T>       replaces T[] (capacity = constructor argument)
 *   - FullException /
 *     EmptyException       lightweight structs derived from std::exception
 *   - head and tail        plain ints (no atomics; mutex protects them)
 *
 * -----------------------------------------------------------------------------
 * LINEARIZATION POINTS (ALL SIX METHODS)
 * -----------------------------------------------------------------------------
 *
 *  enq(x):
 *    LP = items[tail % capacity] = x   while the lock is held.
 *    Justification: The mutex ensures no other thread can observe the queue
 *    between the full-check and this write.  At this precise assignment the
 *    new element joins the queue's abstract state.
 *
 *  deq():
 *    LP = x = items[head % capacity]   while the lock is held.
 *    Justification: The item is logically removed at this read. head++ then
 *    advances the sentinel; both happen atomically from external observers.
 *
 *  front():
 *    LP = return items[head % capacity]   while the lock is held.
 *    Justification: The lock freezes all of head, tail, and items for the
 *    duration of the call.  The returned value is the queue's front at the
 *    unique instant defined by the critical section.  No ABA hazard exists
 *    because no other thread can modify head between the empty-check and the
 *    read while we hold the lock.
 *
 *  rear():
 *    LP = return items[(tail-1+cap) % cap]   while the lock is held.
 *    Justification: enq() writes items[tail%cap] then increments tail, so
 *    the last-written slot is always (tail-1)%cap.  Under the lock, tail
 *    has its most-recent committed value and that slot is guaranteed to hold
 *    a valid element (ensured by the non-empty check).  The expression
 *    (tail-1+cap)%cap avoids negative-modulo behaviour in C++.
 *
 * -----------------------------------------------------------------------------
 * EXTRA-CREDIT TESTER ADDITIONS
 * -----------------------------------------------------------------------------
 * The QTester is extended beyond the base assignment:
 *
 *   - A third probability bucket (using rndLt2, read from inp-params.txt)
 *     exercises front() and rear() in addition to enq() / deq().
 *   - front() and rear() are called under a uniform random choice between
 *     them when the operation selector falls in [rndLt, rndLt2).
 *   - Per-thread counters and timing for front/rear operations feed into
 *     computeStats(), which writes separate averages to CLQ-EC-avgs.txt.
 *   - Log events for front/rear follow the same "started / completed"
 *     pattern as enq/deq and are merged into CLQ-EC-out.log.
 *
 * -----------------------------------------------------------------------------
 * INPUT FILE FORMAT  (inp-params.txt)
 * -----------------------------------------------------------------------------
 *   n        numOps    rndLt    lambda   [rndLt2]   [capacity]
 *
 *   n       : number of threads
 *   numOps  : operations per thread
 *   rndLt   : P(enq)  –  if p < rndLt  → enq()
 *   lambda  : mean exponential sleep between ops (ms)
 *   rndLt2  : P(enq) + P(front/rear)  –  if rndLt <= p < rndLt2 → front()/rear()
 *             (default = rndLt + 0.1; remainder → deq())
 *   capacity: circular buffer capacity (default = max(1024, n*numOps))
 *
 * OUTPUT FILES
 * -----------------------------------------------------------------------------
 *   CLQ-EC-avgs.txt    –  average latency and throughput statistics
 *   CLQ-EC-out.log     –  time-sorted merged event log
 *
 * COMPILATION
 * -----------------------------------------------------------------------------
 *   g++ -std=c++17 -O2 -pthread CLQ-EC-<rollno>.cpp -o clq_ec
 *
 * =============================================================================
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
#include <optional>

// =============================================================================
//  Custom exceptions  (mirror Java FullException / EmptyException)
// =============================================================================

struct FullException : public std::exception {
    const char* what() const noexcept override { return "CLQ: queue is full"; }
};

struct EmptyException : public std::exception {
    const char* what() const noexcept override { return "CLQ: queue is empty"; }
};

// =============================================================================
//  Timing helpers
// =============================================================================

/** Returns a human-readable wall-clock timestamp: HH:MM:SS.mmm */
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

/** Returns microseconds since process start (monotonic, for duration measurement) */
static long long getTimeMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(
               steady_clock::now().time_since_epoch()).count();
}

// =============================================================================
//  Thread-local log file
//  Opened inside each thread function to ensure correct ownership.
// =============================================================================
thread_local std::ofstream tlLogFile;

// =============================================================================
//  CLQ  –  Coarse-Grained Locking Queue  (Extra Credit version)
//
//  Implements: enq(), deq(), front(), rear()
// =============================================================================
template <typename T>
class CLQ
{
    // ── Internal state (mirrors Java LockBasedQueue<T>) ──────────────────
    int            head;    // index of the next item to dequeue (front of queue)
    int            tail;    // index of the next free slot      (one past rear)
    std::vector<T> items;   // circular buffer of fixed capacity
    std::mutex     lock;    // single coarse-grained lock  ≡  ReentrantLock

public:

    /**
     * Constructor.
     * @param capacity  Maximum number of items the queue can hold simultaneously.
     *                  Mirrors: items = (T[]) new Object[capacity]
     */
    explicit CLQ(int capacity)
        : head(0), tail(0), items(capacity)
    {}

    // ──────────────────────────────────────────────────────────────────────
    //  enq(x)  –  Enqueue
    //
    //  Java reference:
    //    lock.lock();
    //    if (tail - head == items.length) throw new FullException();
    //    items[tail % items.length] = x;
    //    tail++;
    //    lock.unlock();
    //
    //  LP: items[tail % capacity] = x   (write, while lock is held)
    // ──────────────────────────────────────────────────────────────────────
    void enq(T x)
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue started in enq method at time: "
                      << getCurrentTime() << "\n";

        {
            std::lock_guard<std::mutex> lk(lock);               // lock.lock()

            if (tail - head == static_cast<int>(items.size()))
                throw FullException{};                          // queue full

            items[tail % static_cast<int>(items.size())] = x;  // ← LP
            tail++;

        }                                                       // lock.unlock()

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue completed in enq method at time: "
                      << getCurrentTime() << "\n";
    }

    // ──────────────────────────────────────────────────────────────────────
    //  deq()  –  Dequeue
    //
    //  Java reference:
    //    lock.lock();
    //    if (tail == head) throw new EmptyException();
    //    T x = items[head % items.length];
    //    head++;
    //    return x;
    //    lock.unlock();
    //
    //  LP: x = items[head % capacity]   (read, while lock is held)
    // ──────────────────────────────────────────────────────────────────────
    T deq()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue started in deq method at time: "
                      << getCurrentTime() << "\n";

        T x{};
        {
            std::lock_guard<std::mutex> lk(lock);               // lock.lock()

            if (tail == head)
                throw EmptyException{};                         // queue empty

            x = items[head % static_cast<int>(items.size())];  // ← LP
            head++;

        }                                                       // lock.unlock()

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue completed in deq method at time: "
                      << getCurrentTime() << "\n";

        return x;
    }

    // ──────────────────────────────────────────────────────────────────────
    //  EXTRA CREDIT
    //  front()  –  Peek at the front element without removing it
    //
    //  Correctness:
    //    The mutex is acquired before any access to head, tail, or items.
    //    This prevents any concurrent enq() or deq() from modifying the
    //    queue's state during the read — the queue is frozen for the entire
    //    duration of the method.
    //
    //    The empty check (tail == head) is identical to deq(), so front()
    //    throws EmptyException exactly when deq() would.
    //
    //    items[head % capacity] is the same slot deq() would read next.
    //    Reading it without incrementing head leaves the queue unchanged.
    //
    //    No ABA hazard: between the empty check and the read, no other
    //    thread can modify head or tail because the lock is held.
    //
    //  LP: return items[head % capacity]   (read, while lock is held)
    //    At this instant head has its most-recent committed value and
    //    items[head%capacity] holds the front element. The lock ensures
    //    this observation is at a well-defined, atomic point in time.
    // ──────────────────────────────────────────────────────────────────────
    T front()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Front started in front method at time: "
                      << getCurrentTime() << "\n";

        T x{};
        {
            std::lock_guard<std::mutex> lk(lock);               // acquire lock

            if (tail == head)
                throw EmptyException{};                         // queue empty

            x = items[head % static_cast<int>(items.size())];  // ← LP
            // head is NOT incremented – read-only peek
        }

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Front completed in front method at time: "
                      << getCurrentTime() << "\n";

        return x;
    }

    // ──────────────────────────────────────────────────────────────────────
    //  EXTRA CREDIT
    //  rear()  –  Peek at the rear element without removing it
    //
    //  Correctness:
    //    The mutex is acquired first, freezing the queue state. No concurrent
    //    enq() can increment tail (adding a new rear) during the read.
    //
    //    enq() writes items[tail%cap] and then increments tail.
    //    Therefore the last written slot is always at (tail-1) % cap.
    //    Since tail > head (non-empty) the slot holds a valid element.
    //
    //    Expression (tail-1+cap)%cap is used instead of (tail-1)%cap to
    //    avoid implementation-defined negative-modulo behaviour in C++,
    //    even though tail >= 1 is guaranteed by the non-empty check.
    //
    //    tail is NOT modified – read-only peek.
    //
    //  LP: return items[(tail-1+cap) % cap]   (read, while lock is held)
    //    Under the lock, tail has its most-recent committed value and
    //    items[(tail-1+cap)%cap] holds the last-enqueued element.
    //    No enq() or deq() can race with this read.
    // ──────────────────────────────────────────────────────────────────────
    T rear()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Rear started in rear method at time: "
                      << getCurrentTime() << "\n";

        T x{};
        {
            std::lock_guard<std::mutex> lk(lock);               // acquire lock

            if (tail == head)
                throw EmptyException{};                         // queue empty

            int cap = static_cast<int>(items.size());
            x = items[(tail - 1 + cap) % cap];                  // ← LP
            // tail is NOT modified – read-only peek
        }

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Rear completed in rear method at time: "
                      << getCurrentTime() << "\n";

        return x;
    }

    /** Convenience accessor: current number of items in the queue. */
    int size()
    {
        std::lock_guard<std::mutex> lk(lock);
        return tail - head;
    }
};

// =============================================================================
//  QTester globals
// =============================================================================

int    n        = 4;
int    numOps   = 20;
double rndLt    = 0.5;    // P(enq)
double rndLt2   = 0.6;    // P(enq) + P(front/rear); remainder → deq
double lambda   = 5.0;
int    CAPACITY = 1024;

std::ofstream          globLogFile;
std::mutex             globLogMtx;

// Per-thread timing / count accumulators
std::vector<long long> thrTimes;    // total op time (all ops)
std::vector<long long> enqTimes;
std::vector<long long> deqTimes;
std::vector<long long> frontTimes;  // EC: front() timing
std::vector<long long> rearTimes;   // EC: rear()  timing
std::vector<int>       enqCount;
std::vector<int>       deqCount;
std::vector<int>       frontCount;  // EC
std::vector<int>       rearCount;   // EC
std::vector<int>       enqFull;
std::vector<int>       deqEmpty;
std::vector<int>       frontEmpty;  // EC: EmptyException in front()
std::vector<int>       rearEmpty;   // EC: EmptyException in rear()

CLQ<int>* qObj = nullptr;

// Thread-safe write to the global creation log
static void globLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(globLogMtx);
    if (globLogFile.is_open()) globLogFile << msg << "\n";
}

// =============================================================================
//  testThread  –  Extended QTester worker
//
//  Operation selection:
//    p < rndLt          → enq()
//    rndLt <= p < rndLt2 → front() or rear()  (50/50 random split)
//    p >= rndLt2        → deq()
// =============================================================================
void testThread(int tid)
{
    const std::string tlName = "LogFile-CLQ-EC-" + std::to_string(tid) + ".log";
    tlLogFile.open(tlName);

    std::mt19937                     rng(std::random_device{}());
    std::uniform_real_distribution<> unifDist(0.0, 1.0);
    std::exponential_distribution<>  expDist(1.0 / lambda);

    for (int i = 0; i < numOps; ++i)
    {
        const double    p     = unifDist(rng);
        const long long start = getTimeMicros();

        // ── enq() path ────────────────────────────────────────────────────
        if (p < rndLt)
        {
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue started in testThread at time: "
                          << getCurrentTime() << "\n";

            const int v = static_cast<int>(rng() % 1000);
            try {
                qObj->enq(v);
            } catch (const FullException&) {
                enqFull[tid]++;
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": EnQueue skipped (queue full) at time: "
                              << getCurrentTime() << "\n";
            }

            const long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": EnQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";
            enqTimes[tid] += (end - start);
            enqCount[tid]++;
        }

        // ── front() / rear() path  (Extra Credit) ─────────────────────────
        else if (p < rndLt2)
        {
            // Randomly choose between front() and rear()
            const bool doFront = (unifDist(rng) < 0.5);

            if (doFront)
            {
                // ── front() ───────────────────────────────────────────────
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": Front started in testThread at time: "
                              << getCurrentTime() << "\n";

                try {
                    const int v = qObj->front();
                    (void)v;   // value observed; used for timing only
                } catch (const EmptyException&) {
                    frontEmpty[tid]++;
                    if (tlLogFile.is_open())
                        tlLogFile << "Thread " << tid
                                  << ": Front skipped (queue empty) at time: "
                                  << getCurrentTime() << "\n";
                }

                const long long end = getTimeMicros();
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": Front completed in testThread at time: "
                              << getCurrentTime() << "\n";
                frontTimes[tid] += (end - start);
                frontCount[tid]++;
            }
            else
            {
                // ── rear() ────────────────────────────────────────────────
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": Rear started in testThread at time: "
                              << getCurrentTime() << "\n";

                try {
                    const int v = qObj->rear();
                    (void)v;
                } catch (const EmptyException&) {
                    rearEmpty[tid]++;
                    if (tlLogFile.is_open())
                        tlLogFile << "Thread " << tid
                                  << ": Rear skipped (queue empty) at time: "
                                  << getCurrentTime() << "\n";
                }

                const long long end = getTimeMicros();
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": Rear completed in testThread at time: "
                              << getCurrentTime() << "\n";
                rearTimes[tid] += (end - start);
                rearCount[tid]++;
            }
        }

        // ── deq() path ────────────────────────────────────────────────────
        else
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
                              << ": DeQueue skipped (queue empty) at time: "
                              << getCurrentTime() << "\n";
            }

            const long long end = getTimeMicros();
            if (tlLogFile.is_open())
                tlLogFile << "Thread " << tid
                          << ": DeQueue completed in testThread at time: "
                          << getCurrentTime() << "\n";
            deqTimes[tid] += (end - start);
            deqCount[tid]++;
        }

        thrTimes[tid] += (getTimeMicros() - start);

        // Simulate other work: exponential sleep
        const double sleepMs = expDist(rng);
        std::this_thread::sleep_for(
            std::chrono::microseconds(
                static_cast<long long>(sleepMs * 1000.0)));
    }

    tlLogFile.close();
}

// =============================================================================
//  computeStats  →  CLQ-EC-avgs.txt
// =============================================================================
void computeStats()
{
    long long totalEnqTime   = 0, totalDeqTime   = 0;
    long long totalFrontTime = 0, totalRearTime  = 0;
    long long totalTime      = 0;
    int totalEnq = 0, totalDeq = 0, totalFront = 0, totalRear = 0;
    int totalFull = 0, totalDEmpty = 0, totalFEmpty = 0, totalREmpty = 0;

    for (int i = 0; i < n; ++i)
    {
        totalEnqTime   += enqTimes[i];
        totalDeqTime   += deqTimes[i];
        totalFrontTime += frontTimes[i];
        totalRearTime  += rearTimes[i];
        totalTime      += thrTimes[i];
        totalEnq       += enqCount[i];
        totalDeq       += deqCount[i];
        totalFront     += frontCount[i];
        totalRear      += rearCount[i];
        totalFull      += enqFull[i];
        totalDEmpty    += deqEmpty[i];
        totalFEmpty    += frontEmpty[i];
        totalREmpty    += rearEmpty[i];
    }

    const int    ops        = totalEnq + totalDeq + totalFront + totalRear;
    const double avgEnq     = totalEnq   > 0 ? (double)totalEnqTime   / totalEnq   : 0.0;
    const double avgDeq     = totalDeq   > 0 ? (double)totalDeqTime   / totalDeq   : 0.0;
    const double avgFront   = totalFront > 0 ? (double)totalFrontTime / totalFront : 0.0;
    const double avgRear    = totalRear  > 0 ? (double)totalRearTime  / totalRear  : 0.0;
    const double avgTotal   = ops        > 0 ? (double)totalTime      / ops        : 0.0;
    const long long wallUs  = *std::max_element(thrTimes.begin(), thrTimes.end());
    const double throughput = wallUs     > 0 ? (double)ops / (wallUs / 1e6)        : 0.0;

    std::ofstream avgs("CLQ-EC-avgs.txt");
    avgs << "========================================\n"
         << "  CLQ Extra-Credit Statistics\n"
         << "========================================\n"
         << "Threads              : " << n          << "\n"
         << "Ops / thread         : " << numOps     << "\n"
         << "Buffer capacity      : " << CAPACITY   << "\n"
         << "P(enq)               : " << rndLt      << "\n"
         << "P(front/rear)        : " << (rndLt2 - rndLt) << "\n"
         << "P(deq)               : " << (1.0 - rndLt2)   << "\n"
         << "Lambda (ms)          : " << lambda     << "\n"
         << "----------------------------------------\n"
         << "Total enqs tried     : " << totalEnq   << "\n"
         << "Total deqs tried     : " << totalDeq   << "\n"
         << "Total fronts tried   : " << totalFront << "\n"
         << "Total rears tried    : " << totalRear  << "\n"
         << "Full exceptions      : " << totalFull  << "\n"
         << "Empty (deq)          : " << totalDEmpty << "\n"
         << "Empty (front)        : " << totalFEmpty << "\n"
         << "Empty (rear)         : " << totalREmpty << "\n"
         << "----------------------------------------\n"
         << std::fixed << std::setprecision(2)
         << "Avg enq  latency     : " << avgEnq   << " us\n"
         << "Avg deq  latency     : " << avgDeq   << " us\n"
         << "Avg front latency    : " << avgFront << " us\n"
         << "Avg rear  latency    : " << avgRear  << " us\n"
         << "Avg all ops latency  : " << avgTotal << " us\n"
         << "Throughput (all ops) : " << throughput << " ops/sec\n"
         << "========================================\n";
    avgs.close();

    std::cout << "[CLQ-EC] Enq: "   << avgEnq   << " us"
              << " | Deq: "         << avgDeq   << " us"
              << " | Front: "       << avgFront << " us"
              << " | Rear: "        << avgRear  << " us"
              << " | Throughput: "  << throughput << " ops/s\n";
}

// =============================================================================
//  mergeLogs  →  CLQ-EC-out.log  (all events sorted by timestamp)
// =============================================================================
void mergeLogs()
{
    std::vector<std::pair<std::string, std::string>> lines; // (timestamp, line)

    // Collect per-thread log lines
    for (int i = 0; i < n; ++i)
    {
        std::ifstream in("LogFile-CLQ-EC-" + std::to_string(i) + ".log");
        std::string   line;
        while (std::getline(in, line))
        {
            const auto pos = line.rfind("time: ");
            const std::string ts = (pos != std::string::npos)
                                   ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    // Collect global (thread-creation) log lines
    globLogFile.close();
    {
        std::ifstream in("LogFile-CLQ-EC.log");
        std::string   line;
        while (std::getline(in, line))
        {
            const auto pos = line.rfind("time: ");
            const std::string ts = (pos != std::string::npos)
                                   ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    // Sort by HH:MM:SS.mmm (lexicographic order is correct for this format)
    std::stable_sort(lines.begin(), lines.end(),
        [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ofstream out("CLQ-EC-out.log");
    for (const auto& [ts, ln] : lines)
        out << ln << "\n";
    out.close();

    std::cout << "[CLQ-EC] Merged log written to CLQ-EC-out.log\n";
}

// =============================================================================
//  main
// =============================================================================
int main()
{
    // Read parameters from inp-params.txt
    // Format: n  numOps  rndLt  lambda  [rndLt2]  [capacity]
    std::ifstream params("inp-params.txt");
    if (params.is_open())
    {
        params >> n >> numOps >> rndLt >> lambda;
        if (!(params >> rndLt2))  rndLt2   = rndLt + 0.1;
        if (!(params >> CAPACITY)) CAPACITY = 1024;
    }
    else
    {
        std::cerr << "[CLQ-EC] inp-params.txt not found – using defaults.\n";
    }

    // Clamp rndLt2 to valid range
    if (rndLt2 <= rndLt) rndLt2 = rndLt + 0.1;
    if (rndLt2 >  1.0  ) rndLt2 = 1.0;

    // Ensure sufficient capacity
    CAPACITY = std::max(CAPACITY, n * numOps);

    // Construct queue
    qObj = new CLQ<int>(CAPACITY);

    // Initialise per-thread stat vectors
    thrTimes  .assign(n, 0);
    enqTimes  .assign(n, 0);
    deqTimes  .assign(n, 0);
    frontTimes.assign(n, 0);
    rearTimes .assign(n, 0);
    enqCount  .assign(n, 0);
    deqCount  .assign(n, 0);
    frontCount.assign(n, 0);
    rearCount .assign(n, 0);
    enqFull   .assign(n, 0);
    deqEmpty  .assign(n, 0);
    frontEmpty.assign(n, 0);
    rearEmpty .assign(n, 0);

    // Open global log
    globLogFile.open("LogFile-CLQ-EC.log");

    // Spawn n threads
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        globLog("Thread " + std::to_string(i)
                + " created at time: " + getCurrentTime());
        threads.emplace_back(testThread, i);
    }

    // Wait for all threads to complete
    for (auto& t : threads) t.join();

    // Output statistics and merged log
    computeStats();
    mergeLogs();

    delete qObj;
    return 0;
}
