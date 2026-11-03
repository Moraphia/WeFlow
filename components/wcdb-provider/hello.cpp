#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "json.hpp"
#include <cstring>
#include <thread>
#include <userconsentverifierinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
// Desktop Windows Hello interop; only Windows can approve authentication.
extern "C" __declspec(dllexport) void VerifyUser(int64_t hwnd, const char *message, char *output,
                                                 int maxLength) {
  if (!output || maxLength <= 0)
    return;
  nlohmann::json result = {{"success", false}, {"error", "Windows Hello verification failed"}};
  if (!hwnd || !IsWindow(reinterpret_cast<HWND>(hwnd)))
    result["error"] = "A valid application window is required";
  else {
    std::thread verifier([&]() {
      try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using namespace winrt::Windows::Security::Credentials::UI;
        auto availability = UserConsentVerifier::CheckAvailabilityAsync().get();
        if (availability != UserConsentVerifierAvailability::Available)
          result["error"] = "Windows Hello is not configured or is unavailable";
        else {
          auto interop =
              winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
          winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> operation{
              nullptr};
          auto prompt = winrt::to_hstring(message ? message : "");
          winrt::check_hresult(interop->RequestVerificationForWindowAsync(
              reinterpret_cast<HWND>(hwnd), reinterpret_cast<HSTRING>(winrt::get_abi(prompt)),
              winrt::guid_of<decltype(operation)>(), winrt::put_abi(operation)));
          auto status = operation.get();
          if (status == UserConsentVerificationResult::Verified)
            result = {{"success", true}};
          else
            result = {{"success", false},
                      {"error", "Windows Hello verification was cancelled or denied"},
                      {"status", static_cast<int>(status)}};
        }
        winrt::uninit_apartment();
      } catch (const winrt::hresult_error &e) {
        result = {{"success", false}, {"error", winrt::to_string(e.message())}};
      } catch (...) {
        result = {{"success", false}, {"error", "Windows Hello verification failed"}};
      }
    });
    verifier.join();
  }
  auto encoded = result.dump();
  if (encoded.size() >= static_cast<size_t>(maxLength))
    encoded = "{\"success\":false}";
  if (encoded.size() >= static_cast<size_t>(maxLength)) {
    output[0] = '\0';
    return;
  }
  std::memcpy(output, encoded.c_str(), encoded.size() + 1);
}
