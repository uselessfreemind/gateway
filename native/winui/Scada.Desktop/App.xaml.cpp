#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "pch.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif
namespace winrt::Scada::Desktop::implementation {
App::App() {
    InitializeComponent();
}
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &) {
    window_ = winrt::make<MainWindow>();
    window_.Activate();
}
} // namespace winrt::Scada::Desktop::implementation
