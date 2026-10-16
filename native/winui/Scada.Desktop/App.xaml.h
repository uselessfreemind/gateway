#pragma once
#include "App.g.h"
#include "pch.h"
namespace winrt::Scada::Desktop::implementation {
struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &);

  private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};
} // namespace winrt::Scada::Desktop::implementation
