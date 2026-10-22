#include "HttpClient.h"

#include "../util/Strings.h"

namespace localcode {

std::string HttpResponse::header(std::string_view name) const {
    const auto it = headers.find(strings::to_lower(name));
    return it == headers.end() ? std::string{} : it->second;
}

// ---------------------------------------------------------------------------
void SseParser::reset() {
    buffer_.clear();
    searched_bytes_ = 0;
    current_event_.clear();
    current_data_.clear();
    has_data_ = false;
}

void SseParser::dispatch(const std::function<void(const Event&)>& on_event) {
    if (!has_data_ && current_event_.empty()) return;
    Event event;
    event.event = current_event_;
    event.data = current_data_;
    current_event_.clear();
    current_data_.clear();
    has_data_ = false;
    on_event(event);
}

void SseParser::feed(std::string_view chunk, const std::function<void(const Event&)>& on_event) {
    buffer_.append(chunk);

    std::size_t start = 0;
    std::size_t search = searched_bytes_;
    while (true) {
        const auto newline = buffer_.find('\n', search);
        if (newline == std::string::npos) break;

        std::string_view line{buffer_.data() + start, newline - start};
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = newline + 1;
        search = start;

        if (line.empty()) {   // blank line terminates the event
            dispatch(on_event);
            continue;
        }
        if (line[0] == ':') continue;   // comment / keep-alive

        const auto colon = line.find(':');
        std::string_view field = (colon == std::string_view::npos) ? line : line.substr(0, colon);
        std::string_view value =
            (colon == std::string_view::npos) ? std::string_view{} : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);

        if (field == "data") {
            if (has_data_) current_data_.push_back('\n');
            current_data_.append(value);
            has_data_ = true;
        } else if (field == "event") {
            current_event_.assign(value);
        }
        // "id" and "retry" carry nothing we act on.
    }

    // Keep the unterminated remainder for the next chunk.
    if (start > 0) buffer_.erase(0, start);
    searched_bytes_ = buffer_.size();
}

void SseParser::finish(const std::function<void(const Event&)>& on_event) {
    if (!buffer_.empty()) {
        // A final line without its newline: run it through feed() by adding one.
        const std::string remainder = buffer_ + "\n";
        buffer_.clear();
        searched_bytes_ = 0;
        feed(remainder, on_event);
    }
    dispatch(on_event);
}

}   // namespace localcode
