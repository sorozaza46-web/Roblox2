#include <iostream>
#include <spdlog/spdlog.h>
#include "dumper_engine.h"
#include "player.h"

// --- WORKSPACE STAGE (Eski workspace.cpp) ---
namespace dumper::stages::workspace {
    auto dump() -> bool {
        const auto camera = process::Rtti::find(dumper::g_workspace->get_address(), "Camera@RBX");
        if (!camera) {
            spdlog::error("Failed to find CurrentCamera offset in Workspace");
            return false;
        }

        dumper::g_dumper.add_offset("Workspace", "CurrentCamera", *camera);

        FIND_AND_ADD_OFFSET(dumper::g_workspace->get_address(), Workspace, float, ReadOnlyGravity,
                            196.2f, 0x1000, 0x4);

        const auto result = process::helpers::find_offset_in_pointer<float>(
            dumper::g_workspace->get_address(), 196.2f, 0x800, 0x400, 0x8, 0x4);

        if (!result) {
            spdlog::error("Failed to dump World and World Gravity in Workspace");
            return false;
        }

        const auto [world, gravity] = *result;
        g_dumper.add_offset("Workspace", "World", world);
        g_dumper.add_offset("World", "Gravity", gravity);

        const auto world_addr = process::Memory::read<uintptr_t>(g_workspace->get_address() + world);
        if (!world_addr) {
            spdlog::error("Failed to read World offset in Workspace");
            return false;
        }

        FIND_AND_ADD_OFFSET(world_addr, World, float, WorldSteps, 240.0f, 0x1000, 0x4);

        std::optional<size_t> primitives_offset;
        for (size_t offset = 0; offset < 0x1000; offset += 0x8) {
            const auto array_ptr = process::Memory::read<uintptr_t>(world_addr + offset);
            if (!array_ptr || *array_ptr == 0)
                continue;

            const auto check_slot = [&](size_t slot) -> bool {
                const auto primitive_ptr = process::Memory::read<uintptr_t>(array_ptr + slot);
                if (!primitive_ptr || *primitive_ptr == 0)
                    return false;

                const auto names = process::Rtti::get_all_names(primitive_ptr);
                return std::ranges::any_of(names, [](const auto& name) {
                    return name.find("Primitive@RBX") != std::string::npos;
                });
            };

            if (check_slot(0x0) && check_slot(0x8)) {
                primitives_offset = offset;
                break;
            }
        }

        if (!primitives_offset) {
            spdlog::error("Failed to find Primitives offset in World");
            return false;
        }

        g_dumper.add_offset("World", "Primitives", *primitives_offset);
        return true;
    }
}

// --- HUMANOID STAGE (Eski humanoid.cpp) ---
namespace dumper::stages::humanoid {
    struct HumanoidData {
        std::string name;
        uintptr_t address;
        control::client::HumanoidProperty props;
    };

    static auto get_humanoid_data(const control::client::HumanoidPropertiesInfo& props)
        -> std::optional<std::vector<HumanoidData>> {
        std::vector<HumanoidData> humanoid_data;

        auto characters_folder = dumper::g_workspace->find_first_child("Characters");
        if (!characters_folder->is_valid()) {
            spdlog::error("Failed to find Characters folder");
            return std::nullopt;
        }

        for (const auto& prop : props.humanoids) {
            auto character = characters_folder->find_first_child(prop.name);
            if (!character->is_valid()) {
                spdlog::error("Failed to find character: {}", prop.name);
                return std::nullopt;
            }

            auto humanoid = character->find_first_child("Humanoid");
            if (!humanoid->is_valid()) {
                spdlog::error("Failed to find Humanoid in {}", prop.name);
                return std::nullopt;
            }

            HumanoidData data{.name = prop.name, .address = humanoid->get_address(), .props = prop};
            humanoid_data.push_back(data);
        }

        return humanoid_data;
    }

    auto dump() -> bool {
        const auto humanoid_props = control::client::g_client.get_humanoid_properties();
        if (!humanoid_props) {
            spdlog::error("Failed to get humanoid properties from control server");
            return false;
        }

        if (humanoid_props->humanoids.size() < 3) {
            spdlog::error("Not enough humanoids found (need at least 3)");
            return false;
        }

        const auto humanoids = get_humanoid_data(*humanoid_props);
        if (!humanoids) return false;

        std::vector<uintptr_t> humanoid_addrs;
        for (const auto& h : *humanoids) {
            humanoid_addrs.push_back(h.address);
        }

        // Fiziksel Ofsetleri Buluyoruz
        const auto health_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.health; }, 0x800, 0x4);
        if (health_offset) dumper::g_dumper.add_offset("Humanoid", "Health", *health_offset);

        const auto max_health_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.max_health; }, 0x800, 0x4);
        if (max_health_offset) dumper::g_dumper.add_offset("Humanoid", "MaxHealth", *max_health_offset);

        const auto walk_speed_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.walk_speed; }, 0x800, 0x4);
        if (walk_speed_offset) dumper::g_dumper.add_offset("Humanoid", "WalkSpeed", *walk_speed_offset);

        return true;
    }
}

// --- ANA GİRİŞ NOKTASI ---
int main() {
    std::cout << "====================================================" << std::endl;
    std::cout << "     ROBLOX DUMPER - REAL-TIME SCANNING ENGINE" << std::endl;
    std::cout << "====================================================" << std::endl;

    std::cout << "[*] Roblox bekleniyor..." << std::endl;
    if (!process::Memory::attach("RobloxPlayerBeta.exe")) {
        std::cerr << "[!] RobloxPlayerBeta.exe bulunamadi!" << std::endl;
        return 1;
    }
    std::cout << "[+] Roblox basariyla yakalandi! Analiz baslatiliyor..." << std::endl;

    dumper::stages::player::dump();
    dumper::stages::workspace::dump();
    dumper::stages::humanoid::dump();

    std::cout << "[+] Islem tamamlandi!" << std::endl;
    return 0;
}

