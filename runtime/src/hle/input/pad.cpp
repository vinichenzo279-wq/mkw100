#include "hle/auto_driver.h"
#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "runtime_log.h" // R26_PAD_TRACE
#include "motion_trick.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace {

std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

extern "C" bool PAD_HLE_RumbleEnabled()
{
    return g_rumbleEnabled.load(std::memory_order_relaxed);
}

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

extern "C" uint32_t PAD__Init_HLE()
{
    return PADInit() ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF2F0, PAD__Init_HLE, uint32_t, (), ());

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    uint32_t rumbleMask = PADRead(statuses);

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
    MotionTrick::Apply(statuses);
    AutoDriver::FillPad(statuses[0]);  // --race-auto only
    // R26_PAD_TRACE: host-to-guest PAD sample. Do not synthesize inputs.
    // Bounded to the first 3000 reads or 24 changes to avoid per-frame logs.
    if (const char* diag = std::getenv("MKW100_R26_INPUT_DIAG"); diag && std::strcmp(diag, "1") == 0) {
        static uint32_t calls = 0, transitions = 0;
        static uint16_t prevButtons = 0xFFFFu;
        static int prevError = 999;
        const uint32_t current = ++calls;
        const PADStatus& s = statuses[0];
        const bool changed = s.button != prevButtons || s.err != prevError;
        if (current <= 3000 && (current == 1 || current % 120 == 0 || (changed && transitions < 24))) {
            const int portIndex = PADGetIndexForPort(0);
            SDL_Gamepad* device = portIndex < 0 ? nullptr : PADGetSDLGamepadForIndex(static_cast<u32>(portIndex));
            RT_LOGF("r26-input", "PADRead n=%u count=%u port0=%d sdl=%d err=%d buttons=0x%04X stick=(%d,%d) rawSouth=%d blocked=%d\n",
                    current, PADCount(), portIndex, device ? 1 : 0, int(s.err), unsigned(s.button),
                    int(s.stickX), int(s.stickY),
                    device ? int(SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_SOUTH)) : -1,
                    InputBindings::InputBlocked() ? 1 : 0);
        }
        if (changed) { ++transitions; prevButtons = s.button; prevError = s.err; }
    }

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
PPC_NATIVE_OVERRIDE(801AF44C, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF0DC, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF1E4, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
