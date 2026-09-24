#pragma once

namespace I18n
{
// True when the resolved [Menu] Language is Chinese (simplified). "auto" follows the
// system UI language. Font selection uses this to pick CJK glyph ranges.
bool IsChinese();

// Returns the Chinese translation for the given key when the active language is Chinese
// and a translation exists; otherwise returns the key unchanged, so call sites that have
// not been translated yet keep their English source text. Safe to wrap any display string.
const char* Tr(const char* key);
}
