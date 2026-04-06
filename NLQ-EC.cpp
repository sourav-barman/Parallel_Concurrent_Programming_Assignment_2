/**
 * This file extends NLQ.cpp with two additional non-destructive accessor
 * methods required for extra credit:
 *
 *   front() – Returns a pointer to the front element (next to be dequeued)
 *             without removing it.  Returns nullptr if the queue is empty.
 *
 *   rear()  – Returns a pointer to the rear element (most recently enqueued)
 *             without removing it.  Returns nullptr if the queue is empty.
 *
 * Base implementation: C++ port of the modified Herlihy–Wing Queue
 * (textbook Figure 3.14, p.73).
 *
 *   - std::vector<std::atomic<T*>> items  –  atomic pointer array;
 *     nullptr == "slot is empty" sentinel (mirrors Java AtomicReference null)
 *   - std::atomic<int> tail               –  atomically incremented;
 *     each enq() reserves an exclusive slot via fetch_add
 *
 * =============================================================================
 *
 * LINEARIZATION POINTS (ALL SIX METHODS)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  enq(x):
 *    LP = items[i].store(val, memory_order_release)
 *    Justification: Once this release-store is globally visible, the item is
 *    logically in the queue.  fetch_add guarantees slot i is unique to this
 *    thread.  Any subsequent deq() / front() that observes items[i] != nullptr
 *    via an acquire-load sees a fully-written value (release/acquire pairing).
 *
 *  deq():
 *    LP = items[i].exchange(nullptr, memory_order_acq_rel)  that returns
 *         a non-null pointer, for the smallest such i in [0, range).
 *    Justification: exchange() is atomic – at the single instant it transitions
 *    items[i] from non-null to null, the item is logically removed.  Because
 *    exchange() is atomic, no two deq() threads can claim the same slot.
 *
 *  front():
 *    LP = items[i].load(memory_order_acquire)  that returns a non-null pointer,
 *         for the smallest such i in [0, range).
 *    Justification:
 *      (a) We use LOAD, not exchange() – this is the critical design decision.
 *          exchange(nullptr) would remove the item (identical to deq()).
 *          load() peeks without modifying the slot.
 *      (b) The acquire-load of items[i] synchronises-with enq_i's release-store,
 *          establishing happens-before: enq_i → front().  Therefore if we observe
 *          items[i] != nullptr we are guaranteed to read the fully-written value.
 *      (c) A concurrent deq() may exchange items[i] to null before our load.
 *          We then see nullptr and advance to i+1 – safely observing the next
 *          non-removed item.  Both orderings are correct.
 *      (d) The returned value was genuinely the front at the LP, which lies
 *          within front()'s execution interval [inv, resp].
 *
 *  rear():
 *    LP = items[i].load(memory_order_acquire)  that returns a non-null pointer,
 *         for the LARGEST such i in [0, t).
 *    Justification:
 *      (a) Same load-not-exchange reasoning as front().
 *      (b) Scanning backwards from tail-1 finds the highest-indexed non-null
 *          slot, which is the most recently enqueued item still present.
 *      (c) A concurrent enq() may increment tail after our snapshot t.  We do
 *          not see that new slot – we return the element at t-1, which was the
 *          rear at snapshot time.  This is a valid LP.
 *      (d) A concurrent deq() may have removed items[t-1] before our load.
 *          We see nullptr there and scan to t-2, finding the actual rear.
 *
 * =============================================================================
 *
 * NOTE ON MEMORY MANAGEMENT
 * ─────────────────────────────────────────────────────────────────────────────
 * NLQ heap-allocates each enqueued value (new T(x)) to obtain a pointer
 * usable as an atomic "slot occupied" flag.
 *
 *   - deq() returns a raw T*; the caller is responsible for delete.
 *   - front() and rear() return a raw T* (pointer into live queue memory).
 *     THE CALLER MUST NOT DELETE THIS POINTER – it still belongs to the queue.
 *   - The ~NLQ() destructor deletes all unconsumed slots.
 *
 * =============================================================================
 *
 * INPUT FILE FORMAT  (inp-params.txt)
 * ─────────────────────────────────────────────────────────────────────────────
 *   n        numOps    rndLt    lambda   [rndLt2]
 *
 *   n       : number of threads
 *   numOps  : operations per thread
 *   rndLt   : P(enq)  –  if p < rndLt  → enq()
 *   lambda  : mean exponential sleep between ops (ms)
 *   rndLt2  : P(enq) + P(front/rear)  –  if rndLt <= p < rndLt2 → front()/rear()
 *             (default = rndLt + 0.1; remainder → deq())
 *
 * OUTPUT FILES
 * ─────────────────────────────────────────────────────────────────────────────
 *   NLQ-EC-avgs.txt    –  average latency and throughput statistics
 *   NLQ-EC-out.log     –  time-sorted merged event log
 *
 * COMPILATION
 * ─────────────────────────────────────────────────────────────────────────────
 *   g++ -std=c++17 -O2 -pthread NLQ-EC-<rollno>.cpp -o nlq_ec
 *
 * =============================================================================
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
#include <iomanip>
#include <string>

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
// =============================================================================
thread_local std::ofstream tlLogFile;

// =============================================================================
//  NLQ  –  Non-Locking (Modified Herlihy–Wing) Queue  (Extra Credit version)
//
//  Implements: enq(), deq(), front(), rear()
// =============================================================================
template <typename T>
class NLQ
{
    // ── Capacity ─────────────────────────────────────────────────────────
    // The HW Queue uses an ever-growing array (tail never decreases).
    // 1 M slots is sufficient for all experiments in this assignment.
    // In production, an epoch-based reclamation scheme would be required.
    static constexpr int ARRAY_CAPACITY = 1 << 20; // 1 048 576 slots

    // ── Internal state ────────────────────────────────────────────────────
    // items[i] == nullptr  →  slot is empty (not yet enqueued, or already dequeued)
    // items[i] != nullptr  →  slot holds a live heap-allocated value
    std::vector<std::atomic<T*>> items;

    // tail is the index of the next slot to be reserved by enq().
    // It is fetch_add'd atomically; no two enq() calls receive the same index.
    std::atomic<int> tail;

public:

    NLQ() : items(ARRAY_CAPACITY), tail(0)
    {
        for (auto& slot : items)
            slot.store(nullptr, std::memory_order_relaxed);
    }

    ~NLQ()
    {
        // Delete any unconsumed values to prevent memory leaks
        const int t = tail.load(std::memory_order_acquire);
        for (int i = 0; i < t; ++i)
        {
            T* p = items[i].exchange(nullptr, std::memory_order_relaxed);
            delete p;
        }
    }

    // ──────────────────────────────────────────────────────────────────────
    //  enq(x)  –  Enqueue
    //
    //  1. Atomically reserve an exclusive slot index via fetch_add.
    //  2. Heap-allocate the value (so a pointer can serve as the sentinel).
    //  3. Store the pointer with memory_order_release so any subsequent
    //     acquire-load in deq() / front() / rear() sees the full value.
    //
    //  LP: items[i].store(val, memory_order_release)
    // ──────────────────────────────────────────────────────────────────────
    void enq(T x)
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue started in enq method at time: "
                      << getCurrentTime() << "\n";

        const int i  = tail.fetch_add(1, std::memory_order_relaxed); // reserve slot
        T*       val = new T(x);
        items[i].store(val, std::memory_order_release);               // ← LP

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Enqueue completed in enq method at time: "
                      << getCurrentTime() << "\n";
    }

    // ──────────────────────────────────────────────────────────────────────
    //  deq()  –  Dequeue
    //
    //  1. Snapshot the current tail (acquire-load).
    //  2. Scan slots [0, range) for the first non-null pointer.
    //  3. Atomically exchange that slot to nullptr (acq_rel).
    //  4. If exchange returned non-null, that is the dequeued item.
    //
    //  Caller MUST delete the returned pointer.
    //  Returns nullptr if the queue appears empty.
    //
    //  LP: items[i].exchange(nullptr, acq_rel) returning non-null,
    //      for the smallest such i in [0, range)
    // ──────────────────────────────────────────────────────────────────────
    T* deq()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Dequeue started in deq method at time: "
                      << getCurrentTime() << "\n";

        const int range = tail.load(std::memory_order_acquire); // snapshot

        for (int i = 0; i < range; ++i)
        {
            T* val = items[i].exchange(nullptr, std::memory_order_acq_rel); // ← LP
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

    // ──────────────────────────────────────────────────────────────────────
    //  EXTRA CREDIT
    //  front()  –  Peek at the front element without removing it
    //
    //  KEY DESIGN DECISION: uses load(), NOT exchange().
    //    exchange(nullptr) would atomically remove the item — that is what
    //    deq() does.  front() must be non-destructive, so we use a plain
    //    atomic load with acquire memory order.
    //
    //  Correctness:
    //    (a) tail.load(acquire) establishes the upper bound on assigned slots.
    //        The acquire fence ensures all prior fetch_add and store operations
    //        are visible.
    //    (b) items[i].load(acquire) returns non-null only if enq_i has
    //        completed its store(val, release).  The release/acquire pair
    //        establishes happens-before: enq_i → front().  No torn reads.
    //    (c) We do NOT call exchange() – the slot is not set to nullptr.
    //        The item remains in the queue and is available for deq().
    //    (d) A concurrent deq() may exchange items[i] to null between our
    //        tail snapshot and our load.  We then see nullptr and advance to
    //        i+1 – safely observing the next non-removed item.  Correct.
    //    (e) If all slots appear null we return nullptr.  The queue was
    //        empty at some point during the call's interval.  Valid.
    //
    //  Caller MUST NOT delete the returned pointer.
    //  Returns nullptr if the queue appears empty.
    //
    //  LP: items[i].load(memory_order_acquire) returning non-null,
    //      for the smallest such i in [0, range)
    // ──────────────────────────────────────────────────────────────────────
    T* front()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Front started in front method at time: "
                      << getCurrentTime() << "\n";

        const int range = tail.load(std::memory_order_acquire); // snapshot

        for (int i = 0; i < range; ++i)
        {
            // load() – non-destructive peek; do NOT use exchange() here
            T* val = items[i].load(std::memory_order_acquire); // ← LP if non-null
            if (val != nullptr)
            {
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << std::this_thread::get_id()
                              << ": Front completed in front method at time: "
                              << getCurrentTime() << "\n";
                return val; // caller MUST NOT delete
            }
        }

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Front completed (empty) in front method at time: "
                      << getCurrentTime() << "\n";
        return nullptr;
    }

    // ──────────────────────────────────────────────────────────────────────
    //  EXTRA CREDIT
    //  rear()  –  Peek at the rear element without removing it
    //
    //  KEY DESIGN DECISION: same as front() – uses load(), NOT exchange().
    //
    //  Correctness:
    //    (a) tail.load(acquire) gives the exclusive upper bound on assigned
    //        slots.  The rear element is at the highest-indexed non-null slot.
    //    (b) Scanning backwards from t-1 → 0 finds the highest-indexed
    //        non-null slot first, i.e. the most-recently enqueued item still
    //        present.
    //    (c) items[i].load(acquire) pairs with enq_i's store(val, release),
    //        guaranteeing a fully-written value is observed (no torn reads).
    //    (d) A concurrent enq() may increment tail from t to t+1 and store
    //        items[t] after our snapshot.  We do not see slot t; we return
    //        items[t-1], which was the rear at snapshot time.  Valid LP.
    //    (e) A concurrent deq() may have removed items[t-1] before our load.
    //        We observe nullptr there and continue to t-2.  Correct.
    //
    //  Caller MUST NOT delete the returned pointer.
    //  Returns nullptr if the queue appears empty.
    //
    //  LP: items[i].load(memory_order_acquire) returning non-null,
    //      for the LARGEST such i in [0, t)
    // ──────────────────────────────────────────────────────────────────────
    T* rear()
    {
        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Rear started in rear method at time: "
                      << getCurrentTime() << "\n";

        const int t = tail.load(std::memory_order_acquire); // snapshot

        // Walk backwards: highest-index non-null slot = most-recently enqueued
        for (int i = t - 1; i >= 0; --i)
        {
            // load() – non-destructive peek; do NOT use exchange() here
            T* val = items[i].load(std::memory_order_acquire); // ← LP if non-null
            if (val != nullptr)
            {
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << std::this_thread::get_id()
                              << ": Rear completed in rear method at time: "
                              << getCurrentTime() << "\n";
                return val; // caller MUST NOT delete
            }
        }

        if (tlLogFile.is_open())
            tlLogFile << "Thread " << std::this_thread::get_id()
                      << ": Rear completed (empty) in rear method at time: "
                      << getCurrentTime() << "\n";
        return nullptr;
    }
};

// =============================================================================
//  QTester globals
// =============================================================================

int    n       = 4;
int    numOps  = 20;
double rndLt   = 0.5;   // P(enq)
double rndLt2  = 0.6;   // P(enq) + P(front/rear); remainder → deq
double lambda  = 5.0;

std::ofstream          globLogFile;
std::mutex             globLogMtx;

// Per-thread timing / count accumulators
std::vector<long long> thrTimes;
std::vector<long long> enqTimes;
std::vector<long long> deqTimes;
std::vector<long long> frontTimes;  // EC
std::vector<long long> rearTimes;   // EC
std::vector<int>       enqCount;
std::vector<int>       deqCount;
std::vector<int>       frontCount;  // EC
std::vector<int>       rearCount;   // EC
std::vector<int>       deqNullCount;    // deq() returned nullptr (queue empty)
std::vector<int>       frontNullCount;  // EC: front() returned nullptr
std::vector<int>       rearNullCount;   // EC: rear()  returned nullptr

NLQ<int> qObj;

static void globLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(globLogMtx);
    if (globLogFile.is_open()) globLogFile << msg << "\n";
}

// =============================================================================
//  testThread  –  Extended QTester worker
//
//  Operation selection:
//    p < rndLt            → enq()
//    rndLt <= p < rndLt2  → front() or rear() with 50/50 probability
//    p >= rndLt2          → deq()
//
//  IMPORTANT for front() / rear():
//    The returned pointer belongs to the queue and MUST NOT be deleted.
//    We dereference it (read the value) immediately while it is valid.
//    A concurrent deq() may free the memory after we return from front()/rear(),
//    so we do not retain the pointer past the immediate use.
// =============================================================================
void testThread(int tid)
{
    const std::string tlName = "LogFile-NLQ-EC-" + std::to_string(tid) + ".log";
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
            qObj.enq(v);

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
            const bool doFront = (unifDist(rng) < 0.5);

            if (doFront)
            {
                // ── front() ───────────────────────────────────────────────
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": Front started in testThread at time: "
                              << getCurrentTime() << "\n";

                // front() returns a pointer INTO the queue; do NOT delete it.
                const int* val = qObj.front();
                if (val == nullptr) {
                    frontNullCount[tid]++;
                    if (tlLogFile.is_open())
                        tlLogFile << "Thread " << tid
                                  << ": Front returned null (empty) at time: "
                                  << getCurrentTime() << "\n";
                }
                // Use the value immediately before it could be dequeued
                (void)val;

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

                // rear() returns a pointer INTO the queue; do NOT delete it.
                const int* val = qObj.rear();
                if (val == nullptr) {
                    rearNullCount[tid]++;
                    if (tlLogFile.is_open())
                        tlLogFile << "Thread " << tid
                                  << ": Rear returned null (empty) at time: "
                                  << getCurrentTime() << "\n";
                }
                (void)val;

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

            int* val = qObj.deq();
            if (val == nullptr) {
                deqNullCount[tid]++;
                if (tlLogFile.is_open())
                    tlLogFile << "Thread " << tid
                              << ": DeQueue returned null (empty) at time: "
                              << getCurrentTime() << "\n";
            }
            delete val; // safe: delete nullptr is a no-op in C++

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
//  computeStats  →  NLQ-EC-avgs.txt
// =============================================================================
void computeStats()
{
    long long totalEnqTime   = 0, totalDeqTime   = 0;
    long long totalFrontTime = 0, totalRearTime  = 0;
    long long totalTime      = 0;
    int totalEnq = 0, totalDeq = 0, totalFront = 0, totalRear = 0;
    int totalDeqNull = 0, totalFNull = 0, totalRNull = 0;

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
        totalDeqNull   += deqNullCount[i];
        totalFNull     += frontNullCount[i];
        totalRNull     += rearNullCount[i];
    }

    const int    ops        = totalEnq + totalDeq + totalFront + totalRear;
    const double avgEnq     = totalEnq   > 0 ? (double)totalEnqTime   / totalEnq   : 0.0;
    const double avgDeq     = totalDeq   > 0 ? (double)totalDeqTime   / totalDeq   : 0.0;
    const double avgFront   = totalFront > 0 ? (double)totalFrontTime / totalFront : 0.0;
    const double avgRear    = totalRear  > 0 ? (double)totalRearTime  / totalRear  : 0.0;
    const double avgTotal   = ops        > 0 ? (double)totalTime      / ops        : 0.0;
    const long long wallUs  = *std::max_element(thrTimes.begin(), thrTimes.end());
    const double throughput = wallUs     > 0 ? (double)ops / (wallUs / 1e6)        : 0.0;

    std::ofstream avgs("NLQ-EC-avgs.txt");
    avgs << "========================================\n"
         << "  NLQ Extra-Credit Statistics\n"
         << "========================================\n"
         << "Threads              : " << n          << "\n"
         << "Ops / thread         : " << numOps     << "\n"
         << "P(enq)               : " << rndLt      << "\n"
         << "P(front/rear)        : " << (rndLt2 - rndLt) << "\n"
         << "P(deq)               : " << (1.0 - rndLt2)   << "\n"
         << "Lambda (ms)          : " << lambda     << "\n"
         << "----------------------------------------\n"
         << "Total enqs           : " << totalEnq   << "\n"
         << "Total deqs           : " << totalDeq   << "\n"
         << "  deq() → null       : " << totalDeqNull << "\n"
         << "Total fronts         : " << totalFront << "\n"
         << "  front() → null     : " << totalFNull  << "\n"
         << "Total rears          : " << totalRear  << "\n"
         << "  rear()  → null     : " << totalRNull  << "\n"
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

    std::cout << "[NLQ-EC] Enq: "   << avgEnq   << " us"
              << " | Deq: "         << avgDeq   << " us"
              << " | Front: "       << avgFront << " us"
              << " | Rear: "        << avgRear  << " us"
              << " | Throughput: "  << throughput << " ops/s\n";
}

// =============================================================================
//  mergeLogs  →  NLQ-EC-out.log  (all events sorted by timestamp)
// =============================================================================
void mergeLogs()
{
    std::vector<std::pair<std::string, std::string>> lines;

    for (int i = 0; i < n; ++i)
    {
        std::ifstream in("LogFile-NLQ-EC-" + std::to_string(i) + ".log");
        std::string   line;
        while (std::getline(in, line))
        {
            const auto pos = line.rfind("time: ");
            const std::string ts = (pos != std::string::npos)
                                   ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    globLogFile.close();
    {
        std::ifstream in("LogFile-NLQ-EC.log");
        std::string   line;
        while (std::getline(in, line))
        {
            const auto pos = line.rfind("time: ");
            const std::string ts = (pos != std::string::npos)
                                   ? line.substr(pos + 6) : "00:00:00.000";
            lines.emplace_back(ts, line);
        }
    }

    std::stable_sort(lines.begin(), lines.end(),
        [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ofstream out("NLQ-EC-out.log");
    for (const auto& [ts, ln] : lines)
        out << ln << "\n";
    out.close();

    std::cout << "[NLQ-EC] Merged log written to NLQ-EC-out.log\n";
}

// =============================================================================
//  main
// =============================================================================
int main()
{
    // Read parameters from inp-params.txt
    // Format: n  numOps  rndLt  lambda  [rndLt2]
    std::ifstream params("inp-params.txt");
    if (params.is_open())
    {
        params >> n >> numOps >> rndLt >> lambda;
        if (!(params >> rndLt2)) rndLt2 = rndLt + 0.1;
    }
    else
    {
        std::cerr << "[NLQ-EC] inp-params.txt not found – using defaults.\n";
    }

    // Clamp rndLt2 to valid range
    if (rndLt2 <= rndLt) rndLt2 = rndLt + 0.1;
    if (rndLt2 >  1.0  ) rndLt2 = 1.0;

    // Initialise per-thread stat vectors
    thrTimes      .assign(n, 0);
    enqTimes      .assign(n, 0);
    deqTimes      .assign(n, 0);
    frontTimes    .assign(n, 0);
    rearTimes     .assign(n, 0);
    enqCount      .assign(n, 0);
    deqCount      .assign(n, 0);
    frontCount    .assign(n, 0);
    rearCount     .assign(n, 0);
    deqNullCount  .assign(n, 0);
    frontNullCount.assign(n, 0);
    rearNullCount .assign(n, 0);

    // Open global log
    globLogFile.open("LogFile-NLQ-EC.log");

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

    return 0;
}
