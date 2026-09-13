#pragma once

#include <windows.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace ErrorLog
{
    inline std::string FormatSystemError(DWORD errorCode)
    {
        char* messageBuffer = nullptr;
        const DWORD messageLength = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&messageBuffer),
            0,
            nullptr);

        if (messageLength == 0 || messageBuffer == nullptr)
        {
            return "No system description is available.";
        }

        std::string message(messageBuffer, messageLength);
        LocalFree(messageBuffer);
        while (!message.empty() &&
            (message.back() == '\r' || message.back() == '\n'))
        {
            message.pop_back();
        }
        return message;
    }

    inline std::string Narrow(std::wstring_view message)
    {
        if (message.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            message.data(),
            static_cast<int>(message.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (size <= 0)
        {
            return "<wide string conversion failed>";
        }

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            message.data(),
            static_cast<int>(message.size()),
            result.data(),
            size,
            nullptr,
            nullptr);
        return result;
    }

    inline void Write(std::string_view message) noexcept
    {
        try
        {
            std::string line(message);
            line.push_back('\n');
            OutputDebugStringA(line.c_str());
        }
        catch (...)
        {
            OutputDebugStringA("ErrorLog: failed to format an error message.\n");
        }
    }

    inline void HRESULT(std::string_view operation, ::HRESULT result) noexcept
    {
        try
        {
            std::ostringstream message;
            message << operation
                << "\nHRESULT: 0x"
                << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<unsigned long>(result)
                << "\nReason: "
                << FormatSystemError(static_cast<DWORD>(result));
            Write(message.str());
        }
        catch (...)
        {
            Write(operation);
        }
    }

    inline void Win32(std::string_view operation, DWORD errorCode) noexcept
    {
        try
        {
            std::ostringstream message;
            message << operation
                << "\nWin32 error: " << errorCode
                << "\nReason: " << FormatSystemError(errorCode);
            Write(message.str());
        }
        catch (...)
        {
            Write(operation);
        }
    }
}
