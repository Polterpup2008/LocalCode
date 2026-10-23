#include "Conversation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace localcode {

bool use_conversation_path(std::string_view message, RequestMode mode) {
    if (mode == RequestMode::Chat) return true;
    if (mode == RequestMode::Code || message.size() > 160) return false;
    std::string normalized;
    bool space = false;
    for (unsigned char c : message) {
        if (std::isspace(c) || c == ',' || c == '!' || c == '?' || c == '.') {
            space = !normalized.empty();
            continue;
        }
        if (space) normalized += ' ';
        normalized += static_cast<char>(std::tolower(c));
        space = false;
    }
    constexpr std::array social = {"hi",
                                   "hello",
                                   "hey",
                                   "good morning",
                                   "good afternoon",
                                   "good evening",
                                   "how are you",
                                   "hello how are you",
                                   "hi how are you",
                                   "hey how are you",
                                   "thanks",
                                   "thank you",
                                   "thanks a lot",
                                   "thank you very much",
                                   "bye",
                                   "goodbye",
                                   "bonjour",
                                   "salut",
                                   "bonsoir",
                                   "comment vas tu",
                                   "bonjour comment vas tu",
                                   "merci",
                                   "merci beaucoup",
                                   "au revoir",
                                   "hola",
                                   "gracias",
                                   "hallo",
                                   "danke"};
    return std::find(social.begin(), social.end(), normalized) != social.end();
}

}   // namespace localcode
