#pragma once
#include "TabAmbientClip.h"

// Template-adjacent geometry shared by the post-blur mask and selected-tab
// reflection. Stock TabView uses rounded top corners and 4-DIP bottom flares.
namespace TabLightContour {
using Rect = winrt::Windows::Foundation::Rect;
constexpr float Flare = 4.f;
constexpr float Arc = .447715f;

inline void LowerEdges(ID2D1GeometrySink* sink, float width, float bottom, float radius, float start) {
    sink->BeginFigure({radius, start}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({radius, bottom - radius});
    sink->AddBezier({{radius, bottom - radius * Arc}, {radius * Arc, bottom}, {0, bottom}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({width - radius, start}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({width - radius, bottom - radius});
    sink->AddBezier({{width - radius, bottom - radius * Arc}, {width - radius * Arc, bottom}, {width, bottom}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
}

struct Face {
    uint32_t id{};
    Rect bounds{};
    float leftRadius{}, rightRadius{};
    bool selected{};
    bool operator==(Face const& other) const {
        return id == other.id && bounds == other.bounds && leftRadius == other.leftRadius &&
            rightRadius == other.rightRadius && selected == other.selected;
    }
};

inline winrt::com_ptr<ID2D1PathGeometry> Outline(ID2D1Factory* factory, Face const& face) {
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::check_hresult(factory->CreatePathGeometry(path.put()));
    winrt::com_ptr<ID2D1GeometrySink> sink;
    winrt::check_hresult(path->Open(sink.put()));
    const auto b = face.bounds;
    const float l = b.X, t = b.Y, r = l + b.Width, y = t + b.Height;
    const float limit = std::min(b.Width / 2, b.Height / 2);
    const float tl = std::clamp(face.leftRadius, 0.f, limit);
    const float tr = std::clamp(face.rightRadius, 0.f, limit);
    const float flare = face.selected ? std::min(Flare, limit) : 0;
    sink->BeginFigure({l + tl, t}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({r - tr, t});
    sink->AddBezier({{r - tr * Arc, t}, {r, t + tr * Arc}, {r, t + tr}});
    sink->AddLine({r, y - flare});
    sink->AddBezier({{r, y - flare * Arc}, {r + flare * Arc, y}, {r + flare, y}});
    sink->AddLine({l - flare, y});
    sink->AddBezier({{l - flare * Arc, y}, {l, y - flare * Arc}, {l, y - flare}});
    sink->AddLine({l, t + tl});
    sink->AddBezier({{l, t + tl * Arc}, {l + tl * Arc, t}, {l + tl, t}});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(sink->Close());
    return path;
}

// An alpha-only visual surface: full light outside faces, a narrow edge band,
// and a restrained residual inside. No XAML control or content is captured.
struct Mask {
    using Shape = winrt::Microsoft::UI::Composition::ShapeVisual;
    winrt::Microsoft::UI::Composition::ContainerVisual root{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface surface{nullptr};
    winrt::Microsoft::UI::Composition::CompositionPathGeometry exterior{nullptr};
    Shape outside{nullptr};
    std::vector<Face> previous;
    Rect viewport{};
    std::map<uint32_t, Shape> faces;

    void Create(winrt::Microsoft::UI::Composition::Compositor const& compositor) {
        root = compositor.CreateContainerVisual();
        root.Comment(L"tab-face-occlusion");
        exterior = compositor.CreatePathGeometry();
        outside = compositor.CreateShapeVisual();
        auto shape = compositor.CreateSpriteShape(exterior);
        shape.FillBrush(compositor.CreateColorBrush({255, 255, 255, 255}));
        outside.Shapes().Append(shape); root.Children().InsertAtBottom(outside);
        surface = compositor.CreateVisualSurface(); surface.SourceVisual(root);
    }

    void Update(ID2D1Factory* factory, Rect view, std::vector<Face> const& current) {
        if (viewport == view && previous == current) return;
        auto compositor = root.Compositor();
        const winrt::Windows::Foundation::Numerics::float2 size{
            std::max(0.f, view.X + view.Width), std::max(0.f, view.Y + view.Height)};
        root.Size(size); outside.Size(size); surface.SourceSize(size);
        std::vector<winrt::com_ptr<ID2D1PathGeometry>> paths;
        std::vector<ID2D1Geometry*> geometries;
        std::set<uint32_t> live;
        for (auto const& face : current) {
            live.insert(face.id);
            auto path = Outline(factory, face);
            auto& visual = faces.try_emplace(face.id, nullptr).first->second;
            if (!visual) {
                visual = compositor.CreateShapeVisual();
                visual.Comment(L"tab-face:" + winrt::to_hstring(face.id));
                root.Children().InsertAtTop(visual);
                auto shape = compositor.CreateSpriteShape(compositor.CreatePathGeometry());
                auto fill = compositor.CreateLinearGradientBrush();
                fill.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
                for (auto [offset, alpha] : {std::pair{0.f, 255}, {.12f, 170}, {.32f, 24}, {.68f, 24}, {.88f, 170}, {1.f, 255}})
                    fill.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 255, 255, 255}));
                shape.FillBrush(fill);
                shape.StrokeBrush(compositor.CreateColorBrush({180, 255, 255, 255}));
                shape.StrokeThickness(1.5f);
                visual.Shapes().Append(shape);
            }
            visual.Size(size);
            auto shape = visual.Shapes().GetAt(0).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
#ifdef WINUI_PANE_TESTING
            shape.Properties().InsertVector4(L"FaceBounds", {face.bounds.X, face.bounds.Y, face.bounds.Width, face.bounds.Height});
#endif
            auto geometry = shape.Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>();
            geometry.Path(winrt::Microsoft::UI::Composition::CompositionPath(winrt::make<TabAmbientClip>(path)));
            auto fill = shape.FillBrush().as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
            fill.StartPoint({0, face.bounds.Y}); fill.EndPoint({0, face.bounds.Y + face.bounds.Height});
            geometries.push_back(path.get()); paths.push_back(std::move(path));
        }
        for (auto it = faces.begin(); it != faces.end();) {
            if (live.count(it->first)) { ++it; continue; }
            root.Children().Remove(it->second); it = faces.erase(it);
        }
        winrt::com_ptr<ID2D1RectangleGeometry> rectangle;
        winrt::check_hresult(factory->CreateRectangleGeometry({view.X, view.Y, view.X + view.Width, view.Y + view.Height}, rectangle.put()));
        winrt::com_ptr<ID2D1PathGeometry> remaining;
        winrt::check_hresult(factory->CreatePathGeometry(remaining.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(remaining->Open(sink.put()));
        if (geometries.empty()) {
            winrt::check_hresult(rectangle->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, nullptr, sink.get()));
        } else {
            // Winding unites overlapping faces before subtraction; alternate
            // holes alone would expose bright patches during tab movement.
            winrt::com_ptr<ID2D1GeometryGroup> united;
            winrt::check_hresult(factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING,
                geometries.data(), uint32_t(geometries.size()), united.put()));
            winrt::check_hresult(rectangle->CombineWithGeometry(united.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get()));
        }
        winrt::check_hresult(sink->Close());
        exterior.Path(winrt::Microsoft::UI::Composition::CompositionPath(winrt::make<TabAmbientClip>(std::move(remaining))));
        previous = current; viewport = view;
    }
};
}
