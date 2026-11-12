// Local offline VS integration. Guest memory is only touched on the game thread.
#include "local_vs.h"
#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "guest_abi/player_slots.h"
#include "hle/custom_messages.h"
#include "hle/teams.h"
#include "hle/rank_icons.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_product.h"
#include "runtime_log.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <numeric>
#include <random>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace local_vs_detail {
std::atomic<int> desiredCount{100};
std::atomic<bool> unlocks{true};
std::atomic<bool> diversity{true};
std::atomic<bool> thirtyFps{false};
std::atomic<bool> instantBoxes{true};
std::atomic<bool> impossibleCpus{false};  // this race: the CPU row's 4th option (PrepareRace)
std::atomic<bool> impossibleBoot{false};  // --race-impossible; Initialize runs after arguments and must not clear it
std::atomic<uint32_t> scoring{uint32_t(LocalVs::Scoring::ClassicSpread)};
std::atomic<uint32_t> itemDistribution{uint32_t(LocalVs::ItemDistribution::Even)};
std::atomic<uint32_t> itemCycler{uint32_t(LocalVs::ItemCycler::Off)};
constexpr std::array<const char*,3> cyclerKeys{{"off","debug","infinite"}};
constexpr std::array<const char*,3> cyclerNames{{"Off","Debug (items used up)","Infinite"}};
// Config.toml values for the two balancing options, in enum order.
constexpr std::array<const char*,3> scoringKeys{{"classic_spread","classic_top12","linear"}};
constexpr std::array<const char*,3> scoringNames{{"Classic, spread over the field","Classic, top 12 only","Linear (N..1, +3/+1 for 1st/2nd)"}};
constexpr std::array<const char*,4> itemKeys{{"even","generous","tight","classic_top11"}};
constexpr std::array<const char*,4> itemNames{{"Even","Generous","Tight","Classic top 11"}};
// The game's 12-player VS points (row 12 of the u8[12][12] table at 0x80890030).
constexpr std::array<uint32_t,12> classicPoints{{15,12,10,8,7,6,5,4,3,2,1,0}};
template <size_t N>
uint32_t ParseKey(const std::optional<std::string>& value, const std::array<const char*,N>& keys) {
    if (value) for (uint32_t i=0;i<N;++i) if (*value==keys[i]) return i;
    return 0;
}
std::atomic<uint32_t> hudRank{0};
std::atomic<int64_t> hudTime{0};
std::atomic<bool> hudIcons{false};  // the race counter shows a rank icon; the overlay only shows game speed
std::atomic<uint32_t> simulationRate{0};
// Speedometer: player 0's kart speed, from its position change over race frames (game units a
// frame, as the speedometer codes show: 150cc top speed about 84). Written on the game thread.
std::atomic<bool> speedometer{true};
std::atomic<float> hudSpeed{0.0f};
std::atomic<int64_t> hudSpeedTime{0};
int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t seriesCount = 0;
struct Entry { uint32_t id, character, place, earned, total; };
std::mutex standingsMutex;
std::vector<Entry> standings;
constexpr std::array<uint32_t,24> weights{{1,0,2,2,0,0,0,1,0,2,1,2,0,0,0,1,1,1,1,2,1,2,2,2}};
constexpr std::array<const char*,24> names{{"Mario","Baby Peach","Waluigi","Bowser","Baby Daisy","Dry Bones","Baby Mario","Luigi","Toad","Donkey Kong","Yoshi","Wario","Baby Luigi","Toadette","Koopa Troopa","Daisy","Peach","Birdo","Diddy Kong","King Boo","Bowser Jr.","Dry Bowser","Funky Kong","Rosalina"}};
uint32_t Player(uint32_t scenario, uint32_t id) {
    return PlayerSlots::PlayerAddress(Memory::Read32(MKW_GADDR(809BD728)), scenario, id);
}
uint32_t RaceCount() {
    const auto rd = Memory::Read32(MKW_GADDR(809BD728));
    return rd ? Memory::Read8(rd + 0x24) : 0;
}
bool ExpandedOffline() {
    const auto rd = Memory::Read32(MKW_GADDR(809BD728));
    return rd && RaceCount() > 12 && RaceCount() <= PlayerSlots::kMaxPlayers &&
           Memory::Read32(rd + 0x20 + 0xB50) <= 1 && Memory::Read32(rd + 0x20 + 0xB54) == 0;  // GP (0) or VS (1)
}
}

namespace LocalVs {
void Initialize() {
    const auto& c = RuntimeConfigFile::Get();
    local_vs_detail::desiredCount = std::clamp(int(c.localVsPlayers.value_or(100)), 1, int(PlayerSlots::kMaxPlayers));
    local_vs_detail::unlocks = c.localVsUnlocks.value_or(true);
    local_vs_detail::diversity = c.localVsDiversity.value_or(true);
    local_vs_detail::thirtyFps = c.localVsThirtyFps.value_or(false);
    local_vs_detail::instantBoxes = c.localVsInstantBoxes.value_or(true);
    local_vs_detail::speedometer = c.localVsSpeedometer.value_or(true);
    local_vs_detail::scoring = local_vs_detail::ParseKey(c.localVsScoring, local_vs_detail::scoringKeys);
    local_vs_detail::itemDistribution = local_vs_detail::ParseKey(c.localVsItemDistribution, local_vs_detail::itemKeys);
    local_vs_detail::itemCycler = local_vs_detail::ParseKey(c.localVsItemCycler, local_vs_detail::cyclerKeys);
    Teams::Initialize(c.localVsTeams.value_or(0));
}
bool ImpossibleCpus() { return local_vs_detail::impossibleCpus.load() || local_vs_detail::impossibleBoot.load(); }
void SetImpossibleCpus(bool on) { local_vs_detail::impossibleBoot = on; }
ItemCycler CurrentItemCycler() { return ItemCycler(local_vs_detail::itemCycler.load()); }
ItemDistribution CurrentItemDistribution() { return ItemDistribution(local_vs_detail::itemDistribution.load()); }
uint32_t PointsFor(uint32_t place, uint32_t count) {
    using namespace local_vs_detail;
    if (place<1 || place>count) return 0;
    switch (Scoring(scoring.load())) {
    case Scoring::Linear:
        return count-place+1+(place==1 ? 3 : place==2 ? 1 : 0);
    case Scoring::ClassicTop12:
        return place<=classicPoints.size() ? classicPoints[place-1] : 0;
    case Scoring::ClassicSpread:
    default: {
        // The 12-player curve stretched over the field (1st..last -> 15..0) and scaled by
        // count / 12, so it keeps its steep top at any size. 12 racers give the stock table.
        if (count<2) return classicPoints[0];
        const double x=double(place-1)*double(classicPoints.size()-1)/double(count-1);
        const uint32_t i=std::min<uint32_t>(uint32_t(x), uint32_t(classicPoints.size()-2));
        const double v=classicPoints[i]+(double(classicPoints[i+1])-double(classicPoints[i]))*(x-i);
        return uint32_t(v*double(count)/double(classicPoints.size())+0.5);
    }
    }
}
uint32_t ItemRowFor(uint32_t position, uint32_t count, uint32_t rows) {
    if (rows==0 || count==0) return 0;
    const double t=double(position)/double(count);  // 0 for the leader, < 1 for last
    uint32_t row;
    switch (CurrentItemDistribution()) {
    case ItemDistribution::Generous: row=uint32_t(double(rows)*std::sqrt(t)); break;  // back rows reach further forward
    case ItemDistribution::Tight:    row=uint32_t(double(rows)*t*t); break;           // back rows only near the back
    case ItemDistribution::ClassicTop11: row=position; break;                         // 1..11 stock, the rest last place
    case ItemDistribution::Even:
    default: row=position*rows/count; break;
    }
    return std::min(row, rows-1);
}
// Retro Rewind is capped at 100 for now: its per-racer skins (rr_skins.cpp) give every racer its
// own kart archive, and at 128 one no longer fits in memory (Pulsar ArchiveFile::Decompress fails,
// then ARCInitHandle panics).
int MaxRacers() { return RuntimeProduct::IsRetroRewind() ? 100 : int(PlayerSlots::kMaxPlayers); }
int DesiredCount() { return std::min(local_vs_detail::desiredCount.load(), MaxRacers()); }
void SetDesiredCount(int count) {
    count = std::clamp(count, 1, MaxRacers());
    local_vs_detail::desiredCount = count;
    RuntimeConfigFile::WriteSetting("local_vs", "players", std::to_string(count));
}
bool UnlocksEnabled() { return !BootRace::Scripted() && local_vs_detail::unlocks.load(); }
void DrawSettings() {
    using namespace local_vs_detail;
    int count = desiredCount.load();
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderInt("Racers", &count, 1, MaxRacers(), "%d", ImGuiSliderFlags_AlwaysClamp)) {
        SetDesiredCount(count);
    }
    ImGui::TextUnformatted("Applies when starting a new single-player VS series.");
    bool varied = diversity.load();
    if (ImGui::Checkbox("Varied CPU characters, karts and bikes", &varied)) {
        diversity = varied;
        RuntimeConfigFile::WriteSetting("local_vs", "diverse_roster", varied ? "true" : "false");
    }
    bool all = unlocks.load();
    if (ImGui::Checkbox("All characters, vehicles and cups available", &all)) {
        unlocks = all;
        RuntimeConfigFile::WriteSetting("local_vs", "unlock_all", all ? "true" : "false");
    }
    ImGui::TextUnformatted("Reopen character selection after changing unlocks.");
    bool half = thirtyFps.load();
    if (ImGui::Checkbox("30 FPS rendering (experimental, next race)", &half)) {
        thirtyFps = half;
        RuntimeConfigFile::WriteSetting("local_vs", "render_30fps", half ? "true" : "false");
    }
    bool speedo = speedometer.load();
    if (ImGui::Checkbox("Speedometer", &speedo)) {
        speedometer = speedo;
        RuntimeConfigFile::WriteSetting("local_vs", "speedometer", speedo ? "true" : "false");
    }
    bool boxes = instantBoxes.load();
    if (ImGui::Checkbox("Instant item boxes in expanded races", &boxes)) {
        instantBoxes = boxes;
        RuntimeConfigFile::WriteSetting("local_vs", "instant_boxes", boxes ? "true" : "false");
    }
    ImGui::TextUnformatted("Impossible CPUs: the CPU row's 4th option on the rules page (Easy, Normal, Hard, Impossible).");
    ImGui::Separator();
    ImGui::TextUnformatted("Balancing for races of more than 12 racers:");
    int score = int(scoring.load());
    ImGui::SetNextItemWidth(280);
    if (ImGui::Combo("Scoring", &score, scoringNames.data(), int(scoringNames.size()))) {
        scoring = uint32_t(score);
        RuntimeConfigFile::WriteSetting("local_vs", "scoring", RuntimeConfigFile::FormatString(scoringKeys[score]));
    }
    {
        const uint32_t n = std::max(13, desiredCount.load());
        ImGui::Text("At %u racers: 1st %u, 2nd %u, 3rd %u, middle %u, last %u.", n, LocalVs::PointsFor(1,n),
                    LocalVs::PointsFor(2,n), LocalVs::PointsFor(3,n), LocalVs::PointsFor((n+1)/2,n), LocalVs::PointsFor(n,n));
    }
    int items = int(itemDistribution.load());
    ImGui::SetNextItemWidth(280);
    if (ImGui::Combo("Item distribution", &items, itemNames.data(), int(itemNames.size()))) {
        itemDistribution = uint32_t(items);
        RuntimeConfigFile::WriteSetting("local_vs", "item_distribution", RuntimeConfigFile::FormatString(itemKeys[items]));
    }
    {
        static constexpr std::array<const char*,4> help{{
            "The game's 12 position rows are spread evenly over the field.",
            "More of the field gets back-of-the-pack items.",
            "Strong items only for the back of the field.",
            "Places 1-11 use the stock rows; everyone behind gets last-place items."}};
        ImGui::TextUnformatted(help[std::min<uint32_t>(uint32_t(items), 3)]);
        ImGui::TextUnformatted("Item distribution applies from the next race.");
    }
    ImGui::Separator();
    int cycler = int(itemCycler.load());
    ImGui::SetNextItemWidth(280);
    if (ImGui::Combo("Debug item cycler", &cycler, cyclerNames.data(), int(cyclerNames.size()))) {
        itemCycler = uint32_t(cycler);
        RuntimeConfigFile::WriteSetting("local_vs", "item_cycler", RuntimeConfigFile::FormatString(cyclerKeys[cycler]));
    }
    ImGui::TextUnformatted("Offline only. Y + D-pad left/right picks an item (D-pad alone on a Nunchuk). By _Ro.");
    ImGui::Separator();
    {
        static constexpr std::array<const char*, 7> teamNames{{"2 teams","3 teams","4 teams","5 teams","6 teams","7 teams","8 teams"}};
        int teams = int(Teams::Count()) - 2;
        ImGui::SetNextItemWidth(280);
        int size = int(Teams::TeamSize());
        int by = Teams::CustomSizes() ? 2 : size ? 1 : 0;
        static constexpr std::array<const char*, 3> byNames{{"Team count", "Team size", "Custom sizes"}};
        ImGui::SetNextItemWidth(280);
        if (ImGui::Combo("Team VS teams by", &by, byNames.data(), int(byNames.size()))) {
            Teams::SetCustomSizes(by == 2);
            if (by == 0) Teams::SetTeamSize(0);
            if (by == 1) Teams::SetTeamSize(std::max(size, 4));
        }
        ImGui::SetNextItemWidth(280);
        if (by == 0) {
            if (ImGui::Combo("Team count", &teams, teamNames.data(), int(teamNames.size()))) Teams::SetCount(uint32_t(teams) + 2);
        } else if (by == 1) {
            if (ImGui::SliderInt("Racers per team", &size, 2, 64)) Teams::SetTeamSize(uint32_t(size));
        } else {
            uint32_t sum = 0, used = 0;
            for (uint32_t k = 0; k < Teams::kMaxTeams; ++k) {
                const auto& colour = Teams::ColourByIndex(k);
                int racers = int(Teams::ColourSize(k));
                ImGui::PushID(int(k));
                ImGui::SetNextItemWidth(200);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(colour.r / 255.f, colour.g / 255.f, colour.b / 255.f, 1.f));
                if (ImGui::SliderInt(colour.name, &racers, 0, int(PlayerSlots::kMaxPlayers))) Teams::SetColourSize(k, uint32_t(racers));
                ImGui::PopStyleColor();
                ImGui::PopID();
                sum += uint32_t(racers); used += racers > 0;
            }
            ImGui::Text("%u teams, %u racers set; scaled to the field when that differs (0 = colour unused).", used, sum);
        }
        {
            static constexpr std::array<const char*, Teams::kMaxTeams + 1> colourNames{{
                "Random", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Pink", "Cyan"}};
            int colour = Teams::PlayerColour() < Teams::kMaxTeams ? int(Teams::PlayerColour()) + 1 : 0;
            ImGui::SetNextItemWidth(280);
            if (ImGui::Combo("Your team colour", &colour, colourNames.data(), int(colourNames.size())))
                Teams::SetPlayerColour(colour == 0 ? Teams::kNoTeam : uint32_t(colour - 1));
        }
        int scoring = Teams::ScoreByAverage() ? 0 : 1;
        static constexpr std::array<const char*, 2> scoringNames{{"Average per member", "Total"}};
        ImGui::SetNextItemWidth(280);
        if (ImGui::Combo("Team score", &scoring, scoringNames.data(), int(scoringNames.size()))) Teams::SetScoreByAverage(scoring == 0);
        bool hits = Teams::TeamHits();
        if (ImGui::Checkbox("Team hits (items hit teammates)", &hits)) Teams::SetTeamHits(hits);
        ImGui::TextUnformatted("Team VS with more than 12 racers (up to 12 it is the game's red vs blue). At most 8 teams:\n"
                               "a team size that needs more makes the teams bigger. Average keeps uneven teams fair.");
        const auto teamRows = Teams::LastStandings();
        if (!teamRows.empty() && ImGui::BeginTable("Team standings", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Team"); ImGui::TableSetupColumn("Members");
            ImGui::TableSetupColumn("Points"); ImGui::TableSetupColumn("Average"); ImGui::TableHeadersRow();
            for (const auto& s : teamRows) {
                const auto& colour = Teams::ColourOf(s.team);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(colour.r / 255.f, colour.g / 255.f, colour.b / 255.f, 1.f), "%s", colour.name);
                ImGui::TableNextColumn(); ImGui::Text("%u", s.members);
                ImGui::TableNextColumn(); ImGui::Text("%u", s.total);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", s.average);
            }
            ImGui::EndTable();
        }
    }
    std::vector<Entry> copy;
    { std::lock_guard lock(standingsMutex); copy = standings; }
    if (!copy.empty() && ImGui::BeginTable("Full race results", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(520,320))) {
        ImGui::TableSetupColumn("Place"); ImGui::TableSetupColumn("Racer");
        ImGui::TableSetupColumn("Earned"); ImGui::TableSetupColumn("Total"); ImGui::TableHeadersRow();
        for (const auto& e : copy) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%u", e.place);
            ImGui::TableNextColumn(); ImGui::Text("%s #%u%s", e.character < names.size() ? names[e.character] : "Mii", e.id+1, e.id == 0 ? " (you)" : "");
            ImGui::TableNextColumn(); ImGui::Text("+%u", e.earned);
            ImGui::TableNextColumn(); ImGui::Text("%u", e.total);
        }
        ImGui::EndTable();
    }
}

// The stock position counter is a layout animation with twelve pictures, and the game's
// pictures may not be redistributed, so places past 12 are drawn here: slanted seven-segment
// digits with an outline, gold for the podium, shading to blue towards last. Everything is
// sized from the window height, so it stays sharp at any resolution.
namespace {
// Segments a b c d e f g per digit, the usual seven-segment layout.
constexpr std::array<uint8_t,10> kSegments{{0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}};

void DrawSegmentDigit(ImDrawList* list, ImVec2 origin, float height, uint32_t digit, ImU32 fill, ImU32 outline) {
    const float w = height * 0.5f, t = height * 0.14f, slant = height * 0.12f;
    // Segment rectangles in a unit box (x right, y down, 0..1), slanted afterwards.
    struct Seg { float x0, y0, x1, y1; };
    const float h = 0.5f, tw = t / w, th = t / height;
    const Seg segs[7] = {{tw,0,1-tw,th},{1-tw,th,1,h-th/2},{1-tw,h+th/2,1,1-th},{tw,1-th,1-tw,1},
                         {0,h+th/2,tw,1-th},{0,th,tw,h-th/2},{tw,h-th/2,1-tw,h+th/2}};
    auto point = [&](float x, float y) { return ImVec2(origin.x + x*w + (1-y)*slant, origin.y + y*height); };
    for (int pass = 0; pass < 2; ++pass) {
        const float grow = pass == 0 ? t * 0.35f : 0.0f;
        for (int i = 0; i < 7; ++i) {
            if (!(kSegments[digit] >> i & 1)) continue;
            const Seg& g = segs[i];
            const float gx = grow / w, gy = grow / height;
            const ImVec2 q[4] = {point(g.x0-gx,g.y0-gy), point(g.x1+gx,g.y0-gy), point(g.x1+gx,g.y1+gy), point(g.x0-gx,g.y1+gy)};
            list->AddConvexPolyFilled(q, 4, pass == 0 ? outline : fill);
        }
    }
}

float DrawSegmentNumber(ImDrawList* list, ImVec2 origin, float height, uint32_t value, ImU32 fill, ImU32 outline) {
    char text[8];
    std::snprintf(text, sizeof text, "%u", value);
    for (const char* c = text; *c; ++c) {
        DrawSegmentDigit(list, origin, height, uint32_t(*c - '0'), fill, outline);
        origin.x += height * 0.72f;
    }
    return origin.x;
}
}

void DrawSpeedometer() {
    using namespace local_vs_detail;
    if (!speedometer.load() || NowMs() - hudSpeedTime.load() > 250) return;
    const auto* vp = ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    char text[32];
    std::snprintf(text, sizeof text, "%.0f", hudSpeed.load());
    const float size = vp->Size.y * 0.06f;
    ImFont* font = ImGui::GetFont();
    const ImVec2 at(vp->Pos.x + vp->Size.x * 0.04f, vp->Pos.y + vp->Size.y * 0.86f);
    for (int dx = -2; dx <= 2; dx += 2)
        for (int dy = -2; dy <= 2; dy += 2)
            if (dx || dy) list->AddText(font, size, ImVec2(at.x + dx, at.y + dy), IM_COL32(15, 15, 30, 235), text);
    list->AddText(font, size, at, IM_COL32(255, 255, 255, 255), text);
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    list->AddText(font, size * 0.45f, ImVec2(at.x + w + size * 0.15f, at.y + size * 0.48f), IM_COL32(230, 230, 230, 230), "speed");
}

void DrawHud() {
    using namespace local_vs_detail;
    DrawSpeedometer();
    const uint32_t packed=hudRank.load();
    if (!packed || NowMs()-hudTime.load()>250) return;
    const uint32_t rank = packed & 255, count = packed >> 8;
    const auto* vp=ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (hudIcons.load()) {  // the game's counter shows the place; keep the game speed readout
        const uint32_t rate = simulationRate.load();
        if (rate) {
            char speed[32];
            std::snprintf(speed, sizeof speed, "Game speed %u%%", rate*100/60);
            list->AddText(ImVec2(vp->Pos.x + vp->Size.x*0.96f - ImGui::CalcTextSize(speed).x, vp->Pos.y + vp->Size.y*0.95f),
                          IM_COL32(235,235,235,200), speed);
        }
        return;
    }
    const float big = vp->Size.y * 0.13f, small = big * 0.38f;
    // Gold for 1st to 3rd, then from white-blue to deep blue towards last.
    const float f = count > 1 ? float(rank - 1) / float(count - 1) : 0.0f;
    const ImU32 fill = rank <= 3 ? IM_COL32(255, 205, 40, 255)
        : IM_COL32(int(230 - 190*f), int(240 - 150*f), 255, 255);
    const ImU32 outline = IM_COL32(15, 15, 30, 235);
    char total[8];
    std::snprintf(total, sizeof total, "%u", count);
    const float width = big * 0.72f * float(rank >= 100 ? 3 : rank >= 10 ? 2 : 1) +
                        small * 0.72f * float(std::strlen(total) + 1);
    ImVec2 at(vp->Pos.x + vp->Size.x * 0.96f - width, vp->Pos.y + vp->Size.y * 0.80f);
    const float end = DrawSegmentNumber(list, at, big, rank, fill, outline);
    // "/N" in small digits on the baseline.
    const ImVec2 slash0(end + small*0.25f, at.y + big), slash1(end + small*0.65f, at.y + big - small);
    list->AddLine(slash0, slash1, outline, small * 0.22f);
    list->AddLine(slash0, slash1, IM_COL32(235,235,235,255), small * 0.12f);
    DrawSegmentNumber(list, ImVec2(end + small*0.75f, at.y + big - small), small, count, IM_COL32(235,235,235,255), outline);
    // Game speed stays in a small corner readout.
    const uint32_t rate = simulationRate.load();
    if (rate) {
        char speed[32];
        std::snprintf(speed, sizeof speed, "Game speed %u%%", rate*100/60);
        list->AddText(ImVec2(vp->Pos.x + vp->Size.x*0.96f - ImGui::CalcTextSize(speed).x, at.y + big + small*0.3f), IM_COL32(235,235,235,200), speed);
    }
}

// Opt-in integration exercise using real race objects, never enabled by a launcher.
void TestItems(CpuContext* ctx, uint32_t info) {
    static uint32_t observedInfo=0, lastFrame=0;
    static int64_t lastTime=0;
    const int64_t now=local_vs_detail::NowMs();
    const uint32_t frame=Memory::Read32(info+0x20);
    if (observedInfo!=info || frame<lastFrame || (Memory::Read32(0x809C1874)&1)) {
        observedInfo=info; lastFrame=frame; lastTime=now;
    } else if (now-lastTime>=1000) {
        local_vs_detail::simulationRate=uint32_t((frame-lastFrame)*1000/(now-lastTime));
        lastFrame=frame; lastTime=now;
    }
    static const bool enabled=std::getenv("MKW100_TEST_ITEMS")!=nullptr;
    static bool fired=false;
    if (!enabled || !BootRace::Scripted() || fired || !local_vs_detail::ExpandedOffline() ||
        Memory::Read32(info+0x28)!=2 || Memory::Read32(info+0x20)<600) return;
    fired=true;
    const CpuContext saved=*ctx;
    const uint32_t count=local_vs_detail::RaceCount();
    ctx->gpr[3]=count-1;
    MKW_GUEST_FUNC(807B7C20)(ctx);
    *ctx=saved;
    const uint32_t manager=Memory::Read32(0x809C3660); // PAL PowMgr singleton
    const uint32_t players=Memory::Read32(info+0xC);
    uint32_t powOwner=12, worstPlace=0;
    for (uint32_t i=12;i<count;++i) {
        const uint32_t place=Memory::Read8(Memory::Read32(players+4*i)+0x20);
        if (place>worstPlace) { worstPlace=place; powOwner=i; }
    }
    ctx->gpr[3]=manager; ctx->gpr[4]=powOwner;
    InvokeIndirectCpu(MKW_GADDR(807B1DB4),ctx); // exercise the patched entry, not the base translation
    *ctx=saved;
    RT_LOGF("local-vs", "mechanics test injected lightning from slot %u and POW from slot %u (place %u)\n",count-1,powOwner,worstPlace);
}

void PrepareRace(uint32_t rd) {
    using namespace local_vs_detail;
    if (BootRace::Requested()) return; // keep the deterministic direct-boot instrument separate
    const uint32_t menu = rd + 0xC10;
    RT_LOGF("local-vs", "prepare: mode=%u type=%u flags=0x%X p0=%u p1=%u raceNumber=%u\n",
        Memory::Read32(menu+0xB50),Memory::Read32(menu+0xB54),Memory::Read32(menu+0xB70),
        Memory::Read32(Player(menu,0)+0x10),Memory::Read32(Player(menu,1)+0x10),Memory::Read8(menu+0xB6C));
    // The CPU row's 4th option is Impossible (vs_racers_option.cpp: "Off" relabelled, also offered
    // in 1P). It stores cpuMode 3, the game's "no CPUs": the race runs Hard CPUs with
    // ai_impossible.cpp's tables, and in 2-4P the slots the game left empty get CPUs.
    impossibleCpus = Memory::Read32(menu+0xB5C)==3;
    if (impossibleCpus.load()) {
        Memory::Write32(menu+0xB5C,2);
        const uint32_t base=Memory::Read32(Player(menu,0)+0xC);
        for (uint32_t i=1;i<12;++i) {
            const uint32_t p=Player(menu,i);
            if (Memory::Read32(p+0x10)!=5) continue;
            const uint32_t character=(base+i)%24;
            Memory::Write32(p+0x10,1); Memory::Write32(p+0xC,character); Memory::Write32(p+8,weights[character]);
        }
        RT_LOGF("local-vs", "impossible CPUs this race (CPU row option 4)\n");
    }
    const uint32_t gameType=Memory::Read32(menu+0xB54);
    const uint32_t firstType=Memory::Read32(Player(menu,0)+0x10);
    const bool preview=gameType==5;
    bool solo = firstType == 0 || (preview && firstType == 1);
    for (uint32_t i=1;i<12;++i) solo = solo && Memory::Read32(Player(menu,i)+0x10) != 0;
    // Offline VS, solo or Team VS (past 12 racers teams.cpp takes it over after InitRace).
    const bool teamVs = (Memory::Read32(menu+0xB70)&2)!=0;
    // Offline GP, VS or battle (gameMode 0, 1, 3: battles are always red/blue teams; balloon_battle.cpp).
    const uint32_t mode = Memory::Read32(menu+0xB50);
    const bool eligible = solo && (mode==0 || mode==1 || mode==3) && (gameType==0 || preview);
    if (!eligible) {
        for (uint32_t i=12;i<PlayerSlots::kMaxPlayers;++i) Memory::Write32(Player(menu,i)+0x10,5);
        seriesCount=0;
        return;
    }
    const bool first = Memory::Read8(menu+0xB6C)==0 || seriesCount==0;
    if (first) {
        seriesCount=desiredCount.load();
        { std::lock_guard lock(standingsMutex); standings.clear(); }
    }
    const uint32_t humanCharacter=Memory::Read32(Player(menu,0)+0xC);
    for (uint32_t i=0;i<PlayerSlots::kMaxPlayers;++i) {
        const uint32_t p=Player(menu,i);
        Memory::Write32(p+0x10, i>=seriesCount ? 5 : i==0 ? firstType : 1);
        if (i && i<seriesCount && (diversity.load() || i>=12)) {
            const uint32_t character=(humanCharacter+ i)%24;
            // Battles load red/blue kart models (%s_red/_blue.brres), which only the standard kart
            // and bike have (S/M/L_Kart_Red ...): there the CPUs take one of those two.
            const uint32_t vehicle = mode==3 ? weights[character] + ((i/3)%2 ? 18u : 0u)
                                             : weights[character]+3*((i/3)%12);
            Memory::Write32(p+0xC,character); Memory::Write32(p+8,vehicle);
        }
        if (first) {
            Memory::Write16(p+0xD8,0); Memory::Write16(p+0xDA,0);
            Memory::Write8(p+0xE1, i<seriesCount ? seriesCount-i : 0);
            Memory::Write8(p+0xE2, i<seriesCount ? seriesCount-i : 0);
        }
    }
    if (teamVs && mode==3 && seriesCount>12 && first) {
        // Battle past 12: red/blue by the battle settings (Teams::BattleRedFor, BattleTeamChoice).
        const uint32_t choice=Teams::BattleTeamChoice();
        const uint32_t dealt=Memory::Read32(Player(menu,0)+0xCC);
        const uint32_t mine=choice==1 ? 0 : choice==2 ? 1 : (dealt<2 ? dealt : 0);
        const uint32_t red=Teams::BattleRedFor(seriesCount);
        std::vector<uint32_t> cpus(seriesCount-1);
        std::iota(cpus.begin(),cpus.end(),1u);
        std::mt19937 random(uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::shuffle(cpus.begin(),cpus.end(),random);
        const uint32_t redCpus=std::min<uint32_t>(uint32_t(cpus.size()), red-(mine==0 ? 1 : 0));
        Memory::Write32(Player(menu,0)+0xCC,mine);
        for (uint32_t k=0;k<cpus.size();++k) Memory::Write32(Player(menu,cpus[k])+0xCC,k<redCpus ? 0 : 1);
        RT_LOGF("local-vs", "battle teams: red %u blue %u (you %s)\n", redCpus+(mine==0), uint32_t(cpus.size())-redCpus+(mine==1),
                mine==0 ? "red" : "blue");
    } else if (teamVs && mode==3 && seriesCount>12) {
        // Later battles of the series keep the teams dealt at the first.
    } else if (teamVs) {
        // The team page assigns the first 12; the rest join whichever team is smaller.
        uint32_t size[2] = {0, 0};
        for (uint32_t i=0;i<12 && i<seriesCount;++i) { const uint32_t t=Memory::Read32(Player(menu,i)+0xCC); if (t<2) ++size[t]; }
        for (uint32_t i=12;i<seriesCount;++i) {
            const uint32_t t = size[0] <= size[1] ? 0 : 1;
            Memory::Write32(Player(menu,i)+0xCC,t); ++size[t];
        }
        RT_LOGF("local-vs", "team vs: red %u blue %u\n", size[0], size[1]);
    }
    RT_LOGF("local-vs", "menu race: course=0x%02X racers=%u raceNumber=%u humanCharacter=%u\n",
            Memory::Read32(menu+0xB48),seriesCount,Memory::Read8(menu+0xB6C),humanCharacter);
}
}

// GP: between races the game recomputes each racer's start position (race scenario player +0xE1)
// from the cup points, for racers 0-11 only; the rest kept their first-race value and collided
// with those (race_positions.cpp: "racer 13 has rank 11 of 24"). Past 12 every racer's start
// position is set here instead: by series total (+0xDA), ties by the last race's place (+0xE2).
namespace LocalVs {
void AfterInitRace(uint32_t rd) {
    using namespace local_vs_detail;
    const uint32_t race = rd + 0x20;
    if (Memory::Read32(race + 0xB50) != 0 || Memory::Read8(race + 0xB6C) == 0) return;  // GP, after race 1
    const uint32_t count = RaceCount();
    if (count <= 12 || count > PlayerSlots::kMaxPlayers) return;
    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const uint32_t ta = Memory::Read16(Player(race, a) + 0xDA), tb = Memory::Read16(Player(race, b) + 0xDA);
        if (ta != tb) return ta > tb;
        return Memory::Read8(Player(race, a) + 0xE2) < Memory::Read8(Player(race, b) + 0xE2);
    });
    for (uint32_t k = 0; k < count; ++k) Memory::Write8(Player(race, order[k]) + 0xE1, uint8_t(k + 1));
    RT_LOGF("local-vs", "gp grid: %u racers by standings (leader racer %u)\n", count, order[0]);
}
} // namespace LocalVs

// The vanilla function has a 12-entry stack array and indexes pointsRoom[12][12].
// Never call it for expanded offline VS, even to repair the result afterward.
extern "C" void LocalVs_UpdatePoints_8052e950(CpuContext* ctx) {
    using namespace local_vs_detail;
    if (!ExpandedOffline()) { MKW_GUEST_NEXT(8052E950)(ctx); return; }
    const uint32_t menu=ctx->gpr[3], rd=Memory::Read32(MKW_GADDR(809BD728));
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t infos=Memory::Read32(info+0xC), count=RaceCount();
    std::vector<Entry> rows;
    for (uint32_t i=0;i<count;++i) {
        const uint32_t p=Player(menu,i), raceP=Player(rd+0x20,i);
        const uint32_t place=Memory::Read8(Memory::Read32(infos+4*i)+0x20);
        const uint32_t earned=LocalVs::PointsFor(place,count);
        const uint32_t previous=Memory::Read16(raceP+0xD8);
        const uint32_t total=std::min(65535u,previous+earned);
        Memory::Write16(p+0xD8,previous); Memory::Write16(p+0xDA,total);
        Memory::Write8(p+0xE2,place);
        rows.push_back({i,Memory::Read32(raceP+0xC),place,earned,total});
    }
    std::stable_sort(rows.begin(),rows.end(),[](const Entry& a,const Entry& b){return a.place<b.place;});
    { std::lock_guard lock(standingsMutex); standings=std::move(rows); }
    RT_LOGF("local-vs", "scored %u racers without vanilla 12-player table\n",count);
    if (Teams::Active()) {
        std::vector<std::pair<uint32_t, uint32_t>> totals;
        { std::lock_guard lock(standingsMutex); for (const auto& e : standings) totals.emplace_back(e.id, e.total); }
        Teams::SetStandings(Teams::Rank(totals));
        Teams::Log("series totals", totals);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8052E950, LocalVs_UpdatePoints_8052e950, "LocalVs_UpdatePoints_8052e950");

// Only twelve native rows exist; the pages below scroll a 12-row window over the whole field.
extern "C" void LocalVs_RowCount_8085c4f4(CpuContext* ctx) {
    MKW_GUEST_NEXT(8085C4F4)(ctx);
    ctx->gpr[3]=std::min(ctx->gpr[3],12u);
}
REGISTER_NATIVE_WRAPPER_AS(0x8085C4F4, LocalVs_RowCount_8085c4f4, "LocalVs_RowCount_8085c4f4");

// Scrolling results. Both leaderboard pages (after each race: GPVSLeaderboardUpdate, by place;
// series standings: GPVSLeaderboardTotal, by total) keep the game's 12 CtrlRaceResult rows and
// show a 12-row window of the whole field. Up/down moves the window a row (holding the stick
// repeats), left/right a page; the view opens around the local player. Rows come from this
// race's scoring (standings: place, points earned, total), not Raceinfo's place table. A scroll
// refills place, final total and name or time only: re-arming a row's score animation or the
// page's frame counter (+0x44) meets the page's end condition and leaves the page. Places past 12
// have no message (BMG 1300 + place covers 1st..12th), so their place text box gets a runtime
// message, "13th", "21st", ... in 12th's font (custom_messages.cpp); text set directly is lost,
// the row's message holder re-applies its message when drawn. The "+N" box (get_point) is set
// with message 1315 and the racer's points on every fill: FillScoreDiff only runs on the first
// fill, because re-arming its animation ends the page.
namespace results_scroll {
constexpr uint32_t kRows = 12;
constexpr uint32_t kPositionPane = 0x808AA470;  // "position", CtrlRaceResult's place text box
constexpr uint32_t kGetPointPane = 0x808AF204;  // "get_point", hidden on the standings page
constexpr uint32_t kNumberMessage = 1313;       // FillScore's style message ("pts")
constexpr uint32_t kIntegerMessage = 1314;      // Race.bmg: just integer 0 (the score's number)
constexpr uint32_t kPointsPane = 0x808AA416;     // "get_point", the "+N" text box
constexpr uint32_t kSignedMessage = 1315;       // Race.bmg: signed integer 0 ("+N")
constexpr uint32_t kNamePane = 0x808AA39F;       // "player_name"
uint32_t page = 0;       // the leaderboard page being scrolled, 0 when none
bool standingsPage = false;
uint32_t offset = 0;
uint32_t count = 0;
uint32_t repeat = 0;
std::array<uint32_t, 12> rowPlayer{};  // racer shown in each row, for the team tint re-applied each frame

uint32_t Control(uint32_t pg, uint32_t row) { return Memory::Read32(Memory::Read32(pg + 0x1A0) + 4 * row); }

void Call(CpuContext* ctx, const CpuContext& saved, uint32_t fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
          uint32_t r6 = 0) {
    *ctx = saved;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6;
    InvokeIndirectCpu(fn, ctx);
}

// LayoutUIControl::SetTextBoxMessage(control, pane, bmg, info) with intToPass[0] = value; the
// Text::Info sits below the caller's stack (the message holder copies it, as FillScore's does).
void SetPaneMessage(CpuContext* ctx, const CpuContext& saved, uint32_t control, uint32_t pane, uint32_t bmg,
                    uint32_t value) {
    const uint32_t info = (saved.gpr[1] - 0x200) & ~0xFu;
    for (uint32_t i = 0; i < 0xC4; i += 4) Memory::Write32(info + i, 0);
    Memory::Write32(info, value);
    *ctx = saved;
    ctx->gpr[1] = info - 0x80;
    ctx->gpr[3] = control; ctx->gpr[4] = pane; ctx->gpr[5] = bmg; ctx->gpr[6] = info;
    InvokeIndirectCpu(0x8063DCBC, ctx);
}

// CtrlRaceResult::Fill(control, place, player) with the place text set for any place.
void FillPlace(CpuContext* ctx, const CpuContext& saved, uint32_t control, uint32_t place, uint32_t player) {
    Call(ctx, saved, 0x807F5FEC, control, std::min(place, kRows), player);
    if (place <= kRows) return;
    const uint32_t ordinal = CustomMessages::PlaceBmg(place);
    SetPaneMessage(ctx, saved, control, kPositionPane, ordinal ? ordinal : kIntegerMessage, place);
}

std::vector<local_vs_detail::Entry> StandingsByPlace() {
    std::vector<local_vs_detail::Entry> rows;
    { std::lock_guard lock(local_vs_detail::standingsMutex); rows = local_vs_detail::standings; }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.place < b.place; });
    return rows;
}

// A row of the after-race page: place, score before the race and what it earned, name or time.
void FillRaceRow(CpuContext* ctx, const CpuContext& saved, uint32_t pg, uint32_t row, bool animate,
                 const std::vector<local_vs_detail::Entry>& byPlace) {
    if (offset + row >= byPlace.size()) return;
    const auto& e = byPlace[offset + row];
    const uint32_t rd = Memory::Read32(MKW_GADDR(809BD728));
    const uint32_t place = e.place;
    const uint32_t player = e.id;
    const uint32_t control = Control(pg, row);
    const uint32_t after = e.total;
    const uint32_t before = e.total - e.earned;
    FillPlace(ctx, saved, control, place, player);
    Call(ctx, saved, 0x807F56D4, control, animate ? before : after, kNumberMessage);  // FillScore
    if (animate) Call(ctx, saved, 0x807F579C, control, after - before, row == 0 ? 1 : 0);  // FillScoreDiff
    SetPaneMessage(ctx, saved, control, kPointsPane, kSignedMessage, e.earned);  // "+N" of this racer
    Call(ctx, saved, Memory::Read32(rd + 0xB70) == 0 ? 0x807F595C : 0x807F52F4, control, player);  // time or name
    rowPlayer[row] = player;
    Teams::TintResultRow(ctx, control, player);
    *ctx = saved;
}

// A team row of the standings page: team place, its best racer's icon, team name, average.
void FillTeamRow(CpuContext* ctx, const CpuContext& saved, uint32_t pg, uint32_t row, uint32_t place,
                 const Teams::Standing& s, const std::vector<local_vs_detail::Entry>& rows) {
    uint32_t best = 0;
    for (const auto& e : rows) if (Teams::TeamOf(e.id) == s.team) { best = e.id; break; }  // rows: best total first
    const uint32_t control = Control(pg, row);
    FillPlace(ctx, saved, control, place, best);
    Call(ctx, saved, 0x807F56D4, control, s.Score(), kNumberMessage);  // FillScore: average or total
    Call(ctx, saved, 0x8063DEEC, control, kGetPointPane);
    SetPaneMessage(ctx, saved, control, kNamePane, CustomMessages::TeamNameBmg(Teams::ColourIndexOf(s.team), Teams::ScoreByAverage()), 0);
    rowPlayer[row] = best;
    Teams::TintResultRow(ctx, control, best);
    *ctx = saved;
}

void FillStandingsRows(CpuContext* ctx, const CpuContext& saved, uint32_t pg,
                       const std::vector<local_vs_detail::Entry>& rows) {
    const auto teams = Teams::Active() ? Teams::LastStandings() : std::vector<Teams::Standing>{};
    for (uint32_t row = 0; row < kRows && offset + row < teams.size() + rows.size(); ++row) {
        if (offset + row < teams.size()) {
            FillTeamRow(ctx, saved, pg, row, offset + row + 1, teams[offset + row], rows);
            continue;
        }
        const uint32_t index = offset + row - uint32_t(teams.size());
        const auto& e = rows[index];
        const uint32_t control = Control(pg, row);
        const uint32_t entry = Memory::Read32(pg + 0x1A4) + 12 * row;
        Memory::Write32(entry, e.total); Memory::Write32(entry + 4, e.earned); Memory::Write8(entry + 8, e.id);
        FillPlace(ctx, saved, control, index + 1, e.id);
        Call(ctx, saved, 0x807F56D4, control, e.total, kNumberMessage);
        Call(ctx, saved, 0x8063DEEC, control, kGetPointPane);  // as the game's standings page does
        Call(ctx, saved, 0x807F52F4, control, e.id);           // FillName
        rowPlayer[row] = e.id;
        Teams::TintResultRow(ctx, control, e.id);
        *ctx = saved;
    }
}

std::vector<local_vs_detail::Entry> StandingsByTotal() {
    std::vector<local_vs_detail::Entry> rows;
    { std::lock_guard lock(local_vs_detail::standingsMutex); rows = local_vs_detail::standings; }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.total > b.total; });
    return rows;
}

// The window that shows place `place` (1-based) about mid-screen.
uint32_t OffsetAround(uint32_t place, uint32_t total) {
    if (total <= kRows || place == 0) return 0;
    const uint32_t wanted = place > kRows / 2 ? place - kRows / 2 : 0;
    return std::min(wanted, total - kRows);
}
} // namespace results_scroll

// GPVSLeaderboardUpdate::FillRows: score the race once, then fill a window around the player.
extern "C" void LocalVs_UpdatePoints_8052e950(CpuContext* ctx);
extern "C" void LocalVs_UpdateRows_8085c858(CpuContext* ctx) {
    using namespace results_scroll;
    if (!local_vs_detail::ExpandedOffline()) { MKW_GUEST_NEXT(8085C858)(ctx); return; }
    const uint32_t pg = ctx->gpr[3];
    const CpuContext saved = *ctx;
    ctx->gpr[3] = Memory::Read32(MKW_GADDR(809BD728)) + 0xC10;  // menu scenario
    LocalVs_UpdatePoints_8052e950(ctx);
    const auto byPlace = StandingsByPlace();
    uint32_t mine = 0;
    for (const auto& e : byPlace) if (e.id == 0) mine = e.place;
    page = pg; standingsPage = false; count = uint32_t(byPlace.size()); repeat = 0;
    offset = OffsetAround(mine, count);
    for (uint32_t row = 0; row < kRows && row < count; ++row) FillRaceRow(ctx, saved, pg, row, true, byPlace);
    *ctx = saved;
    InvokeIndirectCpu(Memory::Read32(Memory::Read32(pg) + 0x6C), ctx);  // vf 0x6c, as the game's FillRows ends
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8085C858, LocalVs_UpdateRows_8085c858, "LocalVs_UpdateRows_8085c858");

// GPVSLeaderboardTotal::FillRows: the series standings, by total, around the player.
extern "C" void LocalVs_TotalRows_8085cae8(CpuContext* ctx) {
    using namespace results_scroll;
    if (!local_vs_detail::ExpandedOffline()) { MKW_GUEST_NEXT(8085CAE8)(ctx); return; }
    const uint32_t pg = ctx->gpr[3];
    const auto rows = StandingsByTotal();
    uint32_t mine = 0;
    for (uint32_t i = 0; i < rows.size(); ++i) if (rows[i].id == 0) mine = i + 1;
    page = pg; standingsPage = true; repeat = 0;
    count = uint32_t(rows.size() + (Teams::Active() ? Teams::LastStandings().size() : 0));
    {
        const uint32_t rd = Memory::Read32(MKW_GADDR(809BD728));
        for (uint32_t i = 0; i < rows.size(); ++i) Memory::Write8(rd + MKW100_PlayerSlotOffset(rows[i].id) + 0xCF8, uint8_t(i + 1));
    }
    // With teams the page opens at the top, on the team standings; otherwise around the player.
    offset = Teams::Active() ? 0 : OffsetAround(mine, count);
    const CpuContext saved = *ctx;
    FillStandingsRows(ctx, saved, pg, rows);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8085CAE8, LocalVs_TotalRows_8085cae8, "LocalVs_TotalRows_8085cae8");

// Leaderboard::AfterControlUpdate: scroll the page set up above with the local player's input.
extern "C" void LocalVs_LeaderboardScroll_8085c0c4(CpuContext* ctx) {
    using namespace results_scroll;
    const uint32_t pg = ctx->gpr[3];
    MKW_GUEST_NEXT(8085C0C4)(ctx);
    if (pg != page || !local_vs_detail::ExpandedOffline()) return;
    if (Teams::Active()) {  // the rows' own animations repaint them every frame
        const CpuContext before = *ctx;
        for (uint32_t row = 0; row < kRows && row < count; ++row) Teams::TintResultRow(ctx, Control(pg, row), rowPlayer[row]);
        *ctx = before;
    }
    if (count <= kRows) return;
    const CpuContext saved = *ctx;
    Call(ctx, saved, 0x805EEE0C, 0);  // ManipulatorManager::GetControllerHolder(0)
    const uint32_t holder = ctx->gpr[3];
    *ctx = saved;
    if (holder < 0x80000000u) return;
    const uint32_t now = Memory::Read16(holder + 0x5C), before = Memory::Read16(holder + 0x90);  // UIState actions
    const float stickY = Memory::ReadFloat32(holder + 0x64);
    const auto pressed = [&](uint32_t bit) { return (now & bit) != 0 && (before & bit) == 0; };
    int step = 0;
    if (pressed(0x8)) step = -1;
    else if (pressed(0x10)) step = 1;
    else if (pressed(0x20)) step = -int(kRows);
    else if (pressed(0x40)) step = int(kRows);
    else if (stickY > 0.6f || stickY < -0.6f) {
        if (++repeat >= 12 && repeat % 4 == 0) step = stickY > 0 ? -1 : 1;  // hold to repeat after ~0.2 s
    } else {
        repeat = 0;
    }
    if (step == 0) return;
    const int last = int(count - kRows);
    const uint32_t moved = uint32_t(std::clamp(int(offset) + step, 0, last));
    if (moved == offset) return;
    offset = moved;
    if (standingsPage) FillStandingsRows(ctx, saved, pg, StandingsByTotal());
    else {
        const auto byPlace = StandingsByPlace();
        for (uint32_t row = 0; row < kRows; ++row) FillRaceRow(ctx, saved, pg, row, false, byPlace);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8085C0C4, LocalVs_LeaderboardScroll_8085c0c4, "LocalVs_LeaderboardScroll_8085c0c4");

extern "C" void LocalVs_CharacterUnlocked_8054fed8(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(8054FED8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8054FED8, LocalVs_CharacterUnlocked_8054fed8, "LocalVs_CharacterUnlocked_8054fed8");
extern "C" void LocalVs_KartUnlocked_805500cc(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(805500CC)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805500CC, LocalVs_KartUnlocked_805500cc, "LocalVs_KartUnlocked_805500cc");
extern "C" void LocalVs_CupUnlocked_8054fc44(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(8054FC44)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8054FC44, LocalVs_CupUnlocked_8054fc44, "LocalVs_CupUnlocked_8054fc44");

// The stock rank animation only contains twelve pictures. Past 12th the counter shows a rank icon
// (rank_icons.cpp) when rank_icons/ is installed; otherwise the overlay draws the number. The
// overlay never reads guest objects that may be destroyed.
namespace {
// Player 0's kart position (Kart::Manager players[0] -> pointers -> body +0x90 holder -> physics,
// position at +0x68; as auto_driver.cpp reads it).
bool LocalVsKartPosition(float& x, float& y, float& z) {
    const auto ram = [](uint32_t a) { return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x98000000u); };
    const uint32_t manager = Memory::Read32(MKW_GADDR(809C18F8));
    if (!ram(manager)) return false;
    const uint32_t players = Memory::Read32(manager + 0x20);
    if (!ram(players)) return false;
    const uint32_t kart = Memory::Read32(players);
    if (!ram(kart)) return false;
    const uint32_t pointers = Memory::Read32(kart);
    if (!ram(pointers)) return false;
    const uint32_t body = Memory::Read32(pointers + 8);
    if (!ram(body)) return false;
    const uint32_t holder = Memory::Read32(body + 0x90);
    if (!ram(holder)) return false;
    const uint32_t physics = Memory::Read32(holder + 4);
    if (!ram(physics)) return false;
    x = Memory::ReadFloat32(physics + 0x68);
    y = Memory::ReadFloat32(physics + 0x6C);
    z = Memory::ReadFloat32(physics + 0x70);
    return true;
}

void LocalVsSampleSpeed() {
    static uint32_t lastFrame = 0;
    static float lx = 0, ly = 0, lz = 0;
    static float smooth = 0;
    const uint32_t info = Memory::Read32(MKW_GADDR(809BD730));
    if (info < 0x80000000u) return;
    const uint32_t frame = Memory::Read32(info + 0x20);
    float x, y, z;
    if (!LocalVsKartPosition(x, y, z)) return;
    if (frame == lastFrame + 1) {
        const float d = std::sqrt((x - lx) * (x - lx) + (y - ly) * (y - ly) + (z - lz) * (z - lz));
        smooth = d < 400.0f ? smooth * 0.7f + d * 0.3f : smooth;  // a respawn jump is not speed
        local_vs_detail::hudSpeed = smooth;
        local_vs_detail::hudSpeedTime = local_vs_detail::NowMs();
    } else if (frame != lastFrame) {
        smooth = 0;
    }
    if (frame != lastFrame) { lastFrame = frame; lx = x; ly = y; lz = z; }
}
} // namespace

extern "C" void LocalVs_Rank_807f4a08(CpuContext* ctx) {
    const uint32_t self=ctx->gpr[3];
    MKW_GUEST_NEXT(807F4A08)(ctx);
    if (Memory::Read32(self+0x19C)==0) LocalVsSampleSpeed();  // any race, player 0's counter
    if (!local_vs_detail::ExpandedOffline()) return;
    const uint32_t id=Memory::Read32(self+0x19C);
    if (id!=0) return; // current menu integration supports one local player
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t player=Memory::Read32(Memory::Read32(info+0xC)+4*id);
    const uint32_t rank=Memory::Read8(player+0x20);
    if (!rank || rank>local_vs_detail::RaceCount()) return;
    const bool icon=RankIcons::Show(ctx,self,rank);
    local_vs_detail::hudIcons=icon;
    if (!icon) Memory::Write8(self+0x80,1);  // no icon: hide the counter, the overlay draws the number
    local_vs_detail::hudRank=rank | (local_vs_detail::RaceCount()<<8);
    local_vs_detail::hudTime=local_vs_detail::NowMs();
}
REGISTER_NATIVE_WRAPPER_AS(0x807F4A08, LocalVs_Rank_807f4a08, "LocalVs_Rank_807f4a08");

// Equivalent to preventing the inactive-state store in Anarion's instant-box
// patch (PAL 80828EDC). Keep the original collision, roulette and sound behavior.
// Reference: https://mariokartwii.com/showthread.php?tid=2206 (Unnamed, credits Anarion).
extern "C" void LocalVs_ItemBox_80828cb8(CpuContext* ctx) {
    const uint32_t self=ctx->gpr[3];
    // Expanded offline GP, VS and battle (battle arenas have item boxes too).
    const uint32_t rd=Memory::Read32(MKW_GADDR(809BD728));
    const uint32_t mode=rd ? Memory::Read32(rd+0x20+0xB50) : 0;
    const bool expanded=rd && local_vs_detail::RaceCount()>12 && (mode<=1 || mode==3) && Memory::Read32(rd+0x20+0xB54)==0;
    const bool instant=expanded && local_vs_detail::instantBoxes.load();
    const uint32_t active=Memory::Read32(self+0xB0);
    MKW_GUEST_NEXT(80828CB8)(ctx);
    if (instant && active==1) Memory::Write32(self+0xB0,1);
}
REGISTER_NATIVE_WRAPPER_AS(0x80828CB8, LocalVs_ItemBox_80828cb8, "LocalVs_ItemBox_80828cb8");

// The normal effect function retains star/bullet/other immunity handling.
// Only replace the rank-to-duration lookup, whose stock guard rejects rank >12.
extern "C" void LocalVs_Lightning_80580438(CpuContext* ctx) {
    if (!local_vs_detail::ExpandedOffline()) { MKW_GUEST_NEXT(80580438)(ctx); return; }
    const uint32_t self=ctx->gpr[3];
    const uint32_t id=Memory::Read8(Memory::Read32(Memory::Read32(self))+0x10);
    const uint32_t count=local_vs_detail::RaceCount();
    if (id>=count) { ctx->gpr[3]=0; return; }
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t place=Memory::Read8(Memory::Read32(Memory::Read32(info+0xC)+4*id)+0x20);
    if (!place || place>count) { ctx->gpr[3]=0; return; }
    const uint32_t row=(place-1)*11/(count-1);
    const uint32_t frames=Memory::Read16(0x80891A20u+2*row)+72; // PAL duration table
    ctx->gpr[4]=frames; ctx->gpr[5]=0; ctx->gpr[6]=0;
    MKW_GUEST_FUNC(80580778)(ctx);
    RT_LOGF("local-vs", "lightning: slot=%u place=%u duration=%u applied=%u\n",id,place,frames,ctx->gpr[3]);
}
REGISTER_NATIVE_WRAPPER_AS(0x80580438, LocalVs_Lightning_80580438, "LocalVs_Lightning_80580438");

// RKSystem's existing half-rate path alternates a simulation-only iteration
// with a rendered iteration. Menus and the deterministic CPU oracle stay default.
extern "C" void LocalVs_Framerate_8051af84(CpuContext* ctx) {
    const bool test30=BootRace::Scripted() && std::getenv("MKW100_TEST_30FPS")!=nullptr;
    if (local_vs_detail::ExpandedOffline() && (test30 || (!BootRace::Scripted() && local_vs_detail::thirtyFps.load()))) {
        ctx->gpr[4]=1;
        RT_LOGF("local-vs", "alternate-frame rendering enabled\n");
    }
    MKW_GUEST_NEXT(8051AF84)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8051AF84, LocalVs_Framerate_8051af84, "LocalVs_Framerate_8051af84");

// GPVSLeaderboardUpdate::GetNextPage (0x8085C974): the Team VS sections go on to the team columns
// page (0x32, TeamVSLeaderboardTotal: six rows a team); a teams race past 12 goes to the scrolling
// standings page (0x30) instead, which lists the teams first (FillStandingsRows).
extern "C" void LocalVs_NextPage_8085c974(CpuContext* ctx) {
    MKW_GUEST_NEXT(8085C974)(ctx);
    if (ctx->gpr[3] == 0x32 && Teams::Active()) ctx->gpr[3] = 0x30;
}
REGISTER_NATIVE_WRAPPER_AS(0x8085C974, LocalVs_NextPage_8085c974, "LocalVs_NextPage_8085c974");

// Section::CreateSectionPages (0x8062C3A4, this, section id) builds a section's pages; the Team VS
// sections have the team columns page (0x32) but no standings page (0x30), so the redirect above
// would activate a page that was never built. Such a section also gets 0x30
// (Section::CreateAndInitPage 0x80622D08; pages[id] at section + 8 + id * 4).
extern "C" void LocalVs_SectionPages_8062c3a4(CpuContext* ctx) {
    const uint32_t section = ctx->gpr[3];
    MKW_GUEST_NEXT(8062C3A4)(ctx);
    constexpr uint32_t kPages = 0x8;
    if (section == 0 || Memory::Read32(section + kPages + 0x32 * 4) == 0 || Memory::Read32(section + kPages + 0x30 * 4) != 0) return;
    const CpuContext saved = *ctx;
    ctx->gpr[3] = section;
    ctx->gpr[4] = 0x30;
    InvokeIndirectCpu(MKW_GADDR(80622D08), ctx);
    *ctx = saved;
    RT_LOGF("local-vs", "team vs section: added the standings page\n");
}
REGISTER_NATIVE_WRAPPER_AS(0x8062C3A4, LocalVs_SectionPages_8062c3a4, "LocalVs_SectionPages_8062c3a4");
