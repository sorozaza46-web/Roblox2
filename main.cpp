#include <iostream>
#include <ranges>
#include <spdlog/spdlog.h>
#include "dumper_engine.h"

// --- STAGE: PLAYER ---
namespace dumper::stages::player {
    auto dump() -> bool {
        const auto players = dumper::g_data_model.find_first_child_of_class("Players");

        if (!players) {
            spdlog::error("Failed to find Players instance inside DataModel");
            return false;
        }

        const auto local_player = process::Rtti::find(players->get_address(), "Player@RBX");

        if (!local_player) {
            spdlog::error("Failed to find LocalPlayer in Players");
            return false;
        }

        dumper::g_dumper.add_offset("Players", "LocalPlayer", *local_player);

        const auto local_player_addr =
            process::Memory::read<uintptr_t>(players->get_address() + *local_player);

        const auto character = process::Rtti::find(local_player_addr, "ModelInstance@RBX");
        if (!character) {
            spdlog::error("Failed to find Character offset for Player");
            return false;
        }

        dumper::g_dumper.add_offset("Player", "Character", *character);

        const auto player_info = control::client::g_client.get_player_information();

        if (!player_info) {
            spdlog::error("Failed to receive player information via control server.");
            return false;
        }

        FIND_AND_ADD_OFFSET(local_player_addr, Player, uint64_t, UserId, player_info->user_id,
                            0x800, 0x8);

        const auto display_name = process::helpers::find_string_offset(
            local_player_addr, player_info->display_name, 0x400, 0x8, 0x256, true);

        if (!display_name) {
            spdlog::error("Failed to get DisplayName from Player");
            return false;
        }

        dumper::g_dumper.add_offset("Player", "DisplayName", *display_name);

        FIND_AND_ADD_OFFSET(local_player_addr, Player, uint32_t, AccountAge,
                            player_info->account_age, 0x600, 0x4);

        const auto locale_id = process::helpers::find_string_by_regex(
            local_player_addr, R"([a-z]{2}-[a-z]{2})", 0x800, 0x8, 32, true);

        if (!locale_id) {
            spdlog::error("Failed to get LocaleId from Player");
            return false;
        }

        dumper::g_dumper.add_offset("Player", "LocaleId", *locale_id);

        // really red = 1004 (https://create.roblox.com/docs/reference/engine/datatypes/BrickColor)
        FIND_AND_ADD_OFFSET(local_player_addr, Player, uint32_t, TeamColor, 1004, 0x800, 0x4);

        const auto team_ptr = process::helpers::find_pointer_offset(
            local_player_addr, dumper::g_team_addr, 0x400, 0x8);

        if (!team_ptr) {
            spdlog::error("Failed to get Team from Player");
            return false;
        }

        dumper::g_dumper.add_offset("Player", "Team", *team_ptr);

        FIND_AND_ADD_OFFSET(local_player_addr, Player, float, HealthDisplayDistance, 87.12f, 0x800,
                            0x4);
        FIND_AND_ADD_OFFSET(local_player_addr, Player, float, NameDisplayDistance, 56.89f, 0x800,
                            0x4);

        return true;
    }
}

// --- STAGE: WORKSPACE ---
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

        const auto world_addr =
            process::Memory::read<uintptr_t>(g_workspace->get_address() + world);

        if (!world_addr) {
            spdlog::error("Failed to read World offset in Workspace");
            return false;
        }

        FIND_AND_ADD_OFFSET(world_addr, World, float, WorldSteps, 240.0f, 0x1000, 0x4);

        std::optional<size_t> primitives_offset;

        for (size_t offset = 0; offset < 0x1000; offset += 0x8) {
            const auto array_ptr = process::Memory::read<uintptr_t>(world_addr + offset);
            if (!array_ptr)
                continue;

            const auto check_slot = [&](size_t slot) -> bool {
                const auto primitive_ptr = process::Memory::read<uintptr_t>(array_ptr + slot);
                if (!primitive_ptr)
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

// --- STAGE: HUMANOID ---
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
        if (!humanoids) {
            return false;
        }

        std::vector<uintptr_t> humanoid_addrs;
        for (const auto& h : *humanoids) {
            humanoid_addrs.push_back(h.address);
        }

        const auto health_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.health; }, 0x800, 0x4);
        if (!health_offset) {
            spdlog::error("Failed to find Health offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "Health", *health_offset);

        const auto max_health_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.max_health; }, 0x800, 0x4);
        if (!max_health_offset) {
            spdlog::error("Failed to find MaxHealth offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "MaxHealth", *max_health_offset);

        const auto walk_speed_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.walk_speed; }, 0x800, 0x4);
        if (!walk_speed_offset) {
            spdlog::error("Failed to find WalkSpeed offset");
            return false;
        }

        const auto walk_speed_check_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.walk_speed; }, 0x800, 0x4,
            {*walk_speed_offset});

        if (walk_speed_check_offset) {
            dumper::g_dumper.add_offset("Humanoid", "WalkSpeed", *walk_speed_offset);
            dumper::g_dumper.add_offset("Humanoid", "WalkSpeedCheck", *walk_speed_check_offset);
        }

        const auto jump_power_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.jump_power; }, 0x800, 0x4);
        if (!jump_power_offset) {
            spdlog::error("Failed to find JumpPower offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "JumpPower", *jump_power_offset);

        const auto jump_height_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.jump_height; }, 0x800,
            0x4);
        if (!jump_height_offset) {
            spdlog::error("Failed to find JumpHeight offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "JumpHeight", *jump_height_offset);

        const auto hip_height_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.hip_height; }, 0x800, 0x4);
        if (!hip_height_offset) {
            spdlog::error("Failed to find HipHeight offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "HipHeight", *hip_height_offset);

        const auto max_slope_angle_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.max_slope_angle; }, 0x800,
            0x4);
        if (!max_slope_angle_offset) {
            spdlog::error("Failed to find MaxSlopeAngle offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "MaxSlopeAngle", *max_slope_angle_offset);

        const auto health_display_distance_offset =
            process::helpers::find_offset_with_getter<float>(
                humanoid_addrs,
                [&](size_t i) { return (*humanoids)[i].props.health_display_distance; }, 0x800,
                0x4);
        if (!health_display_distance_offset) {
            spdlog::error("Failed to find HealthDisplayDistance offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "HealthDisplayDistance",
                                    *health_display_distance_offset);

        const auto name_display_distance_offset = process::helpers::find_offset_with_getter<float>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.name_display_distance; },
            0x800, 0x4);
        if (!name_display_distance_offset) {
            spdlog::error("Failed to find NameDisplayDistance offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "NameDisplayDistance",
                                    *name_display_distance_offset);

        const auto auto_jump_enabled_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs,
            [&](size_t i) { return (*humanoids)[i].props.auto_jump_enabled ? 1 : 0; }, 0x800, 0x1);
        if (!auto_jump_enabled_offset) {
            spdlog::error("Failed to find AutoJumpEnabled offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "AutoJumpEnabled", *auto_jump_enabled_offset);

        const auto automatic_scaling_enabled_offset =
            process::helpers::find_offset_with_getter<uint8_t>(
                humanoid_addrs,
                [&](size_t i) { return (*humanoids)[i].props.automatic_scaling_enabled ? 1 : 0; },
                0x800, 0x1);
        if (!automatic_scaling_enabled_offset) {
            spdlog::error("Failed to find AutomaticScalingEnabled offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "AutomaticScalingEnabled",
                                    *automatic_scaling_enabled_offset);

        const auto auto_rotate_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.auto_rotate ? 1 : 0; },
            0x800, 0x1);
        if (!auto_rotate_offset) {
            spdlog::error("Failed to find AutoRotate offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "AutoRotate", *auto_rotate_offset);

        const auto break_joints_on_death_offset =
            process::helpers::find_offset_with_getter<uint8_t>(
                humanoid_addrs,
                [&](size_t i) { return (*humanoids)[i].props.break_joints_on_death ? 1 : 0; },
                0x800, 0x1);
        if (!break_joints_on_death_offset) {
            spdlog::error("Failed to find BreakJointsOnDeath offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "BreakJointsOnDeath",
                                    *break_joints_on_death_offset);

        const auto evaluate_state_machine_offset =
            process::helpers::find_offset_with_getter<uint8_t>(
                humanoid_addrs,
                [&](size_t i) { return (*humanoids)[i].props.evaluate_state_machine ? 1 : 0; },
                0x800, 0x1);
        if (!evaluate_state_machine_offset) {
            spdlog::error("Failed to find EvaluateStateMachine offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "EvaluateStateMachine",
                                    *evaluate_state_machine_offset);

        const auto requires_neck_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.requires_neck ? 1 : 0; },
            0x800, 0x1);
        if (!requires_neck_offset) {
            spdlog::error("Failed to find RequiresNeck offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "RequiresNeck", *requires_neck_offset);

        const auto sit_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.sit ? 1 : 0; }, 0x800,
            0x1);
        if (!sit_offset) {
            spdlog::error("Failed to find Sit offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "Sit", *sit_offset);

        const auto use_jump_power_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.use_jump_power ? 1 : 0; },
            0x800, 0x1);
        if (!use_jump_power_offset) {
            spdlog::error("Failed to find UseJumpPower offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "UseJumpPower", *use_jump_power_offset);

        const auto camera_offset_offset = process::helpers::find_vec3_offset_multi<glm::vec3>(
            humanoid_addrs,
            [&](size_t i) {
                const auto& p = (*humanoids)[i].props;
                return glm::vec3(p.camera_offset_x, p.camera_offset_y, p.camera_offset_z);
            },
            0x800, 0.01f);
        if (!camera_offset_offset) {
            spdlog::error("Failed to find CameraOffset offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "CameraOffset", *camera_offset_offset);

        const auto target_point_offset = process::helpers::find_vec3_offset_multi<glm::vec3>(
            humanoid_addrs,
            [&](size_t i) {
                const auto& p = (*humanoids)[i].props;
                return glm::vec3(p.target_point_x, p.target_point_y, p.target_point_z);
            },
            0x800, 5.0f);
        if (!target_point_offset) {
            spdlog::error("Failed to find TargetPoint offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "TargetPoint", *target_point_offset);

        const auto walk_to_point_offset = process::helpers::find_vec3_offset_multi<glm::vec3>(
            humanoid_addrs,
            [&](size_t i) {
                const auto& p = (*humanoids)[i].props;
                return glm::vec3(p.walk_to_point_x, p.walk_to_point_y, p.walk_to_point_z);
            },
            0x800, 5.0f);
        if (!walk_to_point_offset) {
            spdlog::error("Failed to find WalkToPoint offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "WalkToPoint", *walk_to_point_offset);

        const auto rig_type_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.rig_type; }, 0x800, 0x1);
        if (!rig_type_offset) {
            spdlog::error("Failed to find RigType offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "RigType", *rig_type_offset);

        const auto display_distance_type_offset =
            process::helpers::find_offset_with_getter<uint8_t>(
                humanoid_addrs,
                [&](size_t i) { return (*humanoids)[i].props.display_distance_type; }, 0x800, 0x1);
        if (!display_distance_type_offset) {
            spdlog::error("Failed to find DisplayDistanceType offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "DisplayDistanceType",
                                    *display_distance_type_offset);

        const auto health_display_type_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.health_display_type; },
            0x800, 0x1);
        if (!health_display_type_offset) {
            spdlog::error("Failed to find HealthDisplayType offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "HealthDisplayType", *health_display_type_offset);

        const auto name_occlusion_offset = process::helpers::find_offset_with_getter<uint8_t>(
            humanoid_addrs, [&](size_t i) { return (*humanoids)[i].props.name_occlusion; }, 0x800,
            0x1);
        if (!name_occlusion_offset) {
            spdlog::error("Failed to find NameOcclusion offset");
            return false;
        }
        dumper::g_dumper.add_offset("Humanoid", "NameOcclusion", *name_occlusion_offset);

        const auto sitting_npc_hum = dumper::g_workspace->find_first_child("SittingNPC")
                                         ->find_first_child_of_class("Humanoid");

        if (!sitting_npc_hum) {
            spdlog::error("Failed to find SittingNPC.Humanoid in Workspace");
        }

        const auto seat_part = dumper::g_workspace->find_first_child("Seat");

        if (!seat_part) {
            spdlog::error("Failed to find Seat in Workspace");
        }

        const auto seat_part_offset = process::helpers::find_pointer_offset(
            sitting_npc_hum->get_address(), seat_part->get_address(), 0x1000, 0x8);

        if (!seat_part_offset) {
            spdlog::error("Failed to find SeatPart offset");
            return false;
        }

        g_dumper.add_offset("Humanoid", "SeatPart", *seat_part_offset);

        const auto seat_occupant = process::helpers::find_pointer_offset(
            seat_part->get_address(), sitting_npc_hum->get_address(), 0x1000, 0x8);

        if (!seat_occupant) {
            spdlog::error("Failed to find Seat Occupant offset (this is inside Humanoid stage)");
            return false;
        }

        g_dumper.add_offset("Seat", "Occupant", *seat_occupant);

        return true;
    }
}

// --- ANA PROSES GİRİŞİ ---
int main() {
    std::cout << "================================================" << std::endl;
    std::cout << "     ROBLOX REAL DUMPER SCANNING ENGINE" << std::endl;
    std::cout << "================================================" << std::endl;

    std::cout << "[*] Roblox bekleniyor..." << std::endl;
    if (!process::Memory::attach("RobloxPlayerBeta.exe")) {
        std::cerr << "[!] Hata: RobloxPlayerBeta.exe aktif bulunamadi!" << std::endl;
        return 1;
    }
    std::cout << "[+] Baglanti kuruldu. Tarama basliyor..." << std::endl;

    // Sırasıyla senin yazdığın aşamaları tetikliyoruz
    if (dumper::stages::player::dump()) {
        std::cout << "[SUCCESS] Player stage dumped!" << std::endl;
    }
    if (dumper::stages::workspace::dump()) {
        std::cout << "[SUCCESS] Workspace stage dumped!" << std::endl;
    }
    if (dumper::stages::humanoid::dump()) {
        std::cout << "[SUCCESS] Humanoid stage dumped!" << std::endl;
    }

    std::cout << "[*] Tum dumper islemleri tamamlandi." << std::endl;
    return 0;
}