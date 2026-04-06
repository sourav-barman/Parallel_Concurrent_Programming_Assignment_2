# Parallel_Concurrent_Programming_Assignment_2
=========================================================
  PA2 – Queue Implementation  |  readme.txt
=========================================================

FILES SUBMITTED
---------------
CLQ.cpp          – Coarse-Grained Locking Queue + QTester
NLQ.cpp          – Non-Locking (Herlihy-Wing) Queue + QTester
readme.txt       – This file
Report-PA2.docx  – Assignment report with design + graphs

---------------------------------------------------------
PREREQUISITES
---------------------------------------------------------
  • g++ version 9 or later (C++17 support required)
  • A POSIX-compatible OS (Linux / macOS) or Windows with
    MinGW-w64 / MSVC 2019+
  • pthreads (usually included with g++ on Linux/macOS)

---------------------------------------------------------
INPUT FILE  (required before running)
---------------------------------------------------------
Create a plain-text file named  inp-params.txt  in the
same directory as the compiled binary.  Format:

    <n>  <numOps>  <rndLt>  <lambda>

Example:
    15  30  0.5  5.0

Parameters:
  n       – number of threads
  numOps  – operations executed by each thread
  rndLt   – probability [0,1] of enqueue vs dequeue
  lambda  – mean sleep time (ms) between operations

---------------------------------------------------------
COMPILATION
---------------------------------------------------------
On Linux / macOS:

    g++ -std=c++17 -O2 -pthread CLQ.cpp -o clq
    g++ -std=c++17 -O2 -pthread NLQ.cpp -o nlq

On Windows (MinGW):

    g++ -std=c++17 -O2 CLQ-CS26MTECH02003.cpp -o clq.exe
    g++ -std=c++17 -O2 NLQ-CS26MTECH02003.cpp -o nlq.exe

---------------------------------------------------------
EXECUTION
---------------------------------------------------------
Make sure  inp-params.txt  is present, then:

    ./clq          
    ./nlq         

---------------------------------------------------------
OUTPUT FILES GENERATED
---------------------------------------------------------
CLQ run:
  CLQ-avgs.txt   – avg enq/deq/total times + throughput
  CLQ-out.log    – merged, time-sorted event log

NLQ run:
  NLQ-avgs.txt   – avg enq/deq/total times + throughput
  NLQ-out.log    – merged, time-sorted event log

Per-thread log files (LogFile-<tid>.log) are also
generated and then merged into the combined log.

---------------------------------------------------------
RUNNING EXPERIMENTS (for the report graphs)
---------------------------------------------------------
Graph 1 – Impact of numOps on throughput
    Fix: n=16, rndLt=0.5, lambda=5
    Vary numOps: 10 20 30 40 50 60
    Run each setting 5 times, average throughput.

Graph 2 – Impact of n (threads) on throughput
    Fix: numOps=30, rndLt=0.5, lambda=5
    Vary n: 2 4 8 16 32
    Run each setting 5 times, average throughput.

A helper bash script to automate this:

    for ops in 10 20 30 40 50 60; do
        echo "16 $ops 0.5 5.0" > inp-params.txt
        for run in 1 2 3 4 5; do
            ./clq >> clq_ops_results.txt
            ./nlq >> nlq_ops_results.txt
        done
    done

---------------------------------------------------------
EXTRA CREDIT
---------------------------------------------------------
Both CLQ and NLQ implement front() and rear() methods.
See the report for linearization point analysis.
=========================================================
---------------------------------------------------------
COMPILATION
---------------------------------------------------------
On Linux / macOS:

    g++ -std=c++17 -O2 -pthread CLQ-EC.cpp -o clq_ec
    g++ -std=c++17 -O2 -pthread NLQ-EC.cpp -o nlq_ec

On Windows (MinGW):

    g++ -std=c++17 -O2 CLQ-EC.cpp -o clq_ec.exe
    g++ -std=c++17 -O2 NLQ-EC.cpp -o nlq_ec.exe

---------------------------------------------------------
EXECUTION
---------------------------------------------------------
Make sure  inp-params.txt  is present, then:

    ./clq_ec         
    ./nlq_ec         
