// ============================================================================
// IsleCPPUtilities (formerly IsleNotify) — The Isle server-side C++ toolkit:
// notifications, announcements, chat delivery, slash-command capture, mutation
// registration, corpse spawning, teleports, and live diagnostics. Started as a
// notification relay, hence the file layout; history below.
//
// THE ORIGINAL JOB: deliver DinoStorage's player-facing messages as the game's own
// on-screen notification, by calling TIPlayerController:ClientShowNotification
// from C++ instead of from Lua.
//
// WHY THIS EXISTS, AND WHAT THE LUA CRASH ACTUALLY WAS
//   ClientShowNotification(Message: FText) resolves on this build and is callable
//   from UE4SS Lua right up until it segfaults the server, which it did on the
//   first live player to trigger it. The Stage C probe found the reason, and it
//   is not the function:
//
//     [IsleCPPUtilities] ClientShowNotification: parms_size=16 num_parms=1
//     [IsleCPPUtilities]   Message : TextProperty @0
//     ... and at boot ...
//     Was unable to detect FText size, using default: 0x18 bytes.
//
//   The engine's FText here is 16 bytes - a UE5 TSharedRef<ITextData>, two
//   pointers - which is why a one-FText function has a 16-byte parameter block,
//   and why GetControllerBySteamId puts its return value at +16 after an FString.
//   UE4SS could not detect that and fell back to sizeof(RC::Unreal::FText), which
//   is 24: the UE4-era layout with two trailing uint32s. So every FText marshal
//   through the Lua bridge writes 24 bytes into a 16-byte slot and runs 8 bytes
//   past the end of the parameter buffer. That is the crash, it is a heap
//   overflow rather than anything to do with FText being reference counted, and
//   no pcall can catch it because it happens below the Lua boundary.
//
//   Hence the two rules this file lives by:
//     * Copy the property's REAL size, derived from the function's own layout
//       (parms_size and the next parameter's offset). Not sizeof(FText), and not
//       FProperty::GetSize() either - GetSize() returns 0 on this build, so the
//       upstream advice to use it would silently copy nothing.
//     * Never let two owners hold one FString allocation. See send_one.
//
// WHAT IT IS NOT
//   Not a feature. All the logic - what to say, to whom, when - stays in the Lua
//   mod. This tails the result file that mod already writes and turns each
//   player-addressed line into a notification, using the same "does it carry a
//   short field" rule the Lua mod's emitResult applies. It replaced
//   isle-server/chat_relay.py, which delivered the same lines to the chat box over
//   RCON; that script is retired and no longer started.
//
// THE SECOND JOB
//   It also runs engine calls the Lua bridge cannot make safely, requested a line
//   at a time through cppcmd.ndjson: SetReplicatedMutationsData (the only way to
//   register parent and elder mutation slots on this build, see kPushFn), corpse
//   spawning for MercyCorpse (spawn_corpse), and player teleports for the web
//   portal (teleport_pawn).
// ============================================================================

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DynamicOutput/Output.hpp>
#include <Mod/CppUserModBase.hpp>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr const CharType* kNotifyFn = STR("/Script/TheIsle.TIPlayerController:ClientShowNotification");

    // The game's own announcement banner, per-client. Unlike the notification
    // toast its display time is a PARAMETER (fnsig: Message[StrProperty]@0,
    // Announcer[TextProperty]@16, AnnouncementTime[FloatProperty]@32,
    // parms_size=36), so server-wide announcements get a real duration instead
    // of the double-fire simulation the toast path needs.
    constexpr const CharType* kAnnounceFn = STR("/Script/TheIsle.TIPlayerController:Client_ProcessIncomingAnnouncement");

    // The server-side chat reception point, per EVRIMA_Chat_System.md: fires once
    // per receiving controller in range, with the SENDER in the
    // ChatPlayerController parameter. Hooked here for two reasons the Lua bridge
    // cannot serve:
    //   1. Diagnosis: whether "/"-prefixed messages reach the server at all. The
    //      chat LOG never shows them, but the log only proves they are not
    //      broadcast - a server that receives and silently drops them looks
    //      identical from the log. This hook sees the receive itself.
    //   2. Suppression: a pre-hook may rewrite parameters, so a command line
    //      ("!store") can be blanked out of every recipient's chat box while the
    //      chat-log tailer (which observed the ORIGINAL receive) still serves it.
    // Reading the FText params from Lua is the known-fatal marshalling path; from
    // C++ the text is read through Conv_TextToString with a measured-extent copy.
    constexpr const CharType* kChatFn = STR("/Script/TheIsle.TIPlayerController:GetChatMessage");

    // Where "/" messages actually go. A "/test" typed in game fires NEITHER the
    // GetChatMessage hook NOR LogTheIsleChatData (proven live 2026-07-27, with
    // "hello" firing both moments later), because the client routes
    // slash-prefixed input away from chat entirely and into this Server RPC -
    // which is how unofficial servers have working "/" commands and why they
    // never echo in anyone's chat box. Signature (live fnsig probe):
    // CommandLine[StrProperty]@0, parms_size=16.
    constexpr const CharType* kChatCmdFn = STR("/Script/TheIsle.TIPlayerController:ServerExecuteChatCommand");

    // Where captured slash commands are handed to the Lua mod's dispatcher: one
    // line per command, {"id":"slash","steam":"7656..","cmd":"store 2"}.
    // DinoStorage tails it exactly like the chat log, so /store == !store.
    constexpr const char* kSlashOutPath = "/isle_config/cppbridge/chatcmd.ndjson";

    // The game's chat-delivery Client RPC, for typing a line INTO a player's
    // chat box as "Server". C++-only per EVRIMA_Chat_System.md (the Lua FText
    // marshal is the known-fatal path). Live fnsig: Sender[Text]@0 ext16,
    // Text[Text]@16 ext16, SenderSteamId[Str]@32, ChatMode[Enum]@48,
    // bIsDev@49, bIsAdmin@50, parms_size=51.
    constexpr const CharType* kUpdateChatFn = STR("/Script/TheIsle.TIPlayerController:UpdateChat");
    constexpr const CharType* kTextToStringFn = STR("/Script/Engine.KismetTextLibrary:Conv_TextToString");
    constexpr const CharType* kKismetTextSelf = STR("/Script/Engine.Default__KismetTextLibrary");
    constexpr const char* kResultPath = "/isle_config/dinostorage/result.ndjson";

    // Second, separate channel: actions the writers cannot perform themselves.
    //
    // Kept out of result.ndjson on purpose. That file is a log of outcomes, read
    // by the web UI and by this mod's notification path; putting imperatives in it
    // would mean a replayed or rotated log could re-run engine writes.
    //
    // Lives in its own /isle_config/cppbridge directory rather than under
    // dinostorage: DinoStorage was merely the first writer. MercyCorpse
    // (spawncorpse), the web portal (teleport, announce) and any future mod all
    // speak this channel, so its home names the utility, not one client.
    constexpr const char* kCmdPath = "/isle_config/cppbridge/cppcmd.ndjson";
    constexpr const char* kCmdResultPath = "/isle_config/cppbridge/cppresult.ndjson";

    // SetReplicatedMutationsData is the ONLY way to register parent and elder
    // mutation slots on this build. A probe of the pawn's whole class chain found
    // SetSlot1/2EquippedMutation for the active slots and nothing whatsoever for
    // parent or elder - no SetParentSlot*, no SetElder*, not even an OnRep. So a
    // restored parent mutation can be written into the struct field but never
    // registered, which is why it reads 0% on the client while the server values
    // it correctly.
    //
    // From Lua this call segfaults: it takes the struct BY VALUE, and the Lua
    // bridge mis-sizes by-value parameters (see the FText note above - it assumes
    // 24 bytes where the engine uses 16). In C++ the parameter block is measured
    // from the function's own layout, so the size is whatever the engine actually
    // says it is.
    constexpr const CharType* kPushFn = STR("/Script/TheIsle.TICharacterBase:SetReplicatedMutationsData");

    // The same ordered candidate list findGameMode() uses in the Lua mod, so both
    // halves agree on which object is "the game mode".
    constexpr const CharType* kGameModeNames[] = {
        STR("BP_SurvivalGameMode_C"),
        STR("TISurvivalGameMode"),
        STR("TIGameModeBase"),
        STR("GameModeBase"),
    };

    // A burst of notifications is one RCON connection per line for chat_relay.py,
    // and one ProcessEvent per line here. !prime alone sends six. Spreading them
    // over several frames keeps any one frame cheap.
    constexpr int kMaxPerTick = 2;

    // Every notification is delivered twice: once when dequeued, and once more
    // when the first toast is falling off screen - the point of the repeat is to
    // SIMULATE a longer display, since the toast's duration is baked into the
    // client's widget and nothing server-side can extend it. That duration is
    // also not discoverable server-side (it lives in the client UI blueprint),
    // so the gap is tunable at runtime: put `resend_ms=<n>` in notify.cfg, time
    // the fall-off once in-game, and set n to match. 0 disables the second
    // showing entirely.
    constexpr int kDefaultResendMs = 5000;
    constexpr const char* kCfgPath = "/isle_config/cppbridge/notify.cfg";

    // A line addressed to this "steam id" goes to EVERY live player controller.
    // Used for server-wide announcements (corpse-wipe warnings and the like);
    // real steam ids are all digits so the sentinel cannot collide.
    constexpr const char* kBroadcastSteam = "all";

    // How long a MercyCorpse body persists before the engine's decay reclaims
    // it (ActivateDeadbody's timer argument). Matches the CorpseWipe mod's
    // 20-minute server rule so a body the wipe somehow misses still dies on
    // schedule; the BodyDrop doc's default was 3600.
    constexpr double kCorpseDecaySeconds = 1200.0;
    // How often the corpse first-seen census walks the object array. Ages only
    // accrue between walks, so this has to be far tighter than the wipe cycle,
    // and it is the resolution of the fallback age (a body can read up to this
    // much younger than it is).
    constexpr double kCensusIntervalSeconds = 10.0;
    // Prime zone-visit counts, republished for PrimeWatch. Zones are cached on
    // first discovery, so this now reads a small vector instead of ForEachUObject.
    constexpr double kZoneVisitsIntervalSeconds = 5.0;
    constexpr const char* kZoneVisitsPath = "/isle_config/primewatch/zonevisits.json";
    constexpr size_t kMaxQueue = 64;   // a backlog this deep means nobody is reading it anyway

    struct Pending
    {
        std::string steam;
        std::string text;
        // Redeliveries left. Fresh messages default to 1: fire now, fire once
        // more kResendDelay later, done.
        int resend{1};
    };

    struct Action
    {
        std::string id;
        std::string verb;
        std::string steam;
        // spawncorpse only. Written by MercyCorpse as string fields so the
        // existing string-only line reader can carry them.
        std::string cls;
        std::string to;     // teleport only: a steam id - destination is that player's live position
        std::string text;   // announce/chatdm: the message
        std::string from;   // announce/chatdm: the sender label (default "Server")
        double secs{10.0};  // announce only: on-screen duration in seconds
        int mode{0};        // chatdm only: EChatMode byte (0 = the Spatial the hook observed)
        bool admin{false};  // chatdm only: bIsAdmin styling flag
        double x{0.0};
        double y{0.0};
        double z{0.0};
        double g{0.6};      // growth of the spawned corpse: drives mesh size AND food value
        // wipecorpses only: seconds a body must have been on the floor before it
        // may be destroyed. 0 = the old age-blind behaviour, wipe everything.
        double min_age{0.0};
        bool kill{false};
    };

    // ---------------------------------------------------------------- JSON
    //
    // Deliberately not a parser. These lines are written by emitResult in the Lua
    // mod with a known shape, and the only fields needed are two strings. Escapes
    // are handled because the message text is player-facing prose that has
    // already been through jsonEscape.
    auto json_string_field(const std::string& line, const char* key) -> std::string
    {
        const std::string needle = std::string("\"") + key + "\":\"";
        size_t at = line.find(needle);
        if (at == std::string::npos) return {};
        at += needle.size();

        std::string out;
        for (size_t i = at; i < line.size(); ++i)
        {
            const char c = line[i];
            if (c == '\\')
            {
                if (i + 1 >= line.size()) break;
                const char n = line[++i];
                switch (n)
                {
                case 'n': out.push_back('\n'); break;
                case 'r': break;
                case 't': out.push_back(' '); break;
                // Covers an escaped quote and an escaped backslash. Do not end
                // this comment with a backslash: it would splice the next line.
                default: out.push_back(n); break;
                }
                continue;
            }
            if (c == '"') return out;
            out.push_back(c);
        }
        return {};
    }

    auto all_digits(const std::string& s) -> bool
    {
        if (s.empty()) return false;
        for (char c : s)
        {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }
} // namespace

class IsleCPPUtilities : public CppUserModBase
{
  private:
    std::atomic<bool> m_tick_registered{false};
    std::atomic<bool> m_probed{false};
    std::atomic<bool> m_stop{false};
    std::thread m_tailer;

    std::mutex m_lock;
    std::deque<Pending> m_queue;
    std::deque<Action> m_actions;
    // (ticks remaining, action) - used by the two-phase mutation refresh.
    std::deque<std::pair<int, Action>> m_delayed;
    // (due time, message) - the second showing of every notification. Wall-clock
    // rather than tick-counted because "3 seconds" should mean 3 seconds
    // regardless of what the server's tick rate is doing under load.
    std::deque<std::pair<std::chrono::steady_clock::time_point, Pending>> m_resend;
    // Live-tunable from notify.cfg; re-read once a second by the tailer thread.
    std::atomic<int> m_resend_ms{kDefaultResendMs};

    // Corpse first-seen census: object name -> steady-clock seconds when this mod
    // first saw that body on the floor. The fallback age source for wipecorpses'
    // freshness gate, used whenever the engine's own stamp cannot be read. Game
    // thread only (drain and the wipe both run there), so it needs no lock.
    std::unordered_map<std::string, double> m_corpse_seen;
    double m_census_at{0.0};
    double m_zone_at{0.0};
    bool m_census_primed{false};

    // Cached zone actors (MigrationVisitorIDs + PatrolVisitorIDs). Populated
    // once on first discovery and never rebuilt (zones don't spawn/despawn). Game
    // thread only. This saves us from ForEachUObject every 5s.
    std::vector<UObject*> m_zones;
    bool m_zones_discovered{false};

    // Chat hook state. Registered once from the game-thread tick; the two flags
    // are live-tunable from notify.cfg (chat_diag=0/1, chat_blank=0/1) so the
    // experiment can be switched without a rebuild, a redeploy, or a restart.
    std::atomic<bool> m_chat_hooked{false};
    std::atomic<bool> m_chat_diag{true};
    std::atomic<bool> m_chat_blank{false};
    bool m_chat_warned{false};
    UFunction* m_conv_fn{nullptr};
    UObject* m_conv_self{nullptr};
    int32 m_conv_in_off{0};
    int32 m_conv_in_ext{0};
    int32 m_conv_ret_off{0};
    int32 m_chat_text_off{-1};
    int32 m_chat_text_ext{0};
    int32 m_chat_nofilter_off{-1};
    int32 m_chat_nofilter_ext{0};
    int32 m_chat_sender_off{-1};
    int32 m_chat_mode_off{-1};
    int32 m_cmd_off{-1};
    // (steam id, command line) captured by on_chat_command, flushed by drain().
    std::deque<std::pair<std::string, std::string>> m_slash;

    // Resolved once on the game thread, then reused.
    UFunction* m_notify_fn{nullptr};
    int32 m_notify_offset{0};
    int32 m_notify_size{0};
    bool m_ready{false};

    // ------------------------------------------------------------ resolution

    // The real byte size of a parameter, from the function's own layout.
    //
    // FProperty::GetSize() returns 0 on this build (it is GetArrayDim() *
    // GetElementSize(), and one of those members is not resolving), so the size
    // has to be inferred: a parameter runs until the next parameter starts, or
    // until the end of the parameter block if it is the last one. For
    // ClientShowNotification that yields 16 - the correct engine FText size, and
    // 8 less than sizeof(FText) would have given.
    auto param_extent(UFunction* fn, FProperty* target) -> int32
    {
        const int32 start = target->GetOffset_Internal();
        int32 end = static_cast<int32>(fn->GetParmsSize());

        for (FProperty* prop : fn->ForEachProperty())
        {
            if (!prop || prop == target) continue;
            const int32 off = prop->GetOffset_Internal();
            if (off > start && off < end) end = off;
        }
        return end - start;
    }

    auto resolve_once() -> void
    {
        if (m_probed.exchange(true)) return;

        m_notify_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kNotifyFn);
        if (!m_notify_fn)
        {
            Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] ClientShowNotification not found - delivery disabled\n"));
            return;
        }

        FProperty* msg = m_notify_fn->FindProperty(FName(STR("Message"), FNAME_Find));
        if (!msg)
        {
            Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] ClientShowNotification has no Message property - delivery disabled\n"));
            return;
        }

        m_notify_offset = msg->GetOffset_Internal();
        m_notify_size = param_extent(m_notify_fn, msg);

        // Refuse rather than guess. Copying more than the slot holds is the
        // overflow this whole file exists to avoid, and copying a size we did not
        // derive from the live layout is how that gets reintroduced.
        if (m_notify_size <= 0 || m_notify_size > static_cast<int32>(sizeof(FText)))
        {
            Output::send<LogLevel::Error>(
                STR("[IsleCPPUtilities] Message extent {} is not usable (sizeof(FText)={}) - delivery disabled\n"),
                m_notify_size, static_cast<int32>(sizeof(FText)));
            return;
        }

        m_ready = true;
        Output::send<LogLevel::Verbose>(
            STR("[IsleCPPUtilities] ready: Message @{} extent {} (parms_size {}, sizeof(FText) {}, FText::StaticSize {})\n"),
            m_notify_offset, m_notify_size, static_cast<int32>(m_notify_fn->GetParmsSize()),
            static_cast<int32>(sizeof(FText)), FText::StaticSize());
    }

    // ------------------------------------------------------------- delivery

    auto controller_for(const std::string& steam) -> UObject*
    {
        for (const CharType* name : kGameModeNames)
        {
            UObject* gm = UObjectGlobals::FindFirstOf(name);
            if (!gm) continue;

            UFunction* fn = gm->GetFunctionByNameInChain(FName(STR("GetControllerBySteamId"), FNAME_Find));
            if (!fn) return nullptr;

            FProperty* p_id = fn->FindProperty(FName(STR("SteamId"), FNAME_Find));
            FProperty* p_ret = fn->GetReturnProperty();
            if (!p_id || !p_ret) return nullptr;

            std::vector<uint8> buf(fn->GetParmsSize(), 0);

            // The FString rule, and the reason the local is declared here rather
            // than inline: assignment into the buffer is a SHALLOW TArray copy, so
            // after this line the local and the buffer point at one allocation.
            // Two owners, one buffer - which is the double-free upstream warns
            // about. Keeping the local alive until after ProcessEvent means the
            // buffer's pointer is valid for the whole call, and never destructing
            // the buffer copy means it is only ever released once, by the local.
            //
            // Assigning into zeroed memory is safe: FString::operator= only frees
            // the destination when Data.Num() > 0, and this buffer is all zeroes.
            FString id{StringType(steam.begin(), steam.end())};
            *std::bit_cast<FString*>(buf.data() + p_id->GetOffset_Internal()) = id;

            gm->ProcessEvent(fn, buf.data());

            return *std::bit_cast<UObject**>(buf.data() + p_ret->GetOffset_Internal());
        }
        return nullptr;
    }

    // ------------------------------------------------- mutation struct push
    //
    // Registers whatever is currently in the pawn's ReplicatedMutationsData by
    // handing that same struct back through the setter. The Lua mod has already
    // written the FNames into the live struct; this call is what makes the engine
    // and the client treat those slots as equipped.
    //
    // The struct is copied out of the pawn and into the parameter block verbatim.
    // ReplicatedMutationsData is sixteen FNames - all POD, 8 bytes each - so a
    // byte copy creates no second owner of anything and there is nothing to
    // destruct afterwards. That is what makes this safe to do by value, and it is
    // worth re-checking if the struct ever gains an FString or a TArray member.
    auto push_mutations(UObject* pawn, std::string& detail) -> bool
    {
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kPushFn);
        if (!fn) { detail = "SetReplicatedMutationsData not found"; return false; }

        FProperty* src = pawn->GetPropertyByNameInChain(STR("ReplicatedMutationsData"));
        if (!src) { detail = "pawn has no ReplicatedMutationsData property"; return false; }

        // Parameters in declaration order: the struct first, then whatever else
        // the signature carries (the Lua call passed a trailing bool).
        FProperty* p_struct = nullptr;
        int32 lowest = 0x7fffffff;
        int parm_count = 0;
        for (FProperty* p : fn->ForEachProperty())
        {
            if (!p) continue;
            ++parm_count;
            if (p->GetOffset_Internal() < lowest) { lowest = p->GetOffset_Internal(); p_struct = p; }
        }
        if (!p_struct) { detail = "setter has no parameters"; return false; }

        const int32 extent = param_extent(fn, p_struct);
        if (extent <= 0) { detail = "could not measure the struct parameter"; return false; }

        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        std::memcpy(buf.data() + p_struct->GetOffset_Internal(),
                    src->ContainerPtrToValuePtr<void>(pawn),
                    static_cast<size_t>(extent));

        // The Lua call site passed `true` as the second argument. Any trailing
        // parameter here is a single byte, so setting every remaining byte after
        // the struct is the same thing without needing to know its name.
        for (size_t i = static_cast<size_t>(p_struct->GetOffset_Internal()) + static_cast<size_t>(extent);
             i < buf.size(); ++i)
        {
            buf[i] = 1;
        }

        pawn->ProcessEvent(fn, buf.data());

        detail = "pushed " + std::to_string(extent) + " bytes, " +
                 std::to_string(parm_count) + " parm(s), parms_size " +
                 std::to_string(static_cast<int>(fn->GetParmsSize()));
        return true;
    }

    // ------------------------------------------------ mutation config probe
    //
    // The question: a fresh hatchling at the mutation-selection screen sees every
    // option at 0%, before anything is equipped or restored. The server is fine -
    // IsLifecycleMutationEquipped returns exactly the Game.ini values - so the
    // values are not reaching the client.
    //
    // The single fact that decides what can be done about it is whether
    // EnabledMutations is replicated at all. If CPF_Net is absent, no server-side
    // mod can push it: clients get their percentages from their own install, and
    // the fault is not something this repo can reach. If it IS replicated, forcing
    // the GameState to resend becomes a real option.
    //
    // Lua could not answer this. TArray indexing there returns element 1 for every
    // index, so the array cannot even be walked, let alone its flags read.
    auto probe_mutation_config(std::string& detail) -> bool
    {
        UObject* gs = nullptr;
        for (const CharType* n : { STR("BP_SurvivalGameState_C"), STR("TIGameStateBase"),
                                   STR("TIGameState"), STR("GameStateBase") })
        {
            gs = UObjectGlobals::FindFirstOf(n);
            if (gs) break;
        }
        if (!gs) { detail = "game state not found"; return false; }

        FProperty* prop = gs->GetPropertyByNameInChain(STR("EnabledMutations"));
        if (!prop) { detail = "GameState has no EnabledMutations property"; return false; }

        const bool replicated = prop->HasAnyPropertyFlags(CPF_Net);
        auto* arrProp = CastField<FArrayProperty>(prop);

        std::string out = "EnabledMutations: CPF_Net=";
        out += replicated ? "YES(replicated)" : "NO(server-only)";

        if (!arrProp)
        {
            out += ", not an FArrayProperty";
            detail = out;
            return true;
        }

        void* addr = prop->ContainerPtrToValuePtr<void>(gs);
        FScriptArrayHelper helper(arrProp, addr);
        const int32 num = helper.Num();

        FProperty* inner = arrProp->GetInner();
        out += ", Num=" + std::to_string(num);
        out += ", inner_elem_size=" + std::to_string(inner ? inner->GetElementSize() : -1);
        out += ", inner_arraydim=" + std::to_string(inner ? inner->GetArrayDim() : -1);

        // Only walk the array if the stride is sane. GetElementSize() returns 0 for
        // function parameters on this build, and a zero stride would make every
        // index resolve to the same element - exactly the artefact that made the
        // Lua probe look like it had verified all 41 entries when it had seen one.
        const int32 stride = inner ? inner->GetElementSize() : 0;
        if (stride > 0 && num > 0)
        {
            int zeros = 0;
            int counted = 0;
            for (int32 i = 0; i < num && i < 64; ++i)
            {
                uint8* elem = reinterpret_cast<uint8*>(helper.GetRawPtr(i));
                if (!elem) continue;
                ++counted;
                // The struct is (MutationName, EffectValue). Find the float by name
                // rather than by assuming an offset.
                if (auto* sp = CastField<FStructProperty>(inner))
                {
                    if (UScriptStruct* ss = sp->GetStruct())
                    {
                        if (FProperty* vp = ss->FindProperty(FName(STR("EffectValue"), FNAME_Find)))
                        {
                            float v = *std::bit_cast<float*>(elem + vp->GetOffset_Internal());
                            if (v == 0.0f) ++zeros;
                        }
                    }
                }
            }
            out += ", walked=" + std::to_string(counted) + ", zero_values=" + std::to_string(zeros);
        }
        else
        {
            out += ", stride unusable - cannot walk";
        }

        detail = out;
        return true;
    }

    // ------------------------------------------------- mutation property scan
    //
    // The Replicate? screen shows "Value: 0%" for a mutation Game.ini defines at
    // 0.25. That rules out the obvious explanation: a client that had simply never
    // received the server's config would fall back to its own install's defaults,
    // not display zero. Showing zero means the client IS being told a value and
    // the value is zero - so some replicated property carries it, and
    // GameState.EnabledMutations (CPF_Net=NO) is not the one.
    //
    // So: dump every property with "Mutation" in its name across the objects that
    // could plausibly feed that screen, with its replication flag and type. The one
    // that is replicated and holds zeros is the bug.
    auto scan_mutation_props(UObject* obj, const char* label, std::string& out) -> void
    {
        if (!obj) { out += std::string(label) + "=<null> "; return; }

        UClass* cls = obj->GetClassPrivate();
        if (!cls) { out += std::string(label) + "=<noclass> "; return; }

        out += std::string("\\n") + label + ": ";
        int shown = 0;
        for (FProperty* p : cls->ForEachPropertyInChain())
        {
            if (!p) continue;
            StringType nameW = p->GetName();
            std::string name(nameW.begin(), nameW.end());
            if (name.find("utation") == std::string::npos) continue;   // Mutation / mutation
            if (++shown > 24) break;

            StringType typeW = p->GetClass().GetName();
            std::string type(typeW.begin(), typeW.end());

            out += name + "[" + type;
            out += p->HasAnyPropertyFlags(CPF_Net) ? ",NET" : ",local";
            out += "] ";
        }
        if (shown == 0) out += "(none) ";
    }

    // Dump a TArray<FSomeStruct> as "name=value" pairs.
    //
    // The stride cannot come from GetInner()->GetElementSize(): that returns 0 on
    // this build, which is what makes FScriptArrayHelper's own constructor useless
    // here and what made every Lua index resolve to element 1. UScriptStruct's
    // PropertiesSize is a different member and does report a real size, so that is
    // what strides the walk. Num() is safe either way - it reads the FScriptArray
    // header and never touches the stride.
    auto dump_struct_array(UObject* obj, const CharType* propName, std::string& out) -> void
    {
        StringType pn(propName);
        std::string label(pn.begin(), pn.end());

        FProperty* prop = obj->GetPropertyByNameInChain(propName);
        if (!prop) { out += "\\n" + label + ": <absent>"; return; }

        auto* arrProp = CastField<FArrayProperty>(prop);
        if (!arrProp) { out += "\\n" + label + ": <not an array>"; return; }

        void* addr = prop->ContainerPtrToValuePtr<void>(obj);
        FScriptArrayHelper helper(arrProp, addr);
        const int32 num = helper.Num();

        out += "\\n" + label + "[" + (prop->HasAnyPropertyFlags(CPF_Net) ? "NET" : "local") +
               "] Num=" + std::to_string(num);
        if (num <= 0) return;

        auto* inner = CastField<FStructProperty>(arrProp->GetInner());
        UScriptStruct* ss = inner ? inner->GetStruct() : nullptr;
        if (!ss) { out += " <inner not a struct>"; return; }

        const int32 stride = ss->GetPropertiesSize();
        out += " stride=" + std::to_string(stride);
        if (stride <= 0) { out += " <unusable>"; return; }

        FProperty* pName = ss->FindProperty(FName(STR("MutationName"), FNAME_Find));
        FProperty* pVal  = ss->FindProperty(FName(STR("EffectValue"), FNAME_Find));
        if (!pVal) { out += " <no EffectValue member>"; return; }

        uint8* base = reinterpret_cast<uint8*>(helper.GetRawPtr(0));
        if (!base) { out += " <null data>"; return; }

        int zeros = 0;
        std::string sample;
        for (int32 i = 0; i < num; ++i)
        {
            uint8* e = base + static_cast<size_t>(i) * static_cast<size_t>(stride);
            const float v = *std::bit_cast<float*>(e + pVal->GetOffset_Internal());
            if (v == 0.0f) ++zeros;
            if (i < 5)
            {
                std::string nm = "?";
                if (pName)
                {
                    auto* fn = std::bit_cast<FName*>(e + pName->GetOffset_Internal());
                    StringType s = fn->ToString();
                    nm.assign(s.begin(), s.end());
                }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.3f", v);
                sample += (sample.empty() ? "" : ", ") + nm + "=" + buf;
            }
        }
        out += " zeros=" + std::to_string(zeros) + " | " + sample;
    }

    // Dump the scalar members of a struct property, whatever they turn out to be.
    //
    // TemporaryEntumbMutationData ("Entumb" is the game's spelling) is the staging
    // struct the entomb confirmation screen works from - the one showing
    // "Value: 0%" for a mutation Game.ini defines at 0.25. It is not replicated, so
    // the server's copy is its own; if the values are already zero HERE, the zero
    // is being produced server-side during the entomb flow and is not a client
    // display bug at all.
    auto dump_struct_members(UObject* obj, const CharType* propName, std::string& out) -> void
    {
        StringType pn(propName);
        std::string label(pn.begin(), pn.end());

        FProperty* prop = obj->GetPropertyByNameInChain(propName);
        if (!prop) { out += "\\n" + label + ": <absent>"; return; }

        auto* sp = CastField<FStructProperty>(prop);
        if (!sp || !sp->GetStruct()) { out += "\\n" + label + ": <not a struct>"; return; }

        uint8* base = prop->ContainerPtrToValuePtr<uint8>(obj);
        out += "\\n" + label + "[" + (prop->HasAnyPropertyFlags(CPF_Net) ? "NET" : "local") + "]: ";

        int shown = 0;
        for (FProperty* m : sp->GetStruct()->ForEachProperty())
        {
            if (!m || ++shown > 20) break;
            StringType nw = m->GetName();
            std::string name(nw.begin(), nw.end());
            StringType tw = m->GetClass().GetName();
            std::string type(tw.begin(), tw.end());
            uint8* at = base + m->GetOffset_Internal();

            char buf[96];
            if (type == "FloatProperty")
                std::snprintf(buf, sizeof(buf), "%s=%.4f ", name.c_str(), *std::bit_cast<float*>(at));
            else if (type == "DoubleProperty")
                std::snprintf(buf, sizeof(buf), "%s=%.4f ", name.c_str(), *std::bit_cast<double*>(at));
            else if (type == "IntProperty")
                std::snprintf(buf, sizeof(buf), "%s=%d ", name.c_str(), *std::bit_cast<int32*>(at));
            else if (type == "BoolProperty")
                std::snprintf(buf, sizeof(buf), "%s=%d ", name.c_str(), (int)(*at != 0));
            else if (type == "NameProperty")
            {
                StringType s = std::bit_cast<FName*>(at)->ToString();
                std::string v(s.begin(), s.end());
                std::snprintf(buf, sizeof(buf), "%s=%s ", name.c_str(), v.c_str());
            }
            else
                std::snprintf(buf, sizeof(buf), "%s<%s> ", name.c_str(), type.c_str());
            out += buf;
        }
        if (shown == 0) out += "(no members)";
    }

    auto probe_mutation_props(const std::string& steam, std::string& detail) -> bool
    {
        std::string out;

        UObject* gs = nullptr;
        for (const CharType* n : { STR("BP_SurvivalGameState_C"), STR("TIGameStateBase") })
        {
            gs = UObjectGlobals::FindFirstOf(n);
            if (gs) break;
        }
        scan_mutation_props(gs, "GameState", out);

        UObject* ctrl = controller_for(steam);
        scan_mutation_props(ctrl, "Controller", out);

        // The entomb screen's own staging data. Run !mutscan WHILE the Replicate?
        // screen is open - the struct is "Temporary" and is very likely empty at
        // any other moment, so an all-zero reading outside the entomb flow means
        // nothing.
        if (ctrl)
        {
            dump_struct_members(ctrl, STR("TemporaryEntumbMutationData"), out);
            dump_struct_members(ctrl, STR("TemporaryMutationsRequirementsData"), out);
        }

        if (ctrl)
        {
            if (UFunction* getPS = ctrl->GetFunctionByNameInChain(FName(STR("K2_GetPawn"), FNAME_Find)))
            {
                std::vector<uint8> pb(getPS->GetParmsSize(), 0);
                ctrl->ProcessEvent(getPS, pb.data());
                if (FProperty* ret = getPS->GetReturnProperty())
                {
                    UObject* pawn = *std::bit_cast<UObject**>(pb.data() + ret->GetOffset_Internal());
                    scan_mutation_props(pawn, "Pawn", out);

                    // These two are the replicated arrays, and therefore the only
                    // plausible route by which a mutation's value reaches the
                    // client's Replicate? screen. If they are populated and correct
                    // here, the server is doing its job and the fault is downstream;
                    // if they are zero, this is where the 0% comes from.
                    if (pawn)
                    {
                        dump_struct_array(pawn, STR("LifecycleMutationsList"), out);
                        dump_struct_array(pawn, STR("LifecycleMutationsListAll"), out);
                    }
                }
            }
            if (FProperty* psp = ctrl->GetPropertyByNameInChain(STR("PlayerState")))
            {
                UObject* ps = *psp->ContainerPtrToValuePtr<UObject*>(ctrl);
                scan_mutation_props(ps, "PlayerState", out);
            }
        }

        detail = out;
        return true;
    }

    // ------------------------------------------------- post-entomb mut refresh
    //
    // The bug being worked around is the game's, not ours: after an entomb the
    // client shows every mutation at 0%, while the server keeps the correct values
    // throughout. Verified on a VANILLA server - all mods off AND libUE4SS.so not
    // injected - so nothing in this repo causes it. A fresh dino that has never
    // entombed shows the correct percentages, so it is specifically the transition
    // that loses them on the client.
    //
    // Since the server data is right and only the client's copy is wrong, the
    // workaround is to make the server say it again once the offspring exists.
    // Three levers, applied together because it is not knowable from here which one
    // the client's UI actually responds to:
    //
    //   1. Dirty the replicated arrays. UE only resends a property whose value
    //      differs from its shadow copy, so writing the SAME values back is a
    //      no-op and would resend nothing. Hence the epsilon nudge here and the
    //      exact restore on the follow-up pass - two real changes, ending on the
    //      correct value.
    //   2. Toggle bIsMutationEnabled. If the mutation UI binds to that replicated
    //      bool, flipping it is what makes the panel rebuild rather than redraw
    //      stale state.
    //   3. ForceNetUpdate, so both land on the next net tick instead of whenever
    //      the actor would next have been considered.
    //
    // Phase 1 nudges, phase 2 restores. If phase 2 were ever missed the values stay
    // 0.0001 high, which is harmless and self-corrects on the next refresh.
    static constexpr float kEpsilon = 0.0001f;

    auto call_void_fn(UObject* obj, const CharType* fnName) -> bool
    {
        UFunction* fn = obj->GetFunctionByNameInChain(FName(fnName, FNAME_Find));
        if (!fn) return false;
        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        obj->ProcessEvent(fn, buf.data());
        return true;
    }

    // Adds `delta` to element 0's EffectValue. Returns false when the array cannot
    // be walked - the stride comes from UScriptStruct::PropertiesSize because
    // GetElementSize() reports 0 on this build.
    auto nudge_array(UObject* pawn, const CharType* propName, float delta) -> bool
    {
        FProperty* prop = pawn->GetPropertyByNameInChain(propName);
        auto* arrProp = CastField<FArrayProperty>(prop);
        if (!arrProp) return false;

        FScriptArrayHelper helper(arrProp, prop->ContainerPtrToValuePtr<void>(pawn));
        if (helper.Num() <= 0) return false;

        auto* inner = CastField<FStructProperty>(arrProp->GetInner());
        UScriptStruct* ss = inner ? inner->GetStruct() : nullptr;
        if (!ss || ss->GetPropertiesSize() <= 0) return false;

        FProperty* vp = ss->FindProperty(FName(STR("EffectValue"), FNAME_Find));
        if (!vp) return false;

        uint8* e0 = reinterpret_cast<uint8*>(helper.GetRawPtr(0));
        if (!e0) return false;

        float* v = std::bit_cast<float*>(e0 + vp->GetOffset_Internal());
        *v += delta;
        return true;
    }

    auto refresh_mutations(UObject* pawn, float delta, std::string& detail) -> bool
    {
        const bool a = nudge_array(pawn, STR("LifecycleMutationsList"), delta);
        const bool b = nudge_array(pawn, STR("LifecycleMutationsListAll"), delta);

        // Phase 1 (delta > 0) drops the flag, phase 2 raises it again.
        bool flagged = false;
        if (FProperty* fp = pawn->GetPropertyByNameInChain(STR("bIsMutationEnabled")))
        {
            *fp->ContainerPtrToValuePtr<uint8>(pawn) = (delta > 0.0f) ? 0 : 1;
            flagged = true;
        }

        const bool dorm = call_void_fn(pawn, STR("FlushNetDormancy"));
        const bool net  = call_void_fn(pawn, STR("ForceNetUpdate"));

        detail = std::string(delta > 0.0f ? "nudge" : "restore") +
                 ": list=" + (a ? "y" : "n") +
                 " listAll=" + (b ? "y" : "n") +
                 " flag=" + (flagged ? "y" : "n") +
                 " dormancy=" + (dorm ? "y" : "n") +
                 " forceNet=" + (net ? "y" : "n");
        return a || b || flagged;
    }

    // ------------------------------------------------------- corpse spawning
    //
    // MercyCorpse (Lua) decides WHEN a corpse is deserved; this is the HOW. The
    // spawn itself must live here: from Lua every actor-spawn path takes a
    // struct (FTransform) by value, and the Lua bridge mis-sizes by-value
    // parameters on this build — the FText crash class. UE4SS's own
    // UGameplayStatics wrapper is not used either: it picks a hardcoded params
    // layout by detected engine version, and version detection is exactly what
    // failed for FText. Instead the parameter block is built from the live
    // UFunction's own property offsets, the same rule the notify path lives by.

    auto scan_spawn_classes(std::string& detail) -> bool
    {
        static const char* pats[] = {"corpse", "carcass", "deer", "goat", "boar",
                                     "fish", "crab", "turtle", "meat", "food",
                                     "pickup", "spawner"};
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };

        std::string out;
        int shown = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || !obj->IsA<UClass>()) return LoopAction::Continue;
            StringType nw = obj->GetName();
            const std::string name = lower(std::string(nw.begin(), nw.end()));
            for (const char* p : pats)
            {
                if (name.find(p) != std::string::npos)
                {
                    StringType fw = obj->GetFullName();
                    out += "\\n" + std::string(fw.begin(), fw.end());
                    ++shown;
                    break;
                }
            }
            return shown >= 120 ? LoopAction::Break : LoopAction::Continue;
        });

        // What the game mode itself can do: a dedicated corpse- or AI-spawning
        // UFunction here would beat spawning a pawn and killing it.
        for (const CharType* gmName : kGameModeNames)
        {
            UObject* gm = UObjectGlobals::FindFirstOf(gmName);
            if (!gm || !gm->GetClassPrivate()) continue;
            out += "\\nGameMode fns:";
            int fns = 0;
            for (UFunction* f : gm->GetClassPrivate()->ForEachFunctionInChain())
            {
                if (!f) continue;
                StringType nw = f->GetName();
                const std::string n(nw.begin(), nw.end());
                const std::string ln = lower(n);
                if (ln.find("spawn") == std::string::npos && ln.find("corpse") == std::string::npos) continue;
                out += " " + n;
                if (++fns >= 80) break;
            }
            break;
        }

        detail = out.empty() ? "no matches" : out;
        return true;
    }

    // A short name ("BP_Deer_C") is matched against every loaded UClass; a
    // path ("/Game/....BP_Deer.BP_Deer_C") goes straight to StaticFindObject.
    // Either way only ALREADY-LOADED classes resolve — corpsescan shows what
    // those are.
    auto resolve_class(const std::string& spec) -> UClass*
    {
        const StringType wanted(spec.begin(), spec.end());
        if (spec.find('/') != std::string::npos)
        {
            return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, wanted);
        }
        UClass* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || !obj->IsA<UClass>()) return LoopAction::Continue;
            if (obj->GetName() == wanted)
            {
                found = static_cast<UClass*>(obj);
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });
        return found;
    }

    // Writes an identity-rotation, unit-scale transform with the given location
    // into an FTransform parameter slot. The layout is derived from the slot's
    // measured extent: 96 bytes is the UE5 double layout (quat @0, translation
    // @32, scale @64), 48 is the pre-LWC float layout (quat @0, translation
    // @16, scale @32). Anything else is refused by the caller.
    auto fill_transform(uint8* at, int32 extent, double x, double y, double z) -> void
    {
        if (extent == 96)
        {
            auto* q = std::bit_cast<double*>(at);
            q[3] = 1.0;
            auto* t = std::bit_cast<double*>(at + 32);
            t[0] = x; t[1] = y; t[2] = z;
            auto* s = std::bit_cast<double*>(at + 64);
            s[0] = s[1] = s[2] = 1.0;
        }
        else
        {
            auto* q = std::bit_cast<float*>(at);
            q[3] = 1.0f;
            auto* t = std::bit_cast<float*>(at + 16);
            t[0] = static_cast<float>(x); t[1] = static_cast<float>(y); t[2] = static_cast<float>(z);
            auto* s = std::bit_cast<float*>(at + 32);
            s[0] = s[1] = s[2] = 1.0f;
        }
    }

    // First live instance of a class named by short name or full path.
    auto first_instance_of(const std::string& spec) -> UObject*
    {
        UClass* cls = resolve_class(spec);
        if (!cls) return nullptr;
        UObject* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj == cls) return LoopAction::Continue;
            if (!obj->IsA(cls)) return LoopAction::Continue;
            // Skip the CDO: its values are the class defaults, not a live actor.
            StringType nw = obj->GetName();
            const std::string n(nw.begin(), nw.end());
            if (n.rfind("Default__", 0) == 0) return LoopAction::Continue;
            found = obj;
            return LoopAction::Break;
        });
        return found;
    }

    // Diagnostic: every property (in chain) of the first instance of `cls`,
    // with values for the scalar types. How we find out what a class we did
    // not write actually carries — property names are not guessable from Lua.
    auto prop_dump(const std::string& spec, std::string& detail) -> bool
    {
        UObject* obj = first_instance_of(spec);
        if (!obj) { detail = "no live instance of " + spec; return false; }

        StringType fw = obj->GetFullName();
        std::string out = std::string(fw.begin(), fw.end());

        int shown = 0;
        for (FProperty* p : obj->GetClassPrivate()->ForEachPropertyInChain())
        {
            if (!p || ++shown > 160) break;
            StringType nw = p->GetName();
            const std::string name(nw.begin(), nw.end());
            StringType tw = p->GetClass().GetName();
            const std::string type(tw.begin(), tw.end());
            uint8* at = p->ContainerPtrToValuePtr<uint8>(obj);

            char buf[128];
            if (type == "FloatProperty")
                std::snprintf(buf, sizeof(buf), "\\n%s[f]=%.3f", name.c_str(), *std::bit_cast<float*>(at));
            else if (type == "DoubleProperty")
                std::snprintf(buf, sizeof(buf), "\\n%s[d]=%.3f", name.c_str(), *std::bit_cast<double*>(at));
            else if (type == "IntProperty")
                std::snprintf(buf, sizeof(buf), "\\n%s[i]=%d", name.c_str(), *std::bit_cast<int32*>(at));
            else if (type == "BoolProperty")
                std::snprintf(buf, sizeof(buf), "\\n%s[b]=%d", name.c_str(), (int)(*at != 0));
            else if (type == "ByteProperty" || type == "EnumProperty")
                std::snprintf(buf, sizeof(buf), "\\n%s[e]=%d", name.c_str(), (int)*at);
            else if (type == "NameProperty")
            {
                StringType s = std::bit_cast<FName*>(at)->ToString();
                std::string v(s.begin(), s.end());
                std::snprintf(buf, sizeof(buf), "\\n%s[n]=%s", name.c_str(), v.c_str());
            }
            else if (type == "ObjectProperty")
            {
                UObject* o = *std::bit_cast<UObject**>(at);
                std::string v = "null";
                if (o)
                {
                    StringType s = o->GetName();
                    v.assign(s.begin(), s.end());
                }
                std::snprintf(buf, sizeof(buf), "\\n%s[o]=%s", name.c_str(), v.c_str());
            }
            else
                std::snprintf(buf, sizeof(buf), "\\n%s<%s>", name.c_str(), type.c_str());
            out += buf;
        }
        detail = out;
        return true;
    }

    // ------------------------------------------------------ prime zone visits
    //
    // WHY THIS EXISTS. Prime conditions 5 and 6 are tallies - "visit 2 migration
    // zones", "visit 4 patrol zones" - but FEligiblePrimeElder stores only the
    // finished boolean, which flips at 2/2 and 4/4 and never in between, and no
    // counter UPROPERTY exists anywhere on the pawn. The engine's own bump calls
    // (TIDinosaurBase:SetNumberOfPrimeCondition5/6) are SCRIPT UFunctions, so
    // UE4SS cannot hook them here: that needs the ProcessInternal detour, and
    // ProcessInternal has no resolved address in UE4SS_Addresses.ini.
    //
    // The zones themselves hold the answer. Each is a TIEdibleSpawner
    // (BP_EdiblePlantsSpawnable_C) carrying MigrationVisitorIDs and
    // PatrolVisitorIDs; the count of zones listing a given dino IS that dino's
    // progress. Reading them needs C++ - Lua TArray indexing is broken on this
    // build - which is what this verb is for.
    //
    // THIS PASS IS DIAGNOSTIC ONLY. Two things are still unknown and both have
    // to be answered from live data before any counting can be trusted:
    //   1. the arrays' INNER TYPE (int32? FString? FName?), and
    //   2. what a "visitor id" actually is - the pawn propdump shows FatherId,
    //      MotherId, AncestorIds and IdPrefix but no obvious self-id.
    // So rather than guess, this collects every visitor id in the world and then
    // walks the pawn's own integer properties looking for one whose value is in
    // that set. If the player has visited a zone, that match NAMES the id
    // property outright, and the counting version writes itself.
    //
    // Stride is hand-computed on purpose: GetInner()->GetElementSize() returns 0
    // on this build (see dump_struct_array), so FScriptArrayHelper cannot stride
    // itself. Num() is safe - it reads the FScriptArray header only.
    struct ArrInfo { int32 num = 0; std::string inner; std::vector<int64> ints; };

    // One visitor array off one zone actor. Only integer inners are decoded;
    // anything else is reported by type name and left alone, because misreading
    // an FString array as ints would be silent garbage. (Live data says
    // IntProperty for both arrays, so the other branches are belt and braces.)
    //
    // `cap` bounds how many elements are decoded - the diagnostic wants a sample,
    // the publisher wants all of them.
    auto read_visitor_array(UObject* obj, const CharType* name, int32 cap) -> ArrInfo
    {
        ArrInfo info;
        FProperty* prop = obj->GetPropertyByNameInChain(name);
        if (!prop) return info;
        auto* arrProp = CastField<FArrayProperty>(prop);
        if (!arrProp) { info.inner = "<not an array>"; return info; }

        FProperty* inner = arrProp->GetInner();
        if (inner)
        {
            StringType iw = inner->GetClass().GetName();
            info.inner.assign(iw.begin(), iw.end());
        }

        FScriptArrayHelper helper(arrProp, prop->ContainerPtrToValuePtr<void>(obj));
        info.num = helper.Num();
        if (info.num <= 0) return info;

        int32 stride = 0;
        if      (info.inner == "IntProperty")   stride = 4;
        else if (info.inner == "Int64Property") stride = 8;
        if (stride != 4 && stride != 8) return info;   // decode ints only

        uint8* base = reinterpret_cast<uint8*>(helper.GetRawPtr(0));
        if (!base) return info;
        const int32 n = (cap > 0 && info.num > cap) ? cap : info.num;
        for (int32 i = 0; i < n; ++i)
        {
            uint8* e = base + static_cast<size_t>(i) * static_cast<size_t>(stride);
            info.ints.push_back(stride == 4 ? static_cast<int64>(*std::bit_cast<int32*>(e))
                                            : *std::bit_cast<int64*>(e));
        }
        return info;
    }

    // Discover all zone actors (those with MigrationVisitorIDs OR PatrolVisitorIDs)
    // in one full ForEachUObject scan. Cached and never rebuilt since zones don't
    // spawn/despawn. Only called on first publish_zone_visits() tick.
    //
    // Gating on MigrationVisitorIDs alone (the original version of this function)
    // silently dropped every actor that carries PatrolVisitorIDs but not
    // MigrationVisitorIDs - live evidence (zone_visits diagnostic, run while an
    // admin stood inside a Patrol Zone) showed patNonEmpty=0 across all 82
    // Migration-gated actors even with an active visit in progress, meaning
    // Patrol Zones are not all instances of the same class as Migration Zones.
    // The OR gate below is the general fix regardless of which class turns out
    // to carry which property.
    auto discover_zones() -> void
    {
        if (m_zones_discovered) return;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj->IsA<UClass>()) return LoopAction::Continue;
            if (!obj->GetPropertyByNameInChain(STR("MigrationVisitorIDs")) &&
                !obj->GetPropertyByNameInChain(STR("PatrolVisitorIDs"))) return LoopAction::Continue;
            m_zones.push_back(obj);
            return LoopAction::Continue;
        });
        m_zones_discovered = true;
    }

    // dino ID -> {migration zones visited, patrol zones visited}. Reads visitor
    // arrays from cached zones, no full-object-array scan.
    //
    // MUST run on the game thread - this touches live UObjects. It is called from
    // drain(), which is the engine-tick post-hook, never from the tailer thread.
    auto collect_zone_visits(std::unordered_map<int64, std::pair<int, int>>& out) -> int
    {
        discover_zones();  // one-time discovery on first call
        for (UObject* zone : m_zones) {
            if (!zone) continue;  // object may have been destroyed
            for (int64 v : read_visitor_array(zone, STR("MigrationVisitorIDs"), 0).ints)
                ++out[v].first;
            for (int64 v : read_visitor_array(zone, STR("PatrolVisitorIDs"), 0).ints)
                ++out[v].second;
        }
        return m_zones.size();
    }

    // Publish those counts for PrimeWatch, which does the per-player half: Lua can
    // read a pawn's ID (an IntProperty - verified reading back as a number) but
    // NOT a TArray, so C++ contributes exactly the part Lua cannot do and stays
    // out of the messaging, baselining and dedupe that PrimeWatch already owns.
    //
    // Written whole to a temp file and renamed, so a reader never sees half a
    // file - the same discipline dinoStorage.ts uses for storage.json.
    auto publish_zone_visits() -> void
    {
        const double now = steady_seconds();
        if (m_zone_at > 0.0 && (now - m_zone_at) < kZoneVisitsIntervalSeconds) return;
        m_zone_at = now;

        std::unordered_map<int64, std::pair<int, int>> counts;
        const int zones = collect_zone_visits(counts);

        std::string body = "{\"zones\":" + std::to_string(zones) + ",\"ids\":{";
        bool first = true;
        for (const auto& [id, c] : counts)
        {
            if (!first) body += ",";
            first = false;
            body += "\"" + std::to_string(id) + "\":[" +
                    std::to_string(c.first) + "," + std::to_string(c.second) + "]";
        }
        body += "}}";

        const std::string tmp = std::string(kZoneVisitsPath) + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb"))
        {
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
            std::rename(tmp.c_str(), kZoneVisitsPath);
        }
    }

    auto zone_visits(const std::string& steam, std::string& detail) -> bool
    {
        int zones = 0, migNonEmpty = 0, patNonEmpty = 0;
        int migOnlyClass = 0, patOnlyClass = 0, bothClass = 0;
        // Activation state: BP_MigrationManager assigns bMigrateHere to whichever
        // zone(s) are the current live target(s); NumberOfActivations is that
        // zone's lifetime activation count. A zone with bMigrateHere=false and
        // NumberOfActivations=0 has never been selected, so standing inside it
        // - for any duration - writes nothing, regardless of dwell time.
        int activeNow = 0, everActivated = 0;
        std::string activeSample;
        std::string innerMig = "?", innerPat = "?";
        std::unordered_set<int64> allIds;
        std::string sample, patOnlySample;

        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj->IsA<UClass>()) return LoopAction::Continue;
            const bool hasMig = obj->GetPropertyByNameInChain(STR("MigrationVisitorIDs")) != nullptr;
            const bool hasPat = obj->GetPropertyByNameInChain(STR("PatrolVisitorIDs")) != nullptr;
            if (!hasMig && !hasPat) return LoopAction::Continue;
            ++zones;
            if (hasMig && hasPat) ++bothClass;
            else if (hasMig) ++migOnlyClass;
            else ++patOnlyClass;

            bool migrateHere = false;
            get_bool_prop(obj, STR("bMigrateHere"), migrateHere);
            int32 activations = 0;
            if (FProperty* ap = obj->GetPropertyByNameInChain(STR("NumberOfActivations")))
                activations = *ap->ContainerPtrToValuePtr<int32>(obj);
            if (migrateHere) ++activeNow;
            if (activations > 0) ++everActivated;
            if (migrateHere && activeSample.size() < 300)
            {
                StringType nw = obj->GetName();
                activeSample += "\\n  ACTIVE " + std::string(nw.begin(), nw.end()) +
                                 " activations=" + std::to_string(activations);
            }

            ArrInfo m = hasMig ? read_visitor_array(obj, STR("MigrationVisitorIDs"), 32) : ArrInfo{};
            ArrInfo p = hasPat ? read_visitor_array(obj, STR("PatrolVisitorIDs"), 32) : ArrInfo{};
            if (innerMig == "?" && !m.inner.empty()) innerMig = m.inner;
            if (innerPat == "?" && !p.inner.empty()) innerPat = p.inner;
            if (m.num > 0) ++migNonEmpty;
            if (p.num > 0) ++patNonEmpty;
            for (int64 v : m.ints) allIds.insert(v);
            for (int64 v : p.ints) allIds.insert(v);

            if ((m.num > 0 || p.num > 0) && sample.size() < 400)
            {
                StringType nw = obj->GetName();
                sample += "\\n  " + std::string(nw.begin(), nw.end()) +
                          " mig=" + std::to_string(m.num) + " pat=" + std::to_string(p.num);
                for (size_t i = 0; i < m.ints.size() && i < 4; ++i)
                    sample += (i ? "," : " migIds=") + std::to_string(m.ints[i]);
                for (size_t i = 0; i < p.ints.size() && i < 4; ++i)
                    sample += (i ? "," : " patIds=") + std::to_string(p.ints[i]);
            }
            // A patrol-only class is exactly the case the old Migration-only gate
            // dropped silently. Name it so we know for certain what it is.
            if (!hasMig && hasPat && patOnlySample.size() < 200)
            {
                UClass* cls = obj->GetClassPrivate();
                StringType cn = cls ? cls->GetName() : STR("<noclass>");
                patOnlySample += "\\n  patrolOnlyClass=" + std::string(cn.begin(), cn.end()) +
                                  " pat=" + std::to_string(p.num);
            }
            return LoopAction::Continue;
        });

        detail = "zones=" + std::to_string(zones) +
                 " migOnlyClass=" + std::to_string(migOnlyClass) +
                 " patOnlyClass=" + std::to_string(patOnlyClass) +
                 " bothClass=" + std::to_string(bothClass) +
                 " activeNow=" + std::to_string(activeNow) +
                 " everActivated=" + std::to_string(everActivated) +
                 " migNonEmpty=" + std::to_string(migNonEmpty) +
                 " patNonEmpty=" + std::to_string(patNonEmpty) +
                 " innerMig=" + innerMig + " innerPat=" + innerPat +
                 " distinctIds=" + std::to_string(allIds.size()) + sample + activeSample + patOnlySample;

        if (steam.empty() || steam == "0") return true;

        // Now the other half: which property on this player's dino carries a
        // value that appears in a visitor array. That is the id, by definition.
        UObject* ctrl = controller_for(steam);
        if (!ctrl) { detail += "\\nplayer: no controller for " + steam; return true; }
        UObject* pawn = nullptr;
        if (UFunction* getPawn = ctrl->GetFunctionByNameInChain(FName(STR("K2_GetPawn"), FNAME_Find)))
        {
            std::vector<uint8> pb(getPawn->GetParmsSize(), 0);
            ctrl->ProcessEvent(getPawn, pb.data());
            if (FProperty* ret = getPawn->GetReturnProperty())
                pawn = *std::bit_cast<UObject**>(pb.data() + ret->GetOffset_Internal());
        }
        if (!pawn) { detail += "\\nplayer: no live pawn"; return true; }

        std::string hits, ints;
        UClass* cls = pawn->GetClassPrivate();
        if (cls)
        {
            for (FProperty* p : cls->ForEachPropertyInChain())
            {
                if (!p) continue;
                StringType tw = p->GetClass().GetName();
                const std::string type(tw.begin(), tw.end());
                if (type != "IntProperty" && type != "Int64Property") continue;
                const int64 v = (type == "IntProperty")
                    ? static_cast<int64>(*p->ContainerPtrToValuePtr<int32>(pawn))
                    : *p->ContainerPtrToValuePtr<int64>(pawn);
                if (v == 0) continue;   // 0 is every uninitialised int, not an id
                StringType nw = p->GetName();
                const std::string nm(nw.begin(), nw.end());
                if (ints.size() < 300) ints += " " + nm + "=" + std::to_string(v);
                if (allIds.count(v)) hits += " " + nm + "=" + std::to_string(v);
            }
        }
        detail += "\\nplayer " + steam + " idMatches:" + (hits.empty() ? " <none>" : hits);
        detail += "\\nplayer intProps:" + (ints.empty() ? " <none>" : ints);
        return true;
    }

    // Diagnostic: one UFunction's full parameter layout - name, type, offset,
    // measured extent, and whether it is the return/out param. spec is
    // "Class:Function" with short names, or a full "/Script/..." path. This is
    // how a candidate call (say, an announcement RPC that might carry a
    // Duration) is judged safe to invoke before any buffer is ever built for it.
    auto fn_signature(const std::string& spec, std::string& detail) -> bool
    {
        UFunction* fn = nullptr;
        if (spec.find('/') != std::string::npos)
        {
            fn = UObjectGlobals::StaticFindObject<UFunction*>(
                nullptr, nullptr, StringType(spec.begin(), spec.end()));
        }
        else
        {
            const size_t colon = spec.find(':');
            if (colon == std::string::npos) { detail = "spec must be Class:Function"; return false; }
            UClass* cls = resolve_class(spec.substr(0, colon));
            if (!cls) { detail = "class not found: " + spec.substr(0, colon); return false; }
            const std::string want = spec.substr(colon + 1);
            const StringType wantW(want.begin(), want.end());
            for (UFunction* f : cls->ForEachFunctionInChain())
            {
                if (f && f->GetName() == wantW) { fn = f; break; }
            }
        }
        if (!fn) { detail = "function not found: " + spec; return false; }

        std::string out = "parms_size=" + std::to_string(static_cast<int>(fn->GetParmsSize()));
        for (FProperty* p : fn->ForEachProperty())
        {
            if (!p) continue;
            StringType nw = p->GetName();
            StringType tw = p->GetClass().GetName();
            out += "\\n" + std::string(nw.begin(), nw.end()) +
                   "[" + std::string(tw.begin(), tw.end()) + "]" +
                   " @" + std::to_string(p->GetOffset_Internal()) +
                   " extent " + std::to_string(param_extent(fn, p));
            if (p->HasAnyPropertyFlags(CPF_ReturnParm)) out += " RETURN";
            else if (p->HasAnyPropertyFlags(CPF_OutParm)) out += " OUT";
        }
        detail = out;
        return true;
    }

    // Diagnostic: every UFunction (in chain) of a class, names only.
    auto class_fns(const std::string& spec, std::string& detail) -> bool
    {
        UClass* cls = resolve_class(spec);
        if (!cls) { detail = "class not found: " + spec; return false; }

        std::string out;
        int shown = 0;
        for (UFunction* f : cls->ForEachFunctionInChain())
        {
            if (!f || ++shown > 200) break;
            StringType nw = f->GetName();
            out += (shown % 6 == 1 ? "\\n" : " ") + std::string(nw.begin(), nw.end());
        }
        detail = out.empty() ? "no functions" : out;
        return true;
    }

    // Calls a UFunction whose interesting parameter is a single bool; every
    // other parameter stays zeroed. Bool params go through FBoolProperty so a
    // bitfield-packed flag cannot clobber its neighbours.
    auto call_bool_fn(UObject* obj, const CharType* fnName, bool value) -> bool
    {
        UFunction* fn = obj->GetFunctionByNameInChain(FName(fnName, FNAME_Find));
        if (!fn) return false;
        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        for (FProperty* p : fn->ForEachProperty())
        {
            if (auto* bp = CastField<FBoolProperty>(p))
            {
                bp->SetPropertyValue(buf.data() + bp->GetOffset_Internal(), value);
                break;
            }
        }
        obj->ProcessEvent(fn, buf.data());
        return true;
    }

    // Calls a UFunction whose interesting parameter is its first numeric one,
    // written at the width the live property declares. Bool params stay 0 —
    // which is exactly what ActivateDeadbody(false, seconds) needs.
    auto call_num_fn(UObject* obj, const CharType* fnName, double value) -> bool
    {
        UFunction* fn = obj->GetFunctionByNameInChain(FName(fnName, FNAME_Find));
        if (!fn) return false;
        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        for (FProperty* p : fn->ForEachProperty())
        {
            StringType tw = p->GetClass().GetName();
            const std::string t(tw.begin(), tw.end());
            if (t == "FloatProperty")
            {
                *std::bit_cast<float*>(buf.data() + p->GetOffset_Internal()) = static_cast<float>(value);
                break;
            }
            if (t == "DoubleProperty")
            {
                *std::bit_cast<double*>(buf.data() + p->GetOffset_Internal()) = value;
                break;
            }
        }
        obj->ProcessEvent(fn, buf.data());
        return true;
    }

    auto set_bool_prop(UObject* obj, const CharType* name, bool v) -> bool
    {
        FProperty* p = obj->GetPropertyByNameInChain(name);
        if (!p) return false;
        if (auto* bp = CastField<FBoolProperty>(p))
        {
            bp->SetPropertyValueInContainer(obj, v);
            return true;
        }
        *p->ContainerPtrToValuePtr<uint8>(obj) = v ? 1 : 0;
        return true;
    }

    auto get_bool_prop(UObject* obj, const CharType* name, bool& out) -> bool
    {
        FProperty* p = obj->GetPropertyByNameInChain(name);
        if (!p) return false;
        if (auto* bp = CastField<FBoolProperty>(p))
        {
            out = bp->GetPropertyValue(p->ContainerPtrToValuePtr<void>(obj));
            return true;
        }
        out = *p->ContainerPtrToValuePtr<uint8>(obj) != 0;
        return true;
    }

    auto get_num_prop(UObject* obj, const CharType* name, double& out) -> bool
    {
        FProperty* p = obj->GetPropertyByNameInChain(name);
        if (!p) return false;
        StringType tw = p->GetClass().GetName();
        const std::string t(tw.begin(), tw.end());
        uint8* at = p->ContainerPtrToValuePtr<uint8>(obj);
        if (t == "FloatProperty")  { out = *std::bit_cast<float*>(at);  return true; }
        if (t == "DoubleProperty") { out = *std::bit_cast<double*>(at); return true; }
        return false;
    }

    // Calls a UFunction that takes nothing and returns a number, and hands back
    // the return value. call_num_fn WRITES a number into a parameter; this READS
    // one out, which nothing else here needed until corpse ages. The return
    // property's declared width is honoured rather than assumed - the same rule
    // measure_clearance follows for its vector return.
    auto call_num_ret(UObject* obj, const CharType* fnName, double& out) -> bool
    {
        UFunction* fn = obj->GetFunctionByNameInChain(FName(fnName, FNAME_Find));
        if (!fn) return false;
        FProperty* r = fn->GetReturnProperty();
        if (!r) return false;
        StringType tw = r->GetClass().GetName();
        const std::string t(tw.begin(), tw.end());
        const int32 width = (t == "FloatProperty") ? 4 : (t == "DoubleProperty") ? 8 : 0;
        if (width == 0) return false;
        const int32 parms = static_cast<int32>(fn->GetParmsSize());
        const int32 at_off = r->GetOffset_Internal();
        // Refuse anything whose return would read past the parameter buffer, and
        // anything with real parameters: a zeroed buffer is only a safe call when
        // the return value is all there is.
        if (at_off < 0 || parms < at_off + width || parms > 16) return false;
        std::vector<uint8> buf(static_cast<size_t>(parms), 0);
        obj->ProcessEvent(fn, buf.data());
        uint8* at = buf.data() + at_off;
        out = (width == 4) ? static_cast<double>(*std::bit_cast<float*>(at))
                           : *std::bit_cast<double*>(at);
        return true;
    }

    // Ground under (x, y), per EVRIMA_BodyDrop_Architecture.md: "never a blind
    // vertical offset" — SpawnActor succeeds in the void below the landscape
    // and the corpse is buried where nobody can eat it. Traces straight down
    // through the anchor player's altitude on the Visibility channel and
    // returns the impact Z. Refusing on a miss is deliberate: no hit means the
    // candidate XY is off the landscape, and the right move is not to spawn.
    auto trace_ground(UObject* world_ctx, double x, double y, double z_ref,
                      double& out_z, std::string& note) -> bool
    {
        UObject* ksl = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:LineTraceSingle"));
        if (!ksl || !fn) { note = "LineTraceSingle unresolvable"; return false; }

        FProperty* p_ctx = fn->FindProperty(FName(STR("WorldContextObject"), FNAME_Find));
        FProperty* p_start = fn->FindProperty(FName(STR("Start"), FNAME_Find));
        FProperty* p_end = fn->FindProperty(FName(STR("End"), FNAME_Find));
        FProperty* p_hit = fn->FindProperty(FName(STR("OutHit"), FNAME_Find));
        FProperty* p_ret = fn->GetReturnProperty();
        if (!p_ctx || !p_start || !p_end || !p_hit || !p_ret)
        {
            note = "LineTraceSingle signature unexpected";
            return false;
        }

        const int32 vec = param_extent(fn, p_start);
        if (vec != 24 && vec != 12) { note = "FVector extent " + std::to_string(vec); return false; }

        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        *std::bit_cast<UObject**>(buf.data() + p_ctx->GetOffset_Internal()) = world_ctx;

        auto write_vec = [&](FProperty* p, double vx, double vy, double vz) {
            uint8* at = buf.data() + p->GetOffset_Internal();
            if (vec == 24)
            {
                auto* d = std::bit_cast<double*>(at);
                d[0] = vx; d[1] = vy; d[2] = vz;
            }
            else
            {
                auto* f = std::bit_cast<float*>(at);
                f[0] = (float)vx; f[1] = (float)vy; f[2] = (float)vz;
            }
        };
        write_vec(p_start, x, y, z_ref + 10000.0);
        write_vec(p_end, x, y, z_ref - 30000.0);
        // TraceChannel 0 (Visibility), bTraceComplex false, ActorsToIgnore
        // zeroed = empty TArray, DrawDebugType 0 (None): all already correct in
        // the zero-filled buffer.

        ksl->ProcessEvent(fn, buf.data());

        bool hit = false;
        if (auto* bp = CastField<FBoolProperty>(p_ret))
            hit = bp->GetPropertyValue(buf.data() + p_ret->GetOffset_Internal());
        else
            hit = buf[p_ret->GetOffset_Internal()] != 0;
        if (!hit) { note = "no ground hit at candidate XY"; return false; }

        auto* sp = CastField<FStructProperty>(p_hit);
        UScriptStruct* ss = sp ? sp->GetStruct() : nullptr;
        FProperty* ip = ss ? ss->FindProperty(FName(STR("ImpactPoint"), FNAME_Find)) : nullptr;
        if (!ip) { note = "FHitResult.ImpactPoint not found"; return false; }

        uint8* at = buf.data() + p_hit->GetOffset_Internal() + ip->GetOffset_Internal();
        out_z = (vec == 24) ? std::bit_cast<double*>(at)[2]
                            : static_cast<double>(std::bit_cast<float*>(at)[2]);
        return true;
    }

    auto spawn_corpse(const Action& act, std::string& detail) -> bool
    {
        if (act.cls.empty()) { detail = "no cls given"; return false; }
        if (act.x == 0.0 && act.y == 0.0 && act.z == 0.0)
        {
            detail = "all-zero location - refusing to spawn at map origin";
            return false;
        }

        // cls is a comma-separated candidate list; the first LOADED class wins.
        // MercyCorpse sends its preferred species first and the anchor player's
        // own class last, so a server where no candidate species is loaded
        // still yields a corpse (same-species, cannibal-diet flavoured).
        UClass* cls = nullptr;
        std::string chosen;
        {
            size_t from = 0;
            while (from <= act.cls.size() && !cls)
            {
                size_t comma = act.cls.find(',', from);
                if (comma == std::string::npos) comma = act.cls.size();
                std::string cand = act.cls.substr(from, comma - from);
                if (!cand.empty())
                {
                    cls = resolve_class(cand);
                    if (cls) chosen = cand;
                }
                from = comma + 1;
            }
        }
        if (!cls) { detail = "no candidate class is loaded: " + act.cls; return false; }

        UObject* world_ctx = nullptr;
        for (const CharType* name : kGameModeNames)
        {
            world_ctx = UObjectGlobals::FindFirstOf(name);
            if (world_ctx) break;
        }
        if (!world_ctx) { detail = "no game mode for world context"; return false; }

        double ground_z = 0.0;
        std::string trace_note;
        if (!trace_ground(world_ctx, act.x, act.y, act.z, ground_z, trace_note))
        {
            detail = "ground trace failed: " + trace_note;
            return false;
        }
        // 300 units above the hit, per the BodyDrop doc; the ragdoll settles.
        const double spawn_z = ground_z + 300.0;

        UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        UFunction* begin_fn = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"));
        UFunction* finish_fn = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.GameplayStatics:FinishSpawningActor"));
        if (!statics || !begin_fn || !finish_fn)
        {
            detail = "GameplayStatics spawn functions not resolvable";
            return false;
        }

        FProperty* p_ctx = begin_fn->FindProperty(FName(STR("WorldContextObject"), FNAME_Find));
        FProperty* p_cls = begin_fn->FindProperty(FName(STR("ActorClass"), FNAME_Find));
        FProperty* p_xform = begin_fn->FindProperty(FName(STR("SpawnTransform"), FNAME_Find));
        FProperty* p_coll = begin_fn->FindProperty(FName(STR("CollisionHandlingOverride"), FNAME_Find));
        FProperty* p_ret = begin_fn->GetReturnProperty();
        if (!p_ctx || !p_cls || !p_xform || !p_ret)
        {
            detail = "BeginDeferredActorSpawnFromClass signature unexpected";
            return false;
        }

        const int32 xf_extent = param_extent(begin_fn, p_xform);
        if (xf_extent != 96 && xf_extent != 48)
        {
            // Refuse rather than guess: writing a layout we did not derive from
            // the live function is the overflow class this mod exists to avoid.
            detail = "SpawnTransform extent " + std::to_string(xf_extent) + " is not a known FTransform layout";
            return false;
        }

        std::vector<uint8> buf(begin_fn->GetParmsSize(), 0);
        *std::bit_cast<UObject**>(buf.data() + p_ctx->GetOffset_Internal()) = world_ctx;
        *std::bit_cast<UClass**>(buf.data() + p_cls->GetOffset_Internal()) = cls;
        fill_transform(buf.data() + p_xform->GetOffset_Internal(), xf_extent, act.x, act.y, spawn_z);
        if (p_coll) buf[p_coll->GetOffset_Internal()] = 1;   // AlwaysSpawn

        statics->ProcessEvent(begin_fn, buf.data());

        UObject* actor = *std::bit_cast<UObject**>(buf.data() + p_ret->GetOffset_Internal());
        if (!actor) { detail = "BeginDeferredActorSpawnFromClass returned null"; return false; }

        FProperty* f_actor = finish_fn->FindProperty(FName(STR("Actor"), FNAME_Find));
        FProperty* f_xform = finish_fn->FindProperty(FName(STR("SpawnTransform"), FNAME_Find));
        if (!f_actor || !f_xform)
        {
            detail = "FinishSpawningActor signature unexpected";
            return false;
        }
        const int32 fin_extent = param_extent(finish_fn, f_xform);
        if (fin_extent != xf_extent)
        {
            detail = "FinishSpawningActor transform extent " + std::to_string(fin_extent) +
                     " != " + std::to_string(xf_extent);
            return false;
        }

        std::vector<uint8> fin(finish_fn->GetParmsSize(), 0);
        *std::bit_cast<UObject**>(fin.data() + f_actor->GetOffset_Internal()) = actor;
        fill_transform(fin.data() + f_xform->GetOffset_Internal(), fin_extent, act.x, act.y, spawn_z);
        statics->ProcessEvent(finish_fn, fin.data());

        StringType fw = actor->GetFullName();
        detail = "spawned " + std::string(fw.begin(), fw.end()) +
                 " at z=" + std::to_string(spawn_z) + " (ground " + std::to_string(ground_z) + ")";

        // Corpse transition, in the exact order EVRIMA_BodyDrop_Architecture.md
        // prescribes: "ActivateDeadbody alone does not kill; it is post-death
        // cleanup that expects bIsDead=true already, and SetHealth(0) alone
        // leaves the pawn in idle animation." Growth is set BEFORE the death
        // flags because it drives both corpse mesh size and food value. The
        // engine's own corpse decay destroys the actor afterwards (the doc is
        // explicit that mod-side cleanup on a stored pointer crashes), so the
        // pointer is dropped at the end of this function and never kept.
        if (act.kill)
        {
            const bool rep  = call_bool_fn(actor, STR("SetReplicates"), true);
            const bool grw  = call_num_fn(actor, STR("SetGrowth"), act.g);
            const bool hp   = call_void_fn(actor, STR("SetHealth"));   // zeroed buffer = 0 health
            const bool dead = set_bool_prop(actor, STR("bIsDead"), true);
            const bool onr  = call_void_fn(actor, STR("OnRep_IsNowDead"));
            const bool rag  = call_bool_fn(actor, STR("ToggleServerRagdoll"), true);
            const bool dbd  = call_num_fn(actor, STR("ActivateDeadbody"), kCorpseDecaySeconds);
            const bool net  = call_void_fn(actor, STR("ForceNetUpdate"));
            detail += std::string(", corpse seq: rep=") + (rep ? "y" : "n") +
                      " growth=" + (grw ? "y" : "n") +
                      " hp0=" + (hp ? "y" : "n") +
                      " bIsDead=" + (dead ? "y" : "n") +
                      " onrep=" + (onr ? "y" : "n") +
                      " ragdoll=" + (rag ? "y" : "n") +
                      " deadbody=" + (dbd ? "y" : "n") +
                      " forcenet=" + (net ? "y" : "n");
        }
        return true;
    }

    // -------------------------------------------------------- corpse wiping
    //
    // The CorpseWipe Lua mod decides WHEN (a fixed cycle with announce
    // warnings); this is the HOW — the same split as spawncorpse. A corpse on
    // this build is a TICharacterBase pawn whose bIsDead is set: player
    // deaths, AI deaths and spawncorpse bodies all end in that state, so one
    // filter collects them all. Pawns still possessed by a controller are
    // skipped — a freshly-dead player can stay attached to their body on the
    // death screen, and destroying it under them is an untested client path;
    // the next cycle collects the body once they release. Candidates are
    // gathered first and destroyed after the walk so K2_DestroyActor never
    // mutates the object array mid-enumeration.
    // Every wipeable body on the map right now, paired with its object name (the
    // census keys on that). Shared by the wipe and by the census that ages them,
    // so one filter defines "corpse" for both and they can never disagree.
    auto collect_corpses(std::vector<std::pair<UObject*, std::string>>& out, int& held) -> bool
    {
        UClass* cls = resolve_class("TICharacterBase");
        if (!cls) return false;

        held = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj == cls || !obj->IsA(cls)) return LoopAction::Continue;
            if (obj->IsUnreachable() ||
                obj->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
                return LoopAction::Continue;
            StringType nw = obj->GetName();
            const std::string n(nw.begin(), nw.end());
            if (n.rfind("Default__", 0) == 0) return LoopAction::Continue;

            bool dead = false;
            if (!get_bool_prop(obj, STR("bIsDead"), dead) || !dead) return LoopAction::Continue;

            if (FProperty* pc = obj->GetPropertyByNameInChain(STR("Controller")))
            {
                if (*pc->ContainerPtrToValuePtr<UObject*>(obj) != nullptr)
                {
                    ++held;
                    return LoopAction::Continue;
                }
            }
            out.emplace_back(obj, n);
            return LoopAction::Continue;
        });
        return true;
    }

    // Seconds this body has been on the floor, from the engine's own clock, or
    // -1 when it cannot be established.
    //
    // GetCorpseStateEnterTime() is a world-seconds stamp (probed live: no
    // parameters, float return). World seconds "now" is recovered from the same
    // actor as CreationTime + GetGameTimeSinceCreation(), which avoids needing a
    // UWorld pointer this mod does not otherwise carry.
    //
    // The stamp is per corpse STAGE, so it resets when a body turns
    // not-fresh/rotten/bone. That is harmless for a freshness gate: a body that
    // has changed stage is already older than the fresh stage lasts, so it stays
    // wipe-eligible either way. Only the first stage's age has to be right.
    auto corpse_age_seconds(UObject* corpse) -> double
    {
        double since = 0.0, created = 0.0, enter = 0.0;
        if (!call_num_ret(corpse, STR("GetGameTimeSinceCreation"), since)) return -1.0;
        if (!get_num_prop(corpse, STR("CreationTime"), created)) return -1.0;
        if (!call_num_ret(corpse, STR("GetCorpseStateEnterTime"), enter)) return -1.0;

        const double now = created + since;
        // enter <= 0 means the stamp was never written for this body; a stamp in
        // the future means it is not the clock we think it is. Either way the
        // census answers instead of this guessing.
        if (enter <= 0.0 || enter > now + 1.0) return -1.0;
        const double age = now - enter;
        if (age < 0.0 || age > 86400.0) return -1.0;
        return age;
    }

    static auto steady_seconds() -> double
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // First-seen bookkeeping, the fallback for anything corpse_age_seconds
    // cannot date. Runs on its own cadence because the wipe alone is 20 minutes
    // apart - far too rare for any age to accrue between calls.
    auto census_corpses() -> void
    {
        const double now = steady_seconds();
        if (m_census_at > 0.0 && (now - m_census_at) < kCensusIntervalSeconds) return;
        m_census_at = now;

        std::vector<std::pair<UObject*, std::string>> corpses;
        int held = 0;
        if (!collect_corpses(corpses, held)) return;

        // Bodies present at the FIRST census predate this mod's load (a .so
        // reload must not hand every standing corpse a fresh grace period), so
        // they are stamped as already ancient.
        const double stamp = m_census_primed ? now : (now - 86400.0);

        std::unordered_set<std::string> live;
        live.reserve(corpses.size());
        for (auto& [obj, name] : corpses)
        {
            live.insert(name);
            m_corpse_seen.try_emplace(name, stamp);
        }
        for (auto it = m_corpse_seen.begin(); it != m_corpse_seen.end();)
            it = (live.count(it->first) == 0) ? m_corpse_seen.erase(it) : std::next(it);

        m_census_primed = true;
    }

    // min_age is a grace period in seconds: a body younger than it survives this
    // wipe and is collected by the next one. 0 keeps the original age-blind
    // behaviour, so an older CorpseWipe that sends no min_age is unaffected.
    auto wipe_corpses(double min_age, std::string& detail) -> bool
    {
        std::vector<std::pair<UObject*, std::string>> corpses;
        int held = 0;
        if (!collect_corpses(corpses, held)) { detail = "TICharacterBase class not found"; return false; }

        const double now = steady_seconds();
        std::vector<UObject*> doomed;
        int spared = 0, by_engine = 0, by_census = 0, undated = 0;

        for (auto& [obj, name] : corpses)
        {
            if (min_age <= 0.0) { doomed.push_back(obj); continue; }

            double age = corpse_age_seconds(obj);
            if (age >= 0.0) ++by_engine;
            else if (auto it = m_corpse_seen.find(name); it != m_corpse_seen.end())
            {
                age = now - it->second;
                ++by_census;
            }
            else ++undated;

            // An undated body has never been censused, so it appeared within the
            // last census interval and is by definition brand new. Sparing is the
            // recoverable mistake here - the next cycle collects it - whereas a
            // wrong destroy cannot be undone.
            if (age < 0.0 || age < min_age) { ++spared; continue; }
            doomed.push_back(obj);
        }

        int wiped = 0;
        for (UObject* c : doomed)
            if (call_void_fn(c, STR("K2_DestroyActor"))) ++wiped;

        detail = "wiped " + std::to_string(wiped) + "/" + std::to_string(doomed.size()) +
                 " corpse(s), " + std::to_string(held) + " possessed skipped";
        if (min_age > 0.0)
        {
            detail += ", " + std::to_string(spared) + " spared as fresh (<" +
                      std::to_string(static_cast<int>(min_age)) + "s)" +
                      " [aged: engine=" + std::to_string(by_engine) +
                      " census=" + std::to_string(by_census) +
                      " undated=" + std::to_string(undated) + "]";
        }
        return true;
    }

    // The clearance a pawn has RIGHT NOW: read the current location, trace the
    // ground underneath it, keep the difference. A standing pawn measures its
    // own capsule half-height this way - which nothing else on this build will
    // report: GetSimpleCollisionCylinder read back nothing on the first live
    // run. Returns -1 when the measure cannot be established.
    auto measure_clearance(UObject* pawn) -> double
    {
        UFunction* getLoc = pawn->GetFunctionByNameInChain(FName(STR("K2_GetActorLocation"), FNAME_Find));
        if (!getLoc) return -1.0;
        FProperty* r = getLoc->GetReturnProperty();
        if (!r) return -1.0;
        const int32 ext = param_extent(getLoc, r);
        if (ext != 24 && ext != 12) return -1.0;
        std::vector<uint8> lb(getLoc->GetParmsSize(), 0);
        pawn->ProcessEvent(getLoc, lb.data());
        uint8* at = lb.data() + r->GetOffset_Internal();
        double cx, cy, cz;
        if (ext == 24)
        {
            auto* d = std::bit_cast<double*>(at);
            cx = d[0]; cy = d[1]; cz = d[2];
        }
        else
        {
            auto* f = std::bit_cast<float*>(at);
            cx = f[0]; cy = f[1]; cz = f[2];
        }
        double here_ground = 0.0;
        std::string here_note;
        // z_ref is passed 10000 LOW so the trace starts exactly at the pawn's
        // center (trace_ground begins at z_ref+10000). Starting overhead hit
        // the pawn's own body first - the "ground" came back as the top of the
        // dino, the clearance went negative, and every live run fell back to
        // the blind constant. A line trace that starts inside the capsule
        // ignores it and hits the floor.
        if (!trace_ground(pawn, cx, cy, cz - 10000.0, here_ground, here_note)) return -1.0;
        // Swimming, falling or a canopy trace inflates the measure; keep it
        // only in a plausible standing range.
        const double c = cz - here_ground;
        return (c > 20.0 && c < 900.0) ? c : -1.0;
    }

    // Moves a live pawn to (x, y) at ground level. Underpins the web portal's
    // teleport (and the planned friend-request teleports): the Lua side and the
    // web app write {"verb":"teleport","steam":...,"x":...,"y":...,"z":...} to
    // cppcmd.ndjson, where z is a REFERENCE altitude for the ground trace, not
    // the destination height.
    //
    // Same posture as spawn_corpse: never a blind vertical offset. The real Z
    // comes from trace_ground through the reference altitude, and a trace miss
    // refuses the teleport - off-landscape XY plus a blind Z is how a player
    // ends up under the map, and "refused" is recoverable while "in the void"
    // is a support ticket.
    auto teleport_pawn(UObject* pawn, const Action& act, std::string& detail) -> bool
    {
        // Destination: explicit coordinates, or - when "to" carries a steam id
        // - the live position of that player, resolved right now on the game
        // thread. The friend-teleport flow uses the latter so the web side
        // never needs to know coordinates at all.
        double tx = act.x, ty = act.y, tz = act.z;
        UObject* dest_pawn = nullptr;
        if (!act.to.empty())
        {
            UObject* tctrl = controller_for(act.to);
            if (!tctrl || tctrl->IsUnreachable() ||
                tctrl->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
            {
                detail = "target player is not on the server";
                return false;
            }
            UObject* tpawn = nullptr;
            if (UFunction* getPawn = tctrl->GetFunctionByNameInChain(FName(STR("K2_GetPawn"), FNAME_Find)))
            {
                std::vector<uint8> pb(getPawn->GetParmsSize(), 0);
                tctrl->ProcessEvent(getPawn, pb.data());
                if (FProperty* ret = getPawn->GetReturnProperty())
                    tpawn = *std::bit_cast<UObject**>(pb.data() + ret->GetOffset_Internal());
            }
            if (!tpawn || tpawn->IsUnreachable())
            {
                detail = "target player has no live dino";
                return false;
            }
            if (tpawn == pawn)
            {
                detail = "cannot teleport a player to themselves";
                return false;
            }
            dest_pawn = tpawn;
            tx = ty = tz = 0.0;
            if (UFunction* getLoc = tpawn->GetFunctionByNameInChain(FName(STR("K2_GetActorLocation"), FNAME_Find)))
            {
                FProperty* r = getLoc->GetReturnProperty();
                const int32 ext = r ? param_extent(getLoc, r) : 0;
                if (ext == 24 || ext == 12)
                {
                    std::vector<uint8> lb(getLoc->GetParmsSize(), 0);
                    tpawn->ProcessEvent(getLoc, lb.data());
                    uint8* at = lb.data() + r->GetOffset_Internal();
                    if (ext == 24)
                    {
                        auto* d = std::bit_cast<double*>(at);
                        tx = d[0]; ty = d[1]; tz = d[2];
                    }
                    else
                    {
                        auto* f = std::bit_cast<float*>(at);
                        tx = f[0]; ty = f[1]; tz = f[2];
                    }
                }
            }
            if (tx == 0.0 && ty == 0.0)
            {
                detail = "could not read target position";
                return false;
            }
            // Arrive beside, not inside: 500 units sideways keeps two large
            // capsules from starting the reunion overlapped.
            tx += 500.0;
        }

        if (tx == 0.0 && ty == 0.0 && tz == 0.0)
        {
            detail = "all-zero location - refusing to teleport to map origin";
            return false;
        }

        double ground_z = 0.0;
        std::string trace_note;
        if (!trace_ground(pawn, tx, ty, tz, ground_z, trace_note))
        {
            detail = "ground trace failed: " + trace_note;
            return false;
        }
        // Arrival height = the clearance the pawn has RIGHT NOW - see
        // measure_clearance. Self-measured, a standing traveller lands with
        // its feet ~10 units up. When the traveller's own measure fails
        // (resting, mid-air, swimming), a to= teleport can still measure the
        // DESTINATION pawn: that player is standing on the very ground being
        // arrived at, and the friend-teleport growth gate keeps both dinos in
        // the same size class, so their clearance is the next-best estimate
        // of the traveller's.
        double clearance = measure_clearance(pawn);
        if (clearance < 0.0 && dest_pawn != nullptr) clearance = measure_clearance(dest_pawn);
        if (clearance < 0.0)
        {
            // Blind fallback, sized by who can be on each path. A player
            // destination means the juvie-gated friend teleport, where the
            // 400 chosen for the largest adults was a 4m drop with real fall
            // damage; 120 covers a juvie capsule with a stumble, not a fall.
            // Coordinate teleports (admin rescues) can still move adults, so
            // they keep the height that cannot embed a Rex in the ground.
            clearance = act.to.empty() ? 400.0 : 120.0;
        }
        const double dest_z = ground_z + clearance + 10.0;

        // K2_SetActorLocation with bSweep=false, bTeleport=true - the ONLY
        // move that works on this build. K2_TeleportTo returned false on open,
        // trace-verified ground at both +200 and +550 clearance, so its
        // refusal is a class-level veto, not encroachment; do not bring it
        // back. The unchecked move is safe because the destination is always
        // trace-verified ground plus the measured clearance, and it leaves
        // rotation untouched, so facing survives without a copy.
        UFunction* fn = pawn->GetFunctionByNameInChain(FName(STR("K2_SetActorLocation"), FNAME_Find));
        if (!fn) { detail = "K2_SetActorLocation not found on pawn"; return false; }

        FProperty* p_loc = fn->FindProperty(FName(STR("NewLocation"), FNAME_Find));
        FProperty* p_tel = fn->FindProperty(FName(STR("bTeleport"), FNAME_Find));
        FProperty* p_ret = fn->GetReturnProperty();
        if (!p_loc || !p_tel || !p_ret)
        {
            detail = "K2_SetActorLocation signature unexpected";
            return false;
        }

        const int32 vec = param_extent(fn, p_loc);
        if (vec != 24 && vec != 12)
        {
            detail = "FVector extent " + std::to_string(vec) + " is not a known layout";
            return false;
        }

        std::vector<uint8> buf(fn->GetParmsSize(), 0);
        {
            uint8* at = buf.data() + p_loc->GetOffset_Internal();
            if (vec == 24)
            {
                auto* d = std::bit_cast<double*>(at);
                d[0] = tx; d[1] = ty; d[2] = dest_z;
            }
            else
            {
                auto* f = std::bit_cast<float*>(at);
                f[0] = (float)tx; f[1] = (float)ty; f[2] = (float)dest_z;
            }
        }

        // bSweep stays false in the zeroed buffer and SweepHitResult is an
        // out-param, so zeroed is fine; only bTeleport needs setting.
        if (auto* bp = CastField<FBoolProperty>(p_tel))
            bp->SetPropertyValue(buf.data() + bp->GetOffset_Internal(), true);

        pawn->ProcessEvent(fn, buf.data());

        bool moved = false;
        if (auto* bp = CastField<FBoolProperty>(p_ret))
            moved = bp->GetPropertyValue(buf.data() + p_ret->GetOffset_Internal());
        else
            moved = buf[p_ret->GetOffset_Internal()] != 0;

        call_void_fn(pawn, STR("ForceNetUpdate"));

        detail = std::string(moved ? "teleported to (" : "engine refused move to (") +
                 std::to_string(tx) + ", " + std::to_string(ty) + ", " +
                 std::to_string(dest_z) + "), ground=" + std::to_string(ground_z) +
                 ", clearance=" + std::to_string(clearance) +
                 (act.to.empty() ? "" : (", to=" + act.to));
        return moved;
    }

    auto run_action(const Action& act) -> void
    {
        std::string detail;
        bool ok = false;

        // Server-wide announcement:
        //   {"verb":"announce","steam":"0","text":"...","secs":"10","from":"Server"}
        // Uses the game's announcement banner, whose duration is a parameter -
        // so no double-fire simulation here; the client holds it on screen for
        // exactly `secs`. Runs before the controller lookup because it
        // addresses everyone, not a player.
        if (act.verb == "announce")
        {
            ok = !act.text.empty() && announce_all(act, detail);
            if (act.text.empty()) detail = "no text given";
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] announce: {}\n"),
                                            StringType(detail.begin(), detail.end()));
            if (FILE* f = std::fopen(kCmdResultPath, "a"))
            {
                std::fprintf(f, "{\"id\":\"%s\",\"verb\":\"announce\",\"steam\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}\n",
                             act.id.c_str(), act.steam.c_str(), ok ? "true" : "false", detail.c_str());
                std::fclose(f);
            }
            return;
        }

        // Needs no player: it reads the GameState, so handle it before the
        // controller lookup that everything else depends on.
        if (act.verb == "mutscan")
        {
            ok = probe_mutation_props(act.steam, detail);
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] mutscan: {}\n"),
                                            StringType(detail.begin(), detail.end()));
            if (FILE* f = std::fopen(kCmdResultPath, "a"))
            {
                std::fprintf(f, "{\"id\":\"%s\",\"verb\":\"mutscan\",\"steam\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}\n",
                             act.id.c_str(), act.steam.c_str(), ok ? "true" : "false", detail.c_str());
                std::fclose(f);
            }
            return;
        }

        // Corpse verbs address a location, not a player (steam is "0"), so they
        // must run before the controller lookup everything else depends on.
        if (act.verb == "corpsescan" || act.verb == "spawncorpse" ||
            act.verb == "wipecorpses" ||
            act.verb == "propdump" || act.verb == "classfns" || act.verb == "fnsig" ||
            act.verb == "zonevisits")
        {
            if      (act.verb == "corpsescan")  ok = scan_spawn_classes(detail);
            else if (act.verb == "spawncorpse") ok = spawn_corpse(act, detail);
            else if (act.verb == "wipecorpses") ok = wipe_corpses(act.min_age, detail);
            else if (act.verb == "propdump")    ok = prop_dump(act.cls, detail);
            else if (act.verb == "fnsig")       ok = fn_signature(act.cls, detail);
            // steam is the dino whose id we are trying to name; "0" = world only.
            else if (act.verb == "zonevisits")  ok = zone_visits(act.to, detail);
            else                                ok = class_fns(act.cls, detail);
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] {}: {} ({})\n"),
                                            StringType(act.verb.begin(), act.verb.end()),
                                            ok ? STR("ok") : STR("FAILED"),
                                            StringType(detail.begin(), detail.end()));
            if (FILE* f = std::fopen(kCmdResultPath, "a"))
            {
                std::fprintf(f, "{\"id\":\"%s\",\"verb\":\"%s\",\"steam\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}\n",
                             act.id.c_str(), act.verb.c_str(), act.steam.c_str(),
                             ok ? "true" : "false", detail.c_str());
                std::fclose(f);
            }
            return;
        }

        if (act.verb == "mutcfg")
        {
            ok = probe_mutation_config(detail);
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] mutcfg: {}\n"),
                                            StringType(detail.begin(), detail.end()));
            if (FILE* f = std::fopen(kCmdResultPath, "a"))
            {
                std::fprintf(f, "{\"id\":\"%s\",\"verb\":\"mutcfg\",\"steam\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}\n",
                             act.id.c_str(), act.steam.c_str(), ok ? "true" : "false", detail.c_str());
                std::fclose(f);
            }
            return;
        }

        UObject* ctrl = controller_for(act.steam);
        if (!ctrl || ctrl->IsUnreachable() ||
            ctrl->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
        {
            detail = "no live controller";
        }
        else if (act.verb == "chatdm")
        {
            // Needs the controller but deliberately NOT a pawn: a dead or
            // spectating player still has a chat box.
            ok = send_chat_dm(ctrl, act, detail);
        }
        else
        {
            UObject* pawn = nullptr;
            if (UFunction* getPawn = ctrl->GetFunctionByNameInChain(FName(STR("K2_GetPawn"), FNAME_Find)))
            {
                std::vector<uint8> pb(getPawn->GetParmsSize(), 0);
                ctrl->ProcessEvent(getPawn, pb.data());
                if (FProperty* ret = getPawn->GetReturnProperty())
                {
                    pawn = *std::bit_cast<UObject**>(pb.data() + ret->GetOffset_Internal());
                }
            }

            if (!pawn || pawn->IsUnreachable())
            {
                detail = "no live dino";
            }
            else if (act.verb == "pushmutations")
            {
                ok = push_mutations(pawn, detail);
            }
            else if (act.verb == "mutrefresh")
            {
                ok = refresh_mutations(pawn, kEpsilon, detail);
                // Restore on a later tick so the two writes are separate net
                // updates. Same tick would collapse into one comparison and the
                // client would see no change at all.
                std::lock_guard<std::mutex> guard(m_lock);
                m_delayed.push_back({8, Action{act.id, "mutrestore", act.steam}});
            }
            else if (act.verb == "mutrestore")
            {
                ok = refresh_mutations(pawn, -kEpsilon, detail);
            }
            else if (act.verb == "teleport")
            {
                ok = teleport_pawn(pawn, act, detail);
            }
            else
            {
                detail = "unknown verb";
            }
        }

        Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] action {} for {}: {} ({})\n"),
                                        StringType(act.verb.begin(), act.verb.end()),
                                        StringType(act.steam.begin(), act.steam.end()),
                                        ok ? STR("ok") : STR("FAILED"),
                                        StringType(detail.begin(), detail.end()));

        if (FILE* f = std::fopen(kCmdResultPath, "a"))
        {
            std::fprintf(f, "{\"id\":\"%s\",\"verb\":\"%s\",\"steam\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}\n",
                         act.id.c_str(), act.verb.c_str(), act.steam.c_str(),
                         ok ? "true" : "false", detail.c_str());
            std::fclose(f);
        }
    }

    auto deliver_to(UObject* ctrl, const std::string& text) -> void
    {
        std::vector<uint8> buf(m_notify_fn->GetParmsSize(), 0);

        // FText is refcounted, so unlike FString a byte copy into the buffer does
        // not create a second owner of a heap allocation - it creates a second
        // handle. Copy exactly m_notify_size bytes: the engine's 16, not the 24
        // this process thinks an FText is.
        FText msg{FString{StringType(text.begin(), text.end())}};
        std::memcpy(buf.data() + m_notify_offset, &msg, static_cast<size_t>(m_notify_size));

        ctrl->ProcessEvent(m_notify_fn, buf.data());
    }

    // Every live TIPlayerController gets the message. Enumerated by IsA against
    // the native class rather than GetControllerBySteamId per player, because
    // this path has no player list to iterate - the controllers ARE the list.
    // One ProcessEvent per player in a single tick is acceptable here: an
    // announcement is rare (corpse-wipe warnings), unlike the per-player chat
    // lines that kMaxPerTick spreads out.
    auto broadcast(const std::string& text) -> int
    {
        UClass* cls = resolve_class("TIPlayerController");
        if (!cls) return 0;

        int sent = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj == cls || !obj->IsA(cls)) return LoopAction::Continue;
            if (obj->IsUnreachable() ||
                obj->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
                return LoopAction::Continue;
            // Skip the CDO: it is not a connected player.
            StringType nw = obj->GetName();
            const std::string n(nw.begin(), nw.end());
            if (n.rfind("Default__", 0) == 0) return LoopAction::Continue;

            deliver_to(obj, text);
            ++sent;
            return LoopAction::Continue;
        });
        return sent;
    }

    // The game's announcement banner to every live controller, with a real
    // on-screen duration. Marshaling per the two standing rules: the FString
    // local outlives ProcessEvent and the buffer copy is never destructed (one
    // owner), and the FText is copied at its MEASURED extent, never
    // sizeof(FText).
    auto announce_all(const Action& act, std::string& detail) -> bool
    {
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kAnnounceFn);
        if (!fn) { detail = "Client_ProcessIncomingAnnouncement not found"; return false; }

        FProperty* p_msg = fn->FindProperty(FName(STR("Message"), FNAME_Find));
        FProperty* p_who = fn->FindProperty(FName(STR("Announcer"), FNAME_Find));
        FProperty* p_time = fn->FindProperty(FName(STR("AnnouncementTime"), FNAME_Find));
        if (!p_msg || !p_who || !p_time)
        {
            detail = "Client_ProcessIncomingAnnouncement signature unexpected";
            return false;
        }

        const int32 who_extent = param_extent(fn, p_who);
        if (who_extent <= 0 || who_extent > static_cast<int32>(sizeof(FText)))
        {
            detail = "Announcer extent " + std::to_string(who_extent) + " is not usable";
            return false;
        }

        UClass* cls = resolve_class("TIPlayerController");
        if (!cls) { detail = "TIPlayerController class not found"; return false; }

        const std::string from = act.from.empty() ? "Server" : act.from;
        int sent = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) -> LoopAction {
            if (!obj || obj == cls || !obj->IsA(cls)) return LoopAction::Continue;
            if (obj->IsUnreachable() ||
                obj->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
                return LoopAction::Continue;
            StringType nw = obj->GetName();
            const std::string n(nw.begin(), nw.end());
            if (n.rfind("Default__", 0) == 0) return LoopAction::Continue;

            std::vector<uint8> buf(fn->GetParmsSize(), 0);
            FString msg{StringType(act.text.begin(), act.text.end())};
            *std::bit_cast<FString*>(buf.data() + p_msg->GetOffset_Internal()) = msg;
            FText who{FString{StringType(from.begin(), from.end())}};
            std::memcpy(buf.data() + p_who->GetOffset_Internal(), &who, static_cast<size_t>(who_extent));
            *std::bit_cast<float*>(buf.data() + p_time->GetOffset_Internal()) = static_cast<float>(act.secs);

            obj->ProcessEvent(fn, buf.data());
            ++sent;
            return LoopAction::Continue;
        });

        detail = "announced to " + std::to_string(sent) + " controller(s), " +
                 std::to_string(act.secs) + "s, from=" + from;
        return true;
    }

    // ------------------------------------------------------------- chat hook

    // The text of an FText parameter slot, via the engine's own
    // Conv_TextToString. The slot is copied at its MEASURED extent (16 on this
    // build) - the RC-side FText is 24 bytes and writing that would clobber the
    // conv function's return slot, which is the exact overflow class that made
    // FText fatal from Lua. Runs on the game thread only (the hook fires there).
    auto text_of(const uint8* slot, int32 extent) -> StringType
    {
        // A null ITextData pointer is a legal empty FText; converting it is the
        // one case Conv_TextToString is documented to abort on.
        if (*std::bit_cast<void* const*>(slot) == nullptr) return {};

        std::vector<uint8> buf(m_conv_fn->GetParmsSize(), 0);
        std::memcpy(buf.data() + m_conv_in_off, slot,
                    static_cast<size_t>(std::min(extent, m_conv_in_ext)));
        m_conv_self->ProcessEvent(m_conv_fn, buf.data());

        FString* ret = std::bit_cast<FString*>(buf.data() + m_conv_ret_off);
        StringType out{**ret};
        // The engine allocated the return string into the parameter block and
        // nothing else will ever free it: plain FString has no destructor here.
        // Clear() releases it through the engine allocator - single owner, one
        // release, no per-message leak.
        ret->Clear();
        return out;
    }

    // Fires on the game thread once per receiving controller (per
    // EVRIMA_Chat_System.md), with the sender in the ChatPlayerController
    // parameter - so one typed message logs once per player in range. That
    // repetition is kept: it is the evidence for how the fanout actually works.
    auto on_chat(UnrealScriptFunctionCallableContext& context) -> void
    {
        uint8* locals = context.TheStack.Locals();
        if (!locals || m_chat_text_off < 0) return;

        const StringType text = text_of(locals + m_chat_text_off, m_chat_text_ext);

        if (m_chat_diag.load())
        {
            StringType self_name = context.Context ? context.Context->GetName() : STR("<null>");
            StringType sender_name = STR("<none>");
            if (m_chat_sender_off >= 0)
            {
                if (UObject* sender = *std::bit_cast<UObject**>(locals + m_chat_sender_off))
                {
                    sender_name = sender->GetName();
                }
            }
            const int mode = (m_chat_mode_off >= 0) ? locals[m_chat_mode_off] : -1;
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chat: to={} from={} mode={} text={}\n"),
                                            self_name, sender_name, mode, text);
        }

        if (!m_chat_blank.load() || text.empty()) return;
        if (text[0] != STR('!') && text[0] != STR('/')) return;

        // Overwrite the message with a fresh single-space FText, at the slot's
        // measured extent. Fresh per call, never a shared static: the engine
        // releases the parameter block's FText reference after the call, so a
        // reused handle would be over-released on the second message. A space
        // rather than an empty text so the client never sees a null-data FText.
        // The overwritten original's reference is deliberately never released -
        // a few bytes per suppressed command line, and only when chat_blank=1.
        FText blank{FString{StringType(STR(" "))}};
        std::memcpy(locals + m_chat_text_off, &blank, static_cast<size_t>(m_chat_text_ext));
        if (m_chat_nofilter_off >= 0)
        {
            FText blank2{FString{StringType(STR(" "))}};
            std::memcpy(locals + m_chat_nofilter_off, &blank2, static_cast<size_t>(m_chat_nofilter_ext));
        }
    }

    // The sender's controller is context.Context itself: ServerExecuteChatCommand
    // is a Server RPC, so it executes ON the invoking player's controller. The
    // CommandLine FString is read in place and never freed here - the engine
    // owns the parameter block and destroys it after the call.
    auto on_chat_command(UnrealScriptFunctionCallableContext& context) -> void
    {
        if (m_cmd_off < 0) return;
        uint8* locals = context.TheStack.Locals();
        if (!locals || !context.Context) return;

        const FString* cmd = std::bit_cast<const FString*>(locals + m_cmd_off);
        StringType cmdw{**cmd};

        // The sender's Steam id is an FString property on the controller
        // itself (SteamId<StrProperty>, propdump-verified). Read in place.
        StringType sidw;
        if (FProperty* p = context.Context->GetPropertyByNameInChain(STR("SteamId")))
        {
            sidw = StringType{**p->ContainerPtrToValuePtr<FString>(context.Context)};
        }

        if (m_chat_diag.load())
        {
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chatcmd: from={} steam={} cmd={}\n"),
                                            context.Context->GetName(), sidw, cmdw);
        }

        // Narrow to the ASCII the ndjson carrier and the Lua dispatcher speak,
        // dropping anything that could break the one-line JSON framing. The
        // game's own recognized commands (/adminpanel and friends) still run:
        // this pre-hook never cancels or rewrites the RPC, it only observes.
        std::string steam(sidw.begin(), sidw.end());
        if (!all_digits(steam)) return;

        std::string body;
        for (auto ch : cmdw)
        {
            if (ch < 0x20 || ch > 0x7e) continue;
            if (ch == '"') { body.push_back('\''); continue; }
            if (ch == '\\') { body.push_back('/'); continue; }
            body.push_back(static_cast<char>(ch));
        }
        if (body.size() > 180) body.resize(180);
        if (body.empty()) return;

        std::lock_guard<std::mutex> guard(m_lock);
        if (m_slash.size() >= kMaxQueue) m_slash.pop_front();
        m_slash.push_back({std::move(steam), std::move(body)});
    }

    // Resolve-and-register, from the game-thread tick. Retries silently while
    // GetChatMessage is not yet findable; a resolved-but-unusable layout logs
    // once and gives up rather than spamming every tick.
    auto register_chat_hook() -> void
    {
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kChatFn);
        if (!fn)
        {
            if (!m_chat_warned)
            {
                m_chat_warned = true;
                Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chat: GetChatMessage not found yet - will retry\n"));
            }
            return;
        }

        m_conv_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kTextToStringFn);
        m_conv_self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kKismetTextSelf);
        FProperty* conv_in = m_conv_fn ? m_conv_fn->FindProperty(FName(STR("InText"), FNAME_Find)) : nullptr;
        FProperty* conv_ret = m_conv_fn ? m_conv_fn->GetReturnProperty() : nullptr;
        FProperty* p_text = fn->FindProperty(FName(STR("NewText"), FNAME_Find));

        // Whatever happens next, log the live signature once: if a future patch
        // renames a parameter, this line is the diagnosis.
        {
            std::string sig = "parms_size=" + std::to_string(static_cast<int>(fn->GetParmsSize()));
            for (FProperty* p : fn->ForEachProperty())
            {
                if (!p) continue;
                StringType nw = p->GetName();
                StringType tw = p->GetClass().GetName();
                sig += " " + std::string(nw.begin(), nw.end()) +
                       "[" + std::string(tw.begin(), tw.end()) + "]@" +
                       std::to_string(p->GetOffset_Internal());
            }
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chat: GetChatMessage {}\n"),
                                            StringType(sig.begin(), sig.end()));
        }

        m_chat_hooked.store(true);   // resolved: this function never runs again

        // The slash-command RPC first: it is independent of the FText chat path
        // below, and must still hook if that path's validation refuses.
        if (UFunction* cmd_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kChatCmdFn))
        {
            if (FProperty* p_cmd = cmd_fn->FindProperty(FName(STR("CommandLine"), FNAME_Find)))
            {
                m_cmd_off = p_cmd->GetOffset_Internal();
                cmd_fn->RegisterPreHook(
                    [](UnrealScriptFunctionCallableContext& context, void* custom_data) {
                        static_cast<IsleCPPUtilities*>(custom_data)->on_chat_command(context);
                    },
                    this);
                Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chatcmd: pre-hook installed (CommandLine @{})\n"), m_cmd_off);
            }
            else
            {
                Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] chatcmd: no CommandLine property - not hooked\n"));
            }
        }
        else
        {
            Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] chatcmd: ServerExecuteChatCommand not found - not hooked\n"));
        }

        if (!m_conv_fn || !m_conv_self || !conv_in || !conv_ret || !p_text)
        {
            Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] chat: signature unusable - hook not installed\n"));
            return;
        }

        m_conv_in_off = conv_in->GetOffset_Internal();
        m_conv_in_ext = param_extent(m_conv_fn, conv_in);
        m_conv_ret_off = conv_ret->GetOffset_Internal();
        m_chat_text_off = p_text->GetOffset_Internal();
        m_chat_text_ext = param_extent(fn, p_text);

        // The same refusal rule as the notify path: a copy size not derived
        // from the live layout, or larger than the RC-side FText we stage the
        // blank in, is the overflow this mod exists to avoid.
        if (m_chat_text_ext <= 0 || m_chat_text_ext > static_cast<int32>(sizeof(FText)) ||
            m_conv_in_ext <= 0 || m_conv_in_ext > static_cast<int32>(sizeof(FText)))
        {
            m_chat_text_off = -1;
            Output::send<LogLevel::Error>(STR("[IsleCPPUtilities] chat: FText extent unusable ({}/{}) - hook not installed\n"),
                                          m_chat_text_ext, m_conv_in_ext);
            return;
        }

        if (FProperty* p = fn->FindProperty(FName(STR("ChatPlayerController"), FNAME_Find)))
        {
            m_chat_sender_off = p->GetOffset_Internal();
        }
        if (FProperty* p = fn->FindProperty(FName(STR("ChatMode"), FNAME_Find)))
        {
            m_chat_mode_off = p->GetOffset_Internal();
        }
        if (FProperty* p = fn->FindProperty(FName(STR("NoFilterMsg"), FNAME_Find)))
        {
            const int32 ext = param_extent(fn, p);
            if (ext > 0 && ext <= static_cast<int32>(sizeof(FText)))
            {
                m_chat_nofilter_off = p->GetOffset_Internal();
                m_chat_nofilter_ext = ext;
            }
        }

        fn->RegisterPreHook(
            [](UnrealScriptFunctionCallableContext& context, void* custom_data) {
                static_cast<IsleCPPUtilities*>(custom_data)->on_chat(context);
            },
            this);
        Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] chat: pre-hook installed (diag={} blank={})\n"),
                                        m_chat_diag.load(), m_chat_blank.load());
    }

    // One line into one player's chat box, from "Server". All three parameter
    // kinds follow the standing marshalling rules: both FTexts are copied at
    // their MEASURED extent, the FString is assigned into zeroed memory with
    // the local kept alive past ProcessEvent, and nothing in the buffer is
    // ever destructed.
    auto send_chat_dm(UObject* ctrl, const Action& act, std::string& detail) -> bool
    {
        if (act.text.empty()) { detail = "no text given"; return false; }

        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kUpdateChatFn);
        if (!fn) { detail = "UpdateChat not found"; return false; }

        FProperty* p_sender = fn->FindProperty(FName(STR("Sender"), FNAME_Find));
        FProperty* p_text = fn->FindProperty(FName(STR("Text"), FNAME_Find));
        FProperty* p_sid = fn->FindProperty(FName(STR("SenderSteamId"), FNAME_Find));
        FProperty* p_mode = fn->FindProperty(FName(STR("ChatMode"), FNAME_Find));
        FProperty* p_admin = fn->FindProperty(FName(STR("bIsAdmin"), FNAME_Find));
        if (!p_sender || !p_text || !p_sid)
        {
            detail = "UpdateChat signature unexpected";
            return false;
        }

        const int32 sender_ext = param_extent(fn, p_sender);
        const int32 text_ext = param_extent(fn, p_text);
        if (sender_ext <= 0 || sender_ext > static_cast<int32>(sizeof(FText)) ||
            text_ext <= 0 || text_ext > static_cast<int32>(sizeof(FText)))
        {
            detail = "FText extent unusable (" + std::to_string(sender_ext) + "/" + std::to_string(text_ext) + ")";
            return false;
        }

        std::vector<uint8> buf(fn->GetParmsSize(), 0);

        const std::string from = act.from.empty() ? "Server" : act.from;
        FText sender{FString{StringType(from.begin(), from.end())}};
        std::memcpy(buf.data() + p_sender->GetOffset_Internal(), &sender, static_cast<size_t>(sender_ext));

        FText body{FString{StringType(act.text.begin(), act.text.end())}};
        std::memcpy(buf.data() + p_text->GetOffset_Internal(), &body, static_cast<size_t>(text_ext));

        // "0" rather than a real player's id: the sender is the server, and a
        // fabricated real id would wire the line to that player's profile.
        FString sid{StringType(STR("0"))};
        *std::bit_cast<FString*>(buf.data() + p_sid->GetOffset_Internal()) = sid;

        if (p_mode) buf[p_mode->GetOffset_Internal()] = static_cast<uint8>(act.mode);
        if (p_admin && act.admin)
        {
            if (auto* bp = CastField<FBoolProperty>(p_admin))
            {
                bp->SetPropertyValue(buf.data() + bp->GetOffset_Internal(), true);
            }
        }

        ctrl->ProcessEvent(fn, buf.data());
        detail = "chat line sent (mode " + std::to_string(act.mode) + ", from=" + from + ")";
        return true;
    }

    auto send_one(const Pending& item) -> void
    {
        if (item.steam == kBroadcastSteam)
        {
            const int n = broadcast(item.text);
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] broadcast to {} controller(s)\n"), n);
            return;
        }

        UObject* ctrl = controller_for(item.steam);

        // Rule 9a from the Lua safety rules, which applies just as much here: a
        // non-null pointer is not proof of a live object during disconnect
        // teardown, and this queue is drained a frame or more after the line that
        // produced it was written. GetControllerBySteamId returning the controller
        // is the game's own assertion that it still tracks the player; the flag
        // checks catch one that is mid-teardown or already collected.
        if (!ctrl || ctrl->IsUnreachable() ||
            ctrl->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed)))
        {
            Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] no live controller for {} - dropping\n"),
                                            StringType(item.steam.begin(), item.steam.end()));
            return;
        }

        deliver_to(ctrl, item.text);
    }

    // --------------------------------------------------------------- tailer

    // A port of chat_relay.py's follow(): start at EOF so a restart does not
    // replay the day's backlog, and handle the file not existing yet, the web app
    // rotating it past 512 KiB (inode change), truncation in place, and a line
    // that is still being written.
    struct Follow
    {
        long pos = -1;
        ino_t inode = 0;
    };

    // One pass over one file. Returns nothing; hands each complete line to `sink`.
    template <typename Sink>
    auto follow_once(const char* pathStr, Follow& f, Sink sink) -> void
    {
        struct stat st{};
        if (::stat(pathStr, &st) != 0) return;

        if (f.pos < 0)                     // first sight of the file
        {
            f.pos = static_cast<long>(st.st_size);
            f.inode = st.st_ino;
        }
        else if (st.st_ino != f.inode)     // rotated - read the new file whole
        {
            f.pos = 0;
            f.inode = st.st_ino;
        }
        else if (static_cast<long>(st.st_size) < f.pos)   // truncated in place
        {
            f.pos = 0;
        }

        if (static_cast<long>(st.st_size) <= f.pos) return;

        FILE* fh = std::fopen(pathStr, "rb");
        if (!fh) return;

        std::fseek(fh, f.pos, SEEK_SET);
        std::string chunk;
        char tmp[4096];
        size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), fh)) > 0) chunk.append(tmp, n);
        std::fclose(fh);

        // Only consume up to the last newline, so a half-written line is re-read
        // intact on the next pass instead of being parsed as truncated JSON.
        const size_t last = chunk.find_last_of('\n');
        if (last == std::string::npos) return;

        f.pos += static_cast<long>(last) + 1;
        size_t from = 0;
        while (from <= last)
        {
            size_t nl = chunk.find('\n', from);
            if (nl == std::string::npos) break;
            sink(chunk.substr(from, nl - from));
            from = nl + 1;
        }
    }

    // notify.cfg: `key=value` lines, unknown lines ignored (so `#` comments are
    // free). Read every pass of the tailer, so a tuning change lands within a
    // second of saving the file - no rebuild, no restart, no reload.
    auto read_config() -> void
    {
        FILE* f = std::fopen(kCfgPath, "rb");
        if (!f) return;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f))
        {
            int v = 0;
            if (std::sscanf(buf, "resend_ms=%d", &v) == 1 && v >= 0 && v <= 60000)
            {
                m_resend_ms.store(v);
            }
            else if (std::sscanf(buf, "chat_diag=%d", &v) == 1)
            {
                m_chat_diag.store(v != 0);
            }
            else if (std::sscanf(buf, "chat_blank=%d", &v) == 1)
            {
                m_chat_blank.store(v != 0);
            }
        }
        std::fclose(f);
    }

    auto tail_loop() -> void
    {
        Follow results, commands;

        while (!m_stop.load())
        {
            read_config();
            follow_once(kResultPath, results, [this](const std::string& l) { consider(l); });
            follow_once(kCmdPath, commands, [this](const std::string& l) { consider_action(l); });
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // The delivery rule, identical to chat_relay.py's deliverable(): a line is for
    // a player only if the MOD put a "short" field on it. Keeping that decision in
    // emitResult, rather than duplicating a list of interesting verbs here, is why
    // both delivery paths agree on what a player should see.
    auto consider(const std::string& line) -> void
    {
        if (line.find("\"short\"") == std::string::npos) return;

        std::string steam = json_string_field(line, "steam");
        std::string text = json_string_field(line, "short");
        if (text.empty() || (steam != kBroadcastSteam && !all_digits(steam))) return;

        std::lock_guard<std::mutex> guard(m_lock);
        if (m_queue.size() >= kMaxQueue) m_queue.pop_front();
        m_queue.push_back(Pending{std::move(steam), std::move(text)});
    }

    // An imperative from the Lua mod: something it cannot do itself.
    auto consider_action(const std::string& line) -> void
    {
        std::string verb = json_string_field(line, "verb");
        std::string steam = json_string_field(line, "steam");
        std::string id = json_string_field(line, "id");
        if (verb.empty() || !all_digits(steam)) return;

        Action act{std::move(id), std::move(verb), std::move(steam)};
        act.cls = json_string_field(line, "cls");
        act.to = json_string_field(line, "to");
        act.text = json_string_field(line, "text");
        act.from = json_string_field(line, "from");
        act.kill = json_string_field(line, "kill") == "1";
        // Coordinates travel as string fields; a missing or malformed one is 0,
        // and spawncorpse refuses an all-zero location rather than spawning at
        // the map origin.
        auto num_of = [&](const char* key) -> double {
            const std::string s = json_string_field(line, key);
            if (s.empty()) return 0.0;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            return (end && *end == '\0') ? v : 0.0;
        };
        act.x = num_of("x");
        act.y = num_of("y");
        act.z = num_of("z");
        if (const double g = num_of("g"); g > 0.0 && g <= 1.0) act.g = g;
        if (const double s = num_of("secs"); s > 0.0 && s <= 300.0) act.secs = s;
        if (const double a = num_of("min_age"); a > 0.0 && a <= 3600.0) act.min_age = a;
        if (const double m = num_of("mode"); m > 0.0 && m <= 255.0) act.mode = static_cast<int>(m);
        act.admin = json_string_field(line, "admin") == "1";

        std::lock_guard<std::mutex> guard(m_lock);
        if (m_actions.size() >= kMaxQueue) m_actions.pop_front();
        m_actions.push_back(std::move(act));
    }

    // ----------------------------------------------------------- game thread

    auto drain() -> void
    {
        resolve_once();
        if (!m_chat_hooked.load()) register_chat_hook();

        // Self-throttled to kCensusIntervalSeconds; this runs every engine tick.
        census_corpses();
        // Same deal, kZoneVisitsIntervalSeconds. Both must be on this thread.
        publish_zone_visits();

        // Hand captured slash commands to the Lua dispatcher. Written from the
        // tick rather than from the RPC hook so the hook itself never does
        // file I/O mid-call.
        {
            std::deque<std::pair<std::string, std::string>> out;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                out.swap(m_slash);
            }
            if (!out.empty())
            {
                if (FILE* f = std::fopen(kSlashOutPath, "a"))
                {
                    for (auto& [steam, cmd] : out)
                    {
                        std::fprintf(f, "{\"id\":\"slash\",\"steam\":\"%s\",\"cmd\":\"%s\"}\n",
                                     steam.c_str(), cmd.c_str());
                    }
                    std::fclose(f);
                }
            }
        }

        // Actions run before notifications and are not gated on m_ready: that flag
        // only says the notification signature resolved, and a mutation push has
        // nothing to do with it. One per tick - it is a replicated struct write,
        // not a chat line.
        {
            // Tick down anything waiting, and promote what is due.
            {
                std::lock_guard<std::mutex> guard(m_lock);
                for (auto it = m_delayed.begin(); it != m_delayed.end();)
                {
                    if (--it->first <= 0)
                    {
                        m_actions.push_back(std::move(it->second));
                        it = m_delayed.erase(it);
                    }
                    else ++it;
                }
            }

            Action act;
            bool have = false;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                if (!m_actions.empty())
                {
                    act = std::move(m_actions.front());
                    m_actions.pop_front();
                    have = true;
                }
            }
            if (have) run_action(act);
        }

        if (!m_ready) return;

        // Promote redeliveries that have come due. They join the back of the
        // normal queue, so kMaxPerTick still bounds the per-frame cost.
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> guard(m_lock);
            for (auto it = m_resend.begin(); it != m_resend.end();)
            {
                if (it->first <= now)
                {
                    if (m_queue.size() < kMaxQueue) m_queue.push_back(std::move(it->second));
                    it = m_resend.erase(it);
                }
                else ++it;
            }
        }

        for (int i = 0; i < kMaxPerTick; ++i)
        {
            Pending item;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                if (m_queue.empty()) return;
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }
            send_one(item);

            // The second showing: same message, resend_ms later, one fewer
            // resend left. A player who disconnected in the gap is dropped by
            // send_one the same way the first attempt would have been.
            const int gap_ms = m_resend_ms.load();
            if (item.resend > 0 && gap_ms > 0)
            {
                std::lock_guard<std::mutex> guard(m_lock);
                if (m_resend.size() < kMaxQueue)
                {
                    m_resend.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(gap_ms),
                                        Pending{item.steam, item.text, item.resend - 1}});
                }
            }
        }
    }

    // Registered from on_unreal_init on a cold boot, and from on_update when the
    // mod is hot-loaded.
    //
    // That second path is not defensive padding: this mod was first loaded by
    // UE4SS's filesystem watcher noticing a new directory ("New C++ mod detected:
    // 'IsleCPPUtilities', starting..."), which happens long after Unreal is up, so
    // on_unreal_init never fired and only the constructor logged. Anything that
    // lives solely in on_unreal_init silently does not exist on a hot load.
    auto register_tick() -> void
    {
        if (m_tick_registered.exchange(true)) return;

        Hook::RegisterEngineTickPostCallback(
            [this](auto&, UEngine*, float, bool) {
                drain();
            },
            Hook::FCallbackOptions{
                .bReadonly = true,
                .OwnerModName = STR("IsleCPPUtilities"),
                .HookName = STR("IsleCPPUtilities::drain"),
            });

        m_tailer = std::thread([this] { tail_loop(); });
        Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] tick callback registered, tailer started\n"));
    }

  public:
    IsleCPPUtilities() : CppUserModBase()
    {
        ModName = STR("IsleCPPUtilities");
        ModVersion = STR("0.15.0");
        ModDescription = STR("Native on-screen notifications for DinoStorage");
        ModAuthors = STR("Natural Selection Isle");

        // The constructor is the earliest point that logs, and it runs before
        // on_program_start despite the name. If the build or the ABI is wrong,
        // this line is the one that will not appear.
        Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] ctor BUILD=k12 - teleport arrival: destination-pawn clearance fallback, to= blind fallback 400 -> 120\n"));
    }

    ~IsleCPPUtilities() override
    {
        m_stop.store(true);
        if (m_tailer.joinable()) m_tailer.join();
    }

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[IsleCPPUtilities] on_unreal_init\n"));
        register_tick();
    }

    // fire_update() is called from UE4SSProgram::update(), which is UE4SS's own
    // event-loop thread - it ends in sleep_for(5ms) and has nothing to do with the
    // game thread. So this does the one thing that is safe from here: make sure
    // the game-thread callback is registered. Every engine call happens in drain().
    auto on_update() -> void override
    {
        if (!m_tick_registered.load()) register_tick();
    }
};

// The bundled example mods use __declspec(dllexport), which is Windows-only and
// does not compile under GCC. CppMod.cpp resolves both of these with dlsym on
// Linux, so they need default visibility in case the tree is ever built with
// -fvisibility=hidden.
#ifdef _WIN32
#define ISLE_CPPUTILITIES_API __declspec(dllexport)
#else
#define ISLE_CPPUTILITIES_API __attribute__((visibility("default")))
#endif

extern "C"
{
    ISLE_CPPUTILITIES_API CppUserModBase* start_mod()
    {
        return new IsleCPPUtilities();
    }

    ISLE_CPPUTILITIES_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
