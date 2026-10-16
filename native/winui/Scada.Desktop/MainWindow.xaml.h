#pragma once
#include "MainWindow.g.h"
#include "pch.h"
namespace winrt::Scada::Desktop::implementation {
struct EngineOwner {
    scada_handle handle{};
    ~EngineOwner() {
        if (handle)
            scada_close(handle);
    }
};
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    void Start_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Demo_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Stop_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Controller_Changed(Windows::Foundation::IInspectable const &,
                            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const &);
    void Tag_Changed(Windows::Foundation::IInspectable const &,
                     Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const &);
    void Chart_SizeChanged(Windows::Foundation::IInspectable const &,
                           Microsoft::UI::Xaml::SizeChangedEventArgs const &);

  private:
    std::shared_ptr<EngineOwner> engine_;
    Microsoft::UI::Xaml::DispatcherTimer timer_{nullptr};
    bool busy_{}, starting_{}, changing_{}, closed_{}, close_requested_{};
    unsigned generation_{};
    Windows::Data::Json::JsonArray points_{nullptr};
    winrt::fire_and_forget StartAsync(bool demo);
    winrt::fire_and_forget RefreshAsync();
    winrt::fire_and_forget StopAsync();
    void Apply(Windows::Data::Json::JsonObject const &snapshot, Windows::Data::Json::JsonArray const &trend);
    void DrawChart();
    void Error(hstring const &message);
    void SetRunning(bool running);
};
} // namespace winrt::Scada::Desktop::implementation
namespace winrt::Scada::Desktop::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::Scada::Desktop::factory_implementation
