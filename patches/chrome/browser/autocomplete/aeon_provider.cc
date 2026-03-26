// Copyright 2026 The Aeon Browser Authors
// AI-powered omnibox provider — debounced, prefix-triggered.
// Queries AeonMind /llm/complete only when:
//   1. Input starts with "@agent " or "@aeon "
//   2. Input ends with "?" and user has paused 600ms
// Never blocks URL bar for normal typing.

#include "chrome/browser/autocomplete/aeon_provider.h"

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "net/base/load_flags.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "content/public/browser/browser_thread.h"

// Shared HTTP client for Ollama queries
#include "services/network/public/cpp/simple_url_loader.h"

namespace {
constexpr char kAeonMindURL[] = "http://localhost:7878/llm/complete";
constexpr int kDebounceMs = 600;  // Wait 600ms after last keystroke
constexpr int kMaxSuggestionLength = 200;

bool ShouldQueryAeon(const std::u16string& text) {
  // Explicit prefixes
  if (text.substr(0, 7) == u"@agent ") return true;
  if (text.substr(0, 6) == u"@aeon ") return true;
  if (text.substr(0, 7) == u"@hive " ) return true;

  // Question detection (must end with ?)
  if (text.length() > 15 && text.back() == u'?') return true;

  return false;
}

std::string StripPrefix(const std::string& input) {
  if (input.substr(0, 7) == "@agent ") return input.substr(7);
  if (input.substr(0, 6) == "@aeon ") return input.substr(6);
  if (input.substr(0, 7) == "@hive " ) return "[hive] " + input.substr(7);
  return input;
}

}  // namespace

AeonProvider::AeonProvider(AutocompleteProviderClient* client)
    : AutocompleteProvider(AutocompleteProvider::TYPE_SEARCH),
      client_(client),
      weak_ptr_factory_(this) {}

AeonProvider::~AeonProvider() = default;

void AeonProvider::Start(const AutocompleteInput& input, bool minimal_changes) {
  // Cancel any pending query
  debounce_timer_.Stop();
  matches_.clear();
  pending_query_.clear();

  const std::u16string& text = input.text();
  if (text.length() < 4 || !ShouldQueryAeon(text)) {
    return;
  }

  pending_query_ = base::UTF16ToUTF8(text);

  // Debounce: only fire after user pauses
  debounce_timer_.Start(
      FROM_HERE,
      base::Milliseconds(kDebounceMs),
      base::BindOnce(&AeonProvider::FetchCompletion,
                     weak_ptr_factory_.GetWeakPtr(),
                     pending_query_));
}

void AeonProvider::Stop(bool clear_cached_results, bool due_to_user_inactivity) {
  debounce_timer_.Stop();
  if (clear_cached_results) matches_.clear();
}

void AeonProvider::FetchCompletion(const std::string& query) {
  // Don't block — fire async request to AeonMind
  // Result comes back via OnCompletionReceived
  last_query_ = query;

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(kAeonMindURL);
  resource_request->method = "POST";
  resource_request->load_flags = net::LOAD_DISABLE_CACHE;

  std::string body = "{\"prompt\":\"" + StripPrefix(query) + "\",\"max_tokens\":100}";

  url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request),
      MISSING_TRAFFIC_ANNOTATION);
  url_loader_->AttachStringForUpload(body, "application/json");
  url_loader_->DownloadToString(
      client_->GetURLLoaderFactory().get(),
      base::BindOnce(&AeonProvider::OnCompletionReceived,
                     weak_ptr_factory_.GetWeakPtr()),
      1024 * 4);  // 4KB max response
}

void AeonProvider::OnCompletionReceived(
    std::unique_ptr<std::string> response) {
  if (!response || response->empty()) return;

  // Parse simple JSON {"response":"..."}
  std::string answer = *response;
  size_t start = answer.find("\"response\":\"");
  if (start != std::string::npos) {
    start += 12;
    size_t end = answer.find("\"", start);
    if (end != std::string::npos) {
      answer = answer.substr(start, end - start);
    }
  }

  if (answer.length() > kMaxSuggestionLength)
    answer = answer.substr(0, kMaxSuggestionLength) + "...";

  // Build match
  AutocompleteMatch match(this, 900, false,
                          AutocompleteMatchType::SEARCH_SUGGEST);
  match.description = base::UTF8ToUTF16("🤖 Aeon: " + answer);
  match.fill_into_edit = base::UTF8ToUTF16(last_query_);
  match.destination_url = GURL("aeon://agent?q=" + answer);

  matches_.push_back(match);
  NotifyListeners(true);
}
