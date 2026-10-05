#pragma once

namespace cs {
// Loads System32\version.dll and fills the export jump table. Called from DllMain.
bool proxy_load();
}
