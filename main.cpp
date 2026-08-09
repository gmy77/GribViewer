#include "GribReader.h"

#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace
{
    constexpr int IdOpen = 1001;
    constexpr int IdFields = 1002;
    constexpr int IdMap = 1003;
    constexpr int IdUpdate = 1004;
    constexpr int IdAbout = 1005;
    constexpr int IdDownload = 1006;
    constexpr UINT MessageInventoryReady = WM_APP + 1;
    constexpr UINT MessageFieldReady = WM_APP + 2;

    HINSTANCE instance{};
    HWND openButton{};
    HWND updateButton{};
    HWND aboutButton{};
    HWND downloadButton{};
    HWND fieldsList{};
    HWND detailsLabel{};
    HWND severityLabel{};
    HWND titleLabel{};
    HWND fieldsHeading{};
    HWND detailsHeading{};
    HWND mapHeading{};
    HBRUSH headerBrush{};
    HBRUSH bodyBrush{};
    HBRUSH infoBrush{};
    HFONT titleFont{};
    HFONT uiFont{};
    std::vector<GribField> fields;
    std::wstring openedPath;
    std::thread loader;
    std::atomic_bool loading{};

    struct GeoPoint { double longitude; double latitude; };
    std::vector<std::vector<GeoPoint>> fvgRings;

    struct InventoryResult
    {
        std::filesystem::path path;
        std::vector<GribField> inventory;
        std::string error;
    };

    struct FieldResult
    {
        std::size_t index{};
        GribField field;
        std::string error;
    };

    void SkipSpace(const char*& cursor)
    {
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
    }

    void ParseGeoJsonCoordinates(const char*& cursor, int depth, std::vector<std::vector<GeoPoint>>& rings)
    {
        SkipSpace(cursor);
        if (*cursor++ != '[')
            throw std::runtime_error("Coordinate GeoJSON non valide.");
        if (depth > 2)
        {
            do
            {
                ParseGeoJsonCoordinates(cursor, depth - 1, rings);
                SkipSpace(cursor);
            } while (*cursor++ == ',');
            return;
        }

        std::vector<GeoPoint> ring;
        do
        {
            SkipSpace(cursor);
            if (*cursor++ != '[')
                throw std::runtime_error("Coordinate GeoJSON non valide.");
            char* end = nullptr;
            const double longitude = std::strtod(cursor, &end);
            cursor = end;
            SkipSpace(cursor);
            if (*cursor++ != ',')
                throw std::runtime_error("Coordinate GeoJSON non valide.");
            const double latitude = std::strtod(cursor, &end);
            cursor = end;
            SkipSpace(cursor);
            while (*cursor && *cursor != ']' && *cursor != ',')
                ++cursor; // Ignore the optional altitude coordinate.
            if (*cursor++ != ']')
                throw std::runtime_error("Coordinate GeoJSON non valide.");
            ring.push_back({ longitude, latitude });
            SkipSpace(cursor);
            if (*cursor == ']')
                break;
            ++cursor;
        } while (*cursor);
        if (*cursor == ']')
            ++cursor;
        if (ring.size() >= 3)
            rings.push_back(std::move(ring));
    }

    void LoadFvgBoundary()
    {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto asset = std::filesystem::path(executable).parent_path() / L"FvgBoundary.geojson";
        std::ifstream input(asset, std::ios::binary);
        if (!input)
            throw std::runtime_error("Asset del confine FVG non trovato.");
        const std::string json{ std::istreambuf_iterator<char>(input), {} };
        const auto key = json.find("\"coordinates\"");
        if (key == std::string::npos)
            throw std::runtime_error("Asset del confine FVG non valido.");
        const char* cursor = json.c_str() + json.find('[', key);
        ParseGeoJsonCoordinates(cursor, 4, fvgRings);
        if (fvgRings.empty())
            throw std::runtime_error("Asset del confine FVG privo di geometrie.");
    }

    bool IsInFvg(double longitude, double latitude)
    {
        bool inside = false;
        for (const auto& ring : fvgRings)
        {
            for (std::size_t current = 0, previous = ring.size() - 1; current < ring.size(); previous = current++)
            {
                const auto& a = ring[current];
                const auto& b = ring[previous];
                if ((a.latitude > latitude) != (b.latitude > latitude)
                    && longitude < (b.longitude - a.longitude) * (latitude - a.latitude)
                        / (b.latitude - a.latitude) + a.longitude)
                    inside = !inside;
            }
        }
        return inside;
    }

    int SeverityFor(const GribField& field)
    {
        if (field.discipline == 0 && field.parameterCategory == 7 && field.parameterNumber == 6 && !field.values.empty())
        {
            if (field.maximumValue >= 4'000) return 10;
            if (field.maximumValue >= 3'000) return 9;
            if (field.maximumValue >= 2'000) return 8;
            if (field.maximumValue >= 1'500) return 7;
            if (field.maximumValue >= 1'000) return 6;
            if (field.maximumValue >= 500) return 4;
            if (field.maximumValue >= 250) return 2;
            return 1;
        }
        if (field.discipline == 0 && field.parameterCategory == 7 && field.parameterNumber == 6)
            return 7;
        if (field.discipline == 0 && field.parameterCategory == 1 && field.parameterNumber == 8)
            return 6;
        if (field.discipline == 0 && field.parameterCategory == 2)
            return 5;
        return 3;
    }

    const GribField* SelectedField()
    {
        if (fields.empty())
            return nullptr;
        const auto selected = static_cast<int>(SendMessageW(fieldsList, LB_GETCURSEL, 0, 0));
        return &fields[std::clamp(selected, 0, static_cast<int>(fields.size() - 1))];
    }

    COLORREF GridColor(double value, double minimum, double maximum)
    {
        const auto range = maximum - minimum;
        const auto normalized = range > 0 ? std::clamp((value - minimum) / range, 0.0, 1.0) : 0.5;
        if (normalized < 0.33)
        {
            const auto transition = normalized / 0.33;
            return RGB(static_cast<int>(35 + 20 * transition), static_cast<int>(115 + 110 * transition), 215);
        }
        if (normalized < 0.66)
        {
            const auto transition = (normalized - 0.33) / 0.33;
            return RGB(static_cast<int>(55 + 200 * transition), static_cast<int>(225 + 20 * transition), static_cast<int>(215 - 155 * transition));
        }
        const auto transition = (normalized - 0.66) / 0.34;
        return RGB(255, static_cast<int>(245 - 165 * transition), static_cast<int>(60 - 25 * transition));
    }

    double BilinearSample(const GribField& field, double column, double row)
    {
        const auto x = std::clamp(column, 0.0, static_cast<double>(field.columns - 1));
        const auto y = std::clamp(row, 0.0, static_cast<double>(field.rows - 1));
        const auto x0 = static_cast<std::uint32_t>(x);
        const auto y0 = static_cast<std::uint32_t>(y);
        const auto x1 = (std::min)(x0 + 1, field.columns - 1);
        const auto y1 = (std::min)(y0 + 1, field.rows - 1);
        const auto horizontal = x - x0;
        const auto vertical = y - y0;
        const auto at = [&](std::uint32_t xIndex, std::uint32_t yIndex) { return field.values[yIndex * field.columns + xIndex]; };
        const auto top = at(x0, y0) * (1.0 - horizontal) + at(x1, y0) * horizontal;
        const auto bottom = at(x0, y1) * (1.0 - horizontal) + at(x1, y1) * horizontal;
        return top * (1.0 - vertical) + bottom * vertical;
    }

    const GribField* FindWindComponent(const GribField& field, int parameterNumber)
    {
        const auto result = std::find_if(fields.begin(), fields.end(), [&](const GribField& candidate)
        {
            return candidate.discipline == 0 && candidate.parameterCategory == 2
                && candidate.parameterNumber == parameterNumber
                && candidate.referenceTime == field.referenceTime && candidate.forecastTime == field.forecastTime
                && candidate.columns == field.columns && candidate.rows == field.rows
                && candidate.values.size() == field.values.size();
        });
        return result == fields.end() ? nullptr : &*result;
    }

    void DrawTextAt(HDC dc, int x, int y, const wchar_t* text, COLORREF color, int size, bool bold = false)
    {
        HFONT font = CreateFontW(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto oldFont = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }

    void PaintMap(HWND window)
    {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        HBRUSH background = CreateSolidBrush(RGB(235, 246, 250));
        FillRect(dc, &rect, background);
        DeleteObject(background);

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const auto field = SelectedField();
        if (field && !field->values.empty() && field->columns > 1 && field->rows > 1)
        {
            const int mapLeft = 32;
            const int mapTop = 28;
            const int mapWidth = (std::max)(1, width - 64);
            const int mapHeight = (std::max)(1, height - 84);
            constexpr double minLatitude = 45.55;
            constexpr double maxLatitude = 46.68;
            constexpr double minLongitude = 12.28;
            constexpr double maxLongitude = 13.95;
            constexpr double latitudeCentre = (minLatitude + maxLatitude) / 2;
            const auto longitudeScale = std::cos(latitudeCentre * 3.14159265358979323846 / 180.0);
            const auto geographicalWidth = (maxLongitude - minLongitude) * longitudeScale;
            const auto geographicalHeight = maxLatitude - minLatitude;
            const auto scale = (std::min)(mapWidth / geographicalWidth, mapHeight / geographicalHeight);
            const auto drawnWidth = static_cast<int>(geographicalWidth * scale);
            const auto drawnHeight = static_cast<int>(geographicalHeight * scale);
            const auto drawLeft = mapLeft + (mapWidth - drawnWidth) / 2;
            const auto drawTop = mapTop + (mapHeight - drawnHeight) / 2;
            const auto projectX = [&](double longitude) { return drawLeft + static_cast<int>((longitude - minLongitude) * longitudeScale * scale); };
            const auto projectY = [&](double latitude) { return drawTop + static_cast<int>((maxLatitude - latitude) * scale); };

            const auto rowStep = (field->lastLatitude - field->firstLatitude) / (field->rows - 1);
            const auto columnStep = (field->lastLongitude - field->firstLongitude) / (field->columns - 1);
            if (rowStep == 0 || columnStep == 0 || field->values.size() < static_cast<std::size_t>(field->rows) * field->columns)
            {
                DrawTextAt(dc, width * 18 / 100, height * 44 / 100, L"La griglia del campo non ha coordinate regolari utilizzabili.", RGB(24, 51, 77), 17, true);
                EndPaint(window, &paint);
                return;
            }

            std::vector<DWORD> raster(static_cast<std::size_t>(drawnWidth) * drawnHeight, RGB(235, 246, 250));
            for (int y = 0; y < drawnHeight; ++y)
            {
                const auto latitude = maxLatitude - (static_cast<double>(y) + 0.5) / scale;
                const auto sourceRow = (latitude - field->firstLatitude) / rowStep;
                if (sourceRow < 0 || sourceRow > field->rows - 1)
                    continue;
                for (int x = 0; x < drawnWidth; ++x)
                {
                    const auto longitude = minLongitude + (static_cast<double>(x) + 0.5) / (longitudeScale * scale);
                    if (!IsInFvg(longitude, latitude))
                        continue;
                    const auto sourceColumn = (longitude - field->firstLongitude) / columnStep;
                    if (sourceColumn < 0 || sourceColumn > field->columns - 1)
                        continue;
                    const auto value = BilinearSample(*field, sourceColumn, sourceRow);
                    const auto color = GridColor(value, field->minimumValue, field->maximumValue);
                    raster[static_cast<std::size_t>(y) * drawnWidth + x] = static_cast<DWORD>(GetBValue(color))
                        | (static_cast<DWORD>(GetGValue(color)) << 8) | (static_cast<DWORD>(GetRValue(color)) << 16);
                }
            }

            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = drawnWidth;
            bitmap.bmiHeader.biHeight = -drawnHeight;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, drawLeft, drawTop, drawnWidth, drawnHeight, 0, 0, drawnWidth, drawnHeight,
                raster.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);

            HPEN borderPen = CreatePen(PS_SOLID, 3, RGB(20, 48, 69));
            const auto oldPen = SelectObject(dc, borderPen);
            const auto oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            for (const auto& ring : fvgRings)
            {
                std::vector<POINT> border;
                border.reserve(ring.size());
                for (const auto& point : ring)
                    border.push_back({ projectX(point.longitude), projectY(point.latitude) });
                Polygon(dc, border.data(), static_cast<int>(border.size()));
            }
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(borderPen);

            if (field->discipline == 0 && field->parameterCategory == 2 && (field->parameterNumber == 2 || field->parameterNumber == 3))
            {
                const auto zonal = field->parameterNumber == 2 ? field : FindWindComponent(*field, 2);
                const auto meridional = field->parameterNumber == 3 ? field : FindWindComponent(*field, 3);
                if (zonal && meridional)
                {
                    double maximumMagnitude = 0;
                    for (std::size_t index = 0; index < zonal->values.size(); ++index)
                        maximumMagnitude = (std::max)(maximumMagnitude, std::hypot(zonal->values[index], meridional->values[index]));

                    HPEN windPen = CreatePen(PS_SOLID, 1, RGB(45, 45, 45));
                    const auto previousPen = SelectObject(dc, windPen);
                    const auto arrowStride = (std::max)(1u, (field->columns + 5) / 6);
                    constexpr double Pi = 3.14159265358979323846;
                    for (std::uint32_t row = 0; row < field->rows; row += arrowStride)
                    {
                        for (std::uint32_t column = 0; column < field->columns; column += arrowStride)
                        {
                            const auto index = row * field->columns + column;
                            const auto zonalValue = zonal->values[index];
                            const auto meridionalValue = meridional->values[index];
                            const auto magnitude = std::hypot(zonalValue, meridionalValue);
                            if (magnitude == 0 || maximumMagnitude == 0)
                                continue;

                            const auto latitude = field->firstLatitude + row * rowStep;
                            const auto longitude = field->firstLongitude + column * columnStep;
                            if (!IsInFvg(longitude, latitude))
                                continue;
                            const auto x = projectX(longitude);
                            const auto y = projectY(latitude);
                            const auto angle = std::atan2(-meridionalValue, zonalValue);
                            const auto length = 6.0 + 12.0 * magnitude / maximumMagnitude;
                            const auto endX = x + static_cast<int>(std::cos(angle) * length);
                            const auto endY = y + static_cast<int>(std::sin(angle) * length);
                            MoveToEx(dc, x, y, nullptr);
                            LineTo(dc, endX, endY);
                            for (const double offset : { Pi * 0.78, -Pi * 0.78 })
                            {
                                MoveToEx(dc, endX, endY, nullptr);
                                LineTo(dc, endX + static_cast<int>(std::cos(angle + offset) * 5), endY + static_cast<int>(std::sin(angle + offset) * 5));
                            }
                        }
                    }
                    SelectObject(dc, previousPen);
                    DeleteObject(windPen);
                }
            }

            const struct { const wchar_t* name; double longitude; double latitude; } cities[] = {
                { L"Udine", 13.234, 46.071 }, { L"Trieste", 13.777, 45.650 },
                { L"Pordenone", 12.660, 45.956 }, { L"Gorizia", 13.620, 45.940 }
            };
            for (const auto& city : cities)
            {
                const int x = projectX(city.longitude);
                const int y = projectY(city.latitude);
                Ellipse(dc, x - 4, y - 4, x + 4, y + 4);
                DrawTextAt(dc, x + 7, y - 9, city.name, RGB(17, 37, 54), 15, true);
            }

            const auto score = SeverityFor(*field);
            const auto legend = std::format(L"{}  |  {:.1f} - {:.1f}  |  Fenomeni intensi: {}/10",
                field->parameterName, field->minimumValue, field->maximumValue, score);
            DrawTextAt(dc, 16, height - 42, legend.c_str(), RGB(24, 51, 77), 16, true);
        }
        else
        {
            DrawTextAt(dc, width * 18 / 100, height * 44 / 100, L"Il campo selezionato non contiene una griglia simple packing visualizzabile.", RGB(24, 51, 77), 17, true);
        }
        EndPaint(window, &paint);
    }

    void StartFieldLoad(HWND parent, std::size_t index);

    void UpdateSelection(HWND parent)
    {
        const auto index = static_cast<int>(SendMessageW(fieldsList, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(fields.size()))
            return;

        const auto& field = fields[index];
        const int score = SeverityFor(field);
        const std::wstring valueRange = field.values.empty() ? L"caricamento..." : std::format(L"{:.1f} - {:.1f}", field.minimumValue, field.maximumValue);
        const auto detail = std::format(
            L"Parametro: {}\r\nRiferimento: {}   Previsione: {}\r\nGriglia: {} x {} | valori: {}\r\nCoordinate: ({:.2f}, {:.2f}) - ({:.2f}, {:.2f})",
            field.parameterName, field.referenceTime, field.forecastTime,
            field.columns, field.rows, valueRange,
            field.firstLatitude, field.firstLongitude, field.lastLatitude, field.lastLongitude);
        const auto severity = field.values.empty() ? L"Decodifica del campo in background..." : std::format(L"Fenomeni intensi: {}/10", score);
        SetWindowTextW(detailsLabel, detail.c_str());
        SetWindowTextW(severityLabel, severity.c_str());
        InvalidateRect(GetDlgItem(parent, IdMap), nullptr, TRUE);
        StartFieldLoad(parent, static_cast<std::size_t>(index));
    }

    void StartFieldLoad(HWND parent, std::size_t index)
    {
        if (loading || index >= fields.size() || !fields[index].values.empty())
            return;
        if (loader.joinable())
            loader.join();
        loading = true;
        const auto path = std::filesystem::path(openedPath);
        const auto inventoryField = fields[index];
        loader = std::thread([parent, path, inventoryField, index]
        {
            auto result = std::make_unique<FieldResult>();
            result->index = index;
            try
            {
                result->field = GribReader{}.DecodeField(path, index, inventoryField);
            }
            catch (const std::exception& error) { result->error = error.what(); }
            PostMessageW(parent, MessageFieldReady, 0, reinterpret_cast<LPARAM>(result.release()));
        });
    }

    void StartInventoryLoad(HWND parent, const std::filesystem::path& path)
    {
        if (loading)
            return;
        if (loader.joinable())
            loader.join();
        loading = true;
        EnableWindow(openButton, FALSE);
        EnableWindow(downloadButton, FALSE);
        SetWindowTextW(detailsLabel, L"Lettura dell'inventario GRIB in background...");
        loader = std::thread([parent, path]
        {
            auto result = std::make_unique<InventoryResult>();
            result->path = path;
            try
            {
                result->inventory = GribReader{}.ReadInventory(path);
            }
            catch (const std::exception& error) { result->error = error.what(); }
            PostMessageW(parent, MessageInventoryReady, 0, reinterpret_cast<LPARAM>(result.release()));
        });
    }

    void ChooseAndLoadFile(HWND parent)
    {
        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = parent;
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"File GRIB (*.grib;*.grib2)\0*.grib;*.grib2\0Tutti i file\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog))
            StartInventoryLoad(parent, path);
    }

    void OpenUpdateChannel()
    {
        ShellExecuteW(nullptr, L"open", L"https://github.com/gmy77/GribViewer/releases/tag/continuous", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void DownloadLatestGrib(HWND parent)
    {
        wchar_t executablePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        const auto script = std::filesystem::path(executablePath).parent_path() / L"Download-LatestFvgGrib.ps1";
        if (!std::filesystem::exists(script))
        {
            MessageBoxW(parent, L"Lo script di download non e disponibile accanto all'eseguibile.", L"FVG GRIB Monitor", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto arguments = std::format(L"-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"{}\"", script.wstring());
        ShellExecuteW(parent, L"open", L"powershell.exe", arguments.c_str(), script.parent_path().c_str(), SW_SHOWNORMAL);
    }

    void ShowAbout(HWND parent)
    {
        MessageBoxW(parent,
            L"FVG GRIB Monitor\n\n"
            L"Dashboard operativa per l'inventario e la consultazione di file GRIB2 "
            L"del Friuli Venezia Giulia.\n\n"
            L"Made by Gimmy Pignolo and Copilot",
            L"About FVG GRIB Monitor", MB_OK | MB_ICONINFORMATION);
    }

    void SetControlFont(HWND control, HFONT font)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    LRESULT CALLBACK MapProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_PAINT)
        {
            PaintMap(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            headerBrush = CreateSolidBrush(RGB(18, 92, 118));
            bodyBrush = CreateSolidBrush(RGB(241, 245, 247));
            infoBrush = CreateSolidBrush(RGB(255, 255, 255));
            titleFont = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            titleLabel = CreateWindowW(L"STATIC", L"FVG GRIB MONITOR", WS_CHILD | WS_VISIBLE, 16, 20, 270, 32, window, nullptr, instance, nullptr);
            openButton = CreateWindowW(L"BUTTON", L"\u25A3  Apri GRIB", WS_CHILD | WS_VISIBLE | BS_FLAT, 0, 17, 140, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdOpen)), instance, nullptr);
            downloadButton = CreateWindowW(L"BUTTON", L"\u21E9  Scarica GRIB", WS_CHILD | WS_VISIBLE | BS_FLAT, 0, 17, 155, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDownload)), instance, nullptr);
            updateButton = CreateWindowW(L"BUTTON", L"\u21BB  Aggiorna", WS_CHILD | WS_VISIBLE | BS_FLAT, 0, 17, 120, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdUpdate)), instance, nullptr);
            aboutButton = CreateWindowW(L"BUTTON", L"\u24D8  About", WS_CHILD | WS_VISIBLE | BS_FLAT, 0, 17, 100, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdAbout)), instance, nullptr);
            fieldsHeading = CreateWindowW(L"STATIC", L"CAMPI METEOROLOGICI", WS_CHILD | WS_VISIBLE, 16, 86, 300, 22, window, nullptr, instance, nullptr);
            fieldsList = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 16, 112, 380, 236, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFields)), instance, nullptr);
            detailsHeading = CreateWindowW(L"STATIC", L"DETTAGLIO DEL CAMPO", WS_CHILD | WS_VISIBLE, 16, 366, 300, 22, window, nullptr, instance, nullptr);
            detailsLabel = CreateWindowW(L"STATIC", L"Seleziona un file GRIB2.", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 392, 380, 126, window, nullptr, instance, nullptr);
            severityLabel = CreateWindowW(L"STATIC", L"Indice: n/d", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 530, 380, 32, window, nullptr, instance, nullptr);
            mapHeading = CreateWindowW(L"STATIC", L"MAPPA OPERATIVA FVG", WS_CHILD | WS_VISIBLE, 410, 86, 300, 22, window, nullptr, instance, nullptr);
            CreateWindowW(L"FvgMap", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER, 410, 112, 660, 440, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMap)), instance, nullptr);

            SetControlFont(titleLabel, titleFont);
            for (const HWND control : { openButton, downloadButton, updateButton, aboutButton, fieldsHeading, fieldsList, detailsHeading, detailsLabel, severityLabel, mapHeading })
                SetControlFont(control, uiFont);
            return 0;
        case WM_SIZE:
        {
            RECT client{};
            GetClientRect(window, &client);
            const int panelWidth = (std::max)(390, static_cast<int>(client.right) / 3);
            const int right = static_cast<int>(client.right) - 16;
            MoveWindow(openButton, right - 545, 17, 140, 36, TRUE);
            MoveWindow(downloadButton, right - 395, 17, 155, 36, TRUE);
            MoveWindow(updateButton, right - 230, 17, 120, 36, TRUE);
            MoveWindow(aboutButton, right - 100, 17, 100, 36, TRUE);
            MoveWindow(fieldsHeading, 16, 86, panelWidth - 32, 22, TRUE);
            MoveWindow(fieldsList, 16, 112, panelWidth - 32, 236, TRUE);
            MoveWindow(detailsHeading, 16, 366, panelWidth - 32, 22, TRUE);
            MoveWindow(detailsLabel, 16, 392, panelWidth - 32, 126, TRUE);
            MoveWindow(severityLabel, 16, 530, panelWidth - 32, 32, TRUE);
            MoveWindow(mapHeading, panelWidth + 16, 86, client.right - panelWidth - 32, 22, TRUE);
            MoveWindow(GetDlgItem(window, IdMap), panelWidth + 16, 112, client.right - panelWidth - 32, client.bottom - 128, TRUE);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, bodyBrush);
            client.bottom = 70;
            FillRect(dc, &client, headerBrush);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const auto control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            if (control == titleLabel)
            {
                SetTextColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(headerBrush);
            }
            if (control == detailsLabel || control == severityLabel)
            {
                SetTextColor(dc, RGB(31, 54, 66));
                return reinterpret_cast<LRESULT>(infoBrush);
            }
            SetTextColor(dc, RGB(33, 79, 96));
            return reinterpret_cast<LRESULT>(bodyBrush);
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IdOpen)
                ChooseAndLoadFile(window);
            else if (LOWORD(wParam) == IdDownload)
                DownloadLatestGrib(window);
            else if (LOWORD(wParam) == IdUpdate)
                OpenUpdateChannel();
            else if (LOWORD(wParam) == IdAbout)
                ShowAbout(window);
            else if (LOWORD(wParam) == IdFields && HIWORD(wParam) == LBN_SELCHANGE)
                UpdateSelection(window);
            return 0;
        case MessageInventoryReady:
        {
            const std::unique_ptr<InventoryResult> result(reinterpret_cast<InventoryResult*>(lParam));
            if (loader.joinable())
                loader.join();
            loading = false;
            EnableWindow(openButton, TRUE);
            EnableWindow(downloadButton, TRUE);
            if (!result->error.empty())
            {
                MessageBoxA(window, result->error.c_str(), "FVG GRIB Monitor", MB_OK | MB_ICONERROR);
                return 0;
            }
            fields = std::move(result->inventory);
            openedPath = result->path.wstring();
            SendMessageW(fieldsList, LB_RESETCONTENT, 0, 0);
            for (const auto& field : fields)
            {
                const auto name = std::format(L"{}  |  {}  |  {}", field.parameterName, field.referenceTime, field.forecastTime);
                SendMessageW(fieldsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
            SendMessageW(fieldsList, LB_SETCURSEL, 0, 0);
            UpdateSelection(window);
            SetWindowTextW(window, std::format(L"FVG GRIB Monitor - {}", result->path.filename().wstring()).c_str());
            return 0;
        }
        case MessageFieldReady:
        {
            const std::unique_ptr<FieldResult> result(reinterpret_cast<FieldResult*>(lParam));
            if (loader.joinable())
                loader.join();
            loading = false;
            if (!result->error.empty())
            {
                MessageBoxA(window, result->error.c_str(), "FVG GRIB Monitor", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (result->index < fields.size())
                fields[result->index] = std::move(result->field);
            UpdateSelection(window);
            return 0;
        }
        case WM_DESTROY:
            if (loader.joinable())
                loader.join();
            DeleteObject(headerBrush);
            DeleteObject(bodyBrush);
            DeleteObject(infoBrush);
            DeleteObject(titleFont);
            DeleteObject(uiFont);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE application, HINSTANCE, PWSTR, int commandShow)
{
    instance = application;
    try
    {
        LoadFvgBoundary();
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "FVG GRIB Monitor", MB_OK | MB_ICONERROR);
        return 1;
    }
    WNDCLASSW mapClass{ .lpfnWndProc = MapProc, .hInstance = instance, .hCursor = LoadCursor(nullptr, IDC_ARROW), .lpszClassName = L"FvgMap" };
    RegisterClassW(&mapClass);
    WNDCLASSW mainClass{ .lpfnWndProc = MainProc, .hInstance = instance, .hCursor = LoadCursor(nullptr, IDC_ARROW), .lpszClassName = L"FvgGribMonitor" };
    RegisterClassW(&mainClass);

    const auto window = CreateWindowExW(0, mainClass.lpszClassName, L"FVG GRIB Monitor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 650, nullptr, nullptr, instance, nullptr);
    constexpr DWORD DwmwaSystemBackdropType = 38;
    constexpr int MicaBackdrop = 2;
    DwmSetWindowAttribute(window, DwmwaSystemBackdropType, &MicaBackdrop, sizeof(MicaBackdrop));
    ShowWindow(window, commandShow);

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        if (argumentCount == 2)
            StartInventoryLoad(window, arguments[1]);
        LocalFree(arguments);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
