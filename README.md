# Lab Fault Tracker

Computer Laboratory Fault Tracking and Repair Scheduling System, with a web dashboard and a C++ backend.

## About
Manages faults across multiple computer labs and schedules repairs by severity and waiting time. The backend stores faults in a **singly linked list with priority-based insertion**, so the most critical, longest-waiting fault is always at the head.

## Features
- Register a fault using computer ID and lab number
- Assign and change severity (1 = Critical to 4 = Low)
- Schedule the next repair (critical faults first)
- Update repair status (Pending, In-Progress, Repaired)
- Remove repaired records
- Pending-fault and repair summary reports
- Search by computer ID or lab number

## Tech Stack
- **Backend:** C++ (singly linked list, HTTP server)
- **Frontend:** HTML, CSS, JavaScript

## Data Structure
| Operation | Time complexity |
|---|---|
| Priority insertion | O(n) |
| Schedule next (remove head) | O(1) |
| Search by ID | O(n) |
| Delete by ID | O(n) |

## How to Run
1. Clone the repo:
   `git clone https://github.com/Piyushcodes03/lab-fault-tracker.git`
2. Compile the backend:
   - Windows: `g++ backend.cpp -o backend -lws2_32 -lwsock32`
   - Linux/Mac: `g++ backend.cpp -o backend -pthread`
3. Run it: `./backend`
4. Open `http://localhost:8080` in your browser.

## Project Structure
