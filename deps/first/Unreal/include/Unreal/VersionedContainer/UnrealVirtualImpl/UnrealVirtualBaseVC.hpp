#pragma once

#include <bit>
#include <cstddef>

#include <ASMHelper/ASMHelper.hpp>
#include <Zydis/Zydis.h>

#define PARAMS(...) __VA_ARGS__
#define ARGS(...)  __VA_ARGS__

// VTableLayoutMap is generated from MSVC builds, where a polymorphic class emits a single
// "vector deleting destructor" slot. The Itanium ABI that GCC uses on Linux emits two in its
// place (D1 complete-object and D0 deleting), so every entry after the destructor sits exactly
// one pointer later than the tables say. Nothing else about the ordering differs.
//
// Verified three independent ways against TheIsleServer-Linux-Shipping (UE 5.6) by dumping the
// binary's exported _ZTV* vtables:
//   1. UObject: MSVC table spans slots 0-85 (86), the ELF vtable has 87. Diffing _ZTV7UObject
//      against _ZTV6AActor lists 32 overridden slots which, shifted by one, name exactly the
//      UObject virtuals AActor really overrides (GetWorld, ProcessEvent, CallRemoteFunction,
//      GetLifetimeReplicatedProps, ...).
//   2. UEngine: diffing _ZTV7UEngine against _ZTV11UGameEngine yields overrides at shifted
//      slots Init, Start, PreExit, Tick, GetMaxTickRate, UpdateRunningAverageDeltaTime.
//   3. FMalloc: __cxa_pure_virtual sits at slots 6/8/10 with implementations at 7/9, matching
//      the declared Malloc(pure), TryMalloc, Realloc(pure), TryRealloc, Free(pure) run at MSVC
//      slots 5-9. This one is why the engine allocator misbehaved: an unshifted Realloc call
//      landed on TryMalloc.
//
// An offset of 0 is left alone: it is both the destructor slot and the "not found" sentinel
// that OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL tests for.
#ifdef __linux__
#define UE4SS_VTABLE_ABI_SHIFT(offset_expr) [](auto RawOffset) { return RawOffset ? RawOffset + sizeof(void*) : RawOffset; }(offset_expr)
#else
#define UE4SS_VTABLE_ABI_SHIFT(offset_expr) (offset_expr)
#endif

// GCC doesn't support std::bit_cast with member function pointers (not trivially copyable)
// Use a union-based cast instead.
//
// A pointer-to-member-function under the Itanium ABI is not a plain pointer: it is a 16-byte
// { function address, this-adjustment } pair. Assigning through a bare `void* in` member only
// writes the first 8 bytes and leaves the adjustment as whatever was on the stack, so the
// subsequent `(this->*func)(args)` shifts `this` by a garbage amount and dereferences it.
// That is fatal for every virtual UE4SS calls this way — it is what crashed the engine
// allocator (FMallocBinned2::Malloc) during UObjectArray listener registration.
// Write both halves explicitly.
#ifndef _MSC_VER
template <typename MemberFuncPtr>
static MemberFuncPtr bit_cast_mfp(void* ptr) {
    struct ItaniumMemberFuncPtr
    {
        void* FunctionAddress;
        std::ptrdiff_t ThisAdjustment;
    };
    static_assert(sizeof(MemberFuncPtr) == sizeof(ItaniumMemberFuncPtr),
                  "Unexpected pointer-to-member-function representation; the { address, adjustment } assumption no longer holds.");
    union { ItaniumMemberFuncPtr in; MemberFuncPtr out; } u;
    // Low bit clear marks a non-virtual target, which is correct: the caller already resolved
    // the concrete address out of the vtable.
    u.in.FunctionAddress = ptr;
    u.in.ThisAdjustment = 0;
    return u.out;
}
#define BIT_CAST_MFP(MemberFuncPtr, expr) bit_cast_mfp<MemberFuncPtr>(expr)
#else
#define BIT_CAST_MFP(MemberFuncPtr, expr) std::bit_cast<MemberFuncPtr>(expr)
#endif

// Helper to resolve function address from potential JMP instruction
#define RESOLVE_JMP(ptr) ASM::resolve_function_address_from_potential_jmp(ptr)

#define IMPLEMENT_UNREAL_VIRTUAL_WRAPPER_NO_PARAMS(class_name, function_name, return_type) \
static const auto offset = []() { \
    auto it = VTableLayoutMap.find(STR(#function_name)); \
    if (it == VTableLayoutMap.end())                                                         \
    {                                                                                              \
        throw std::runtime_error{"Virtual " #class_name "::" #function_name " is unavailable, possibly unsupported in engine version"}; \
    }                                                                                           \
    return UE4SS_VTABLE_ABI_SHIFT(it->second); \
}(); \
std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(this)); \
auto func = BIT_CAST_MFP(return_type(class_name::*)() const, *std::bit_cast<void**>(vtable + offset));\
if (!func) \
{ \
throw std::runtime_error{"Function '" #function_name "' not available"}; \
}                                                                          \
return (this->*func)();

#define IMPLEMENT_UNREAL_VIRTUAL_WRAPPER(class_name, function_name, return_type, params, args) \
static const auto offset = []() { \
    auto it = VTableLayoutMap.find(STR(#function_name)); \
    if (it == VTableLayoutMap.end())                                                         \
    {                                                                                              \
        throw std::runtime_error{"Virtual " #class_name "::" #function_name " is unavailable, possibly unsupported in engine version"}; \
    }\
    return UE4SS_VTABLE_ABI_SHIFT(it->second); \
}(); \
std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(this)); \
auto func = BIT_CAST_MFP(return_type(class_name::*)(params) const, *std::bit_cast<void**>(vtable + offset)); \
if (!func) \
{ \
throw std::runtime_error{"Function '" #function_name "' not available"}; \
}                                                                          \
return (this->*func)(args);

#define IMPLEMENT_UNREAL_VIRTUAL_WRAPPER_MULTI_NAME_NO_PARAMS(class_name, function_name, return_type, ...) \
static const auto offset = []() { \
    auto it = VTableLayoutMap.end(); \
    for (const auto& name : {STR(#function_name), __VA_ARGS__}) \
    { \
        it = VTableLayoutMap.find(name); \
        if (it != VTableLayoutMap.end()) break; \
    } \
    if (it == VTableLayoutMap.end()) \
    { \
        throw std::runtime_error{"Virtual " #class_name "::" #function_name " is unavailable, possibly unsupported in engine version"}; \
    } \
    return UE4SS_VTABLE_ABI_SHIFT(it->second); \
}(); \
std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(this)); \
auto func = BIT_CAST_MFP(return_type(class_name::*)() const, *std::bit_cast<void**>(vtable + offset)); \
if (!func) \
{ \
    throw std::runtime_error{"Function '" #function_name "' not available"}; \
} \
return (this->*func)();

#define IMPLEMENT_UNREAL_VIRTUAL_WRAPPER_MULTI_NAME(class_name, function_name, return_type, params, args, ...) \
static const auto offset = []() { \
    auto it = VTableLayoutMap.end(); \
    for (const auto& name : {STR(#function_name), __VA_ARGS__}) \
    { \
        it = VTableLayoutMap.find(name); \
        if (it != VTableLayoutMap.end()) break; \
    } \
    if (it == VTableLayoutMap.end()) \
    { \
        throw std::runtime_error{"Virtual " #class_name "::" #function_name " is unavailable, possibly unsupported in engine version"}; \
    } \
    return UE4SS_VTABLE_ABI_SHIFT(it->second); \
}(); \
std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(this)); \
auto func = BIT_CAST_MFP(return_type(class_name::*)(params) const, *std::bit_cast<void**>(vtable + offset)); \
if (!func) \
{ \
    throw std::runtime_error{"Function '" #function_name "' not available"}; \
} \
return (this->*func)(args);

#define GET_ADDRESS_OF_UNREAL_VIRTUAL(class_name, function_name, instance) \
[&instance]() -> void* {                                                   \
    static const auto offset = []() { \
        auto it = class_name::VTableLayoutMap.find(STR(#function_name));                        \
        if (it == class_name::VTableLayoutMap.end())                                       \
        {                                                                      \
            throw std::runtime_error{"Virtual " #class_name "::" #function_name " is unavailable, possibly unsupported in engine version"}; \
        }\
        return UE4SS_VTABLE_ABI_SHIFT(it->second); \
    }(); \
    std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(instance)); \
    return RESOLVE_JMP(*std::bit_cast<void**>(vtable + offset));\
}()

#define OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(class_name, function_name, instance) \
[&instance]() -> void* {                                                   \
    static const auto offset = []() -> uint32_t { \
        auto it = class_name::VTableLayoutMap.find(STR(#function_name));                        \
        if (it == class_name::VTableLayoutMap.end())                                       \
        {                                                                      \
            return 0; \
        }\
        return UE4SS_VTABLE_ABI_SHIFT(it->second); \
    }(); \
    if (offset == 0) \
    { \
        return nullptr; \
    } \
    std::byte* vtable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(instance)); \
    return RESOLVE_JMP(*std::bit_cast<void**>(vtable + offset));\
}()

namespace RC::Unreal
{
    class UnrealVirtualBaseVC
    {
    public:
        virtual ~UnrealVirtualBaseVC() = default;

    public:
        virtual auto set_virtual_offsets() -> void = 0;
    };
}


