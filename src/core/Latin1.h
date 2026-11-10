#pragma once
#include <string>
#include <string_view>

namespace talkit {

// tispeech_text_to_phonemes() takes Latin-1, but every host hands us UTF-8.
// Converts, transliterating the punctuation word processors actually produce
// (curly quotes, en/em dashes, ellipsis, non-breaking space) instead of
// dropping it, since a stray '?' in the middle of a sentence changes the
// engine's prosody. Anything else outside Latin-1 becomes '?'.
std::string utf8ToLatin1(std::string_view utf8);

}  // namespace talkit
