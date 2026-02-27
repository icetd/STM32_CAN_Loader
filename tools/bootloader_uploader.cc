// tools/bootloader_uploader_ncurses.cc
#include "can.h"
#include "log.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <ncurses.h>
#include <string.h>
#include <cmath>
#include <sstream>
#include <iomanip>

// Color pair definitions
#define COLOR_PAIR_NORMAL     1
#define COLOR_PAIR_TITLE       2
#define COLOR_PAIR_MENU        3
#define COLOR_PAIR_SELECTED    4
#define COLOR_PAIR_STATUS      5
#define COLOR_PAIR_ERROR       6
#define COLOR_PAIR_SUCCESS     7
#define COLOR_PAIR_PROGRESS    8
#define COLOR_PAIR_INFO        9

// Window area definitions
#define WIN_MAIN_HEIGHT       20
#define WIN_STATUS_HEIGHT     4
#define WIN_INPUT_HEIGHT      3
#define WIN_PROGRESS_HEIGHT   3
#define WIN_LOG_HEIGHT        8

Can *can0;
std::mutex mtx;
std::condition_variable cv;
bool confirm_received = false;
bool confirm_success = false;
uint32_t received_crc = 0;
bool crc_received = false;
bool verbose_logging = true;

uint8_t node_id = 0x01;
std::string current_filename;
std::string status_message = "Ready";
std::string error_message = "";
std::vector<std::string> log_messages;
int progress_current = 0;
int progress_total = 100;
bool operation_running = false;

WINDOW *win_main, *win_status, *win_input, *win_progress, *win_log;
int selected_menu = 0;

// Function prototypes - NO DEFAULT PARAMETERS HERE
void init_ncurses();
void cleanup_ncurses();
void draw_title();
void draw_menu();
void draw_progress();
void draw_status();
void draw_log();
std::string get_user_input(const char* prompt);
void add_log_message(const std::string& msg);
void set_status(const std::string& msg);
void set_error(const std::string& msg);
void reset_progress();
void update_progress(int current, int total);
void show_device_info();
bool write_bin_file(const std::string &filename);
uint32_t calculateFileCRC(const std::string& filename);
bool sendCommand(uint8_t cmd, const std::vector<uint8_t> &data, bool show_status);
bool waitConfirm(int timeout_ms);
bool waitCRC(int timeout_ms);

void setVerboseLogging(bool enabled) {
    verbose_logging = enabled;
}

void setNodeId(uint8_t id) {
    node_id = id;
    std::stringstream ss;
    ss << "Node ID set to: 0x" << std::hex << std::setw(2) << std::setfill('0') << (int)node_id;
    set_status(ss.str());
    add_log_message(ss.str());
}

uint8_t getNodeId() {
    return node_id;
}

void rx_callback(struct can_frame rx_frame)
{
    std::lock_guard<std::mutex> lock(mtx);

    uint8_t nodeId = rx_frame.can_id >> 7;
    uint8_t cmd    = rx_frame.can_id & 0x7F;

    if (verbose_logging) {
        std::stringstream ss;
        ss << "RX: Node=" << (int)nodeId << ", Cmd=0x" << std::hex << (int)cmd 
           << ", DLC=" << std::dec << (int)rx_frame.can_dlc;
        add_log_message(ss.str());
    }

    if(cmd == 0x12 && rx_frame.can_dlc >= 4) {
        received_crc = (rx_frame.data[0] << 24) | (rx_frame.data[1] << 16) | 
                      (rx_frame.data[2] << 8) | rx_frame.data[3];
        crc_received = true;
        if (verbose_logging) {
            std::stringstream ss;
            ss << "CRC received: 0x" << std::hex << received_crc;
            add_log_message(ss.str());
        }
        cv.notify_one();
        return;
    }

    if(cmd == 0x11 && rx_frame.can_dlc >= 3) {
        uint8_t status = rx_frame.data[0];
        confirm_received = true;
        confirm_success = (status == 0xFF);
        
        if (verbose_logging) {
            if(confirm_success) {
                add_log_message("Operation confirmed");
            } else {
                std::stringstream ss;
                ss << "Operation failed, status: 0x" << std::hex << (int)status;
                add_log_message(ss.str());
            }
        }
        cv.notify_one();
        return;
    }

    if (verbose_logging) {
        std::stringstream ss;
        ss << "Response from node 0x" << std::hex << (int)nodeId 
           << ", cmd=0x" << (int)cmd << ", DLC=" << std::dec << (int)rx_frame.can_dlc;
        add_log_message(ss.str());
    }
}

// Implementation with default parameters HERE (only in definition)
bool waitConfirm(int timeout_ms)
{
    if(timeout_ms == 0) timeout_ms = 10000; // Default if 0 passed
    
    std::unique_lock<std::mutex> lock(mtx);
    if(cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return confirm_received; }))
    {
        confirm_received = false;
        bool success = confirm_success;
        confirm_success = false;
        return success;
    }
    add_log_message("Timeout waiting for confirmation!");
    return false;
}

bool waitCRC(int timeout_ms)
{
    if(timeout_ms == 0) timeout_ms = 1000; // Default if 0 passed
    
    std::unique_lock<std::mutex> lock(mtx);
    if(cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return crc_received; }))
    {
        crc_received = false;
        return true;
    }
    add_log_message("Timeout waiting for CRC!");
    return false;
}

uint32_t calculateFileCRC(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if(!file.is_open()) {
        std::stringstream ss;
        ss << "Cannot open file for CRC calculation: " << filename;
        add_log_message(ss.str());
        return 0;
    }
    
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    uint32_t crc = 0xFFFFFFFF;
    for(uint8_t byte : data) {
        crc ^= byte;
        for(int i = 0; i < 8; i++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

std::string getCommandDescription(uint8_t cmd) {
    switch(cmd) {
        case 0x01: return "Erase flash";
        case 0x02: return "Start write";
        case 0x03: return "Write data";
        case 0x04: return "End write";
        case 0x05: return "Request CRC";
        default: return "Unknown command";
    }
}

bool sendCommand(uint8_t cmd, const std::vector<uint8_t> &data, bool show_status)
{
    if(cmd > 0x7F) {
        add_log_message("Command > 0x7F not allowed for 11-bit CAN ID");
        return false;
    }

    struct can_frame tx;
    tx.can_id = (node_id << 7) | cmd;
    tx.can_dlc = data.size();
    for(size_t i = 0; i < data.size(); i++)
        tx.data[i] = data[i];

    can0->transmit(&tx);
    
    if(show_status && verbose_logging) {
        std::string cmd_desc = getCommandDescription(cmd);
        std::stringstream ss;
        if(data.empty()) {
            ss << "Sent: " << cmd_desc << " to node 0x" << std::hex << (int)node_id;
        } else {
            if (cmd == 0x03) {
                ss << "Sent: " << cmd_desc << " to node 0x" << std::hex << (int)node_id;
            } else {
                ss << "Sent: " << cmd_desc << " to node 0x" << std::hex << (int)node_id 
                   << ", Data length: " << std::dec << data.size();
            }
        }
        add_log_message(ss.str());
    }

    if(cmd == 0x05)
        return waitCRC(1000); // Pass explicit timeout

    return waitConfirm(10000); // Pass explicit timeout
}

void init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    
    if(has_colors()) {
        start_color();
        init_pair(COLOR_PAIR_NORMAL, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_PAIR_TITLE, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_PAIR_MENU, COLOR_YELLOW, COLOR_BLACK);
        init_pair(COLOR_PAIR_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_PAIR_STATUS, COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_PAIR_ERROR, COLOR_RED, COLOR_BLACK);
        init_pair(COLOR_PAIR_SUCCESS, COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_PAIR_PROGRESS, COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_PAIR_INFO, COLOR_MAGENTA, COLOR_BLACK);
    }
    
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    // Create windows
    win_main = newwin(WIN_MAIN_HEIGHT, max_x, 0, 0);
    win_progress = newwin(WIN_PROGRESS_HEIGHT, max_x, WIN_MAIN_HEIGHT, 0);
    win_status = newwin(WIN_STATUS_HEIGHT, max_x, WIN_MAIN_HEIGHT + WIN_PROGRESS_HEIGHT, 0);
    win_log = newwin(WIN_LOG_HEIGHT, max_x, WIN_MAIN_HEIGHT + WIN_PROGRESS_HEIGHT + WIN_STATUS_HEIGHT, 0);
    win_input = newwin(WIN_INPUT_HEIGHT, max_x, WIN_MAIN_HEIGHT + WIN_PROGRESS_HEIGHT + WIN_STATUS_HEIGHT + WIN_LOG_HEIGHT, 0);
    
    // Enable keypad for all windows
    keypad(win_main, TRUE);
    keypad(win_progress, TRUE);
    keypad(win_status, TRUE);
    keypad(win_log, TRUE);
    keypad(win_input, TRUE);
}

void cleanup_ncurses() {
    delwin(win_main);
    delwin(win_progress);
    delwin(win_status);
    delwin(win_log);
    delwin(win_input);
    endwin();
}

void draw_title() {
    wattron(win_main, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
    mvwprintw(win_main, 0, 2, "+--------------------------------------------------------------+");
    mvwprintw(win_main, 1, 2, "|              CAN BootLoader Uploader v2.0                     |");
    mvwprintw(win_main, 2, 2, "+--------------------------------------------------------------+");
    wattroff(win_main, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
}

void draw_menu() {
    const char* menu_items[] = {
        "Erase Flash",
        "Upload Firmware",
        "Verify CRC",
        "Set Node ID",
        "Device Info",
        "Exit Program"
    };

    int start_y = 4;
    for(int i = 0; i < 6; i++) {
        if(i == selected_menu) {
            wattron(win_main, COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
            mvwprintw(win_main, start_y + i * 2, 4, ">> %-20s <<", menu_items[i]);
            wattroff(win_main, COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
        } else {
            wattron(win_main, COLOR_PAIR(COLOR_PAIR_MENU));
            mvwprintw(win_main, start_y + i * 2, 4, "   %-20s   ", menu_items[i]);
            wattroff(win_main, COLOR_PAIR(COLOR_PAIR_MENU));
        }
    }

    // 安全地显示文件名
    std::string display_filename;
    if(current_filename.empty()) {
        display_filename = "<none>";
    } else {
        // 只取文件名部分，不包含路径
        size_t pos = current_filename.find_last_of("/\\");
        if(pos != std::string::npos) {
            display_filename = current_filename.substr(pos + 1);
        } else {
            display_filename = current_filename;
        }

        // 限制长度并过滤非打印字符
        std::string filtered;
        for(char c : display_filename) {
            if(isprint(c) && c < 128) {  // 只保留可打印的 ASCII 字符
                filtered += c;
            }
        }
        display_filename = filtered;

        if(display_filename.length() > 25) {
            display_filename = display_filename.substr(0, 22) + "...";
        }
    }

    // Show current configuration
    wattron(win_main, COLOR_PAIR(COLOR_PAIR_INFO));
    mvwprintw(win_main, 4, 35, "+------------------------+");
    mvwprintw(win_main, 5, 35, "| Current Configuration |");
    mvwprintw(win_main, 6, 35, "+------------------------+");
    mvwprintw(win_main, 7, 35, "Node ID: 0x%02X", node_id);
    mvwprintw(win_main, 8, 35, "File: %-25s", display_filename.c_str());

    mvwprintw(win_main, 10, 35, "+-------------------+");
    mvwprintw(win_main, 11, 35, "| Controls          |");
    mvwprintw(win_main, 12, 35, "+-------------------+");
    mvwprintw(win_main, 13, 35, "UP/DOWN : Select");
    mvwprintw(win_main, 14, 35, "ENTER   : Execute");
    mvwprintw(win_main, 15, 35, "Q       : Quit");
    wattroff(win_main, COLOR_PAIR(COLOR_PAIR_INFO));

    wrefresh(win_main);
}

void draw_progress() {
    werase(win_progress);
    box(win_progress, 0, 0);
    
    if(progress_total > 0) {
        int percent = (progress_current * 100) / progress_total;
        int bar_width = getmaxx(win_progress) - 20;
        int filled = (percent * bar_width) / 100;
        
        mvwprintw(win_progress, 1, 2, "Progress: [");
        wattron(win_progress, COLOR_PAIR(COLOR_PAIR_PROGRESS) | A_BOLD);
        for(int i = 0; i < filled; i++) {
            waddch(win_progress, '=');
        }
        for(int i = filled; i < bar_width; i++) {
            waddch(win_progress, ' ');
        }
        wattroff(win_progress, COLOR_PAIR(COLOR_PAIR_PROGRESS) | A_BOLD);
        mvwprintw(win_progress, 1, bar_width + 5, "] %3d%%", percent);
    }
    
    wrefresh(win_progress);
}

void draw_status() {
    werase(win_status);
    box(win_status, 0, 0);
    
    if(error_message.empty()) {
        wattron(win_status, COLOR_PAIR(COLOR_PAIR_STATUS));
        mvwprintw(win_status, 1, 2, "Status: %s", status_message.c_str());
        wattroff(win_status, COLOR_PAIR(COLOR_PAIR_STATUS));
    } else {
        wattron(win_status, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
        mvwprintw(win_status, 1, 2, "Error: %s", error_message.c_str());
        wattroff(win_status, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
    }
    
    wrefresh(win_status);
}

void draw_log() {
    werase(win_log);
    box(win_log, 0, 0);
    
    int max_y, max_x;
    getmaxyx(win_log, max_y, max_x);
    max_y -= 2;  // Account for border
    
    int start_idx = 0;
    if(log_messages.size() > max_y) {
        start_idx = log_messages.size() - max_y;
    }
    
    for(int i = 0; i < max_y && (start_idx + i) < log_messages.size(); i++) {
        std::string msg = log_messages[start_idx + i];
        if(msg.length() > max_x - 4) {
            msg = msg.substr(0, max_x - 7) + "...";
        }
        mvwprintw(win_log, i + 1, 2, "%s", msg.c_str());
    }
    
    wrefresh(win_log);
}

std::string get_user_input(const char* prompt) {
    werase(win_input);
    box(win_input, 0, 0);
    mvwprintw(win_input, 1, 2, "%s", prompt);
    wrefresh(win_input);
    
    curs_set(1);
    echo();
    
    char input[256];
    wgetnstr(win_input, input, sizeof(input) - 1);
    
    curs_set(0);
    noecho();
    
    // Clear input window
    werase(win_input);
    wrefresh(win_input);
    
    return std::string(input);
}

void add_log_message(const std::string& msg) {
    log_messages.push_back(msg);
    if(log_messages.size() > 100) {
        log_messages.erase(log_messages.begin());
    }
    draw_log();
}

void set_status(const std::string& msg) {
    status_message = msg;
    error_message = "";
    draw_status();
}

void set_error(const std::string& msg) {
    error_message = msg;
    draw_status();
    add_log_message("ERROR: " + msg);
}

void reset_progress() {
    progress_current = 0;
    progress_total = 100;
    draw_progress();
}

void update_progress(int current, int total) {
    progress_current = current;
    progress_total = total;
    draw_progress();
}

void show_device_info() {
    werase(win_main);
    draw_title();
    
    wattron(win_main, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
    mvwprintw(win_main, 4, 4, "Device Information");
    wattroff(win_main, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
    
    wattron(win_main, COLOR_PAIR(COLOR_PAIR_NORMAL));
    mvwprintw(win_main, 6, 4, "Current Node ID: 0x%02X", node_id);
    mvwprintw(win_main, 7, 4, "Application Start: 0x08008000");
    mvwprintw(win_main, 8, 4, "Application End: 0x080C0000");
    mvwprintw(win_main, 9, 4, "Flash Size: 1MB");
    mvwprintw(win_main, 10, 4, "RAM Size: 256KB");
    wattroff(win_main, COLOR_PAIR(COLOR_PAIR_NORMAL));
    
    mvwprintw(win_main, 12, 4, "Querying device status...");
    wrefresh(win_main);
    
    if(sendCommand(0x05, {}, true)) {
        std::stringstream ss;
        ss << "Application CRC: 0x" << std::hex << received_crc;
        mvwprintw(win_main, 13, 4, "%s", ss.str().c_str());
        
        if(received_crc != 0xFFFFFFFF) {
            mvwprintw(win_main, 14, 4, "Application: VALID");
        } else {
            mvwprintw(win_main, 14, 4, "Application: INVALID or not programmed");
        }
    }
    
    mvwprintw(win_main, 17, 4, "Press any key to return to main menu...");
    wrefresh(win_main);
    
    wgetch(win_main);
}

bool write_bin_file(const std::string &filename)
{
    std::ifstream fin(filename, std::ios::binary);
    if(!fin.is_open()) {
        set_error("Cannot open file: " + filename);
        return false;
    }

    fin.seekg(0, std::ios::end);
    size_t file_size = fin.tellg();
    fin.seekg(0, std::ios::beg);
    
    if(file_size == 0) {
        set_error("File is empty: " + filename);
        return false;
    }

    std::string confirm = get_user_input("Proceed with firmware upload? (y/n): ");
    if(confirm != "y" && confirm != "Y") {
        set_status("Upload cancelled");
        return false;
    }

    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();

    uint32_t local_crc = calculateFileCRC(filename);
    std::stringstream ss;
    ss << "Local file CRC: 0x" << std::hex << local_crc;
    add_log_message(ss.str());

    set_status("Erasing flash...");
    if(!sendCommand(0x01, {}, true)) {
        set_error("Erase failed!");
        return false;
    }

    set_status("Starting write...");
    if(!sendCommand(0x02, {}, true)) {
        set_error("Write start failed!");
        return false;
    }

    set_status("Writing data...");
    size_t idx = 0;
    size_t success_count = 0;
    
    bool old_verbose = verbose_logging;
    setVerboseLogging(false);
    
    update_progress(0, file_size);
    
    while(idx < buf.size())
    {
        std::vector<uint8_t> word(4, 0xFF);
        for(int i=0; i<4 && idx<buf.size(); i++)
            word[i] = buf[idx++];

        if(sendCommand(0x03, word, false)) {
            success_count++;
        } else {
            setVerboseLogging(old_verbose);
            std::stringstream ss;
            ss << "Write failed at offset: 0x" << std::hex << (idx-4);
            set_error(ss.str());
            return false;
        }

        update_progress(idx, file_size);
    }
    
    setVerboseLogging(old_verbose);

    set_status("Finishing write...");
    if(!sendCommand(0x04, {}, true)) {
        set_error("Write finish failed!");
        return false;
    }

    set_status("Verifying CRC...");
    
    if(sendCommand(0x05, {}, true)) {
        std::stringstream ss;
        ss << "Device CRC: 0x" << std::hex << received_crc;
        add_log_message(ss.str());
        ss.str("");
        ss << "Local CRC:  0x" << std::hex << local_crc;
        add_log_message(ss.str());
        
        if(received_crc == local_crc) {
            set_status("CRC verification passed!");
            return true;
        } else {
            set_error("CRC verification failed!");
            return false;
        }
    } else {
        set_error("Failed to get device CRC");
        return false;
    }
}

int main()
{
    initLogger(INFO);
    
    // Initialize CAN
    can0 = new Can((char*)"can0");
    if (can0->init()) {
        std::cerr << "Failed to initialize CAN interface!" << std::endl;
        return -1;
    }
    can0->setOnCanReceiveDataCallback(rx_callback);
    can0->startAutoRead();

    // Initialize ncurses
    init_ncurses();
    
    add_log_message("CAN BootLoader Uploader v2.0");
    add_log_message("CAN interface initialized");
    set_status("Ready - Use arrow keys to navigate");
    
    bool running = true;
    int ch;
    
    while(running) {
        // Draw main interface
        werase(win_main);
        draw_title();
        draw_menu();
        
        // Handle input
        ch = wgetch(win_main);
        
        switch(ch) {
            case KEY_UP:
            case 'k':
            case 'K':
                selected_menu = (selected_menu - 1 + 6) % 6;
                break;
                
            case KEY_DOWN:
            case 'j':
            case 'J':
                selected_menu = (selected_menu + 1) % 6;
                break;
                
            case KEY_LEFT:
            case 'h':
            case 'H':
                // Can be used for additional functionality
                add_log_message("Left arrow pressed");
                break;
                
            case KEY_RIGHT:
            case 'l':
            case 'L':
                // Can be used for additional functionality
                add_log_message("Right arrow pressed");
                break;
                
            case 10:  // Line feed
            case 13:  // Carriage return
            case KEY_ENTER:
                operation_running = true;
                reset_progress();
                
                switch(selected_menu) {
                    case 0: {  // Erase flash
                        set_status("Erasing flash...");
                        if(sendCommand(0x01, {}, true)) {
                            set_status("Flash erase successful!");
                        } else {
                            set_error("Flash erase failed!");
                        }
                        break;
                    }
                    
                    case 1: {  // Upload firmware
                        std::string filename = get_user_input("Enter firmware file path: ");
                        if(!filename.empty()) {
                            current_filename = filename;
                            if(write_bin_file(filename)) {
                                set_status("Firmware upload completed successfully!");
                            }
                        }
                        break;
                    }
                    
                    case 2: {  // Verify CRC
                        set_status("Requesting CRC...");
                        if(sendCommand(0x05, {}, true)) {
                            std::stringstream ss;
                            ss << "Application CRC: 0x" << std::hex << received_crc;
                            set_status(ss.str());
                        } else {
                            set_error("Failed to get CRC!");
                        }
                        break;
                    }
                    
                    case 3: {  // Set node ID
                        std::string id_str = get_user_input("Enter node ID (hex, e.g., 0x01): ");
                        if(!id_str.empty()) {
                            try {
                                uint8_t new_id;
                                if (id_str.find("0x") == 0 || id_str.find("0X") == 0) {
                                    new_id = std::stoul(id_str, nullptr, 16);
                                } else {
                                    new_id = std::stoul(id_str, nullptr, 10);
                                }
                                
                                if (new_id <= 0x1F) {
                                    setNodeId(new_id);
                                } else {
                                    set_error("Node ID must be between 0 and 0x1F");
                                }
                            } catch (const std::exception& e) {
                                set_error("Invalid node ID format");
                            }
                        }
                        break;
                    }
                    
                    case 4: {  // Device info
                        show_device_info();
                        break;
                    }
                    
                    case 5: {  // Exit program
                        running = false;
                        break;
                    }
                }
                
                operation_running = false;
                reset_progress();
                break;
                
            case 'q':
            case 'Q':
                running = false;
                break;
                
            default:
                // Ignore other keys
                break;
        }
    }

    add_log_message("Shutting down...");
    cleanup_ncurses();
    delete can0;
    
    return 0;
}
