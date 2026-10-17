#include "MainWindow.xaml.h"
#include "pch.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Data::Json;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
namespace Shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace winrt::Scada::Desktop::implementation {
static std::string take(char *p) {
    if (!p)
        throw std::runtime_error(scada_last_error());
    std::unique_ptr<char, decltype(&scada_free)> owned(p, scada_free);
    return p;
}
static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
MainWindow::MainWindow() {
    InitializeComponent();
    AppWindow().Closing(
        [weak = get_weak()](auto const &, Microsoft::UI::Windowing::AppWindowClosingEventArgs const &args) {
            if (auto self = weak.get()) {
                if (self->engine_ || self->starting_) {
                    args.Cancel(true);
                    self->close_requested_ = true;
                    if (!self->starting_)
                        self->StopAsync();
                }
            }
        });
    timer_ = DispatcherTimer();
    timer_.Interval(std::chrono::milliseconds(200));
    timer_.Tick([weak = get_weak()](auto &&, auto &&) {
        if (auto self = weak.get())
            self->RefreshAsync();
    });
    Closed([weak = get_weak()](auto &&, auto &&) {
        if (auto self = weak.get()) {
            self->closed_ = true;
            self->timer_.Stop();
            self->StopAsync();
        }
    });
}
void MainWindow::Error(hstring const &message) {
    if (closed_)
        return;
    StatusBar().Severity(InfoBarSeverity::Error);
    StatusBar().Title(L"Monitoring error");
    StatusBar().Message(message);
}
void MainWindow::SetRunning(bool running) {
    if (closed_)
        return;
    StartButton().IsEnabled(!running && !starting_);
    DemoButton().IsEnabled(!running && !starting_);
    StopButton().IsEnabled(running && !starting_);
}
void MainWindow::Start_Click(IInspectable const &, RoutedEventArgs const &) {
    StartAsync(false);
}
void MainWindow::Demo_Click(IInspectable const &, RoutedEventArgs const &) {
    StartAsync(true);
}
void MainWindow::Stop_Click(IInspectable const &, RoutedEventArgs const &) {
    StopAsync();
}
fire_and_forget MainWindow::StartAsync(bool demo) {
    auto lifetime = get_strong();
    if (engine_ || starting_)
        co_return;
    starting_ = true;
    SetRunning(false);
    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    auto path = to_string(ConfigPath().Text());
    co_await resume_background();
    std::shared_ptr<EngineOwner> owner;
    std::string error;
    try {
        owner = std::make_shared<EngineOwner>();
        wchar_t local[32768]{};
        auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!length || length >= 32768)
            throw std::runtime_error("LOCALAPPDATA is unavailable");
        auto folder = std::filesystem::path(local) / L"OmronScada";
        std::filesystem::create_directories(folder);
        auto raw = (folder / L"scada-demo.db").u8string();
        auto database = std::string(raw.begin(), raw.end());
        owner->handle = demo ? scada_demo(2, 20, 10, database.c_str()) : scada_open(path.c_str());
        if (!owner->handle)
            throw std::runtime_error(scada_last_error());
    } catch (const std::exception &ex) {
        error = ex.what();
    } catch (const hresult_error &ex) {
        error = to_string(ex.message());
    }
    dispatcher.TryEnqueue([weak, owner, error, demo] {
        if (auto self = weak.get()) {
            self->starting_ = false;
            if (self->closed_)
                return;
            if (!error.empty()) {
                self->Error(to_hstring(error));
                self->SetRunning(false);
                if (self->close_requested_)
                    self->Close();
                return;
            }
            self->engine_ = owner;
            ++self->generation_;
            self->ControllerPicker().Items().Clear();
            self->TagPicker().Items().Clear();
            self->SetRunning(true);
            self->StatusBar().Severity(InfoBarSeverity::Success);
            self->StatusBar().Title(demo ? L"Simulated demo" : L"Acquiring");
            self->StatusBar().Message(
                L"Read-only acquisition active. Inspect missed deadlines before accepting the scan rate.");
            self->timer_.Start();
            if (self->close_requested_)
                self->StopAsync();
            else
                self->RefreshAsync();
        }
    });
}
fire_and_forget MainWindow::StopAsync() {
    auto lifetime = get_strong();
    timer_.Stop();
    ++generation_;
    auto owner = std::move(engine_);
    if (!owner)
        co_return;
    starting_ = true;
    SetRunning(false);
    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    co_await resume_background();
    std::string error;
    try {
        if (scada_stop(owner->handle) != 0)
            throw std::runtime_error(scada_last_error());
        auto result = JsonObject::Parse(to_hstring(take(scada_snapshot(owner->handle, "@catalog"))));
        error = to_string(result.GetNamedString(L"archiveError"));
    } catch (const std::exception &ex) {
        error = ex.what();
    } catch (const hresult_error &ex) {
        error = to_string(ex.message());
    }
    owner.reset();
    dispatcher.TryEnqueue([weak, error] {
        if (auto self = weak.get()) {
            self->starting_ = false;
            self->SetRunning(false);
            if (!self->closed_) {
                self->StatusBar().Severity(InfoBarSeverity::Informational);
                self->StatusBar().Title(L"Stopped");
                self->StatusBar().Message(
                    L"Acquisition stopped; accepted samples were drained to the archive.");
                if (!error.empty())
                    self->Error(to_hstring(error));
                if (self->close_requested_)
                    self->Close();
            }
        }
    });
}
void MainWindow::Controller_Changed(IInspectable const &, SelectionChangedEventArgs const &) {
    if (changing_)
        return;
    ++generation_;
    changing_ = true;
    TagPicker().Items().Clear();
    changing_ = false;
    points_ = nullptr;
    DrawChart();
    RefreshAsync();
}
void MainWindow::Tag_Changed(IInspectable const &, SelectionChangedEventArgs const &) {
    if (!changing_) {
        ++generation_;
        RefreshAsync();
    }
}
void MainWindow::Chart_SizeChanged(IInspectable const &, SizeChangedEventArgs const &) {
    DrawChart();
}
fire_and_forget MainWindow::RefreshAsync() {
    auto lifetime = get_strong();
    if (!engine_ || busy_ || closed_)
        co_return;
    busy_ = true;
    auto owner = engine_;
    auto generation = generation_;
    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    std::string controller, tag;
    if (ControllerPicker().SelectedItem())
        controller = to_string(unbox_value<hstring>(ControllerPicker().SelectedItem()));
    if (TagPicker().SelectedItem())
        tag = to_string(unbox_value<hstring>(TagPicker().SelectedItem()));
    auto range = RangePicker().SelectedIndex();
    co_await resume_background();
    std::string snapshot, trend = "[]", error;
    try {
        snapshot = take(scada_snapshot(owner->handle, controller.empty() ? "@catalog" : controller.c_str()));
        if (!controller.empty() && !tag.empty()) {
            auto now = now_ms();
            std::int64_t duration = range == 1   ? 3600000
                                    : range == 2 ? 86400000
                                    : range == 3 ? 2678400000LL
                                                 : 60000;
            auto bucket = std::max<std::int64_t>(10, (duration + 998) / 999);
            trend = take(
                scada_trend(owner->handle, controller.c_str(), tag.c_str(), now - duration, now, bucket));
        }
    } catch (const std::exception &ex) {
        error = ex.what();
    }
    dispatcher.TryEnqueue([weak, generation, snapshot, trend, error] {
        if (auto self = weak.get()) {
            self->busy_ = false;
            if (self->closed_ || generation != self->generation_)
                return;
            try {
                if (!error.empty()) {
                    self->Error(to_hstring(error));
                    return;
                }
                self->Apply(JsonObject::Parse(to_hstring(snapshot)), JsonArray::Parse(to_hstring(trend)));
            } catch (const hresult_error &ex) {
                self->Error(ex.message());
            }
        }
    });
}
void MainWindow::Apply(JsonObject const &snapshot, JsonArray const &trend) {
    changing_ = true;
    auto controllers = snapshot.GetNamedArray(L"controllers");
    if (ControllerPicker().Items().Size() == 0) {
        for (auto const &item : controllers)
            ControllerPicker().Items().Append(box_value(item.GetObject().GetNamedString(L"id")));
        if (ControllerPicker().Items().Size())
            ControllerPicker().SelectedIndex(0);
    }
    auto selected =
        ControllerPicker().SelectedItem() ? unbox_value<hstring>(ControllerPicker().SelectedItem()) : L"";
    for (auto const &item : controllers) {
        auto c = item.GetObject();
        if (c.GetNamedString(L"id") == selected) {
            std::wostringstream line;
            line << L"Target: " << c.GetNamedNumber(L"intervalMs") << L" ms  •  Last scan: "
                 << c.GetNamedNumber(L"scanMs") << L" ms\nJitter: " << c.GetNamedNumber(L"jitterMs")
                 << L" ms  •  Missed deadlines: " << c.GetNamedNumber(L"missedDeadlines")
                 << L"  •  Failed reads: " << c.GetNamedNumber(L"failedReads");
            ScanSummary().Text(line.str());
        }
    }
    ValuesList().Items().Clear();
    auto samples = snapshot.GetNamedArray(L"samples");
    auto fillTags = TagPicker().Items().Size() == 0;
    for (auto const &item : samples) {
        auto s = item.GetObject();
        auto id = s.GetNamedString(L"tag");
        if (fillTags)
            TagPicker().Items().Append(box_value(id));
        auto value = s.GetNamedValue(L"value");
        std::wostringstream line;
        line << id.c_str() << L"   ";
        if (value.ValueType() == JsonValueType::Number)
            line << value.GetNumber();
        else
            line << L"—";
        line << L" " << s.GetNamedString(L"unit").c_str() << L"   [" << s.GetNamedString(L"quality").c_str()
             << L"]";
        ValuesList().Items().Append(box_value(hstring(line.str())));
    }
    if (fillTags && TagPicker().Items().Size())
        TagPicker().SelectedIndex(0);
    auto secs = snapshot.GetNamedObject(L"secs");
    std::wostringstream archive;
    archive << L"Archived this session: " << snapshot.GetNamedNumber(L"archivedSamples")
            << L"  •  Backpressure events: " << snapshot.GetNamedNumber(L"backpressureEvents")
            << L"  •  HSMS: " << (secs.GetNamedBoolean(L"selected") ? L"selected" : L"disconnected")
            << L"  •  Retention: one calendar month";
    ArchiveSummary().Text(archive.str());
    auto archiveError = snapshot.GetNamedString(L"archiveError");
    auto secsError = secs.GetNamedString(L"error");
    if (!archiveError.empty())
        Error(archiveError);
    else if (!secsError.empty()) {
        StatusBar().Severity(InfoBarSeverity::Warning);
        StatusBar().Message(L"HSMS: " + secsError);
    }
    points_ = trend;
    changing_ = false;
    DrawChart();
}
void MainWindow::DrawChart() {
    if (!TrendCanvas())
        return;
    auto canvas = TrendCanvas();
    canvas.Children().Clear();
    if (!points_ || points_.Size() == 0) {
        TrendSummary().Text(L"Waiting for archived points in the selected time range.");
        return;
    }
    auto width = std::max(1.0, canvas.ActualWidth()), height = std::max(1.0, canvas.ActualHeight());
    double min = std::numeric_limits<double>::max(), max = std::numeric_limits<double>::lowest(), first = 0,
           last = 0;
    bool any = false;
    for (auto const &p : points_) {
        auto o = p.GetObject();
        if (first == 0)
            first = o.GetNamedNumber(L"timestampMs");
        last = o.GetNamedNumber(L"timestampMs");
        if (o.GetNamedValue(L"average").ValueType() != JsonValueType::Number)
            continue;
        min = std::min(min, o.GetNamedNumber(L"min"));
        max = std::max(max, o.GetNamedNumber(L"max"));
        any = true;
    }
    if (!any) {
        TrendSummary().Text(L"No good-quality samples in this range.");
        return;
    }
    if (max == min) {
        max += 0.5;
        min -= 0.5;
    }
    std::wostringstream description;
    description << L"Range " << min << L" to " << max << L"  •  " << points_.Size()
                << L" archive buckets (min/max whiskers, mean line)";
    TrendSummary().Text(description.str());
    Shapes::Polyline line;
    line.Stroke(SolidColorBrush(Microsoft::UI::Colors::DodgerBlue()));
    line.StrokeThickness(2);
    PointCollection points;
    double previous = -1, spacing = 0;
    if (points_.Size() > 1)
        spacing = points_.GetAt(1).GetObject().GetNamedNumber(L"timestampMs") - first;
    auto flush = [&] {
        if (points.Size() > 0) {
            line.Points(points);
            canvas.Children().Append(line);
        }
        line = Shapes::Polyline();
        line.Stroke(SolidColorBrush(Microsoft::UI::Colors::DodgerBlue()));
        line.StrokeThickness(2);
        points = PointCollection();
    };
    for (auto const &p : points_) {
        auto o = p.GetObject();
        auto time = o.GetNamedNumber(L"timestampMs");
        if (o.GetNamedValue(L"average").ValueType() != JsonValueType::Number ||
            (previous >= 0 && spacing > 0 && time - previous > spacing * 1.5)) {
            flush();
            if (o.GetNamedValue(L"average").ValueType() != JsonValueType::Number) {
                previous = time;
                continue;
            }
        }
        auto x =
            static_cast<float>(8 + (time - first) / std::max(1.0, last - first) * std::max(1.0, width - 16));
        auto y = [&](double v) {
            return static_cast<float>(8 + (max - v) / (max - min) * std::max(1.0, height - 16));
        };
        Shapes::Line whisker;
        whisker.X1(x);
        whisker.X2(x);
        whisker.Y1(y(o.GetNamedNumber(L"min")));
        whisker.Y2(y(o.GetNamedNumber(L"max")));
        whisker.Stroke(SolidColorBrush(Microsoft::UI::Colors::LightSteelBlue()));
        canvas.Children().Append(whisker);
        points.Append(Point{x, y(o.GetNamedNumber(L"average"))});
        previous = time;
    }
    flush();
}
} // namespace winrt::Scada::Desktop::implementation
