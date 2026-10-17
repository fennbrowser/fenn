#pragma once
#include <string_view>

// Presentation vocabulary only. Firefox supplies the kind and state; no URL,
// permission or security decisions are made here. Empty means extension artwork.
namespace OmnibarIcons {
inline wchar_t const* Glyph(std::wstring_view kind) {
    struct Entry { std::wstring_view kind; wchar_t const* glyph; };
    static constexpr Entry entries[] = {
        {L"connection", L"\xE946"}, {L"warning", L"\xE7BA"}, {L"danger", L"\xEA39"},
        {L"protection", L"\xEA18"}, {L"permissions", L"\xE713"},
        {L"reader", L"\xE736"}, {L"pip", L"\xE8A7"}, {L"taskbar", L"\xE718"},
        {L"translation", L"\xF2B7"}, {L"zoom", L"\xE71E"}, {L"split", L"\xE89A"},
        {L"share", L"\xE72D"}, {L"bookmark", L"\xE734"}, {L"bookmark-filled", L"\xE735"},
        {L"geo", L"\xE707"}, {L"xr", L"\xF119"}, {L"notification", L"\xEA8F"},
        {L"camera", L"\xE722"}, {L"microphone", L"\xE720"}, {L"screen", L"\xE7F4"},
        {L"network", L"\xE968"}, {L"storage", L"\xEDA2"}, {L"popup", L"\xE8A7"},
        {L"autoplay", L"\xE768"}, {L"canvas", L"\xE790"}, {L"midi", L"\xE8D6"},
        {L"serial", L"\xE88E"}, {L"install", L"\xE896"}, {L"password", L"\xE9A8"},
        {L"speaker", L"\xE767"}, {L"service", L"\xE713"}, {L"drm", L"\xE72E"},
        {L"credential", L"\xE77B"}, {L"storage-access", L"\xE8F1"},
        {L"fingerprint", L"\xE928"}, {L"briefcase", L"\xE821"}, {L"dollar", L"\xE8C7"},
        {L"cart", L"\xE7BF"}, {L"vacation", L"\xE709"}, {L"gift", L"\xF133"},
        {L"food", L"\xED56"}, {L"fruit", L"\xE719"}, {L"pet", L"\xED54"},
        {L"tree", L"\xE8BE"}, {L"chill", L"\xE708"}, {L"circle", L"\xEA3A"},
        {L"fence", L"\xE80A"}, {L"revert", L"\xE7A7"}, {L"info", L"\xE946"},
    };
    for (auto const& entry : entries) if (entry.kind == kind) return entry.glyph;
    return nullptr;
}
// Small filled paths where a stock glyph does not express the feature or
// container identity. Coordinates use the same 16-DIP icon box as FontIcon.
inline wchar_t const* Path(std::wstring_view kind) {
    if (kind == L"pip") return L"F0 M1,2 H15 V14 H1 Z M2,3 V13 H14 V3 Z M8,8 H13 V12 H8 Z";
    if (kind == L"fruit") return L"M8,4 C2,-1 -1,8 4,14 C6,16 7,13 8,14 C12,17 16,8 13,5 C11,3 10,4 8,4 Z M8,3 C8,0 11,0 12,0 C12,2 10,3 8,3 Z";
    if (kind == L"pet") return L"M4,10 C6,5 10,5 12,10 C16,16 9,14 8,14 C6,14 0,16 4,10 Z M2,3 A1.5,2 0 1 1 2,7 A1.5,2 0 1 1 2,3 Z M6,0 A1.5,2 0 1 1 6,4 A1.5,2 0 1 1 6,0 Z M10,0 A1.5,2 0 1 1 10,4 A1.5,2 0 1 1 10,0 Z M14,3 A1.5,2 0 1 1 14,7 A1.5,2 0 1 1 14,3 Z";
    if (kind == L"tree") return L"M8,0 L13,6 H11 L15,12 H9 V16 H7 V12 H1 L5,6 H3 Z";
    if (kind == L"fence") return L"M1,3 L3,1 5,3 V6 H11 V3 L13,1 15,3 V15 H11 V10 H5 V15 H1 Z M5,7 V9 H11 V7 Z";
    return nullptr;
}
inline bool ValidState(std::wstring_view state) {
    return state == L"normal" || state == L"active" || state == L"blocked" ||
        state == L"recording" || state == L"paused" || state == L"warning" || state == L"danger" || state == L"busy";
}
}
