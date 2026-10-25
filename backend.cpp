/**
 * ==============================================================================================
 * Project: Computer Laboratory Fault Tracking and Repair Scheduling System
 * Course: Data Structures and Algorithms (2nd Year Engineering Project)
 * Author: Senior Full-Stack Engineer & DSA Instructor
 * Architecture: REST API Web Service with Custom Singly Linked List Data Structures
 * File: backend.cpp
 * Standard: C++17
 * 
 * CORE DATA STRUCTURE SPECIFICATION:
 * - Hand-written Singly Linked List (Priority Queue via Ordered Insertion)
 * - Main Fault List: Ordered by Severity (1=Critical, 2=High, 3=Medium, 4=Low), then FIFO (timestamp)
 * - Archive Fault List: Singly Linked List storing resolved/repaired historical records
 * - No STL containers (std::list, std::vector, std::priority_queue) used for core fault storage!
 * ==============================================================================================
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wsock32.lib")
#endif

#include "httplib.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <cctype>
#include <algorithm>
#include <regex>

// ==============================================================================================
// 1. DATA STRUCTURE DEFINITIONS: SINGLY LINKED LIST NODES
// ==============================================================================================

/**
 * @struct FaultNode
 * @brief Represents a single computer laboratory fault record in the singly linked list.
 * 
 * Memory Layout:
 * [ faultID | computerID | labNumber | description | severity | registeredTime | status | next* ]
 *                                                                                            |
 *                                                                                            v
 *                                                                                   [ Next FaultNode ]
 */
struct FaultNode {
    int faultID;                 // Unique auto-incremented identifier (Primary Key)
    std::string computerID;      // e.g., "LAB1-PC04"
    std::string labNumber;       // e.g., "Lab 101", "Lab 2"
    std::string description;     // Brief fault synopsis (e.g., "GPU artifacting on boot")
    int severity;                // 1 = Critical, 2 = High, 3 = Medium, 4 = Low
    long long registeredTime;    // Epoch timestamp in seconds (for FIFO tie-break & aging calculations)
    std::string status;          // "Pending", "In-Progress", "Repaired"
    FaultNode* next;             // Pointer to subsequent node in the singly linked list

    // Constructor for clean heap allocation
    FaultNode(int id,
              const std::string& compId,
              const std::string& lab,
              const std::string& desc,
              int sev,
              long long regTime,
              const std::string& stat = "Pending")
        : faultID(id),
          computerID(compId),
          labNumber(lab),
          description(desc),
          severity(sev),
          registeredTime(regTime),
          status(stat),
          next(nullptr) {}
};

// ==============================================================================================
// 2. HELPER UTILITIES: STRINGS, TIME, JSON SERIALIZATION & PARSING (NO EXTERNAL JSON LIB)
// ==============================================================================================

namespace Utils {

    // Trims leading and trailing whitespace from string
    inline std::string trim(const std::string& s) {
        auto wsfront = std::find_if_not(s.begin(), s.end(), [](int c){ return std::isspace(c); });
        auto wsback = std::find_if_not(s.rbegin(), s.rend(), [](int c){ return std::isspace(c); }).base();
        return (wsback <= wsfront ? std::string() : std::string(wsfront, wsback));
    }

    // Convert string to uppercase for case-insensitive comparisons
    inline std::string toUpper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
        return s;
    }

    // Current epoch timestamp in seconds
    inline long long getCurrentTimeSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // Formats epoch timestamp to human-readable local time: "YYYY-MM-DD HH:MM:SS"
    inline std::string formatTimestamp(long long epochSeconds) {
        std::time_t t = static_cast<std::time_t>(epochSeconds);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    // Translates integer severity into descriptive textual label
    inline std::string severityToString(int severity) {
        switch (severity) {
            case 1: return "Critical";
            case 2: return "High";
            case 3: return "Medium";
            case 4: return "Low";
            default: return "Unknown";
        }
    }

    // Escapes special characters for safe standard JSON output
    inline std::string escapeJson(const std::string& input) {
        std::ostringstream ss;
        for (char c : input) {
            switch (c) {
                case '"':  ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b";  break;
                case '\f': ss << "\\f";  break;
                case '\n': ss << "\\n";  break;
                case '\r': ss << "\\r";  break;
                case '\t': ss << "\\t";  break;
                default:
                    if ('\x00' <= c && c <= '\x1f') {
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    } else {
                        ss << c;
                    }
            }
        }
        return ss.str();
    }

    /**
     * Hand-written JSON parser: Extracts string value for a given key from a JSON payload.
     * Searches for: "key" \s* : \s* "value"
     */
    inline std::string extractJsonString(const std::string& json, const std::string& key) {
        std::string pattern = "\"" + key + "\"";
        size_t keyPos = json.find(pattern);
        if (keyPos == std::string::npos) return "";

        size_t colonPos = json.find(':', keyPos + pattern.length());
        if (colonPos == std::string::npos) return "";

        size_t quoteStart = json.find('"', colonPos + 1);
        if (quoteStart == std::string::npos) return "";

        // Find closing unescaped quote
        size_t quoteEnd = quoteStart + 1;
        while (quoteEnd < json.length()) {
            if (json[quoteEnd] == '"' && json[quoteEnd - 1] != '\\') {
                break;
            }
            quoteEnd++;
        }
        if (quoteEnd >= json.length()) return "";

        std::string raw = json.substr(quoteStart + 1, quoteEnd - (quoteStart + 1));
        // Unescape standard characters
        std::string result;
        for (size_t i = 0; i < raw.length(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.length()) {
                char next = raw[i + 1];
                if (next == '"') { result += '"'; i++; }
                else if (next == '\\') { result += '\\'; i++; }
                else if (next == 'n') { result += '\n'; i++; }
                else if (next == 't') { result += '\t'; i++; }
                else if (next == 'r') { result += '\r'; i++; }
                else { result += raw[i]; }
            } else {
                result += raw[i];
            }
        }
        return trim(result);
    }

    /**
     * Hand-written JSON parser: Extracts integer value for a given key from a JSON payload.
     * Searches for: "key" \s* : \s* (-?\d+)
     */
    inline int extractJsonInt(const std::string& json, const std::string& key, int defaultVal = -1) {
        std::string pattern = "\"" + key + "\"";
        size_t keyPos = json.find(pattern);
        if (keyPos == std::string::npos) return defaultVal;

        size_t colonPos = json.find(':', keyPos + pattern.length());
        if (colonPos == std::string::npos) return defaultVal;

        size_t digitStart = colonPos + 1;
        while (digitStart < json.length() && (std::isspace(json[digitStart]) || json[digitStart] == '"')) {
            digitStart++;
        }
        if (digitStart >= json.length()) return defaultVal;

        size_t digitEnd = digitStart;
        if (json[digitEnd] == '-') digitEnd++;
        while (digitEnd < json.length() && std::isdigit(json[digitEnd])) {
            digitEnd++;
        }

        if (digitStart == digitEnd) return defaultVal;

        try {
            return std::stoi(json.substr(digitStart, digitEnd - digitStart));
        } catch (...) {
            return defaultVal;
        }
    }

    // Serializes a single FaultNode into a JSON object string
    inline std::string faultToJson(const FaultNode* node, long long now = 0) {
        if (!node) return "null";
        if (now == 0) now = getCurrentTimeSeconds();

        long long waitSeconds = std::max(0LL, now - node->registeredTime);
        long long waitMinutes = waitSeconds / 60;
        long long waitHours = waitSeconds / 3600;
        double waitDays = static_cast<double>(waitSeconds) / 86400.0;

        std::ostringstream ss;
        ss << "{"
           << "\"faultID\":" << node->faultID << ","
           << "\"computerID\":\"" << escapeJson(node->computerID) << "\","
           << "\"labNumber\":\"" << escapeJson(node->labNumber) << "\","
           << "\"description\":\"" << escapeJson(node->description) << "\","
           << "\"severity\":" << node->severity << ","
           << "\"severityName\":\"" << severityToString(node->severity) << "\","
           << "\"registeredTime\":" << node->registeredTime << ","
           << "\"registeredTimeString\":\"" << formatTimestamp(node->registeredTime) << "\","
           << "\"status\":\"" << escapeJson(node->status) << "\","
           << "\"waitingMinutes\":" << waitMinutes << ","
           << "\"waitingHours\":" << waitHours << ","
           << "\"waitingDays\":" << std::fixed << std::setprecision(1) << waitDays
           << "}";
        return ss.str();
    }
}

// ==============================================================================================
// 3. CORE DATA STRUCTURE: ACTIVE PRIORITY FAULT LIST (HAND-WRITTEN SINGLY LINKED LIST)
// ==============================================================================================

/**
 * @class PriorityFaultList
 * @brief Manages active laboratory faults in a singly linked list maintained in strict priority order.
 * 
 * ORDERING INVARIANT:
 * 1. Severity: 1 (Critical) > 2 (High) > 3 (Medium) > 4 (Low) -> (a->severity < b->severity)
 * 2. Tie-Breaker: FIFO by registration timestamp -> (a->registeredTime <= b->registeredTime)
 * 
 * DSA Operations Implemented:
 * - insertPriority(node): O(N) worst-case, O(1) best-case (head insertion). Preserves sorting invariant.
 * - findById(id): O(N) linear search traversal.
 * - detachById(id): O(N) pointer manipulation unlinking the node from list without deallocating.
 * - updateSeverity(id, newSev): O(N) detach + O(N) priority re-insertion.
 * - scheduleNext(): O(1) peek/update of head, or O(N) search for top Pending node.
 * - applyAging(daysThreshold): O(N * P) batch escalation of stale pending tickets.
 * - memory cleanup in destructor: O(N) safe traversal deletion.
 */
class PriorityFaultList {
private:
    FaultNode* head;  // Pointer to the highest-priority fault node in the queue
    int nodeCount;    // Tracks active list size

    /**
     * Priority Comparator Function:
     * Returns true if node 'a' must precede node 'b' in the priority queue.
     */
    static bool hasHigherPriority(const FaultNode* a, const FaultNode* b) {
        if (!a || !b) return false;
        // Higher severity has smaller integer value (1 is highest)
        if (a->severity != b->severity) {
            return a->severity < b->severity;
        }
        // If equal severity, older timestamp takes precedence (FIFO fairness)
        return a->registeredTime <= b->registeredTime;
    }

public:
    PriorityFaultList() : head(nullptr), nodeCount(0) {}

    // Destructor: ensures every dynamically allocated node is freed
    ~PriorityFaultList() {
        clear();
    }

    // Frees all linked nodes in O(N)
    void clear() {
        FaultNode* curr = head;
        while (curr != nullptr) {
            FaultNode* temp = curr;
            curr = curr->next;
            delete temp;
        }
        head = nullptr;
        nodeCount = 0;
    }

    // Accessor for list head (read-only traversal)
    const FaultNode* getHead() const { return head; }
    FaultNode* getHead() { return head; }

    int size() const { return nodeCount; }
    bool isEmpty() const { return head == nullptr; }

    /**
     * @brief Inserts a new node into the singly linked list maintaining priority order.
     * @param newNode Dynamically allocated FaultNode.
     * 
     * Pointer Mechanics:
     * Case 1: Empty list OR newNode has higher priority than current head:
     *         newNode->next = head;
     *         head = newNode;
     * Case 2: Intermediate or tail insertion:
     *         Traverse until (curr->next == nullptr OR newNode > curr->next)
     *         newNode->next = curr->next;
     *         curr->next = newNode;
     */
    void insertPriority(FaultNode* newNode) {
        if (!newNode) return;
        newNode->next = nullptr;

        // Case 1: Insert at Head
        if (head == nullptr || hasHigherPriority(newNode, head)) {
            newNode->next = head;
            head = newNode;
            nodeCount++;
            return;
        }

        // Case 2: Traverse to find correct insertion locus
        FaultNode* current = head;
        while (current->next != nullptr && !hasHigherPriority(newNode, current->next)) {
            current = current->next;
        }

        // Splice newNode between current and current->next
        newNode->next = current->next;
        current->next = newNode;
        nodeCount++;
    }

    /**
     * @brief Finds a node by its unique faultID.
     * @return Pointer to node if found, nullptr otherwise.
     */
    FaultNode* findById(int id) const {
        FaultNode* current = head;
        while (current != nullptr) {
            if (current->faultID == id) {
                return current;
            }
            current = current->next;
        }
        return nullptr;
    }

    /**
     * @brief Unlinks (detaches) a node by ID from the list without deleting memory.
     * Essential for:
     * 1. Changing severity (detach -> modify -> priority re-insert)
     * 2. Moving repaired fault to the Archive list.
     * @return Detached FaultNode pointer, or nullptr if not found.
     */
    FaultNode* detachById(int id) {
        if (head == nullptr) return nullptr;

        // Case 1: Detach Head
        if (head->faultID == id) {
            FaultNode* target = head;
            head = head->next;
            target->next = nullptr;
            nodeCount--;
            return target;
        }

        // Case 2: Detach Middle or Tail
        FaultNode* prev = head;
        while (prev->next != nullptr && prev->next->faultID != id) {
            prev = prev->next;
        }

        if (prev->next == nullptr) {
            return nullptr; // Node not found
        }

        FaultNode* target = prev->next;
        prev->next = target->next;
        target->next = nullptr;
        nodeCount--;
        return target;
    }

    /**
     * @brief Duplicate fault check: Checks whether computerID already has an active unresolved fault.
     * Prevents multiple open tickets for the same physical computer workstation.
     */
    bool hasActiveFaultForComputer(const std::string& compId) const {
        std::string target = Utils::toUpper(Utils::trim(compId));
        FaultNode* current = head;
        while (current != nullptr) {
            if (Utils::toUpper(current->computerID) == target && current->status != "Repaired") {
                return true;
            }
            current = current->next;
        }
        return false;
    }

    /**
     * @brief Updates severity of a fault and repositions it in the priority list.
     * Detaches node in O(N), updates severity, and re-inserts in O(N) to maintain invariant.
     */
    bool updateSeverity(int id, int newSeverity) {
        if (newSeverity < 1 || newSeverity > 4) return false;

        FaultNode* node = detachById(id);
        if (!node) return false;

        node->severity = newSeverity;
        insertPriority(node);
        return true;
    }

    /**
     * @brief Updates status of a node in place (Pending, In-Progress, Repaired).
     */
    bool updateStatus(int id, const std::string& newStatus) {
        FaultNode* node = findById(id);
        if (!node) return false;
        node->status = newStatus;
        return true;
    }

    /**
     * @brief Takes the highest-priority pending fault (or head node) and schedules it for repair.
     * Marks status as "In-Progress".
     * @return Pointer to the scheduled FaultNode, or nullptr if empty.
     */
    FaultNode* scheduleNext() {
        if (head == nullptr) return nullptr;

        // Find the first pending fault from the top of the priority queue
        FaultNode* current = head;
        while (current != nullptr) {
            if (current->status == "Pending") {
                current->status = "In-Progress";
                return current;
            }
            current = current->next;
        }

        // If all are already In-Progress or Repaired, pick the head node if not Repaired
        if (head->status != "Repaired") {
            head->status = "In-Progress";
            return head;
        }

        return nullptr;
    }

    /**
     * @brief Aging Mechanism:
     * Periodically or on-demand elevates the severity of pending tickets that have been
     * waiting longer than a specified threshold (e.g., 3 days).
     * Escalates: Low (4) -> Medium (3) -> High (2) -> Critical (1).
     * Since severity changes, nodes are detached and re-sorted.
     * @param secondsThreshold Age threshold in seconds (default: 3 days = 259,200 seconds).
     * @return Number of aged tickets escalated.
     */
    int applyAging(long long secondsThreshold) {
        long long now = Utils::getCurrentTimeSeconds();
        int agedCount = 0;

        // Collect IDs of nodes eligible for aging escalation
        // Note: We avoid modifying pointers while iterating, so we collect IDs first
        // using a temporary dynamically allocated array or simple scan.
        // For strict standard compliance without STL vector, we can count and allocate:
        int candidateCount = 0;
        FaultNode* curr = head;
        while (curr != nullptr) {
            if (curr->status == "Pending" && curr->severity > 1 && (now - curr->registeredTime) >= secondsThreshold) {
                candidateCount++;
            }
            curr = curr->next;
        }

        if (candidateCount == 0) return 0;

        int* eligibleIds = new int[candidateCount];
        int idx = 0;
        curr = head;
        while (curr != nullptr && idx < candidateCount) {
            if (curr->status == "Pending" && curr->severity > 1 && (now - curr->registeredTime) >= secondsThreshold) {
                eligibleIds[idx++] = curr->faultID;
            }
            curr = curr->next;
        }

        // Perform detach, severity elevation, and re-insertion for each candidate
        for (int i = 0; i < candidateCount; ++i) {
            FaultNode* node = detachById(eligibleIds[i]);
            if (node) {
                node->severity = node->severity - 1; // Elevate priority (e.g. 4 -> 3, 3 -> 2, 2 -> 1)
                insertPriority(node);
                agedCount++;
            }
        }

        delete[] eligibleIds;
        return agedCount;
    }

    // Serializes entire active linked list to JSON array
    std::string toJsonArray() const {
        std::ostringstream ss;
        ss << "[";
        long long now = Utils::getCurrentTimeSeconds();
        FaultNode* curr = head;
        bool first = true;
        while (curr != nullptr) {
            if (!first) ss << ",";
            ss << Utils::faultToJson(curr, now);
            first = false;
            curr = curr->next;
        }
        ss << "]";
        return ss.str();
    }
};

// ==============================================================================================
// 4. ARCHIVE LIST DATA STRUCTURE: SECOND SINGLY LINKED LIST FOR REPAIRED RECORDS
// ==============================================================================================

/**
 * @class RepairedArchiveList
 * @brief Secondary singly linked list dedicated to storing historical completed/repaired faults.
 * 
 * Separating active tickets from completed records keeps active scheduling list O(1) for head
 * operations and minimal memory overhead during active priority sweeps.
 */
class RepairedArchiveList {
private:
    FaultNode* head;
    FaultNode* tail;
    int count;

public:
    RepairedArchiveList() : head(nullptr), tail(nullptr), count(0) {}

    ~RepairedArchiveList() {
        clear();
    }

    void clear() {
        FaultNode* curr = head;
        while (curr != nullptr) {
            FaultNode* temp = curr;
            curr = curr->next;
            delete temp;
        }
        head = nullptr;
        tail = nullptr;
        count = 0;
    }

    const FaultNode* getHead() const { return head; }
    FaultNode* getHead() { return head; }
    int size() const { return count; }

    /**
     * Appends a newly repaired node to the archive list in O(1).
     */
    void append(FaultNode* node) {
        if (!node) return;
        node->next = nullptr;
        node->status = "Repaired";

        if (tail == nullptr) {
            head = tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
        count++;
    }

    FaultNode* findById(int id) const {
        FaultNode* curr = head;
        while (curr != nullptr) {
            if (curr->faultID == id) return curr;
            curr = curr->next;
        }
        return nullptr;
    }

    std::string toJsonArray() const {
        std::ostringstream ss;
        ss << "[";
        long long now = Utils::getCurrentTimeSeconds();
        FaultNode* curr = head;
        bool first = true;
        while (curr != nullptr) {
            if (!first) ss << ",";
            ss << Utils::faultToJson(curr, now);
            first = false;
            curr = curr->next;
        }
        ss << "]";
        return ss.str();
    }
};

// ==============================================================================================
// 5. APPLICATION STATE & PERSISTENCE MANAGER (faults.txt)
// ==============================================================================================

class FaultManager {
private:
    PriorityFaultList activeList;
    RepairedArchiveList archiveList;
    int nextFaultId;
    std::mutex mtx; // Thread safety for concurrent HTTP requests
    const std::string filename = "faults.txt";

public:
    FaultManager() : nextFaultId(1) {}

    // Lock guard helper
    std::mutex& getMutex() { return mtx; }
    PriorityFaultList& getActiveList() { return activeList; }
    RepairedArchiveList& getArchiveList() { return archiveList; }

    int getNextFaultId() {
        return nextFaultId++;
    }

    /**
     * Saves both active and archive linked lists to faults.txt
     * Line Format: TYPE|faultID|computerID|labNumber|description|severity|registeredTime|status
     */
    void saveToFile() {
        std::ofstream outFile(filename, std::ios::trunc);
        if (!outFile.is_open()) {
            std::cerr << "[ERROR] Could not open " << filename << " for saving data." << std::endl;
            return;
        }

        // Save Active Faults
        const FaultNode* curr = activeList.getHead();
        while (curr != nullptr) {
            outFile << "ACTIVE|"
                    << curr->faultID << "|"
                    << curr->computerID << "|"
                    << curr->labNumber << "|"
                    << curr->description << "|"
                    << curr->severity << "|"
                    << curr->registeredTime << "|"
                    << curr->status << "\n";
            curr = curr->next;
        }

        // Save Archived Faults
        curr = archiveList.getHead();
        while (curr != nullptr) {
            outFile << "ARCHIVE|"
                    << curr->faultID << "|"
                    << curr->computerID << "|"
                    << curr->labNumber << "|"
                    << curr->description << "|"
                    << curr->severity << "|"
                    << curr->registeredTime << "|"
                    << curr->status << "\n";
            curr = curr->next;
        }

        outFile.close();
    }

    /**
     * Loads saved faults from faults.txt into the active and archive linked lists.
     */
    void loadFromFile() {
        std::ifstream inFile(filename);
        if (!inFile.is_open()) {
            std::cout << "[INFO] No existing faults.txt found. Starting with clean state." << std::endl;
            return;
        }

        std::string line;
        int maxId = 0;
        int loadedActive = 0;
        int loadedArchive = 0;

        while (std::getline(inFile, line)) {
            line = Utils::trim(line);
            if (line.empty()) continue;

            // Split line by '|'
            std::stringstream ss(line);
            std::string segment;
            std::string fields[8];
            int fieldIdx = 0;

            while (std::getline(ss, segment, '|') && fieldIdx < 8) {
                fields[fieldIdx++] = segment;
            }

            if (fieldIdx < 8) continue; // Malformed line

            std::string type = fields[0];
            int id = std::stoi(fields[1]);
            std::string compId = fields[2];
            std::string labNum = fields[3];
            std::string desc = fields[4];
            int sev = std::stoi(fields[5]);
            long long regTime = std::stoll(fields[6]);
            std::string stat = fields[7];

            if (id > maxId) maxId = id;

            FaultNode* node = new FaultNode(id, compId, labNum, desc, sev, regTime, stat);

            if (type == "ACTIVE") {
                activeList.insertPriority(node);
                loadedActive++;
            } else if (type == "ARCHIVE") {
                archiveList.append(node);
                loadedArchive++;
            } else {
                delete node;
            }
        }

        inFile.close();
        nextFaultId = maxId + 1;
        std::cout << "[INFO] Loaded from faults.txt: " << loadedActive << " active, "
                  << loadedArchive << " archived. Next Fault ID: " << nextFaultId << std::endl;
    }

    /**
     * Seeds initial sample faults if storage is empty (great for project evaluation & viva demonstration)
     */
    void seedSampleDataIfEmpty() {
        if (!activeList.isEmpty() || archiveList.size() > 0) return;

        std::cout << "[INFO] Populating initial sample laboratory faults..." << std::endl;
        long long now = Utils::getCurrentTimeSeconds();

        struct SeedData {
            std::string comp;
            std::string lab;
            std::string desc;
            int sev;
            long long timeOffsetSeconds;
            std::string status;
            bool isArchive;
        };

        SeedData seeds[] = {
            {"LAB1-PC04", "Lab 1", "SMPS power unit burnt out, system cannot POST", 1, 3600 * 28, "Pending", false},
            {"LAB2-PC12", "Lab 2", "Blue screen of death (MEMORY_MANAGEMENT) during compile", 2, 3600 * 18, "In-Progress", false},
            {"LAB1-PC09", "Lab 1", "LAN NIC port flapping, disconnects every 2 minutes", 2, 3600 * 42, "Pending", false},
            {"LAB3-PC01", "Lab 3", "Display resolution locked to 800x600, missing driver", 3, 3600 * 5, "Pending", false},
            {"LAB2-PC15", "Lab 2", "Mechanical keyboard spacebar stuck / keys unresponsive", 4, 3600 * 80, "Pending", false},
            {"LAB4-PC08", "Lab 4", "System CPU fan rattling loudly, thermal throttling", 2, 3600 * 12, "Pending", false},
            {"LAB1-PC02", "Lab 1", "Master boot record corrupted following OS lab session", 1, 3600 * 70, "Pending", false},
            {"LAB3-PC20", "Lab 3", "Optical mouse laser sensor dead", 4, 3600 * 15, "Pending", false},
            {"LAB2-PC06", "Lab 2", "Secondary 1TB HDD clicking noise, S.M.A.R.T failure", 3, 3600 * 95, "Pending", false},
            {"LAB4-PC03", "Lab 4", "CMOS battery depleted, system date and time resets", 4, 3600 * 110, "Repaired", true}
        };

        for (const auto& item : seeds) {
            int id = getNextFaultId();
            FaultNode* node = new FaultNode(
                id, item.comp, item.lab, item.desc, item.sev, now - item.timeOffsetSeconds, item.status
            );
            if (item.isArchive) {
                archiveList.append(node);
            } else {
                activeList.insertPriority(node);
            }
        }

        saveToFile();
        std::cout << "[INFO] Initial sample dataset generated successfully." << std::endl;
    }
};

// Global singleton manager
FaultManager g_manager;

// ==============================================================================================
// 6. REPORT GENERATOR LOGIC (NO STL CONTAINERS USED FOR STORAGE)
// ==============================================================================================

namespace Reports {

    /**
     * Generates Pending Faults grouped by Lab as JSON.
     * Output format:
     * {
     *   "Lab 1": [ ... ],
     *   "Lab 2": [ ... ]
     * }
     */
    std::string generatePendingGroupedByLab(const PriorityFaultList& activeList) {
        long long now = Utils::getCurrentTimeSeconds();

        // 1. Collect distinct lab names using singly linked traversal
        // Since no STL containers, we maintain a small dynamic array of strings
        int maxLabs = 50;
        std::string* labNames = new std::string[maxLabs];
        int labCount = 0;

        const FaultNode* curr = activeList.getHead();
        while (curr != nullptr) {
            if (curr->status == "Pending") {
                bool found = false;
                for (int i = 0; i < labCount; ++i) {
                    if (labNames[i] == curr->labNumber) {
                        found = true;
                        break;
                    }
                }
                if (!found && labCount < maxLabs) {
                    labNames[labCount++] = curr->labNumber;
                }
            }
            curr = curr->next;
        }

        // Sort lab names alphabetically for consistent report presentation
        for (int i = 0; i < labCount - 1; ++i) {
            for (int j = 0; j < labCount - i - 1; ++j) {
                if (labNames[j] > labNames[j + 1]) {
                    std::swap(labNames[j], labNames[j + 1]);
                }
            }
        }

        std::ostringstream ss;
        ss << "{";
        for (int i = 0; i < labCount; ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << Utils::escapeJson(labNames[i]) << "\":[";
            bool firstNode = true;
            curr = activeList.getHead();
            while (curr != nullptr) {
                if (curr->status == "Pending" && curr->labNumber == labNames[i]) {
                    if (!firstNode) ss << ",";
                    ss << Utils::faultToJson(curr, now);
                    firstNode = false;
                }
                curr = curr->next;
            }
            ss << "]";
        }
        ss << "}";

        delete[] labNames;
        return ss.str();
    }

    /**
     * Generates Comprehensive Statistical Summary as JSON:
     * - Total faults, Pending, In-Progress, Repaired
     * - Critical / High / Medium / Low split
     * - Average waiting time in hours and minutes
     * - Faults per lab distribution
     */
    std::string generateSummary(const PriorityFaultList& activeList, const RepairedArchiveList& archiveList) {
        long long now = Utils::getCurrentTimeSeconds();

        int total = 0;
        int pending = 0;
        int inProgress = 0;
        int repaired = archiveList.size();

        int critical = 0;
        int high = 0;
        int medium = 0;
        int low = 0;

        long long totalWaitSeconds = 0;
        int pendingWaitCount = 0;

        // Dynamic array for lab distribution
        int maxLabs = 50;
        std::string* labNames = new std::string[maxLabs];
        int* labCounts = new int[maxLabs];
        int labCount = 0;

        auto recordLab = [&](const std::string& lab) {
            for (int i = 0; i < labCount; ++i) {
                if (labNames[i] == lab) {
                    labCounts[i]++;
                    return;
                }
            }
            if (labCount < maxLabs) {
                labNames[labCount] = lab;
                labCounts[labCount] = 1;
                labCount++;
            }
        };

        // Sweep Active List
        const FaultNode* curr = activeList.getHead();
        while (curr != nullptr) {
            total++;
            if (curr->status == "Pending") {
                pending++;
                totalWaitSeconds += std::max(0LL, now - curr->registeredTime);
                pendingWaitCount++;
            } else if (curr->status == "In-Progress") {
                inProgress++;
            } else if (curr->status == "Repaired") {
                repaired++;
            }

            if (curr->severity == 1) critical++;
            else if (curr->severity == 2) high++;
            else if (curr->severity == 3) medium++;
            else if (curr->severity == 4) low++;

            recordLab(curr->labNumber);
            curr = curr->next;
        }

        // Sweep Archive List
        curr = archiveList.getHead();
        while (curr != nullptr) {
            total++;
            recordLab(curr->labNumber);
            curr = curr->next;
        }

        double avgWaitHours = 0.0;
        long long avgWaitMinutes = 0;
        if (pendingWaitCount > 0) {
            avgWaitHours = (static_cast<double>(totalWaitSeconds) / pendingWaitCount) / 3600.0;
            avgWaitMinutes = (totalWaitSeconds / pendingWaitCount) / 60;
        }

        std::ostringstream ss;
        ss << "{"
           << "\"total\":" << total << ","
           << "\"pending\":" << pending << ","
           << "\"inProgress\":" << inProgress << ","
           << "\"repaired\":" << repaired << ","
           << "\"critical\":" << critical << ","
           << "\"high\":" << high << ","
           << "\"medium\":" << medium << ","
           << "\"low\":" << low << ","
           << "\"averageWaitHours\":" << std::fixed << std::setprecision(2) << avgWaitHours << ","
           << "\"averageWaitMinutes\":" << avgWaitMinutes << ","
           << "\"faultsPerLab\":{";

        for (int i = 0; i < labCount; ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << Utils::escapeJson(labNames[i]) << "\":" << labCounts[i];
        }

        ss << "}}";

        delete[] labNames;
        delete[] labCounts;
        return ss.str();
    }
}

// ==============================================================================================
// 7. HTTP REST SERVER CONTROLLER & ENDPOINT BINDINGS
// ==============================================================================================

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    std::cout << "==================================================================" << std::endl;
    std::cout << " Computer Laboratory Fault Tracking and Repair Scheduling System  " << std::endl;
    std::cout << " Data Structures & Algorithms Lab Project (Backend C++17)         " << std::endl;
    std::cout << "==================================================================" << std::endl;

    // 1. Initialize persistent storage and seeds
    g_manager.loadFromFile();
    g_manager.seedSampleDataIfEmpty();

    httplib::Server svr;

    // 2. Global CORS Headers & Options Preflight
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With"}
    });

    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // ------------------------------------------------------------------------------------------
    // STATIC ASSETS: Serve index.html
    // ------------------------------------------------------------------------------------------
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream file("index.html", std::ios::binary);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            res.set_content(ss.str(), "text/html; charset=utf-8");
        } else {
            res.status = 404;
            res.set_content("<h1>404 Not Found</h1><p>index.html was not found in the server root.</p>", "text/html");
        }
    });

    // ------------------------------------------------------------------------------------------
    // API 1: GET /api/faults - Retrieve all active faults in priority order
    // ------------------------------------------------------------------------------------------
    svr.Get("/api/faults", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_manager.getMutex());
        std::string json = g_manager.getActiveList().toJsonArray();
        res.set_content(json, "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 2: POST /api/faults - Register a new fault with priority insertion & duplicate check
    // Payload: { "computerID": "LAB1-PC04", "labNumber": "Lab 1", "description": "...", "severity": 1 }
    // ------------------------------------------------------------------------------------------
    svr.Post("/api/faults", [](const httplib::Request& req, httplib::Response& res) {
        std::string compId = Utils::extractJsonString(req.body, "computerID");
        std::string labNum = Utils::extractJsonString(req.body, "labNumber");
        std::string desc = Utils::extractJsonString(req.body, "description");
        int severity = Utils::extractJsonInt(req.body, "severity", -1);

        // Input Validation
        if (compId.empty() || labNum.empty() || desc.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"computerID, labNumber, and description are required fields.\"}", "application/json");
            return;
        }

        if (severity < 1 || severity > 4) {
            res.status = 400;
            res.set_content("{\"error\":\"severity must be an integer between 1 (Critical) and 4 (Low).\"}", "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        // Duplicate Fault Check: Reject if computer already has unresolved active ticket
        if (g_manager.getActiveList().hasActiveFaultForComputer(compId)) {
            res.status = 409; // HTTP 409 Conflict
            res.set_content("{\"error\":\"A pending or in-progress fault already exists for computer " +
                            Utils::escapeJson(compId) + ". Please resolve it before logging a new ticket.\"}",
                            "application/json");
            return;
        }

        int newId = g_manager.getNextFaultId();
        long long now = Utils::getCurrentTimeSeconds();

        FaultNode* newNode = new FaultNode(newId, compId, labNum, desc, severity, now, "Pending");
        g_manager.getActiveList().insertPriority(newNode);
        g_manager.saveToFile();

        res.status = 201; // HTTP 201 Created
        res.set_content(Utils::faultToJson(newNode, now), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 3: PUT /api/faults/:id/severity - Change severity and reposition node in linked list
    // Payload: { "severity": 2 }
    // ------------------------------------------------------------------------------------------
    svr.Put(R"(/api/faults/(\d+)/severity)", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        int newSeverity = Utils::extractJsonInt(req.body, "severity", -1);

        if (newSeverity < 1 || newSeverity > 4) {
            res.status = 400;
            res.set_content("{\"error\":\"Severity must be an integer between 1 (Critical) and 4 (Low).\"}", "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        bool updated = g_manager.getActiveList().updateSeverity(id, newSeverity);
        if (!updated) {
            res.status = 404;
            res.set_content("{\"error\":\"Active fault with ID " + std::to_string(id) + " not found.\"}", "application/json");
            return;
        }

        g_manager.saveToFile();
        FaultNode* node = g_manager.getActiveList().findById(id);
        res.set_content(Utils::faultToJson(node), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 4: PUT /api/faults/:id/status - Update ticket status (Pending, In-Progress, Repaired)
    // Payload: { "status": "In-Progress" }
    // ------------------------------------------------------------------------------------------
    svr.Put(R"(/api/faults/(\d+)/status)", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        std::string newStatus = Utils::extractJsonString(req.body, "status");

        if (newStatus != "Pending" && newStatus != "In-Progress" && newStatus != "Repaired") {
            res.status = 400;
            res.set_content("{\"error\":\"Status must be one of: 'Pending', 'In-Progress', 'Repaired'.\"}", "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        bool updated = g_manager.getActiveList().updateStatus(id, newStatus);
        if (!updated) {
            res.status = 404;
            res.set_content("{\"error\":\"Active fault with ID " + std::to_string(id) + " not found.\"}", "application/json");
            return;
        }

        g_manager.saveToFile();
        FaultNode* node = g_manager.getActiveList().findById(id);
        res.set_content(Utils::faultToJson(node), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 5: POST /api/schedule/next - Take highest-priority head node and schedule it (In-Progress)
    // ------------------------------------------------------------------------------------------
    svr.Post("/api/schedule/next", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        FaultNode* scheduled = g_manager.getActiveList().scheduleNext();
        if (!scheduled) {
            res.status = 404;
            res.set_content("{\"error\":\"No pending faults available in the priority queue to schedule.\"}", "application/json");
            return;
        }

        g_manager.saveToFile();
        res.set_content(Utils::faultToJson(scheduled), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 6: DELETE /api/faults/:id/repaired - Remove repaired fault from active list & archive it
    // ------------------------------------------------------------------------------------------
    svr.Delete(R"(/api/faults/(\d+)/repaired)", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());

        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        FaultNode* node = g_manager.getActiveList().detachById(id);
        if (!node) {
            res.status = 404;
            res.set_content("{\"error\":\"Fault ID " + std::to_string(id) + " not found in active list.\"}", "application/json");
            return;
        }

        // Append detached node to the archive linked list
        g_manager.getArchiveList().append(node);
        g_manager.saveToFile();

        std::string json = "{\"message\":\"Fault successfully resolved and moved to historical archive.\",\"archivedFault\":" +
                           Utils::faultToJson(node) + "}";
        res.set_content(json, "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 7: GET /api/reports/pending - Group pending faults by laboratory
    // ------------------------------------------------------------------------------------------
    svr.Get("/api/reports/pending", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_manager.getMutex());
        std::string json = Reports::generatePendingGroupedByLab(g_manager.getActiveList());
        res.set_content(json, "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 8: GET /api/reports/summary - Analytical statistical summary and distribution
    // ------------------------------------------------------------------------------------------
    svr.Get("/api/reports/summary", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_manager.getMutex());
        std::string json = Reports::generateSummary(g_manager.getActiveList(), g_manager.getArchiveList());
        res.set_content(json, "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 9: GET /api/search?computerID=&lab= - Search active and archived records
    // ------------------------------------------------------------------------------------------
    svr.Get("/api/search", [](const httplib::Request& req, httplib::Response& res) {
        std::string compFilter = req.has_param("computerID") ? req.get_param_value("computerID") : "";
        std::string labFilter = req.has_param("lab") ? req.get_param_value("lab") : "";

        compFilter = Utils::toUpper(Utils::trim(compFilter));
        labFilter = Utils::toUpper(Utils::trim(labFilter));

        std::lock_guard<std::mutex> lock(g_manager.getMutex());

        std::ostringstream ss;
        ss << "[";
        bool first = true;
        long long now = Utils::getCurrentTimeSeconds();

        // Search active list
        const FaultNode* curr = g_manager.getActiveList().getHead();
        while (curr != nullptr) {
            std::string c = Utils::toUpper(curr->computerID);
            std::string l = Utils::toUpper(curr->labNumber);

            bool matchComp = compFilter.empty() || (c.find(compFilter) != std::string::npos);
            bool matchLab = labFilter.empty() || (l.find(labFilter) != std::string::npos);

            if (matchComp && matchLab) {
                if (!first) ss << ",";
                ss << Utils::faultToJson(curr, now);
                first = false;
            }
            curr = curr->next;
        }

        // Search archive list
        curr = g_manager.getArchiveList().getHead();
        while (curr != nullptr) {
            std::string c = Utils::toUpper(curr->computerID);
            std::string l = Utils::toUpper(curr->labNumber);

            bool matchComp = compFilter.empty() || (c.find(compFilter) != std::string::npos);
            bool matchLab = labFilter.empty() || (l.find(labFilter) != std::string::npos);

            if (matchComp && matchLab) {
                if (!first) ss << ",";
                ss << Utils::faultToJson(curr, now);
                first = false;
            }
            curr = curr->next;
        }

        ss << "]";
        res.set_content(ss.str(), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 10: GET /api/archive - View all resolved archived records
    // ------------------------------------------------------------------------------------------
    svr.Get("/api/archive", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_manager.getMutex());
        res.set_content(g_manager.getArchiveList().toJsonArray(), "application/json");
    });

    // ------------------------------------------------------------------------------------------
    // API 11: POST /api/faults/aging - Trigger Aging Priority Escalation
    // Allows configurable days threshold (or simulated seconds threshold for evaluation)
    // Payload: { "days": 3 } or { "seconds": 3600 }
    // ------------------------------------------------------------------------------------------
    svr.Post("/api/faults/aging", [](const httplib::Request& req, httplib::Response& res) {
        int days = Utils::extractJsonInt(req.body, "days", 3);
        int secondsParam = Utils::extractJsonInt(req.body, "seconds", -1);

        long long thresholdSeconds = (secondsParam > 0) ? secondsParam : (days * 86400LL);

        std::lock_guard<std::mutex> lock(g_manager.getMutex());
        int escalated = g_manager.getActiveList().applyAging(thresholdSeconds);

        if (escalated > 0) {
            g_manager.saveToFile();
        }

        std::ostringstream ss;
        ss << "{"
           << "\"escalatedCount\":" << escalated << ","
           << "\"thresholdSeconds\":" << thresholdSeconds << ","
           << "\"message\":\"Aging policy executed: " << escalated << " pending fault(s) escalated in priority.\""
           << "}";
        res.set_content(ss.str(), "application/json");
    });

    std::cout << "[SERVER STARTED] HTTP listening at http://localhost:" << port << "/" << std::endl;
    std::cout << "Serving index.html dashboard and REST API endpoints." << std::endl;
    std::cout << "Press Ctrl+C to terminate." << std::endl;

    svr.listen("0.0.0.0", port);

    return 0;
}
