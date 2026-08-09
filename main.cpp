#include "GribReader.h"

#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace
{
    constexpr int IdOpen = 1001;
    constexpr int IdFields = 1002;
    constexpr int IdMap = 1003;

    HINSTANCE instance{};
    HWND fieldsList{};
    HWND detailsLabel{};
    HWND severityLabel{};
    std::vector<GribField> fields;
    std::wstring openedPath;

    int SeverityFor(const GribField& field)
    {
        if (field.discipline == 0 && field.parameterCategory == 7 && field.parameterNumber == 6)
            return 7;
        if (field.discipline == 0 && field.parameterCategory == 1 && field.parameterNumber == 8)
            return 6;
        if (field.discipline == 0 && field.parameterCategory == 2)
            return 5;
        return 3;
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
        const int score = fields.empty() ? 0 : SeverityFor(fields[std::clamp<int>(static_cast<int>(SendMessageW(fieldsList, LB_GETCURSEL, 0, 0)), 0, static_cast<int>(fields.size() - 1))]);
        const COLORREF fill = score >= 7 ? RGB(247, 124, 99) : score >= 5 ? RGB(247, 201, 94) : RGB(126, 196, 136);

        POINT border[] = {
            { width * 19 / 100, height * 34 / 100 }, { width * 36 / 100, height * 22 / 100 },
            { width * 62 / 100, height * 19 / 100 }, { width * 82 / 100, height * 30 / 100 },
            { width * 88 / 100, height * 53 / 100 }, { width * 76 / 100, height * 75 / 100 },
            { width * 50 / 100, height * 82 / 100 }, { width * 31 / 100, height * 70 / 100 },
            { width * 19 / 100, height * 51 / 100 }
        };
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(37, 67, 96));
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, pen);
        Polygon(dc, border, static_cast<int>(std::size(border)));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        DrawTextAt(dc, width * 38 / 100, height * 29 / 100, L"Udine", RGB(24, 51, 77), 17, true);
        DrawTextAt(dc, width * 66 / 100, height * 53 / 100, L"Trieste", RGB(24, 51, 77), 17, true);
        DrawTextAt(dc, width * 29 / 100, height * 50 / 100, L"Pordenone", RGB(24, 51, 77), 17, true);
        DrawTextAt(dc, width * 54 / 100, height * 65 / 100, L"Gorizia", RGB(24, 51, 77), 17, true);
        DrawTextAt(dc, width * 20 / 100, 12, L"Friuli Venezia Giulia", RGB(24, 51, 77), 22, true);

        if (score > 0)
        {
            const auto label = std::format(L"Indice eventi intensi: {}/10", score);
            DrawTextAt(dc, width * 28 / 100, height * 41 / 100, label.c_str(), RGB(82, 30, 23), 25, true);
        }
        else
        {
            DrawTextAt(dc, width * 24 / 100, height * 44 / 100, L"Aprire un file .grib/.grib2", RGB(24, 51, 77), 21, true);
        }
        EndPaint(window, &paint);
    }

    void UpdateSelection(HWND parent)
    {
        const auto index = static_cast<int>(SendMessageW(fieldsList, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(fields.size()))
            return;

        const auto& field = fields[index];
        const int score = SeverityFor(field);
        const auto detail = std::format(
            L"Parametro: {}\r\nRiferimento: {}   Previsione: {}\r\nMessaggio: {} byte @ {}   Griglia: template {}, {} punti ({} x {})\r\nProdotto: template {}",
            field.parameterName, field.referenceTime, field.forecastTime, field.length, field.offset,
            field.gridTemplate, field.pointCount, field.columns, field.rows, field.productTemplate);
        const auto severity = std::format(L"Indice indicativo: {}/10", score);
        SetWindowTextW(detailsLabel, detail.c_str());
        SetWindowTextW(severityLabel, severity.c_str());
        InvalidateRect(GetDlgItem(parent, IdMap), nullptr, TRUE);
    }

    bool LoadFile(HWND parent, const std::filesystem::path& path)
    {
        try
        {
            fields = GribReader{}.ReadInventory(path);
            openedPath = path.wstring();
            SendMessageW(fieldsList, LB_RESETCONTENT, 0, 0);
            for (const auto& field : fields)
            {
                const auto name = std::format(L"{}  |  {}  |  {}", field.parameterName, field.referenceTime, field.forecastTime);
                SendMessageW(fieldsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
            SendMessageW(fieldsList, LB_SETCURSEL, 0, 0);
            UpdateSelection(parent);
            SetWindowTextW(parent, std::format(L"FVG GRIB Monitor - {}", path.filename().wstring()).c_str());
            return true;
        }
        catch (const std::exception& error)
        {
            MessageBoxA(parent, error.what(), "FVG GRIB Monitor", MB_OK | MB_ICONERROR);
            return false;
        }
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
            LoadFile(parent, path);
    }

    LRESULT CALLBACK MapProc(HWND window, UINT message, WPARAM, LPARAM)
    {
        if (message == WM_PAINT)
        {
            PaintMap(window);
            return 0;
        }
        return DefWindowProcW(window, message, 0, 0);
    }

    LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            CreateWindowW(L"BUTTON", L"Apri GRIB...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 14, 14, 130, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdOpen)), instance, nullptr);
            CreateWindowW(L"STATIC", L"Inventario e opzioni di visualizzazione", WS_CHILD | WS_VISIBLE | SS_LEFT, 160, 20, 230, 24, window, nullptr, instance, nullptr);
            fieldsList = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 14, 58, 380, 260, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFields)), instance, nullptr);
            detailsLabel = CreateWindowW(L"STATIC", L"Seleziona un file GRIB2.", WS_CHILD | WS_VISIBLE | SS_LEFT, 14, 330, 380, 150, window, nullptr, instance, nullptr);
            severityLabel = CreateWindowW(L"STATIC", L"Indice: n/d", WS_CHILD | WS_VISIBLE | SS_LEFT, 14, 490, 380, 32, window, nullptr, instance, nullptr);
            CreateWindowW(L"FvgMap", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER, 410, 14, 660, 510, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMap)), instance, nullptr);
            return 0;
        case WM_SIZE:
        {
            RECT client{};
            GetClientRect(window, &client);
            const int panelWidth = (std::max)(360, static_cast<int>(client.right) / 3);
            MoveWindow(fieldsList, 14, 58, panelWidth - 28, 260, TRUE);
            MoveWindow(detailsLabel, 14, 330, panelWidth - 28, 150, TRUE);
            MoveWindow(severityLabel, 14, 490, panelWidth - 28, 32, TRUE);
            MoveWindow(GetDlgItem(window, IdMap), panelWidth + 2, 14, client.right - panelWidth - 16, client.bottom - 28, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IdOpen)
                ChooseAndLoadFile(window);
            else if (LOWORD(wParam) == IdFields && HIWORD(wParam) == LBN_SELCHANGE)
                UpdateSelection(window);
            return 0;
        case WM_DESTROY:
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
    WNDCLASSW mapClass{ .lpfnWndProc = MapProc, .hInstance = instance, .hCursor = LoadCursor(nullptr, IDC_ARROW), .lpszClassName = L"FvgMap" };
    RegisterClassW(&mapClass);
    WNDCLASSW mainClass{ .lpfnWndProc = MainProc, .hInstance = instance, .hCursor = LoadCursor(nullptr, IDC_ARROW), .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1), .lpszClassName = L"FvgGribMonitor" };
    RegisterClassW(&mainClass);

    const auto window = CreateWindowExW(0, mainClass.lpszClassName, L"FVG GRIB Monitor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 650, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, commandShow);

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        if (argumentCount == 2)
            LoadFile(window, arguments[1]);
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
