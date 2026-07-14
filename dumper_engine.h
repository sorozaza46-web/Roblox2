#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cmath>
#include <algorithm>
#include <regex>
#include <iostream>
#include <memory>
#include <glm/glm.hpp>

// --- BELLEK OKUMA VE YAZMA MOTORU ---
namespace process {
    class Memory {
    private:
        inline static HANDLE hProcess = nullptr;
        inline static DWORD processId = 0;

    public:
        static auto attach(const std::string& processName) -> bool {
            PROCESSENTRY32 entry;
            entry.dwSize = sizeof(PROCESSENTRY32);
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
            if (snapshot == INVALID_HANDLE_VALUE) return false;

            if (Process32First(snapshot, &entry)) {
                do {
                    if (processName == entry.szExeFile) {
                        processId = entry.th32ProcessID;
                        hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId);
                        CloseHandle(snapshot);
                        return hProcess != nullptr;
                    }
                } while (Process32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
            return false;
        }

        template<typename T>
        static auto read(uintptr_t address) -> T {
            if (!hProcess) return T{};
            T buffer{};
            SIZE_T bytesRead;
            ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &buffer, sizeof(T), &bytesRead);
            return buffer;
        }

        static auto read_string(uintptr_t address, size_t maxLength = 64) -> std::string {
            if (!hProcess) return "";
            std::string str;
            str.resize(maxLength);
            SIZE_T bytesRead;
            if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &str[0], maxLength, &bytesRead)) {
                size_t nullPos = str.find('\0');
                if (nullPos != std::string::npos) {
                    str.resize(nullPos);
                }
                return str;
            }
            return "";
        }
    };
}

// --- RTTI / SINIF ADI BULUCU ---
namespace process {
    class Rtti {
    public:
        static auto get_class_name(uintptr_t inst_address) -> std::string {
            if (!inst_address) return "";
            uintptr_t vtable = process::Memory::read<uintptr_t>(inst_address);
            if (!vtable) return "";
            uintptr_t col = process::Memory::read<uintptr_t>(vtable - sizeof(uintptr_t));
            if (!col) return "";
            uintptr_t type_desc = process::Memory::read<uintptr_t>(col + 0xC);
            if (!type_desc) return "";
            return process::Memory::read_string(type_desc + 0x10, 128);
        }

        static auto find(uintptr_t base_address, const std::string& target_class) -> std::optional<size_t> {
            for (size_t offset = 0; offset < 0x1000; offset += sizeof(uintptr_t)) {
                uintptr_t child = process::Memory::read<uintptr_t>(base_address + offset);
                if (!child) continue;
                if (get_class_name(child).find(target_class) != std::string::npos) {
                    return offset;
                }
            }
            return std::nullopt;
        }

        static auto get_all_names(uintptr_t address) -> std::vector<std::string> {
            std::vector<std::string> names;
            std::string name = get_class_name(address);
            if (!name.empty()) {
                names.push_back(name);
            }
            return names;
        }
    };
}

// --- DİNAMİK BELLEK TARAMA YARDIMCILARI ---
namespace process::helpers {
    template<typename T>
    inline auto find_offset_with_getter(
        const std::vector<uintptr_t>& addresses,
        std::function<T(size_t)> getter,
        size_t max_offset,
        size_t alignment,
        std::vector<size_t> exclude = {}
    ) -> std::optional<size_t> {
        for (size_t offset = 0; offset < max_offset; offset += alignment) {
            if (std::find(exclude.begin(), exclude.end(), offset) != exclude.end())
                continue;

            bool match = true;
            for (size_t i = 0; i < addresses.size(); ++i) {
                T expected_value = getter(i);
                T memory_value = process::Memory::read<T>(addresses[i] + offset);

                if constexpr (std::is_floating_point_v<T>) {
                    if (std::abs(memory_value - expected_value) > 0.01f) {
                        match = false;
                        break;
                    }
                } else {
                    if (memory_value != expected_value) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) return offset;
        }
        return std::nullopt;
    }

    template<typename T>
    inline auto find_vec3_offset_multi(
        const std::vector<uintptr_t>& addresses,
        std::function<T(size_t)> getter,
        size_t max_offset,
        float tolerance
    ) -> std::optional<size_t> {
        for (size_t offset = 0; offset < max_offset; offset += 0x4) {
            bool match = true;
            for (size_t i = 0; i < addresses.size(); ++i) {
                T expected = getter(i);
                T current = process::Memory::read<T>(addresses[i] + offset);

                if (std::abs(current.x - expected.x) > tolerance ||
                    std::abs(current.y - expected.y) > tolerance ||
                    std::abs(current.z - expected.z) > tolerance) {
                    match = false;
                    break;
                }
            }
            if (match) return offset;
        }
        return std::nullopt;
    }

    inline auto find_pointer_offset(uintptr_t base, uintptr_t target, size_t max, size_t align) -> std::optional<size_t> {
        for (size_t offset = 0; offset < max; offset += align) {
            if (process::Memory::read<uintptr_t>(base + offset) == target) {
                return offset;
            }
        }
        return std::nullopt;
    }

    template<typename T>
    inline auto find_offset_in_pointer(uintptr_t base, T target, size_t max1, size_t max2, size_t align1, size_t align2) 
        -> std::optional<std::pair<size_t, size_t>> {
        for (size_t offset1 = 0; offset1 < max1; offset1 += align1) {
            uintptr_t ptr = process::Memory::read<uintptr_t>(base + offset1);
            if (!ptr) continue;

            for (size_t offset2 = 0; offset2 < max2; offset2 += align2) {
                if (process::Memory::read<T>(ptr + offset2) == target) {
                    return std::make_pair(offset1, offset2);
                }
            }
        }
        return std::nullopt;
    }

    // String tarayıcı (Örn: DisplayName)
    inline auto find_string_offset(uintptr_t base, const std::string& target, size_t max, size_t align, size_t str_max, bool is_unicode) -> std::optional<size_t> {
        for (size_t offset = 0; offset < max; offset += align) {
            std::string current = process::Memory::read_string(base + offset, str_max);
            if (current == target) {
                return offset;
            }
        }
        return std::nullopt;
    }

    // Düzenli ifade (regex) ile string tarayıcı (Örn: LocaleId)
    inline auto find_string_by_regex(uintptr_t base, const std::string& pattern, size_t max, size_t align, size_t str_max, bool is_unicode) -> std::optional<size_t> {
        std::regex r(pattern);
        for (size_t offset = 0; offset < max; offset += align) {
            std::string current = process::Memory::read_string(base + offset, str_max);
            if (std::regex_match(current, r)) {
                return offset;
            }
        }
        return std::nullopt;
    }
}

// --- INSAN VE ORTAK YAPILAR ---
namespace dumper {
    class Instance {
    public:
        bool is_valid() const { return true; }
        uintptr_t get_address() const { return 0xDEADBEEF; }
        std::shared_ptr<Instance> find_first_child(const std::string& name) {
            return std::make_shared<Instance>();
        }
        std::shared_ptr<Instance> find_first_child_of_class(const std::string& className) {
            return std::make_shared<Instance>();
        }
    };

    class Dumper {
    public:
        void add_offset(const std::string& category, const std::string& name, size_t val) {
            std::cout << "[" << category << "] " << name << " found at offset: 0x" << std::hex << val << std::endl;
        }
    };

    inline std::shared_ptr<Instance> g_workspace = std::make_shared<Instance>();
    inline Instance g_data_model;
    inline uintptr_t g_team_addr = 0xBC840;
    inline Dumper g_dumper;
}

// --- SUNUCU ILE ILETISIM VERILERI ---
namespace control::client {
    struct PlayerInfo {
        uint64_t user_id = 12345678;
        std::string display_name = "PlayerOne";
        uint32_t account_age = 365;
    };

    struct HumanoidProperty {
        std::string name;
        float health = 100.0f;
        float max_health = 100.0f;
        float walk_speed = 16.0f;
        float jump_power = 50.0f;
        float jump_height = 7.2f;
        float hip_height = 2.0f;
        float max_slope_angle = 89.0f;
        float health_display_distance = 100.0f;
        float name_display_distance = 100.0f;
        bool auto_jump_enabled = true;
        bool automatic_scaling_enabled = true;
        bool auto_rotate = true;
        bool break_joints_on_death = true;
        bool evaluate_state_machine = true;
        bool requires_neck = true;
        bool sit = false;
        bool use_jump_power = true;
        float camera_offset_x = 0, camera_offset_y = 0, camera_offset_z = 0;
        float target_point_x = 0, target_point_y = 0, target_point_z = 0;
        float walk_to_point_x = 0, walk_to_point_y = 0, walk_to_point_z = 0;
        uint8_t rig_type = 0;
        uint8_t display_distance_type = 0;
        uint8_t health_display_type = 0;
        uint8_t name_occlusion = 0;
    };

    struct HumanoidPropertiesInfo {
        std::vector<HumanoidProperty> humanoids;
    };

    class Client {
    public:
        std::optional<PlayerInfo> get_player_information() {
            return PlayerInfo{};
        }
        std::optional<HumanoidPropertiesInfo> get_humanoid_properties() {
            HumanoidPropertiesInfo info;
            info.humanoids = {{"SittingNPC"}, {"EnemyNPC"}, {"Player"}};
            return info;
        }
    };

    inline Client g_client;
}

// --- MACRO TANIMLARI ---
#define FIND_AND_ADD_OFFSET(addr, stage, type, name, val, max, align) \
    { \
        auto res = process::helpers::find_offset_with_getter<type>({addr}, [&](size_t) { return val; }, max, align); \
        if (res) dumper::g_dumper.add_offset(#stage, #name, *res); \
}

