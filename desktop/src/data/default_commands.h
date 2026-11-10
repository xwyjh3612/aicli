#pragma once

namespace aicli {
inline constexpr wchar_t kDefaultCodexCommand[] =
#ifdef AICLI_QT_UI_BUILD
    L"codex --yolo --no-alt-screen resume --last";
#else
    LR"(codex --yolo --no-alt-screen -c ('projects={' + (ConvertTo-Json -InputObject (Get-Location).Path -Compress) + '={trust_level="trusted"}}') resume --last)";
#endif
}
