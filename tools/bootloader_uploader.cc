#include "can.h"
#include "log.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <readline/readline.h>
#include <readline/history.h>

Can *can0;
std::mutex mtx;
std::condition_variable cv;
bool confirm_received = false;
bool confirm_success = false;
uint32_t received_crc = 0;
bool crc_received = false;
bool verbose_logging = true;

uint8_t node_id = 0x01;

void setVerboseLogging(bool enabled) {
    verbose_logging = enabled;
}

void setNodeId(uint8_t id) {
    node_id = id;
    LOG(NOTICE, "Node ID set to: 0x%02X", node_id);
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
        LOG(INFO, "Node: %d, Cmd: 0x%02X, DLC: %d", nodeId, cmd, rx_frame.can_dlc);
    }

    if(cmd == 0x12 && rx_frame.can_dlc >= 4) {
        received_crc = (rx_frame.data[0] << 24) | (rx_frame.data[1] << 16) | 
                      (rx_frame.data[2] << 8) | rx_frame.data[3];
        crc_received = true;
        if (verbose_logging) {
            LOG(NOTICE, "CRC received: 0x%08X", received_crc);
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
                LOG(NOTICE, "Operation confirmed");
            } else {
                LOG(ERROR, "Operation failed, status: 0x%02X", status);
            }
        }
        cv.notify_one();
        return;
    }

    if (verbose_logging) {
        LOG(NOTICE, "Response from node 0x%X, cmd=0x%X, DLC=%d", nodeId, cmd, rx_frame.can_dlc);
    }
}

bool waitConfirm(int timeout_ms = 10000)
{
    std::unique_lock<std::mutex> lock(mtx);
    if(cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return confirm_received; }))
    {
        confirm_received = false;
        bool success = confirm_success;
        confirm_success = false;
        return success;
    }
    LOG(ERROR, "Timeout waiting for confirmation!");
    return false;
}

bool waitCRC(int timeout_ms = 1000)
{
    std::unique_lock<std::mutex> lock(mtx);
    if(cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return crc_received; }))
    {
        crc_received = false;
        return true;
    }
    LOG(ERROR, "Timeout waiting for CRC!");
    return false;
}

uint32_t calculateFileCRC(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if(!file.is_open()) {
        LOG(ERROR, "Cannot open file for CRC calculation: %s", filename.c_str());
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

// 获取命令描述
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

bool sendCommand(uint8_t cmd, const std::vector<uint8_t> &data, bool verbose = true)
{
    if(cmd > 0x7F) {
        LOG(ERROR, "Command > 0x7F not allowed for 11-bit CAN ID");
        return false;
    }

    struct can_frame tx;
    tx.can_id = (node_id << 7) | cmd;
    tx.can_dlc = data.size();
    for(size_t i = 0; i < data.size(); i++)
        tx.data[i] = data[i];

    can0->transmit(&tx);
    
    if(verbose && verbose_logging) {
        std::string cmd_desc = getCommandDescription(cmd);
        if(data.empty()) {
            LOG(NOTICE, "Sent: %s to node 0x%02X", cmd_desc.c_str(), node_id);
        } else {
            if (cmd == 0x03) {
                LOG(NOTICE, "Sent: %s to node 0x%02X", cmd_desc.c_str(), node_id);
            } else {
                LOG(NOTICE, "Sent: %s to node 0x%02X, Data length: %zu", cmd_desc.c_str(), node_id, data.size());
            }
        }
    }

    if(cmd == 0x05)
        return waitCRC();

    return waitConfirm();
}

void printWelcome() {
    LOG(NOTICE, "==========================================");
    LOG(NOTICE, "         BootLoader Uploader v1.0");
    LOG(NOTICE, "==========================================");
    LOG(NOTICE, "Current Node ID: 0x%02X", node_id);
    LOG(NOTICE, "Available commands:");
    LOG(NOTICE, "  setid   - Set CAN node ID");
    LOG(NOTICE, "  erase   - Erase application flash");
    LOG(NOTICE, "  write   - Upload firmware file");
    LOG(NOTICE, "  crc     - Check application CRC");
    LOG(NOTICE, "  info    - Show device information");
    LOG(NOTICE, "  exit    - Quit application");
    LOG(NOTICE, "==========================================");
}

char** commandCompletion(const char* text, int start, int end) {
    rl_attempted_completion_over = 1;
    
    static std::vector<std::string> commands = {
        "setid", "erase", "write", "crc", "info", "exit", "help"
    };
    
    if (start != 0) {
        return nullptr;
    }
    
    std::vector<const char*> matches;
    for (const auto& cmd : commands) {
        if (cmd.find(text) == 0) {
            matches.push_back(strdup(cmd.c_str()));
        }
    }
    
    if (matches.empty()) {
        return nullptr;
    }
    
    matches.push_back(nullptr);
    
    char** result = (char**)malloc(matches.size() * sizeof(char*));
    for (size_t i = 0; i < matches.size() - 1; i++) {
        result[i] = (char*)matches[i];
    }
    result[matches.size() - 1] = nullptr;
    
    return result;
}

void initializeReadline() {
    rl_attempted_completion_function = commandCompletion;
    rl_bind_key('\t', rl_complete);
    read_history(".bootloader_history");
}

void showDeviceInfo() {
    LOG(NOTICE, "Device Information:");
    LOG(NOTICE, "  - Current Node ID: 0x%02X", node_id);
    LOG(NOTICE, "  - Application Start: 0x%08X", 0x08008000);
    LOG(NOTICE, "  - Application End: 0x%08X", 0x080C0000);
    LOG(NOTICE, "  - Flash Size: 1MB");
    LOG(NOTICE, "  - RAM Size: 256KB");
    
    LOG(NOTICE, "Querying device status...");
    if(sendCommand(0x05, {})) {
        LOG(NOTICE, "  - Application CRC: 0x%08X", received_crc);
        if(received_crc != 0xFFFFFFFF) {
            LOG(NOTICE, "  - Application: VALID");
        } else {
            LOG(NOTICE, "  - Application: INVALID or not programmed");
        }
    }
}

bool writeBinFile(const std::string &filename)
{
    std::ifstream fin(filename, std::ios::binary);
    if(!fin.is_open()) {
        LOG(ERROR, "File not found: %s", filename.c_str());
        return false;
    }

    fin.seekg(0, std::ios::end);
    size_t file_size = fin.tellg();
    fin.seekg(0, std::ios::beg);
    
    if(file_size == 0) {
        LOG(ERROR, "File is empty: %s", filename.c_str());
        return false;
    }

    LOG(NOTICE, "Firmware file: %s", filename.c_str());
    LOG(NOTICE, "File size: %zu bytes (%.2f KB)", file_size, file_size / 1024.0);

    char* confirm = readline("Proceed with firmware upload? (y/n): ");
    std::string confirm_str = confirm ? confirm : "";
    free(confirm);
    
    size_t cstart = confirm_str.find_first_not_of(" \t\n\r");
    size_t cend = confirm_str.find_last_not_of(" \t\n\r");
    if (cstart != std::string::npos && cend != std::string::npos) {
        confirm_str = confirm_str.substr(cstart, cend - cstart + 1);
    } else {
        confirm_str = "";
    }
    
    if(confirm_str != "y" && confirm_str != "Y") {
        LOG(NOTICE, "Upload cancelled");
        return false;
    }

    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();

    uint32_t local_crc = calculateFileCRC(filename);
    LOG(NOTICE, "Local file CRC: 0x%08X", local_crc);

    LOG(NOTICE, "Sending erase command...");
    if(!sendCommand(0x01, {})) {
        LOG(ERROR, "Erase failed!");
        return false;
    }

    LOG(NOTICE, "Sending start write command...");
    if(!sendCommand(0x02, {})) {
        LOG(ERROR, "Begin write failed!");
        return false;
    }

    LOG(NOTICE, "Writing data...");
    size_t idx = 0;
    size_t success_count = 0;
    size_t fail_count = 0;
    
    bool old_verbose = verbose_logging;
    setVerboseLogging(false);
    
    while(idx < buf.size())
    {
        std::vector<uint8_t> word(4, 0xFF);
        for(int i=0; i<4 && idx<buf.size(); i++)
            word[i] = buf[idx++];

        if(sendCommand(0x03, word, false)) {
            success_count++;
        } else {
            fail_count++;
            setVerboseLogging(old_verbose);
            LOG(ERROR, "Write word failed at idx=%zu", idx-4);
            LOG(NOTICE, "Successful writes: %zu, Failed writes: %zu", success_count, fail_count);
            return false;
        }

        if(idx % 1024 == 0 || idx == buf.size()) {
            int percent = (idx * 100) / buf.size();
            printf("\r[PROGRESS] %zu/%zu bytes (%d%%)", idx, buf.size(), percent);
            fflush(stdout);
        }
    }
    
    setVerboseLogging(old_verbose);
    printf("\n");

    LOG(NOTICE, "Download completed! Successful writes: %zu", success_count);

    LOG(NOTICE, "Sending end write command...");
    if(!sendCommand(0x04, {})) {
        LOG(ERROR, "End write failed!");
        return false;
    }

    LOG(NOTICE, "Write completed, verifying CRC...");
    
    if(sendCommand(0x05, {})) {
        LOG(NOTICE, "Device CRC: 0x%08X", received_crc);
        LOG(NOTICE, "Local CRC:  0x%08X", local_crc);
        
        if(received_crc == local_crc) {
            LOG(NOTICE, "CRC verification passed!");
            return true;
        } else {
            LOG(ERROR, "CRC verification failed!");
            return false;
        }
    } else {
        LOG(ERROR, "Failed to get device CRC");
        return false;
    }
}

int main()
{
    initLogger(INFO);
    printWelcome();
    initializeReadline();

    LOG(NOTICE, "Initializing CAN interface...");
    
    can0 = new Can((char*)"can0");
    if (can0->init()) {
        LOG(ERROR, "Failed to initialize CAN interface!");
        return -1;
    }
    can0->setOnCanReceiveDataCallback(rx_callback);
    can0->startAutoRead();
    
    LOG(NOTICE, "CAN interface ready");

    char* input;
    while((input = readline("bootloader> ")) != nullptr) {
        std::string cmd = input;
        
        size_t start = cmd.find_first_not_of(" \t\n\r");
        size_t end = cmd.find_last_not_of(" \t\n\r");
        if (start != std::string::npos && end != std::string::npos) {
            cmd = cmd.substr(start, end - start + 1);
        } else {
            cmd = "";
        }
        
        if (!cmd.empty()) {
            add_history(input);
        }
        free(input);

        if(cmd == "setid") {
            char* id_input = readline("Enter node ID (hex, e.g., 0x01): ");
            std::string id_str = id_input ? id_input : "";
            free(id_input);
            
            size_t istart = id_str.find_first_not_of(" \t\n\r");
            size_t iend = id_str.find_last_not_of(" \t\n\r");
            if (istart != std::string::npos && iend != std::string::npos) {
                id_str = id_str.substr(istart, iend - istart + 1);
            } else {
                id_str = "";
            }
            
            if (!id_str.empty()) {
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
                        LOG(ERROR, "Node ID must be between 0 and 0x1F");
                    }
                } catch (const std::exception& e) {
                    LOG(ERROR, "Invalid node ID format: %s", id_str.c_str());
                }
            }
        }
        else if(cmd == "erase") {
            LOG(NOTICE, "Erasing application flash...");
            if(sendCommand(0x01, {})) {
                LOG(NOTICE, "Erase completed successfully!");
            } else {
                LOG(ERROR, "Erase failed!");
            }
        }
        else if(cmd == "write") {
            char* filename_input = readline("Enter firmware file path: ");
            std::string filename = filename_input ? filename_input : "";
            free(filename_input);
            
            size_t fstart = filename.find_first_not_of(" \t\n\r");
            size_t fend = filename.find_last_not_of(" \t\n\r");
            if (fstart != std::string::npos && fend != std::string::npos) {
                filename = filename.substr(fstart, fend - fstart + 1);
            } else {
                filename = "";
            }
            
            if(writeBinFile(filename)) {
                LOG(NOTICE, "Firmware upload completed successfully!");
            } else {
                LOG(ERROR, "Firmware upload failed!");
            }
        }
        else if(cmd == "crc") {
            LOG(NOTICE, "Requesting application CRC...");
            if(sendCommand(0x05, {})) {
                LOG(NOTICE, "Application CRC: 0x%08X", received_crc);
            } else {
                LOG(ERROR, "Failed to get CRC!");
            }
        }
        else if(cmd == "info") {
            showDeviceInfo();
        }
        else if(cmd == "exit" || cmd == "quit") {
            break;
        }
        else if(cmd == "help") {
            printWelcome();
        }
        else if(!cmd.empty()) {
            LOG(ERROR, "Unknown command: %s", cmd.c_str());
            LOG(NOTICE, "Type 'help' for available commands");
        }
    }

    write_history(".bootloader_history");
    
    delete can0;
    LOG(NOTICE, "Goodbye!");
    return 0;
}
